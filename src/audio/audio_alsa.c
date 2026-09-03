// Системный вывод Linux через ALSA. Выбор именно её, а не PipeWire
// напрямую: PipeWire и PulseAudio предоставляют совместимый ALSA-выход,
// поэтому один интерфейс покрывает и обычный десктоп, и Steam Deck.
//
// Библиотека загружается в рантайме, а не линкуется. Причина в том, что
// движок поставляется как SDK: слинкованный модуль не загрузился бы
// вовсе на машине без libasound.so.2 — вместе с микшером и его
// offscreen-бэкендом, которым звуковая подсистема не нужна. При
// динамической загрузке отсутствие ALSA означает лишь отказ системного
// вывода. Заголовки ALSA на сборке тоже не требуются: нужные типы и
// константы объявлены здесь, а их значения сверены с компилятором на
// настоящих заголовках, а не взяты по памяти.
//
// Запись блокирующая: поток вывода просыпается ровно тогда, когда
// устройство готово принять период, и не крутит опрос.

#include "audio/audio_backend.h"
#include "platform/system.h"

#include <string.h>

#define AUDIO_ALSA_DEFAULT_SAMPLE_RATE 48000u
#define AUDIO_ALSA_DEFAULT_LATENCY_MICROSECONDS 20000u
#define AUDIO_ALSA_CHANNELS 2u

// Значения получены компиляцией пробы с настоящими заголовками ALSA:
// перечисления в них неявные, и считать их глазами нельзя.
#define ALSA_STREAM_PLAYBACK 0
#define ALSA_FORMAT_FLOAT_LE 14
#define ALSA_ACCESS_RW_INTERLEAVED 3

typedef struct AlsaPcm AlsaPcm;
typedef unsigned long AlsaUFrames;
typedef long AlsaSFrames;

typedef int (*AlsaPcmOpen)(AlsaPcm **pcm, const char *name, int stream, int mode);
typedef int (*AlsaPcmSetParams)(AlsaPcm *pcm, int format, int access, unsigned int channels,
                                unsigned int rate, int softResample, unsigned int latency);
typedef int (*AlsaPcmGetParams)(AlsaPcm *pcm, AlsaUFrames *bufferSize, AlsaUFrames *periodSize);
typedef AlsaSFrames (*AlsaPcmWriteInterleaved)(AlsaPcm *pcm, const void *buffer, AlsaUFrames size);
typedef int (*AlsaPcmRecover)(AlsaPcm *pcm, int error, int silent);
typedef int (*AlsaPcmDrop)(AlsaPcm *pcm);
typedef int (*AlsaPcmClose)(AlsaPcm *pcm);

typedef struct AlsaApi
{
    PlatformDynamicLibrary library;
    AlsaPcmOpen open;
    AlsaPcmSetParams setParams;
    AlsaPcmGetParams getParams;
    AlsaPcmWriteInterleaved writeInterleaved;
    AlsaPcmRecover recover;
    AlsaPcmDrop drop;
    AlsaPcmClose close;
} AlsaApi;

typedef struct AudioAlsaBackend
{
    AudioBackend base;

    AudioRenderCallback render;
    void *context;

    AlsaApi api;
    AlsaPcm *pcm;
    PlatformThread thread;
    bool threadStarted;
    volatile uint32_t running;
    volatile int64_t underruns;

    float *mixFrames;
    uint32_t periodFrames;
} AudioAlsaBackend;

static void AlsaApiUnload(AlsaApi *api)
{
    if (api->library != NULL) PlatformDynamicLibraryClose(api->library);
    memset(api, 0, sizeof(*api));
}

static bool AlsaApiLoad(AlsaApi *api)
{
    memset(api, 0, sizeof(*api));

    // Загружается версионированное имя: symlink libasound.so ставится
    // только пакетом разработки, а он на игровой машине не нужен.
    api->library = PlatformDynamicLibraryOpen(L"libasound.so.2");
    if (api->library == NULL) return false;

    struct
    {
        const char *name;
        void **target;
    } symbols[] = {
        { "snd_pcm_open", (void **)&api->open },
        { "snd_pcm_set_params", (void **)&api->setParams },
        { "snd_pcm_get_params", (void **)&api->getParams },
        { "snd_pcm_writei", (void **)&api->writeInterleaved },
        { "snd_pcm_recover", (void **)&api->recover },
        { "snd_pcm_drop", (void **)&api->drop },
        { "snd_pcm_close", (void **)&api->close },
    };
    for (uint32_t index = 0; index < sizeof(symbols) / sizeof(symbols[0]); ++index)
    {
        *symbols[index].target = PlatformDynamicLibrarySymbol(api->library, symbols[index].name);
        if (*symbols[index].target == NULL)
        {
            AlsaApiUnload(api);
            return false;
        }
    }
    return true;
}

