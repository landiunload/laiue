// Системный вывод Android через AAudio (API 26+, профиль движка — 28+).
// AAudio выбран вместо OpenSL ES: это нынешний штатный путь Android с
// низкой задержкой, и его callback-модель совпадает с контрактом микшера —
// система сама зовёт заполнение буфера на своём realtime-потоке.
//
// Библиотека загружается в рантайме по той же причине, что ALSA на Linux:
// статическому приложению не нужно добавлять -laaudio в свою финальную
// линковку, а отсутствие AAudio означает лишь отказ системного вывода.
// Типы и константы берутся из заголовка NDK, функции — через dlsym.

#include "audio/audio_backend.h"
#include "platform/system.h"

#include <aaudio/AAudio.h>
#include <string.h>

#define AUDIO_AAUDIO_CHANNELS 2
#define AUDIO_AAUDIO_STOP_TIMEOUT_NANOSECONDS INT64_C(200000000)

typedef aaudio_result_t (*AAudioCreateBuilderFn)(AAudioStreamBuilder **builder);
typedef void (*AAudioBuilderSetInt32Fn)(AAudioStreamBuilder *builder, int32_t value);
typedef void (*AAudioBuilderSetDataCallbackFn)(AAudioStreamBuilder *builder,
                                               AAudioStream_dataCallback callback,
                                               void *userData);
typedef void (*AAudioBuilderSetErrorCallbackFn)(AAudioStreamBuilder *builder,
                                                AAudioStream_errorCallback callback,
                                                void *userData);
typedef aaudio_result_t (*AAudioBuilderOpenFn)(AAudioStreamBuilder *builder,
                                               AAudioStream **stream);
typedef aaudio_result_t (*AAudioBuilderDeleteFn)(AAudioStreamBuilder *builder);
typedef aaudio_result_t (*AAudioStreamCallFn)(AAudioStream *stream);
typedef int32_t (*AAudioStreamGetInt32Fn)(AAudioStream *stream);
typedef aaudio_result_t (*AAudioStreamWaitFn)(AAudioStream *stream,
                                              aaudio_stream_state_t inputState,
                                              aaudio_stream_state_t *nextState,
                                              int64_t timeoutNanoseconds);

typedef struct AAudioApi
{
    PlatformDynamicLibrary library;
    AAudioCreateBuilderFn createBuilder;
    AAudioBuilderSetInt32Fn setDirection;
    AAudioBuilderSetInt32Fn setSampleRate;
    AAudioBuilderSetInt32Fn setChannelCount;
    AAudioBuilderSetInt32Fn setFormat;
    AAudioBuilderSetInt32Fn setPerformanceMode;
    AAudioBuilderSetInt32Fn setSharingMode;
    AAudioBuilderSetInt32Fn setFramesPerDataCallback;
    AAudioBuilderSetDataCallbackFn setDataCallback;
    AAudioBuilderSetErrorCallbackFn setErrorCallback;
    AAudioBuilderOpenFn openStream;
    AAudioBuilderDeleteFn deleteBuilder;
    AAudioStreamCallFn requestStart;
    AAudioStreamCallFn requestStop;
    AAudioStreamCallFn close;
    AAudioStreamWaitFn waitForStateChange;
    AAudioStreamGetInt32Fn getSampleRate;
    AAudioStreamGetInt32Fn getChannelCount;
    AAudioStreamGetInt32Fn getFormat;
    AAudioStreamGetInt32Fn getBufferSize;
    AAudioStreamGetInt32Fn getXRunCount;
} AAudioApi;

typedef struct AudioAAudioBackend
{
    AudioBackend base;

    AudioRenderCallback render;
    void *context;

    AAudioApi api;
    AAudioStream *stream;
    // Отключение устройства (наушники, Bluetooth) останавливает поток
    // навсегда; приложение пересоздаёт вывод, а счётчик даёт это увидеть.
    volatile int64_t disconnects;
} AudioAAudioBackend;

