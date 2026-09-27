#pragma once

#include "core/math/scalar.h"

#include <stdint.h>

/* Clamp a virtual stick to a unit circle while preserving its direction and
 * sub-maximal magnitude. */
static inline void AndroidWalkClampStick(float *x, float *y)
{
    if (x == 0 || y == 0)
        return;
    const float lengthSquared = *x * *x + *y * *y;
    if (lengthSquared > 1.0f)
    {
        const float inverseLength = 1.0f / ScalarSqrt(lengthSquared);
        *x *= inverseLength;
        *y *= inverseLength;
    }
}

static inline void AndroidWalkCameraRelativeMovement(
    float strafe, float forwardInput, float cameraForwardX,
    float cameraForwardY, float *worldX, float *worldY)
{
    AndroidWalkClampStick(&strafe, &forwardInput);
    const float forwardLengthSquared = cameraForwardX * cameraForwardX +
                                       cameraForwardY * cameraForwardY;
    if (forwardLengthSquared > 0.000001f)
    {
        const float inverseForwardLength =
            1.0f / ScalarSqrt(forwardLengthSquared);
        cameraForwardX *= inverseForwardLength;
        cameraForwardY *= inverseForwardLength;
    }
    else
    {
        cameraForwardX = 0.0f;
        cameraForwardY = 1.0f;
    }
    *worldX = forwardInput * cameraForwardX + strafe * cameraForwardY;
    *worldY = forwardInput * cameraForwardY - strafe * cameraForwardX;
}

static inline int32_t AndroidWalkAxisToFixed(float value)
{
    const float scaled = value * 32768.0f;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}
