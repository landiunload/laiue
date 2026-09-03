#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Диагностический доступ к смешанным кадрам. Существует только у
// устройства с бэкендом AUDIO_BACKEND_OFFSCREEN: без него микс не
// проверить ни на машине без звуковой карты, ни в CI. В SDK заголовок не
// устанавливается — это внутренний контракт движка и его тестов.
//
// Кадры возвращаются как стерео float в диапазоне [-1, 1], чередуясь по
// каналам. Вызов сам выполняет смешивание, поэтому применяет и все
// команды, накопленные с прошлого раза.

typedef struct AudioDevice AudioDevice;

LAIUE_AUDIO_API bool AudioDeviceRenderFrames(AudioDevice *device, float *outFrames,
                                             uint32_t frameCount);
