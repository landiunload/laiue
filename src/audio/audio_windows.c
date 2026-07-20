#define COBJMACROS
#include <windows.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlwapi.h>

#include "audio/audio.h"

#include <stddef.h>

#define AUDIO_EVENT_CAPACITY 32U

typedef struct AudioMediaCallback AudioMediaCallback;

struct AudioPlayer
{
    IMFMediaEngine* engine;
    AudioMediaCallback* callback;
    bool mediaFoundationStarted;
    bool uninitializeCom;

    SRWLOCK stateLock;
    AudioPlaybackState state;
    AudioStreamError streamError;
    HRESULT platformCode;
    float volume;
    bool muted;
    bool looping;

    AudioEvent events[AUDIO_EVENT_CAPACITY];
    uint32_t eventHead;
    uint32_t eventCount;
};

struct AudioMediaCallback
{
    IMFMediaEngineNotify interfaceValue;
    volatile LONG referenceCount;

    // Отдельная блокировка защищает время жизни owner. Destroy получает
    // exclusive-lock и ждёт завершения уже исполняющегося callback.
    SRWLOCK ownerLock;
    AudioPlayer* owner;
};

static void AudioPushEventLocked(AudioPlayer* player, AudioEventType type,
    AudioStreamError streamError, HRESULT platformCode)
{
    if (player->eventCount == AUDIO_EVENT_CAPACITY)
    {
        player->eventHead = (player->eventHead + 1U) % AUDIO_EVENT_CAPACITY;
        player->eventCount--;
    }

    uint32_t index = (player->eventHead + player->eventCount)
        % AUDIO_EVENT_CAPACITY;
    player->events[index].type = type;
    player->events[index].streamError = streamError;
    player->events[index].platformCode = (int32_t)platformCode;
    player->eventCount++;
}

static void AudioSetState(AudioPlayer* player, AudioPlaybackState state,
    AudioEventType eventType)
{
    AcquireSRWLockExclusive(&player->stateLock);
    if (player->state != state)
    {
        player->state = state;
        AudioPushEventLocked(player, eventType,
            AUDIO_STREAM_ERROR_NONE, S_OK);
    }
    ReleaseSRWLockExclusive(&player->stateLock);
}

static AudioStreamError AudioMapStreamError(DWORD_PTR mediaError)
{
    switch ((MF_MEDIA_ENGINE_ERR)mediaError)
    {
        case MF_MEDIA_ENGINE_ERR_ABORTED:
            return AUDIO_STREAM_ERROR_ABORTED;
        case MF_MEDIA_ENGINE_ERR_NETWORK:
            return AUDIO_STREAM_ERROR_NETWORK;
        case MF_MEDIA_ENGINE_ERR_DECODE:
            return AUDIO_STREAM_ERROR_DECODE;
        case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
            return AUDIO_STREAM_ERROR_SOURCE_NOT_SUPPORTED;
        case MF_MEDIA_ENGINE_ERR_ENCRYPTED:
            return AUDIO_STREAM_ERROR_ENCRYPTED;
        default:
            return AUDIO_STREAM_ERROR_UNKNOWN;
    }
}

static void AudioSetError(AudioPlayer* player, AudioStreamError streamError,
    HRESULT platformCode)
{
    AcquireSRWLockExclusive(&player->stateLock);
    player->state = AUDIO_PLAYBACK_ERROR;
    player->streamError = streamError;
    player->platformCode = platformCode;
    AudioPushEventLocked(player, AUDIO_EVENT_ERROR,
        streamError, platformCode);
    ReleaseSRWLockExclusive(&player->stateLock);
}

static AudioPlaybackState AudioGetState(const AudioPlayer* player)
{
    AcquireSRWLockShared((PSRWLOCK)&player->stateLock);
    AudioPlaybackState state = player->state;
    ReleaseSRWLockShared((PSRWLOCK)&player->stateLock);
    return state;
}

