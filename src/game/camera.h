#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct Camera
{
    float position[3];
    float yaw;
    float pitch;
} Camera;

void CameraInit(Camera* camera, float x, float y, float z, float yaw, float pitch);

void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyW, bool keyA, bool keyS, bool keyD,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed);

void CameraGetViewMatrix(const Camera* camera, float outMatrix[16]);
void CameraGetProjectionMatrix(float aspectRatio, float fovRadians, float nearPlane, float farPlane, float outMatrix[16]);
