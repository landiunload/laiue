#include "game/camera.h"
#include "core/math.h"

#include <string.h>

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

    float sp = ScalarSin(camera->pitch);
    float cp = ScalarCos(camera->pitch);
    float sy = ScalarSin(camera->yaw);
    float cy = ScalarCos(camera->yaw);

    float fx = sy * cp;
    float fy = sp;
    float fz = cy * cp;

    float rx = cy;
    float rz = -sy;

    float step = speed * deltaSeconds;
    if (keyW) { camera->position[0] += fx * step; camera->position[1] += fy * step; camera->position[2] += fz * step; }
    if (keyS) { camera->position[0] -= fx * step; camera->position[1] -= fy * step; camera->position[2] -= fz * step; }
    if (keyD) { camera->position[0] += rx * step; camera->position[2] += rz * step; }
    if (keyA) { camera->position[0] -= rx * step; camera->position[2] -= rz * step; }
}

void CameraGetViewMatrix(const Camera* camera, float outMatrix[16])
{
    float sp = ScalarSin(camera->pitch);
    float cp = ScalarCos(camera->pitch);
    float sy = ScalarSin(camera->yaw);
    float cy = ScalarCos(camera->yaw);

    float fx = sy * cp;
    float fy = sp;
    float fz = cy * cp;

    float rx = cy;
    float ry = 0.0f;
    float rz = -sy;

    float ux = fy * rz - fz * ry;
    float uy = fz * rx - fx * rz;
    float uz = fx * ry - fy * rx;

    float px = camera->position[0];
    float py = camera->position[1];
    float pz = camera->position[2];

    outMatrix[0] = rx;  outMatrix[1] = ux;  outMatrix[2] = fx;  outMatrix[3] = 0.0f;
    outMatrix[4] = ry;  outMatrix[5] = uy;  outMatrix[6] = fy;  outMatrix[7] = 0.0f;
    outMatrix[8] = rz;  outMatrix[9] = uz;  outMatrix[10] = fz; outMatrix[11] = 0.0f;
    outMatrix[12] = -(rx * px + ry * py + rz * pz);
    outMatrix[13] = -(ux * px + uy * py + uz * pz);
    outMatrix[14] = -(fx * px + fy * py + fz * pz);
    outMatrix[15] = 1.0f;
}

void CameraGetProjectionMatrix(float aspectRatio, float fovRadians, float nearPlane, float farPlane, float outMatrix[16])
{
    float yScale = 1.0f / ScalarTan(fovRadians * 0.5f);
    float xScale = yScale / aspectRatio;
    float depth = farPlane / (farPlane - nearPlane);

    memset(outMatrix, 0, 16 * sizeof(float));
    outMatrix[0] = xScale;
    outMatrix[5] = yScale;
    outMatrix[10] = depth;
    outMatrix[11] = 1.0f;
    outMatrix[14] = -nearPlane * depth;
}