static void AudioHandleMediaEvent(AudioPlayer* player, DWORD event,
    DWORD_PTR param1, DWORD param2)
{
    // Clear переводит объект в EMPTY до остановки backend. Поэтому все
    // запоздалые callbacks прежнего источника безопасно отбрасываются.
    if (AudioGetState(player) == AUDIO_PLAYBACK_EMPTY)
    {
        return;
    }

    switch ((MF_MEDIA_ENGINE_EVENT)event)
    {
        case MF_MEDIA_ENGINE_EVENT_LOADSTART:
            AudioSetState(player, AUDIO_PLAYBACK_LOADING,
                AUDIO_EVENT_LOADING);
            break;

        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:
            if (player->engine != NULL
                && !IMFMediaEngine_IsPaused(player->engine))
            {
                AudioSetState(player, AUDIO_PLAYBACK_PLAYING,
                    AUDIO_EVENT_PLAYING);
            }
            else
            {
                AudioSetState(player, AUDIO_PLAYBACK_READY,
                    AUDIO_EVENT_READY);
            }
            break;

        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            AudioSetState(player, AUDIO_PLAYBACK_PLAYING,
                AUDIO_EVENT_PLAYING);
            break;

        case MF_MEDIA_ENGINE_EVENT_PAUSE:
            if (AudioGetState(player) != AUDIO_PLAYBACK_LOADING)
            {
                AudioSetState(player, AUDIO_PLAYBACK_PAUSED,
                    AUDIO_EVENT_PAUSED);
            }
            break;

        case MF_MEDIA_ENGINE_EVENT_WAITING:
        case MF_MEDIA_ENGINE_EVENT_STALLED:
        case MF_MEDIA_ENGINE_EVENT_BUFFERINGSTARTED:
            AudioSetState(player, AUDIO_PLAYBACK_BUFFERING,
                AUDIO_EVENT_BUFFERING_STARTED);
            break;

        case MF_MEDIA_ENGINE_EVENT_BUFFERINGENDED:
        {
            bool paused = player->engine == NULL
                || IMFMediaEngine_IsPaused(player->engine);
            AudioSetState(player,
                paused ? AUDIO_PLAYBACK_PAUSED : AUDIO_PLAYBACK_PLAYING,
                AUDIO_EVENT_BUFFERING_ENDED);
            break;
        }

        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            AcquireSRWLockExclusive(&player->stateLock);
            AudioPushEventLocked(player, AUDIO_EVENT_SEEK_COMPLETED,
                AUDIO_STREAM_ERROR_NONE, S_OK);
            ReleaseSRWLockExclusive(&player->stateLock);
            break;

        case MF_MEDIA_ENGINE_EVENT_ENDED:
            AudioSetState(player, AUDIO_PLAYBACK_ENDED,
                AUDIO_EVENT_ENDED);
            break;

        case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            AcquireSRWLockExclusive(&player->stateLock);
            AudioPushEventLocked(player, AUDIO_EVENT_DURATION_CHANGED,
                AUDIO_STREAM_ERROR_NONE, S_OK);
            ReleaseSRWLockExclusive(&player->stateLock);
            break;

        case MF_MEDIA_ENGINE_EVENT_VOLUMECHANGE:
            AcquireSRWLockExclusive(&player->stateLock);
            if (player->engine != NULL)
            {
                player->volume = (float)IMFMediaEngine_GetVolume(player->engine);
                player->muted = IMFMediaEngine_GetMuted(player->engine) != FALSE;
            }
            AudioPushEventLocked(player, AUDIO_EVENT_VOLUME_CHANGED,
                AUDIO_STREAM_ERROR_NONE, S_OK);
            ReleaseSRWLockExclusive(&player->stateLock);
            break;

        case MF_MEDIA_ENGINE_EVENT_ERROR:
            AudioSetError(player, AudioMapStreamError(param1),
                (HRESULT)param2);
            break;

        case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR:
            AudioSetError(player, AUDIO_STREAM_ERROR_DECODE,
                (HRESULT)param1);
            break;

        // ABORT и EMPTIED входят в нормальную смену источника. LOADSTART
        // следующего источника задаёт итоговое состояние и не даёт старому
        // событию превратить новую асинхронную загрузку в ошибку.
        case MF_MEDIA_ENGINE_EVENT_ABORT:
        case MF_MEDIA_ENGINE_EVENT_EMPTIED:
        default:
            break;
    }
}

