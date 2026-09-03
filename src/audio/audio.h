#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Микшер голосов с низкой задержкой. Устройство владеет одним потоком
// вывода и смешивает в него активные голоса; приложение управляет
// голосами из своего потока и никогда не блокирует поток вывода.
//
// Границы модуля: движок отвечает за вывод, смешивание, громкость,
// панораму и скорость воспроизведения. Расстояние, затухание, реверб и
// прочая пространственная политика принадлежат приложению — оно
// вычисляет громкость и панораму сам, как вычисляет свет для рендера.
//
// Чужие контейнеры движок не декодирует: клип создаётся из готовых
// PCM-сэмплов. Свой формат `.la` и звукопаки живут в audio_pack.h — так
// у движка нет ни зависимости от кодеков, ни мнения о форматах файлов
// приложения.

typedef struct AudioDevice AudioDevice;
typedef struct AudioClip AudioClip;

// Дескриптор голоса. Ноль означает «голос не выдан»; ненулевые значения
// не повторяются на протяжении жизни устройства достаточно долго, чтобы
// устаревший дескриптор нельзя было применить к чужому голосу.
typedef uint32_t AudioVoice;
#define AUDIO_VOICE_NONE 0u

// Одновременно звучащих голосов не больше этого числа; попытка сверх
// лимита возвращает AUDIO_VOICE_NONE, а не вытесняет чужой звук.
#define AUDIO_MAX_VOICES 128u

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

typedef enum AudioBackendKind
{
    // Системное устройство вывода: WASAPI на Windows, ALSA на Linux.
    AUDIO_BACKEND_SYSTEM = 0,
    // Вывода нет; кадры отдаёт AudioDeviceRenderFrames. Существует для
    // тестов и для профилей без звуковой подсистемы.
    AUDIO_BACKEND_OFFSCREEN = 1,
} AudioBackendKind;

typedef struct AudioDeviceConfiguration
{
    AudioBackendKind backend;
    // Ноль означает «выбрать частоту устройства»; для offscreen — 48000.
    uint32_t sampleRate;
    // Ноль означает «выбрать по умолчанию». Смешивание всегда стерео.
    uint32_t frameCountHint;
    float masterVolume;   // 0..1, значения вне диапазона ограничиваются
} AudioDeviceConfiguration;

typedef struct AudioDeviceStats
{
    uint32_t sampleRate;
    uint32_t channelCount;
    uint32_t bufferFrameCount;
    uint32_t activeVoices;
    // Команды, отброшенные из-за переполнения очереди: приложение
    // отправляет их быстрее, чем поток вывода успевает разбирать.
    uint64_t droppedCommands;
    // Буферы, которые поток вывода не успел подготовить вовремя.
    uint64_t underruns;
    uint64_t mixedFrames;
} AudioDeviceStats;

// Сэмплы 16-битные знаковые, чередующиеся по каналам. Клип копирует их
// при создании и не удерживает указатель.
typedef struct AudioClipDescription
{
    const int16_t *samples;
    uint32_t frameCount;
    uint32_t channelCount;   // 1 или 2
    uint32_t sampleRate;     // частота исходных сэмплов
} AudioClipDescription;

typedef struct AudioVoiceParameters
{
    float volume;    // 0..1
    float pan;       // -1 слева, 0 по центру, +1 справа
    float speed;     // 1 — исходная скорость; влияет и на высоту тона
    bool looping;
} AudioVoiceParameters;

// NULL-конфигурация означает системный бэкенд, частоту устройства и
// громкость 1. Создавать и уничтожать устройство следует на одном потоке.
LAIUE_AUDIO_API AudioResult AudioDeviceCreate(const AudioDeviceConfiguration *configuration,
                                              AudioDevice **outDevice);
LAIUE_AUDIO_API void AudioDeviceDestroy(AudioDevice *device);
LAIUE_AUDIO_API void AudioDeviceSetMasterVolume(AudioDevice *device, float volume);
LAIUE_AUDIO_API float AudioDeviceGetMasterVolume(const AudioDevice *device);
LAIUE_AUDIO_API bool AudioDeviceGetStats(const AudioDevice *device, AudioDeviceStats *outStats);

// Клип живёт независимо от голосов: остановка голоса его не разрушает.
// Разрушить клип можно и во время звучания — движок сам остановит его
// голоса и освободит память тогда, когда поток вывода на неё больше не
// смотрит. Порядок «сначала остановить, потом разрушить» остаётся
// предпочтительным: он не обрывает звук на середине.
LAIUE_AUDIO_API AudioResult AudioClipCreate(AudioDevice *device,
                                            const AudioClipDescription *description,
                                            AudioClip **outClip);
LAIUE_AUDIO_API void AudioClipDestroy(AudioClip *clip);
LAIUE_AUDIO_API double AudioClipDurationSeconds(const AudioClip *clip);

// NULL-параметры означают громкость 1, центр, исходную скорость, без
// повтора. Возвращает AUDIO_VOICE_NONE, если свободных голосов нет.
LAIUE_AUDIO_API AudioVoice AudioVoicePlay(AudioDevice *device, const AudioClip *clip,
                                          const AudioVoiceParameters *parameters);
// Изменение параметров звучащего голоса. Устаревший дескриптор
// игнорируется и возвращает false.
LAIUE_AUDIO_API bool AudioVoiceSetParameters(AudioDevice *device, AudioVoice voice,
                                             const AudioVoiceParameters *parameters);
LAIUE_AUDIO_API void AudioVoiceStop(AudioDevice *device, AudioVoice voice);
LAIUE_AUDIO_API void AudioDeviceStopAllVoices(AudioDevice *device);
LAIUE_AUDIO_API bool AudioVoiceIsActive(const AudioDevice *device, AudioVoice voice);
