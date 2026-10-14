#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Camera
{
    double position[3];
    float yaw;
    float pitch;
} Camera;

LAIUE_SCENE_API void CameraInit(Camera* camera, double x, double y, double z,
    float yaw, float pitch);

LAIUE_SCENE_API void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyForward, bool keyLeft, bool keyBackward, bool keyRight, bool keyUp,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed, float mouseSensitivity);

LAIUE_SCENE_API void CameraGetForwardVector(
    const Camera* camera, float outForward[3]);

// Матрица вида для позиции глаза относительно начала координат рендера.
LAIUE_SCENE_API void CameraGetViewMatrix(const Camera* camera,
    const float relativeEyePosition[3], float outMatrix[16]);
LAIUE_SCENE_API void CameraGetProjectionMatrix(
    float aspectRatio, float fovRadians,
    float nearPlane, float farPlane, float outMatrix[16]);