static HRESULT STDMETHODCALLTYPE AudioCallbackQueryInterface(
    IMFMediaEngineNotify* interfaceValue, REFIID interfaceId, void** object)
{
    if (object == NULL)
    {
        return E_POINTER;
    }
    *object = NULL;
    if (!IsEqualIID(interfaceId, &IID_IUnknown)
        && !IsEqualIID(interfaceId, &IID_IMFMediaEngineNotify))
    {
        return E_NOINTERFACE;
    }

    *object = interfaceValue;
    IMFMediaEngineNotify_AddRef(interfaceValue);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE AudioCallbackAddRef(
    IMFMediaEngineNotify* interfaceValue)
{
    AudioMediaCallback* callback = CONTAINING_RECORD(interfaceValue,
        AudioMediaCallback, interfaceValue);
    return (ULONG)InterlockedIncrement(&callback->referenceCount);
}

static ULONG STDMETHODCALLTYPE AudioCallbackRelease(
    IMFMediaEngineNotify* interfaceValue)
{
    AudioMediaCallback* callback = CONTAINING_RECORD(interfaceValue,
        AudioMediaCallback, interfaceValue);
    LONG references = InterlockedDecrement(&callback->referenceCount);
    if (references == 0)
    {
        HeapFree(GetProcessHeap(), 0, callback);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE AudioCallbackEventNotify(
    IMFMediaEngineNotify* interfaceValue, DWORD event,
    DWORD_PTR param1, DWORD param2)
{
    AudioMediaCallback* callback = CONTAINING_RECORD(interfaceValue,
        AudioMediaCallback, interfaceValue);

    AcquireSRWLockShared(&callback->ownerLock);
    AudioPlayer* owner = callback->owner;
    if (owner != NULL)
    {
        AudioHandleMediaEvent(owner, event, param1, param2);
    }
    ReleaseSRWLockShared(&callback->ownerLock);
    return S_OK;
}

static IMFMediaEngineNotifyVtbl AUDIO_CALLBACK_VTABLE = {
    AudioCallbackQueryInterface,
    AudioCallbackAddRef,
    AudioCallbackRelease,
    AudioCallbackEventNotify,
};

static AudioMediaCallback* AudioCallbackCreate(AudioPlayer* owner)
{
    AudioMediaCallback* callback = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*callback));
    if (callback == NULL)
    {
        return NULL;
    }

    callback->interfaceValue.lpVtbl = &AUDIO_CALLBACK_VTABLE;
    callback->referenceCount = 1;
    InitializeSRWLock(&callback->ownerLock);
    callback->owner = owner;
    return callback;
}

static void AudioCallbackDetach(AudioMediaCallback* callback)
{
    if (callback == NULL)
    {
        return;
    }
    AcquireSRWLockExclusive(&callback->ownerLock);
    callback->owner = NULL;
    ReleaseSRWLockExclusive(&callback->ownerLock);
}

static float AudioClampVolume(float volume)
{
    if (volume != volume)
    {
        return 1.0f;
    }
    if (volume < 0.0f) return 0.0f;
    if (volume > 1.0f) return 1.0f;
    return volume;
}

static void AudioReleasePlayerResources(AudioPlayer* player)
{
    if (player == NULL)
    {
        return;
    }

    AudioCallbackDetach(player->callback);
    if (player->engine != NULL)
    {
        IMFMediaEngine_Shutdown(player->engine);
        IMFMediaEngine_Release(player->engine);
        player->engine = NULL;
    }
    if (player->callback != NULL)
    {
        IMFMediaEngineNotify_Release(&player->callback->interfaceValue);
        player->callback = NULL;
    }
    if (player->mediaFoundationStarted)
    {
        MFShutdown();
        player->mediaFoundationStarted = false;
    }
    if (player->uninitializeCom)
    {
        CoUninitialize();
        player->uninitializeCom = false;
    }
}

AudioResult AudioPlayerCreate(
    const AudioPlayerConfiguration* configuration, AudioPlayer** outPlayer)
{
    if (outPlayer == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    *outPlayer = NULL;

    AudioPlayer* player = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*player));
    if (player == NULL)
    {
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }
    InitializeSRWLock(&player->stateLock);
    player->state = AUDIO_PLAYBACK_EMPTY;
    player->volume = configuration != NULL
        ? AudioClampVolume(configuration->initialVolume) : 1.0f;
    player->muted = configuration != NULL && configuration->initiallyMuted;
    player->looping = configuration != NULL && configuration->looping;

    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    player->uninitializeCom = result == S_OK || result == S_FALSE;
    if (FAILED(result) && result != RPC_E_CHANGED_MODE)
    {
        HeapFree(GetProcessHeap(), 0, player);
        return AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED;
    }

    result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result))
    {
        AudioReleasePlayerResources(player);
        HeapFree(GetProcessHeap(), 0, player);
        return AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED;
    }
    player->mediaFoundationStarted = true;

    player->callback = AudioCallbackCreate(player);
    if (player->callback == NULL)
    {
        AudioReleasePlayerResources(player);
        HeapFree(GetProcessHeap(), 0, player);
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }

    IMFAttributes* attributes = NULL;
    IMFMediaEngineClassFactory* factory = NULL;
    result = MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(result))
    {
        result = IMFAttributes_SetUnknown(attributes,
            &MF_MEDIA_ENGINE_CALLBACK,
            (IUnknown*)&player->callback->interfaceValue);
    }
    if (SUCCEEDED(result))
    {
        // Системный аудиомикшер применяет корректную политику ducking и
        // маршрутизации для музыки, а не для голоса или системных сигналов.
        result = IMFAttributes_SetUINT32(attributes,
            &MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
    }
    if (SUCCEEDED(result))
    {
        result = CoCreateInstance(&CLSID_MFMediaEngineClassFactory,
            NULL, CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory,
            (void**)&factory);
    }
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngineClassFactory_CreateInstance(factory,
            MF_MEDIA_ENGINE_AUDIOONLY, attributes, &player->engine);
    }

    if (factory != NULL) IMFMediaEngineClassFactory_Release(factory);
    if (attributes != NULL) IMFAttributes_Release(attributes);

    if (FAILED(result) || player->engine == NULL)
    {
        AudioReleasePlayerResources(player);
        HeapFree(GetProcessHeap(), 0, player);
        return AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED;
    }

    result = IMFMediaEngine_SetAutoPlay(player->engine, FALSE);
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_SetPreload(player->engine,
            MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    }
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_SetVolume(player->engine, player->volume);
    }
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_SetMuted(player->engine,
            player->muted ? TRUE : FALSE);
    }
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_SetLoop(player->engine,
            player->looping ? TRUE : FALSE);
    }
    if (FAILED(result))
    {
        AudioReleasePlayerResources(player);
        HeapFree(GetProcessHeap(), 0, player);
        return AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED;
    }

    *outPlayer = player;
    return AUDIO_RESULT_OK;
}