static void AAudioApiUnload(AAudioApi *api)
{
    if (api->library != NULL)
        PlatformDynamicLibraryClose(api->library);
    memset(api, 0, sizeof(*api));
}

static bool AAudioApiLoad(AAudioApi *api)
{
    memset(api, 0, sizeof(*api));
    api->library = PlatformDynamicLibraryOpen(L"libaaudio.so");
    if (api->library == NULL)
        return false;

    struct
    {
        const char *name;
        void **target;
    } symbols[] = {
        {"AAudio_createStreamBuilder", (void **)&api->createBuilder},
        {"AAudioStreamBuilder_setDirection", (void **)&api->setDirection},
        {"AAudioStreamBuilder_setSampleRate", (void **)&api->setSampleRate},
        {"AAudioStreamBuilder_setChannelCount", (void **)&api->setChannelCount},
        {"AAudioStreamBuilder_setFormat", (void **)&api->setFormat},
        {"AAudioStreamBuilder_setPerformanceMode", (void **)&api->setPerformanceMode},
        {"AAudioStreamBuilder_setSharingMode", (void **)&api->setSharingMode},
        {"AAudioStreamBuilder_setFramesPerDataCallback",
         (void **)&api->setFramesPerDataCallback},
        {"AAudioStreamBuilder_setDataCallback", (void **)&api->setDataCallback},
        {"AAudioStreamBuilder_setErrorCallback", (void **)&api->setErrorCallback},
        {"AAudioStreamBuilder_openStream", (void **)&api->openStream},
        {"AAudioStreamBuilder_delete", (void **)&api->deleteBuilder},
        {"AAudioStream_requestStart", (void **)&api->requestStart},
        {"AAudioStream_requestStop", (void **)&api->requestStop},
        {"AAudioStream_close", (void **)&api->close},
        {"AAudioStream_waitForStateChange", (void **)&api->waitForStateChange},
        {"AAudioStream_getSampleRate", (void **)&api->getSampleRate},
        {"AAudioStream_getChannelCount", (void **)&api->getChannelCount},
        {"AAudioStream_getFormat", (void **)&api->getFormat},
        {"AAudioStream_getBufferSizeInFrames", (void **)&api->getBufferSize},
        {"AAudioStream_getXRunCount", (void **)&api->getXRunCount},
    };
    for (uint32_t index = 0u; index < sizeof(symbols) / sizeof(symbols[0]); ++index)
    {
        *symbols[index].target = PlatformDynamicLibrarySymbol(api->library, symbols[index].name);
        if (*symbols[index].target == NULL)
        {
            AAudioApiUnload(api);
            return false;
        }
    }
    return true;
}

