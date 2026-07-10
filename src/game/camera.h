#pragma once

#include <stdint.h>
#include <stdbool.h>

// Позиция — double: рендер работает в координатах относительно камеры
// (origin rebasing), поэтому мир не деградирует на больших расстояниях.
typedef struct Camera
{
    double position[3];
    float yaw;
    float pitch;
} Camera;

void CameraInit(Camera* camera, double x, double y, double z, float yaw, float pitch);

void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyForward, bool keyLeft, bool keyBackward, bool keyRight, bool keyUp,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed, float mouseSensitivity);

// Единичный вектор направления взгляда (для трассировки лучей).
void CameraGetForwardVector(const Camera* camera, float outForward[3]);

// Матрица вида для позиции глаза ОТНОСИТЕЛЬНО начала координат рендера
// (дробная часть позиции камеры — origin rebasing).
void CameraGetViewMatrix(const Camera* camera, const float relativeEyePosition[3], float outMatrix[16]);
void CameraGetProjectionMatrix(float aspectRatio, float fovRadians, float nearPlane, float farPlane, float outMatrix[16]);