void AudioPlayerDestroy(AudioPlayer* player)
{
    if (player == NULL)
    {
        return;
    }
    AudioReleasePlayerResources(player);
    HeapFree(GetProcessHeap(), 0, player);
}

static bool AudioStringIsEmpty(const wchar_t* text)
{
    return text == NULL || text[0] == L'\0';
}

static void AudioPrepareForLoad(AudioPlayer* player)
{
    AcquireSRWLockExclusive(&player->stateLock);
    player->eventHead = 0;
    player->eventCount = 0;
    player->state = AUDIO_PLAYBACK_LOADING;
    player->streamError = AUDIO_STREAM_ERROR_NONE;
    player->platformCode = S_OK;
    AudioPushEventLocked(player, AUDIO_EVENT_LOADING,
        AUDIO_STREAM_ERROR_NONE, S_OK);
    ReleaseSRWLockExclusive(&player->stateLock);
}

static AudioResult AudioPlayerLoadBstr(AudioPlayer* player, BSTR uri)
{
    if (player == NULL || player->engine == NULL || uri == NULL
        || uri[0] == L'\0')
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }

    AudioPrepareForLoad(player);
    IMFMediaEngine_Pause(player->engine);
    HRESULT result = IMFMediaEngine_SetSource(player->engine, uri);
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_Load(player->engine);
    }
    if (FAILED(result))
    {
        AudioSetError(player, AUDIO_STREAM_ERROR_UNKNOWN, result);
        return AUDIO_RESULT_SOURCE_REJECTED;
    }
    return AUDIO_RESULT_OK;
}