static uint32_t RenderThreadEntry(void *parameter)
{
    AudioAlsaBackend *backend = (AudioAlsaBackend *)parameter;

    while (PlatformAtomicLoadU32Acquire(&backend->running) != 0u)
    {
        backend->render(backend->context, backend->mixFrames, backend->periodFrames);

        AlsaSFrames written = backend->api.writeInterleaved(backend->pcm, backend->mixFrames,
                                                            backend->periodFrames);
        if (written < 0)
        {
            // Опустошение буфера и приостановка устройства восстановимы;
            // всё остальное означает, что устройство ушло.
            PlatformAtomicIncrementI64(&backend->underruns);
            if (backend->api.recover(backend->pcm, (int)written, 1) < 0) break;
        }
    }
    return 0u;
}

static void AlsaDestroy(AudioBackend *base)
{
    AudioAlsaBackend *backend = (AudioAlsaBackend *)base;

    PlatformAtomicStoreU32Release(&backend->running, 0u);
    if (backend->threadStarted)
    {
        // Поток стоит внутри snd_pcm_writei; drop прерывает передачу и
        // возвращает вызов с ошибкой, после чего цикл видит running = 0.
        if (backend->pcm != NULL) backend->api.drop(backend->pcm);
        PlatformThreadJoin(&backend->thread);
    }
    if (backend->pcm != NULL) backend->api.close(backend->pcm);
    if (backend->mixFrames != NULL) PlatformFree(backend->mixFrames);
    AlsaApiUnload(&backend->api);
    PlatformFree(backend);
}

static uint64_t AlsaUnderrunCount(const AudioBackend *base)
{
    const AudioAlsaBackend *backend = (const AudioAlsaBackend *)base;
    return (uint64_t)PlatformAtomicLoadI64(&backend->underruns);
}

static const AudioBackendVtable ALSA_VTABLE = {
    .destroy = AlsaDestroy,
    .underrunCount = AlsaUnderrunCount,
};

bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL) return false;
    *outBackend = NULL;

    AudioAlsaBackend *backend = PlatformAllocate(sizeof(*backend), true);
    if (backend == NULL) return false;

    backend->base.vtable = &ALSA_VTABLE;
    backend->render = description->render;
    backend->context = description->context;
    backend->running = 1u;

    if (!AlsaApiLoad(&backend->api))
    {
        PlatformFree(backend);
        return false;
    }

    uint32_t sampleRate =
        description->sampleRate != 0u ? description->sampleRate : AUDIO_ALSA_DEFAULT_SAMPLE_RATE;
    unsigned int latencyMicroseconds = AUDIO_ALSA_DEFAULT_LATENCY_MICROSECONDS;
    if (description->frameCountHint != 0u)
    {
        latencyMicroseconds =
            (unsigned int)(((uint64_t)description->frameCountHint * 1000000u) / sampleRate);
    }

    if (backend->api.open(&backend->pcm, "default", ALSA_STREAM_PLAYBACK, 0) < 0)
    {
        backend->pcm = NULL;
        AlsaDestroy(&backend->base);
        return false;
    }

    // soft_resample = 1: если устройство не умеет запрошенную частоту,
    // ALSA пересчитает сама, и микшер продолжит работать в своей.
    if (backend->api.setParams(backend->pcm, ALSA_FORMAT_FLOAT_LE, ALSA_ACCESS_RW_INTERLEAVED,
                               AUDIO_ALSA_CHANNELS, sampleRate, 1, latencyMicroseconds) < 0)
    {
        AlsaDestroy(&backend->base);
        return false;
    }

    AlsaUFrames bufferFrames = 0;
    AlsaUFrames periodFrames = 0;
    if (backend->api.getParams(backend->pcm, &bufferFrames, &periodFrames) < 0
        || periodFrames == 0u)
    {
        AlsaDestroy(&backend->base);
        return false;
    }

    backend->periodFrames = (uint32_t)periodFrames;
    backend->mixFrames =
        PlatformAllocate((size_t)backend->periodFrames * AUDIO_ALSA_CHANNELS * sizeof(float),
                         false);
    if (backend->mixFrames == NULL)
    {
        AlsaDestroy(&backend->base);
        return false;
    }

    backend->base.sampleRate = sampleRate;
    backend->base.channelCount = AUDIO_ALSA_CHANNELS;
    backend->base.bufferFrameCount = (uint32_t)bufferFrames;

    if (!PlatformThreadStart(&backend->thread, RenderThreadEntry, backend))
    {
        AlsaDestroy(&backend->base);
        return false;
    }
    backend->threadStarted = true;

    *outBackend = &backend->base;
    return true;
}
