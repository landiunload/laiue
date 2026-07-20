#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Независимый потоковый проигрыватель. Экземпляров может быть несколько;
// каждый владеет своим источником, громкостью и очередью событий.
typedef struct AudioPlayer AudioPlayer;

typedef enum AudioResult
{
    AUDIO_RESULT_OK = 0,
    AUDIO_RESULT_INVALID_ARGUMENT,
    AUDIO_RESULT_INVALID_STATE,
    AUDIO_RESULT_OUT_OF_MEMORY,
    AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED,
    AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED,
    AUDIO_RESULT_SOURCE_REJECTED,
    AUDIO_RESULT_OPERATION_FAILED,
} AudioResult;

typedef enum AudioPlaybackState
{
    AUDIO_PLAYBACK_EMPTY = 0,
    AUDIO_PLAYBACK_LOADING,
    AUDIO_PLAYBACK_READY,
    AUDIO_PLAYBACK_PLAYING,
    AUDIO_PLAYBACK_PAUSED,
    AUDIO_PLAYBACK_BUFFERING,
    AUDIO_PLAYBACK_ENDED,
    AUDIO_PLAYBACK_ERROR,
} AudioPlaybackState;

typedef enum AudioStreamError
{
    AUDIO_STREAM_ERROR_NONE = 0,
    AUDIO_STREAM_ERROR_ABORTED,
    AUDIO_STREAM_ERROR_NETWORK,
    AUDIO_STREAM_ERROR_DECODE,
    AUDIO_STREAM_ERROR_SOURCE_NOT_SUPPORTED,
    AUDIO_STREAM_ERROR_ENCRYPTED,
    AUDIO_STREAM_ERROR_UNKNOWN,
} AudioStreamError;

typedef enum AudioEventType
{
    AUDIO_EVENT_LOADING = 0,
    AUDIO_EVENT_READY,
    AUDIO_EVENT_PLAYING,
    AUDIO_EVENT_PAUSED,
    AUDIO_EVENT_BUFFERING_STARTED,
    AUDIO_EVENT_BUFFERING_ENDED,
    AUDIO_EVENT_SEEK_COMPLETED,
    AUDIO_EVENT_ENDED,
    AUDIO_EVENT_DURATION_CHANGED,
    AUDIO_EVENT_VOLUME_CHANGED,
    AUDIO_EVENT_CLEARED,
    AUDIO_EVENT_ERROR,
} AudioEventType;

typedef struct AudioEvent
{
    AudioEventType type;
    AudioStreamError streamError;
    // HRESULT Windows для диагностики; 0, если событие его не содержит.
    int32_t platformCode;
} AudioEvent;

typedef struct AudioPlayerConfiguration
{
    // Диапазон 0..1; значение ограничивается этим диапазоном.
    float initialVolume;
    bool initiallyMuted;
    bool looping;
} AudioPlayerConfiguration;

typedef struct AudioPlayerSnapshot
{
    AudioPlaybackState state;
    AudioStreamError streamError;
    int32_t platformCode;

    // Неизвестные длительность и позиция представлены нулём.
    double positionSeconds;
    double durationSeconds;
    float volume;

    bool muted;
    bool looping;
    bool seeking;
    bool seekable;
    bool hasAudio;
} AudioPlayerSnapshot;

// NULL-конфигурация означает volume=1, muted=false, looping=false.
// COM и Media Foundation инициализируются внутри; создавать и уничтожать
// проигрыватель следует на одном клиентском потоке.
LAIUE_AUDIO_API AudioResult AudioPlayerCreate(
    const AudioPlayerConfiguration* configuration, AudioPlayer** outPlayer);
LAIUE_AUDIO_API void AudioPlayerDestroy(AudioPlayer* player);

// URI принимает http://, https:// и file://. Загрузка асинхронна: успешный
// возврат означает, что запрос принят; итог приходит через AUDIO_EVENT_READY
// либо AUDIO_EVENT_ERROR.
LAIUE_AUDIO_API AudioResult AudioPlayerLoadUri(
    AudioPlayer* player, const wchar_t* uri);
// Преобразует абсолютный или относительный путь Windows в file:// URI.
LAIUE_AUDIO_API AudioResult AudioPlayerLoadFile(
    AudioPlayer* player, const wchar_t* path);
LAIUE_AUDIO_API AudioResult AudioPlayerClear(AudioPlayer* player);

LAIUE_AUDIO_API AudioResult AudioPlayerPlay(AudioPlayer* player);
LAIUE_AUDIO_API AudioResult AudioPlayerPause(AudioPlayer* player);
LAIUE_AUDIO_API AudioResult AudioPlayerStop(AudioPlayer* player);
LAIUE_AUDIO_API AudioResult AudioPlayerSeek(
    AudioPlayer* player, double positionSeconds);

LAIUE_AUDIO_API AudioResult AudioPlayerSetVolume(
    AudioPlayer* player, float volume);
LAIUE_AUDIO_API AudioResult AudioPlayerSetMuted(
    AudioPlayer* player, bool muted);
LAIUE_AUDIO_API AudioResult AudioPlayerSetLooping(
    AudioPlayer* player, bool looping);

// Snapshot безопасно читать из главного потока, пока callbacks Media
// Foundation обновляют состояние на своём рабочем потоке.
LAIUE_AUDIO_API bool AudioPlayerGetSnapshot(
    const AudioPlayer* player, AudioPlayerSnapshot* outSnapshot);

// Неблокирующая очередь. События доставляются в порядке поступления;
// при переполнении сохраняются 32 самых новых события.
LAIUE_AUDIO_API bool AudioPlayerPollEvent(
    AudioPlayer* player, AudioEvent* outEvent);