AudioResult AudioPlayerLoadUri(AudioPlayer* player, const wchar_t* uri)
{
    if (player == NULL || AudioStringIsEmpty(uri))
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }

    BSTR uriCopy = SysAllocString(uri);
    if (uriCopy == NULL)
    {
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }
    AudioResult result = AudioPlayerLoadBstr(player, uriCopy);
    SysFreeString(uriCopy);
    return result;
}

static AudioResult AudioFileUriFromPath(const wchar_t* path, BSTR* outUri)
{
    *outUri = NULL;
    DWORD fullPathCapacity = GetFullPathNameW(path, 0, NULL, NULL);
    if (fullPathCapacity == 0 || fullPathCapacity > UINT32_MAX / 3U - 16U)
    {
        return AUDIO_RESULT_SOURCE_REJECTED;
    }

    wchar_t* fullPath = HeapAlloc(GetProcessHeap(), 0,
        (size_t)fullPathCapacity * sizeof(wchar_t));
    if (fullPath == NULL)
    {
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }

    DWORD written = GetFullPathNameW(path, fullPathCapacity,
        fullPath, NULL);
    if (written == 0 || written >= fullPathCapacity)
    {
        HeapFree(GetProcessHeap(), 0, fullPath);
        return AUDIO_RESULT_SOURCE_REJECTED;
    }

    DWORD uriCapacity = fullPathCapacity * 3U + 16U;
    wchar_t* uri = HeapAlloc(GetProcessHeap(), 0,
        (size_t)uriCapacity * sizeof(wchar_t));
    if (uri == NULL)
    {
        HeapFree(GetProcessHeap(), 0, fullPath);
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }

    HRESULT result = UrlCreateFromPathW(fullPath, uri, &uriCapacity, 0);
    if (FAILED(result))
    {
        HeapFree(GetProcessHeap(), 0, uri);
        HeapFree(GetProcessHeap(), 0, fullPath);
        return AUDIO_RESULT_SOURCE_REJECTED;
    }

    *outUri = SysAllocString(uri);
    HeapFree(GetProcessHeap(), 0, uri);
    HeapFree(GetProcessHeap(), 0, fullPath);
    return *outUri != NULL ? AUDIO_RESULT_OK : AUDIO_RESULT_OUT_OF_MEMORY;
}

AudioResult AudioPlayerLoadFile(AudioPlayer* player, const wchar_t* path)
{
    if (player == NULL || AudioStringIsEmpty(path))
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }

    BSTR uri = NULL;
    AudioResult result = AudioFileUriFromPath(path, &uri);
    if (result == AUDIO_RESULT_OK)
    {
        result = AudioPlayerLoadBstr(player, uri);
    }
    if (uri != NULL) SysFreeString(uri);
    return result;
}

