#pragma once

#include "audio/audio_output_service.h"

#include <stdbool.h>
#include <stdint.h>

// Внутренняя граница offscreen-вывода. Микшер знает только этот контракт и
// потому собирается и проверяется без звуковой подсистемы. Системный
// бэкенд (WASAPI, ALSA) использует тот же layout внутри отдельного
// `audio_output` provider-а и наружу отдаётся только versioned service table.
//
// Заголовок в SDK не устанавливается: это контракт между файлами модуля,
// а не часть публичного API.

// Вызывается потоком вывода. Обязан заполнить frameCount кадров стерео
// float в диапазоне [-1, 1] и не выполнять блокирующих операций.
typedef LaiueAudioOutputRenderFn AudioRenderCallback;

typedef struct AudioBackend AudioBackend;

typedef struct AudioBackendVtable
{
    void (*destroy)(AudioBackend *backend);
    uint64_t (*underrunCount)(const AudioBackend *backend);
} AudioBackendVtable;

// Общая голова каждой реализации: позволяет разрушать и опрашивать
// бэкенд, не зная, какой именно это бэкенд.
struct AudioBackend
{
    const AudioBackendVtable *vtable;
    uint32_t sampleRate;
    uint32_t channelCount;
    uint32_t bufferFrameCount;
};

typedef struct AudioBackendDescription
{
    uint32_t sampleRate;       // 0 — частота устройства по умолчанию
    uint32_t frameCountHint;   // 0 — размер буфера по умолчанию
    AudioRenderCallback render;
    void *context;
} AudioBackendDescription;

// Системный вывод платформы. Реализуется ровно одним файлом на платформу.
bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend);

// Вывода нет: кадры отдаёт AudioDeviceRenderFrames по запросу теста.
bool AudioOffscreenBackendCreate(const AudioBackendDescription *description,
                                 AudioBackend **outBackend);
