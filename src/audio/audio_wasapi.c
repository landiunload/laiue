// Системный вывод Windows: WASAPI в общем режиме с событийной подачей.
// Задержка равна периоду устройства (обычно около 10 мс) против сотен
// миллисекунд у медиаконвейера, а из библиотек нужен только ole32 —
// сам звуковой клиент создаётся через COM без Media Foundation.

// COBJMACROS открывает C-обёртки методов COM вида Интерфейс_Метод; без
// них интерфейсы WASAPI доступны только из C++.
#define COBJMACROS
#include <windows.h>
#include <avrt.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include "audio/audio_backend.h"
#include "platform/system.h"

#include <string.h>

// Заголовки WASAPI объявляют эти CLSID и IID как внешние данные, но ни
// одна библиотека SDK их не определяет: значения обязано предоставить
// приложение. Они взяты из MIDL_INTERFACE в заголовках Windows SDK и из
// записи класса в реестре, а не из памяти. EXTERN_C здесь не пишется:
// он разворачивается в extern, а объект с инициализатором при extern
// clang считает ошибкой. В C объект файловой области и без него имеет
// внешнюю связь и совпадает с объявлением из заголовка.
const CLSID CLSID_MMDeviceEnumerator = {
    0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e }
};
const IID IID_IMMDeviceEnumerator = {
    0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 }
};
const IID IID_IAudioClient = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 }
};
const IID IID_IAudioRenderClient = {
    0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 }
};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT. Значение выписано здесь, чтобы не
// тянуть ksmedia.h ради одной константы.
static const GUID AUDIO_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

#define AUDIO_START_PENDING 0u
#define AUDIO_START_SUCCEEDED 1u
#define AUDIO_START_FAILED 2u

// Ожидание буфера с запасом: событие приходит каждый период устройства,
// а два секунды означают, что устройство исчезло или встало.
#define AUDIO_BUFFER_WAIT_MILLISECONDS 2000u

typedef struct AudioWasapiBackend
{
    AudioBackend base;

    AudioRenderCallback render;
    void *context;

    PlatformThread thread;
    bool threadStarted;
    PlatformMutex startLock;
    PlatformConditionVariable startSignal;
    bool startLockReady;
    bool startSignalReady;
    volatile uint32_t startState;
    volatile uint32_t running;
    volatile int64_t underruns;

    HANDLE bufferEvent;
    uint32_t deviceChannelCount;

    // Микс всегда стерео; в кадр устройства он раскладывается по первым
    // двум каналам, остальные молчат.
    float *mixFrames;
    uint32_t mixFrameCapacity;
} AudioWasapiBackend;

static void SignalStartState(AudioWasapiBackend *backend, uint32_t state)
{
    PlatformMutexLock(&backend->startLock);
    backend->startState = state;
    PlatformConditionVariableWakeAll(&backend->startSignal);
    PlatformMutexUnlock(&backend->startLock);
}

static bool FormatIsFloat32(const WAVEFORMATEX *format)
{
    if (format->wBitsPerSample != 32u) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) return false;
    if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;
    return memcmp(&extensible->SubFormat, &AUDIO_SUBTYPE_IEEE_FLOAT, sizeof(GUID)) == 0;
}

// Раскладка стереомикса в кадр устройства. Каналы сверх первых двух
// заполняются тишиной: движок не решает за приложение, как разносить
// звук по объёмной конфигурации.
static void SpreadStereoToDevice(const float *mixFrames, float *deviceFrames,
                                 uint32_t frameCount, uint32_t deviceChannels)
{
    if (deviceChannels == 2u)
    {
        memcpy(deviceFrames, mixFrames, (size_t)frameCount * 2u * sizeof(float));
        return;
    }
    memset(deviceFrames, 0, (size_t)frameCount * deviceChannels * sizeof(float));
    uint32_t copied = deviceChannels < 2u ? deviceChannels : 2u;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        for (uint32_t channel = 0; channel < copied; ++channel)
        {
            deviceFrames[frame * deviceChannels + channel] = mixFrames[frame * 2u + channel];
        }
    }
}

