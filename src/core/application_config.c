#include "core/application_config.h"

const ApplicationConfiguration g_applicationConfiguration = {
    .windowTitle = L"laiue",
    .windowWidth = 640,
    .windowHeight = 360,
    .cameraSpeed = 80.0f,
    .mouseSensitivity = 0.0025f,
    .defaultFieldOfViewDegrees = 90,
    .dayLengthMinutes = 60.0f,
    .startTimeOfDayHours = 9.0f,
    .nearPlane = 0.1f,
    .farPlane = 1000.0f,
    .editReachDistance = 8.0f,
    .viewRadiusChunks = 5,
    .worldSeed = 42,
    .rebaseThresholdBlocks = 1048576,
    .spawnHeight = 100.0,
};
