#pragma once

#include <stdint.h>
#include <wchar.h>

#include "gameplay/player_controller.h"

typedef struct ApplicationConfiguration
{
    const wchar_t* windowTitle;
    int32_t windowWidth;
    int32_t windowHeight;
    float cameraSpeed;
    float mouseSensitivity;
    float fieldOfViewRadians;
    float nearPlane;
    float farPlane;
    float editReachDistance;
    int32_t viewRadiusChunks;
    int64_t worldSeed;
    int64_t rebaseThresholdBlocks;
    double spawnHeight;
    PlayerControllerConfig player;
} ApplicationConfiguration;

extern const ApplicationConfiguration g_applicationConfiguration;
