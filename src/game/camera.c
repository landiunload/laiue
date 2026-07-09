#include "game/camera.h"
#include "core/math.h"

#define MOUSE_SENSITIVITY 0.0025f
#define PITCH_LIMIT 1.570796f

void CameraInit(Camera* camera, float x, float y, float z, float yaw, float pitch)
{
    camera->position[0] = x;
    camera->position[1] = y;
    camera->position[2] = z;
    camera->yaw = yaw;
    camera->pitch = ScalarClamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyW, bool keyA, bool keyS, bool keyD,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed)
{
    camera->yaw += (float)mouseDeltaX * MOUSE_SENSITIVITY;
    camera->pitch -= (float)mouseDeltaY * MOUSE_SENSITIVITY;
    camera->pitch = ScalarClamp(camera->pitch, -PITCH_LIMIT, PITCH_LIMIT);

    float sinPitch = ScalarSin(camera->pitch);
    float cosPitch = ScalarCos(camera->pitch);
    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);

    float forwardX = sinYaw * cosPitch;
    float forwardY = sinPitch;
    float forwardZ = cosYaw * cosPitch;

    float rightX = cosYaw;
    float rightZ = -sinYaw;

    float step = speed * deltaSeconds;
    if (keyW)
    {
        camera->position[0] += forwardX * step;
        camera->position[1] += forwardY * step;
        camera->position[2] += forwardZ * step;
    }
    if (keyS)
    {
        camera->position[0] -= forwardX * step;
        camera->position[1] -= forwardY * step;
        camera->position[2] -= forwardZ * step;
    }
    if (keyD)
    {
        camera->position[0] += rightX * step;
        camera->position[2] += rightZ * step;
    }
    if (keyA)
    {
        camera->position[0] -= rightX * step;
        camera->position[2] -= rightZ * step;
    }
}

void CameraGetViewMatrix(const Camera* camera, float outMatrix[16])
{
    float sinPitch = ScalarSin(camera->pitch);
    float cosPitch = ScalarCos(camera->pitch);
    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);

    float forwardX = sinYaw * cosPitch;
    float forwardY = sinPitch;
    float forwardZ = cosYaw * cosPitch;

    float rightX = cosYaw;
    float rightY = 0.0f;
    float rightZ = -sinYaw;

    float upX = forwardY * rightZ - forwardZ * rightY;
    float upY = forwardZ * rightX - forwardX * rightZ;
    float upZ = forwardX * rightY - forwardY * rightX;

    float positionX = camera->position[0];
    float positionY = camera->position[1];
    float positionZ = camera->position[2];

    outMatrix[0] = rightX;  outMatrix[1] = upX;  outMatrix[2] = forwardX;  outMatrix[3] = 0.0f;
    outMatrix[4] = rightY;  outMatrix[5] = upY;  outMatrix[6] = forwardY;  outMatrix[7] = 0.0f;
    outMatrix[8] = rightZ;  outMatrix[9] = upZ;  outMatrix[10] = forwardZ; outMatrix[11] = 0.0f;
    outMatrix[12] = -(rightX * positionX + rightY * positionY + rightZ * positionZ);
    outMatrix[13] = -(upX * positionX + upY * positionY + upZ * positionZ);
    outMatrix[14] = -(forwardX * positionX + forwardY * positionY + forwardZ * positionZ);
    outMatrix[15] = 1.0f;
}

void CameraGetProjectionMatrix(float aspectRatio, float fovRadians, float nearPlane, float farPlane, float outMatrix[16])
{
    float yScale = 1.0f / ScalarTan(fovRadians * 0.5f);
    float xScale = yScale / aspectRatio;
    float depth = farPlane / (farPlane - nearPlane);

    for (int32_t i = 0; i < 16; ++i)
    {
        outMatrix[i] = 0.0f;
    }
    outMatrix[0] = xScale;
    outMatrix[5] = yScale;
    outMatrix[10] = depth;
    outMatrix[11] = 1.0f;
    outMatrix[14] = -nearPlane * depth;
}