static uint32_t RenderThreadEntry(void *parameter)
{
    AudioWasapiBackend *backend = (AudioWasapiBackend *)parameter;

    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *endpoint = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *renderClient = NULL;
    WAVEFORMATEX *mixFormat = NULL;
    HANDLE taskHandle = NULL;
    DWORD taskIndex = 0;
    bool clientStarted = false;
    bool comInitialized = false;

    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (result == S_OK || result == S_FALSE) comInitialized = true;
    else if (result != RPC_E_CHANGED_MODE) goto failed;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&enumerator)))
        goto failed;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole,
                                                           &endpoint)))
        goto failed;
    if (FAILED(IMMDevice_Activate(endpoint, &IID_IAudioClient, CLSCTX_ALL, NULL,
                                  (void **)&client)))
        goto failed;
    if (FAILED(IAudioClient_GetMixFormat(client, &mixFormat))) goto failed;

    // В общем режиме устройство принимает только свой формат микса. На
    // Windows это практически всегда 32-битный float; иное означает
    // экзотическую конфигурацию, и честнее отказаться, чем играть шум.
    if (!FormatIsFloat32(mixFormat) || mixFormat->nChannels == 0u) goto failed;

    REFERENCE_TIME duration = 0;   // ноль — период устройства по умолчанию
    if (FAILED(IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0,
                                       mixFormat, NULL)))
        goto failed;
    if (FAILED(IAudioClient_SetEventHandle(client, backend->bufferEvent))) goto failed;

    UINT32 bufferFrameCount = 0;
    if (FAILED(IAudioClient_GetBufferSize(client, &bufferFrameCount)) || bufferFrameCount == 0u)
        goto failed;
    if (FAILED(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&renderClient)))
        goto failed;

    backend->mixFrames = PlatformAllocate((size_t)bufferFrameCount * 2u * sizeof(float), false);
    if (backend->mixFrames == NULL) goto failed;
    backend->mixFrameCapacity = bufferFrameCount;
    backend->deviceChannelCount = mixFormat->nChannels;
    backend->base.sampleRate = mixFormat->nSamplesPerSec;
    backend->base.channelCount = 2u;
    backend->base.bufferFrameCount = bufferFrameCount;

    if (FAILED(IAudioClient_Start(client))) goto failed;
    clientStarted = true;

    // MMCSS поднимает приоритет потока вывода: без него планировщик
    // способен отобрать квант посреди подготовки буфера и дать провал.
    taskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    SignalStartState(backend, AUDIO_START_SUCCEEDED);

    while (PlatformAtomicLoadU32Acquire(&backend->running) != 0u)
    {
        if (WaitForSingleObject(backend->bufferEvent, AUDIO_BUFFER_WAIT_MILLISECONDS)
            != WAIT_OBJECT_0)
        {
            PlatformAtomicIncrementI64(&backend->underruns);
            continue;
        }
        if (PlatformAtomicLoadU32Acquire(&backend->running) == 0u) break;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) break;
        if (padding > bufferFrameCount) break;
        UINT32 available = bufferFrameCount - padding;
        if (available == 0u) continue;

        BYTE *deviceBuffer = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(renderClient, available, &deviceBuffer))) break;

        backend->render(backend->context, backend->mixFrames, available);
        SpreadStereoToDevice(backend->mixFrames, (float *)deviceBuffer, available,
                             backend->deviceChannelCount);

        if (FAILED(IAudioRenderClient_ReleaseBuffer(renderClient, available, 0))) break;
    }

    goto cleanup;

failed:
    SignalStartState(backend, AUDIO_START_FAILED);

cleanup:
    if (taskHandle != NULL) AvRevertMmThreadCharacteristics(taskHandle);
    if (clientStarted) IAudioClient_Stop(client);
    if (renderClient != NULL) IAudioRenderClient_Release(renderClient);
    if (mixFormat != NULL) CoTaskMemFree(mixFormat);
    if (client != NULL) IAudioClient_Release(client);
    if (endpoint != NULL) IMMDevice_Release(endpoint);
    if (enumerator != NULL) IMMDeviceEnumerator_Release(enumerator);
    if (comInitialized) CoUninitialize();
    return 0u;
}

static void WasapiDestroy(AudioBackend *base)
{
    AudioWasapiBackend *backend = (AudioWasapiBackend *)base;

    PlatformAtomicStoreU32Release(&backend->running, 0u);
    if (backend->bufferEvent != NULL) SetEvent(backend->bufferEvent);
    if (backend->threadStarted) PlatformThreadJoin(&backend->thread);

    if (backend->bufferEvent != NULL) CloseHandle(backend->bufferEvent);
    if (backend->mixFrames != NULL) PlatformFree(backend->mixFrames);
    if (backend->startSignalReady) PlatformConditionVariableDestroy(&backend->startSignal);
    if (backend->startLockReady) PlatformMutexDestroy(&backend->startLock);
    PlatformFree(backend);
}

static uint64_t WasapiUnderrunCount(const AudioBackend *base)
{
    const AudioWasapiBackend *backend = (const AudioWasapiBackend *)base;
    return (uint64_t)PlatformAtomicLoadI64(&backend->underruns);
}

static const AudioBackendVtable WASAPI_VTABLE = {
    .destroy = WasapiDestroy,
    .underrunCount = WasapiUnderrunCount,
};

bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL) return false;
    *outBackend = NULL;

    AudioWasapiBackend *backend = PlatformAllocate(sizeof(*backend), true);
    if (backend == NULL) return false;

    backend->base.vtable = &WASAPI_VTABLE;
    backend->render = description->render;
    backend->context = description->context;
    backend->running = 1u;
    backend->startState = AUDIO_START_PENDING;

    if (!PlatformMutexInitialize(&backend->startLock)) goto failed;
    backend->startLockReady = true;
    if (!PlatformConditionVariableInitialize(&backend->startSignal)) goto failed;
    backend->startSignalReady = true;

    backend->bufferEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (backend->bufferEvent == NULL) goto failed;

    if (!PlatformThreadStart(&backend->thread, RenderThreadEntry, backend)) goto failed;
    backend->threadStarted = true;

    // Устройство настраивает сам поток вывода, поэтому создание ждёт его
    // ответа: вызывающая сторона должна получить готовые частоту и
    // размер буфера, а не заполнить их позже.
    PlatformMutexLock(&backend->startLock);
    while (backend->startState == AUDIO_START_PENDING)
    {
        PlatformConditionVariableWait(&backend->startSignal, &backend->startLock);
    }
    uint32_t startState = backend->startState;
    PlatformMutexUnlock(&backend->startLock);

    if (startState != AUDIO_START_SUCCEEDED) goto failed;

    *outBackend = &backend->base;
    return true;

failed:
    WasapiDestroy(&backend->base);
    return false;
}
