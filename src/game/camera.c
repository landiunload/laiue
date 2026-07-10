#include "game/camera.h"
#include "core/math.h"

#define PITCH_LIMIT 1.570796f

void CameraInit(Camera* camera, double x, double y, double z, float yaw, float pitch)
{
    camera->position[0] = x;
    camera->position[1] = y;
    camera->position[2] = z;
    camera->yaw = yaw;
    camera->pitch = ScalarClamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void CameraGetForwardVector(const Camera* camera, float outForward[3])
{
    float sinPitch = ScalarSin(camera->pitch);
    float cosPitch = ScalarCos(camera->pitch);
    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);

    outForward[0] = sinYaw * cosPitch;
    outForward[1] = sinPitch;
    outForward[2] = cosYaw * cosPitch;
}

void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyForward, bool keyLeft, bool keyBackward, bool keyRight, bool keyUp,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed, float mouseSensitivity)
{
    camera->yaw += (float)mouseDeltaX * mouseSensitivity;
    camera->pitch -= (float)mouseDeltaY * mouseSensitivity;
    camera->pitch = ScalarClamp(camera->pitch, -PITCH_LIMIT, PITCH_LIMIT);

    float forward[3];
    CameraGetForwardVector(camera, forward);

    float rightX = ScalarCos(camera->yaw);
    float rightZ = -ScalarSin(camera->yaw);

    float step = speed * deltaSeconds;
    if (keyForward)
    {
        camera->position[0] += (double)(forward[0] * step);
        camera->position[1] += (double)(forward[1] * step);
        camera->position[2] += (double)(forward[2] * step);
    }
    if (keyBackward)
    {
        camera->position[0] -= (double)(forward[0] * step);
        camera->position[1] -= (double)(forward[1] * step);
        camera->position[2] -= (double)(forward[2] * step);
    }
    if (keyRight)
    {
        camera->position[0] += (double)(rightX * step);
        camera->position[2] += (double)(rightZ * step);
    }
    if (keyLeft)
    {
        camera->position[0] -= (double)(rightX * step);
        camera->position[2] -= (double)(rightZ * step);
    }
    if (keyUp)
    {
        camera->position[1] += (double)step;
    }
}

void CameraGetViewMatrix(const Camera* camera, const float relativeEyePosition[3], float outMatrix[16])
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

    float positionX = relativeEyePosition[0];
    float positionY = relativeEyePosition[1];
    float positionZ = relativeEyePosition[2];

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