// Realtime-поток AAudio: никаких блокировок и аллокаций, только микшер.
static aaudio_data_callback_result_t AAudioDataCallback(AAudioStream *stream, void *userData,
                                                        void *audioData, int32_t numFrames)
{
    (void)stream;
    AudioAAudioBackend *backend = (AudioAAudioBackend *)userData;
    if (numFrames > 0)
        backend->render(backend->context, (float *)audioData, (uint32_t)numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void AAudioErrorCallback(AAudioStream *stream, void *userData, aaudio_result_t error)
{
    (void)stream;
    (void)error;
    // Закрывать или переоткрывать поток из этого callback-а запрещено
    // документацией AAudio; здесь только отмечается, что вывод ушёл.
    PlatformAtomicIncrementI64(&((AudioAAudioBackend *)userData)->disconnects);
}

static void AAudioStopAndClose(AudioAAudioBackend *backend)
{
    if (backend->stream == NULL)
        return;
    // Ждём остановки, чтобы data callback гарантированно не шёл во время
    // close: на ранних версиях Android close сам этого не ожидал.
    if (backend->api.requestStop(backend->stream) == AAUDIO_OK)
    {
        aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
        for (uint32_t attempt = 0u; attempt < 10u && state == AAUDIO_STREAM_STATE_STOPPING;
             ++attempt)
        {
            aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
            if (backend->api.waitForStateChange(backend->stream, state, &next,
                                                AUDIO_AAUDIO_STOP_TIMEOUT_NANOSECONDS) !=
                AAUDIO_OK)
                break;
            state = next;
        }
    }
    backend->api.close(backend->stream);
    backend->stream = NULL;
}

static void AAudioDestroy(AudioBackend *base)
{
    AudioAAudioBackend *backend = (AudioAAudioBackend *)base;
    AAudioStopAndClose(backend);
    AAudioApiUnload(&backend->api);
    AudioBackendFree(&backend->base.allocator, backend);
}

static uint64_t AAudioUnderrunCount(const AudioBackend *base)
{
    const AudioAAudioBackend *backend = (const AudioAAudioBackend *)base;
    int32_t xruns = backend->stream != NULL ? backend->api.getXRunCount(backend->stream) : 0;
    if (xruns < 0)
        xruns = 0;
    return (uint64_t)xruns + (uint64_t)PlatformAtomicLoadI64(&backend->disconnects);
}

static const AudioBackendVtable AAUDIO_VTABLE = {
    .destroy = AAudioDestroy,
    .underrunCount = AAudioUnderrunCount,
};

bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL)
        return false;
    *outBackend = NULL;
    if (!AudioBackendAllocatorIsValid(&description->allocator) ||
        description->sampleRate > (uint32_t)INT32_MAX ||
        description->frameCountHint > (uint32_t)INT32_MAX)
        return false;

    AudioAAudioBackend *backend = (AudioAAudioBackend *)AudioBackendAllocate(
        &description->allocator, sizeof(*backend), true);
    if (backend == NULL)
        return false;
    backend->base.vtable = &AAUDIO_VTABLE;
    backend->base.allocator = description->allocator;
    backend->render = description->render;
    backend->context = description->context;

    if (!AAudioApiLoad(&backend->api))
    {
        AudioBackendFree(&backend->base.allocator, backend);
        return false;
    }

    AAudioStreamBuilder *builder = NULL;
    if (backend->api.createBuilder(&builder) != AAUDIO_OK || builder == NULL)
    {
        AAudioDestroy(&backend->base);
        return false;
    }
    backend->api.setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    backend->api.setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    backend->api.setChannelCount(builder, AUDIO_AAUDIO_CHANNELS);
    backend->api.setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    backend->api.setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // 0 означает AAUDIO_UNSPECIFIED: частота и размер callback-а устройства.
    backend->api.setSampleRate(builder, (int32_t)description->sampleRate);
    backend->api.setFramesPerDataCallback(builder, (int32_t)description->frameCountHint);
    backend->api.setDataCallback(builder, AAudioDataCallback, backend);
    backend->api.setErrorCallback(builder, AAudioErrorCallback, backend);
    const aaudio_result_t opened = backend->api.openStream(builder, &backend->stream);
    backend->api.deleteBuilder(builder);
    if (opened != AAUDIO_OK || backend->stream == NULL)
    {
        backend->stream = NULL;
        AAudioDestroy(&backend->base);
        return false;
    }

    // Микшер пишет только чередующийся float-стерео: иной согласованный
    // формат значит, что устройство отказало в преобразовании.
    const int32_t sampleRate = backend->api.getSampleRate(backend->stream);
    const int32_t bufferFrames = backend->api.getBufferSize(backend->stream);
    if (backend->api.getFormat(backend->stream) != AAUDIO_FORMAT_PCM_FLOAT ||
        backend->api.getChannelCount(backend->stream) != AUDIO_AAUDIO_CHANNELS ||
        sampleRate <= 0)
    {
        AAudioDestroy(&backend->base);
        return false;
    }
    backend->base.sampleRate = (uint32_t)sampleRate;
    backend->base.channelCount = AUDIO_AAUDIO_CHANNELS;
    backend->base.bufferFrameCount = bufferFrames > 0 ? (uint32_t)bufferFrames : 0u;

    if (backend->api.requestStart(backend->stream) != AAUDIO_OK)
    {
        AAudioDestroy(&backend->base);
        return false;
    }
    *outBackend = &backend->base;
    return true;
}