AudioResult AudioPlayerClear(AudioPlayer* player)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }

    AcquireSRWLockExclusive(&player->stateLock);
    player->eventHead = 0;
    player->eventCount = 0;
    player->state = AUDIO_PLAYBACK_EMPTY;
    player->streamError = AUDIO_STREAM_ERROR_NONE;
    player->platformCode = S_OK;
    AudioPushEventLocked(player, AUDIO_EVENT_CLEARED,
        AUDIO_STREAM_ERROR_NONE, S_OK);
    ReleaseSRWLockExclusive(&player->stateLock);

    IMFMediaEngine_Pause(player->engine);
    IMFMediaEngine_SetCurrentTime(player->engine, 0.0);
    BSTR emptySource = SysAllocString(L"");
    if (emptySource != NULL)
    {
        if (SUCCEEDED(IMFMediaEngine_SetSource(player->engine, emptySource)))
        {
            IMFMediaEngine_Load(player->engine);
        }
        SysFreeString(emptySource);
    }
    return AUDIO_RESULT_OK;
}

static bool AudioCanControlPlayback(const AudioPlayer* player)
{
    AudioPlaybackState state = AudioGetState(player);
    return state != AUDIO_PLAYBACK_EMPTY
        && state != AUDIO_PLAYBACK_LOADING
        && state != AUDIO_PLAYBACK_ERROR;
}

AudioResult AudioPlayerPlay(AudioPlayer* player)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    if (!AudioCanControlPlayback(player))
    {
        return AUDIO_RESULT_INVALID_STATE;
    }

    HRESULT result = IMFMediaEngine_Play(player->engine);
    return SUCCEEDED(result)
        ? AUDIO_RESULT_OK : AUDIO_RESULT_OPERATION_FAILED;
}

AudioResult AudioPlayerPause(AudioPlayer* player)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    if (!AudioCanControlPlayback(player))
    {
        return AUDIO_RESULT_INVALID_STATE;
    }

    HRESULT result = IMFMediaEngine_Pause(player->engine);
    return SUCCEEDED(result)
        ? AUDIO_RESULT_OK : AUDIO_RESULT_OPERATION_FAILED;
}

AudioResult AudioPlayerStop(AudioPlayer* player)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    if (!AudioCanControlPlayback(player))
    {
        return AUDIO_RESULT_INVALID_STATE;
    }

    HRESULT result = IMFMediaEngine_Pause(player->engine);
    if (SUCCEEDED(result))
    {
        result = IMFMediaEngine_SetCurrentTime(player->engine, 0.0);
    }
    return SUCCEEDED(result)
        ? AUDIO_RESULT_OK : AUDIO_RESULT_OPERATION_FAILED;
}

AudioResult AudioPlayerSeek(AudioPlayer* player, double positionSeconds)
{
    if (player == NULL || player->engine == NULL
        || positionSeconds != positionSeconds || positionSeconds < 0.0)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    if (!AudioCanControlPlayback(player))
    {
        return AUDIO_RESULT_INVALID_STATE;
    }

    double duration = IMFMediaEngine_GetDuration(player->engine);
    if (duration == duration && duration > 0.0
        && positionSeconds > duration)
    {
        positionSeconds = duration;
    }
    HRESULT result = IMFMediaEngine_SetCurrentTime(player->engine,
        positionSeconds);
    return SUCCEEDED(result)
        ? AUDIO_RESULT_OK : AUDIO_RESULT_OPERATION_FAILED;
}

AudioResult AudioPlayerSetVolume(AudioPlayer* player, float volume)
{
    if (player == NULL || player->engine == NULL || volume != volume)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    volume = AudioClampVolume(volume);
    HRESULT result = IMFMediaEngine_SetVolume(player->engine, volume);
    if (FAILED(result))
    {
        return AUDIO_RESULT_OPERATION_FAILED;
    }

    AcquireSRWLockExclusive(&player->stateLock);
    player->volume = volume;
    ReleaseSRWLockExclusive(&player->stateLock);
    return AUDIO_RESULT_OK;
}

