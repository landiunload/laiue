#include "core/camera.h"
#include "core/math.h"

#define PITCH_LIMIT 1.570796f
#define YAW_LIMIT   3.1415927f
#define YAW_PERIOD  6.2831853f

static float NormalizeYaw(float yaw)
{
    // Верхняя граница защищает преобразование в int32 от повреждённого
    // сохранения; обычный raw-input даже при int32 delta существенно меньше.
    if (yaw != yaw || yaw > 100000000.0f || yaw < -100000000.0f)
    {
        return 0.0f;
    }
    int32_t periods = (int32_t)(yaw / YAW_PERIOD);
    yaw -= (float)periods * YAW_PERIOD;
    if (yaw > YAW_LIMIT)
    {
        yaw -= YAW_PERIOD;
    }
    else if (yaw < -YAW_LIMIT)
    {
        yaw += YAW_PERIOD;
    }
    return yaw;
}

void CameraInit(Camera* camera, double x, double y, double z, float yaw, float pitch)
{
    camera->position[0] = x;
    camera->position[1] = y;  // вторая горизонталь
    camera->position[2] = z;  // высота
    camera->yaw = NormalizeYaw(yaw);
    camera->pitch = ScalarClamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

void CameraGetForwardVector(const Camera* camera, float outForward[3])
{
    float sinPitch = ScalarSin(camera->pitch);
    float cosPitch = ScalarCos(camera->pitch);
    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);

    outForward[0] = sinYaw * cosPitch;
    outForward[1] = cosYaw * cosPitch;   // вторая горизонталь (бывший Z)
    outForward[2] = sinPitch;             // высота (бывший Y)
}

void CameraUpdate(Camera* camera, float deltaSeconds,
    bool keyForward, bool keyLeft, bool keyBackward, bool keyRight, bool keyUp,
    int32_t mouseDeltaX, int32_t mouseDeltaY,
    float speed, float mouseSensitivity)
{
    camera->yaw += (float)mouseDeltaX * mouseSensitivity;
    camera->yaw = NormalizeYaw(camera->yaw);
    camera->pitch -= (float)mouseDeltaY * mouseSensitivity;
    camera->pitch = ScalarClamp(camera->pitch, -PITCH_LIMIT, PITCH_LIMIT);

    // Поворот выше должен применяться даже без перемещения. Векторы движения
    // и тригонометрия после него idle-кадру не нужны.
    if (!keyForward && !keyLeft && !keyBackward && !keyRight && !keyUp)
    {
        return;
    }

    float forward[3];
    CameraGetForwardVector(camera, forward);

    float rightX = ScalarCos(camera->yaw);
    float rightY = -ScalarSin(camera->yaw);  // вторая горизонталь (бывший Z)

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
        camera->position[1] += (double)(rightY * step);
    }
    if (keyLeft)
    {
        camera->position[0] -= (double)(rightX * step);
        camera->position[1] -= (double)(rightY * step);
    }
    if (keyUp)
    {
        camera->position[2] += (double)step;
    }
}

void CameraGetViewMatrix(const Camera* camera, const float relativeEyePosition[3], float outMatrix[16])
{
    float sinPitch = ScalarSin(camera->pitch);
    float cosPitch = ScalarCos(camera->pitch);
    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);

    float forwardX = sinYaw * cosPitch;
    float forwardY = cosYaw * cosPitch;   // вторая горизонталь
    float forwardZ = sinPitch;             // высота

    float rightX = cosYaw;
    float rightY = -sinYaw;
    float rightZ = 0.0f;

    float upX = rightY * forwardZ - rightZ * forwardY;
    float upY = rightZ * forwardX - rightX * forwardZ;
    float upZ = rightX * forwardY - rightY * forwardX;

    float posX = relativeEyePosition[0];
    float posY = relativeEyePosition[1];
    float posZ = relativeEyePosition[2];

    outMatrix[0] = rightX;  outMatrix[1] = upX;  outMatrix[2] = forwardX;  outMatrix[3] = 0.0f;
    outMatrix[4] = rightY;  outMatrix[5] = upY;  outMatrix[6] = forwardY;  outMatrix[7] = 0.0f;
    outMatrix[8] = rightZ;  outMatrix[9] = upZ;  outMatrix[10] = forwardZ; outMatrix[11] = 0.0f;
    outMatrix[12] = -(rightX * posX + rightY * posY + rightZ * posZ);
    outMatrix[13] = -(upX * posX + upY * posY + upZ * posZ);
    outMatrix[14] = -(forwardX * posX + forwardY * posY + forwardZ * posZ);
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
