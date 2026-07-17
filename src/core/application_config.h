#pragma once

#include <stdint.h>
#include <wchar.h>

typedef struct ApplicationConfiguration
{
    const wchar_t* windowTitle;
    int32_t windowWidth;
    int32_t windowHeight;
    float cameraSpeed;
    float mouseSensitivity;
    // Горизонтальное поле зрения по умолчанию (ползунок в настройках 0..360).
    int32_t defaultFieldOfViewDegrees;
    // Длительность игровых суток на скорости «Обычная», минуты.
    float dayLengthMinutes;
    // Время суток при старте, часы 0..24.
    float startTimeOfDayHours;
    float nearPlane;
    float farPlane;
    float editReachDistance;
    int32_t viewRadiusChunks;
    int64_t worldSeed;
    int64_t rebaseThresholdBlocks;
    double spawnHeight;
} ApplicationConfiguration;

extern const ApplicationConfiguration g_applicationConfiguration;