AudioResult AudioPlayerSetMuted(AudioPlayer* player, bool muted)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    HRESULT result = IMFMediaEngine_SetMuted(player->engine,
        muted ? TRUE : FALSE);
    if (FAILED(result))
    {
        return AUDIO_RESULT_OPERATION_FAILED;
    }

    AcquireSRWLockExclusive(&player->stateLock);
    player->muted = muted;
    ReleaseSRWLockExclusive(&player->stateLock);
    return AUDIO_RESULT_OK;
}

AudioResult AudioPlayerSetLooping(AudioPlayer* player, bool looping)
{
    if (player == NULL || player->engine == NULL)
    {
        return AUDIO_RESULT_INVALID_ARGUMENT;
    }
    HRESULT result = IMFMediaEngine_SetLoop(player->engine,
        looping ? TRUE : FALSE);
    if (FAILED(result))
    {
        return AUDIO_RESULT_OPERATION_FAILED;
    }

    AcquireSRWLockExclusive(&player->stateLock);
    player->looping = looping;
    ReleaseSRWLockExclusive(&player->stateLock);
    return AUDIO_RESULT_OK;
}

static double AudioFiniteNonnegative(double value)
{
    // Media Engine использует NaN/Infinity для неизвестной длительности.
    return value == value && value >= 0.0 && value < 1.0e12
        ? value : 0.0;
}

bool AudioPlayerGetSnapshot(const AudioPlayer* player,
    AudioPlayerSnapshot* outSnapshot)
{
    if (player == NULL || player->engine == NULL || outSnapshot == NULL)
    {
        return false;
    }

    AcquireSRWLockShared((PSRWLOCK)&player->stateLock);
    outSnapshot->state = player->state;
    outSnapshot->streamError = player->streamError;
    outSnapshot->platformCode = (int32_t)player->platformCode;
    outSnapshot->volume = player->volume;
    outSnapshot->muted = player->muted;
    outSnapshot->looping = player->looping;
    ReleaseSRWLockShared((PSRWLOCK)&player->stateLock);

    if (outSnapshot->state == AUDIO_PLAYBACK_EMPTY)
    {
        outSnapshot->positionSeconds = 0.0;
        outSnapshot->durationSeconds = 0.0;
        outSnapshot->seeking = false;
        outSnapshot->seekable = false;
        outSnapshot->hasAudio = false;
        return true;
    }

    outSnapshot->positionSeconds = AudioFiniteNonnegative(
        IMFMediaEngine_GetCurrentTime(player->engine));
    outSnapshot->durationSeconds = AudioFiniteNonnegative(
        IMFMediaEngine_GetDuration(player->engine));
    outSnapshot->seeking = IMFMediaEngine_IsSeeking(player->engine) != FALSE;
    outSnapshot->hasAudio = IMFMediaEngine_HasAudio(player->engine) != FALSE;
    outSnapshot->seekable = false;

    IMFMediaTimeRange* seekable = NULL;
    if (SUCCEEDED(IMFMediaEngine_GetSeekable(player->engine, &seekable))
        && seekable != NULL)
    {
        outSnapshot->seekable = IMFMediaTimeRange_GetLength(seekable) != 0;
        IMFMediaTimeRange_Release(seekable);
    }
    return true;
}

bool AudioPlayerPollEvent(AudioPlayer* player, AudioEvent* outEvent)
{
    if (player == NULL || outEvent == NULL)
    {
        return false;
    }

    AcquireSRWLockExclusive(&player->stateLock);
    if (player->eventCount == 0)
    {
        ReleaseSRWLockExclusive(&player->stateLock);
        return false;
    }

    *outEvent = player->events[player->eventHead];
    player->eventHead = (player->eventHead + 1U) % AUDIO_EVENT_CAPACITY;
    player->eventCount--;
    ReleaseSRWLockExclusive(&player->stateLock);
    return true;
}
