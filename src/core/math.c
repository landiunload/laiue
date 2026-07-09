#include "core/math.h"

#include <stdint.h>

float ScalarSin(float radians)
{
    float x = ScalarWrap(radians);
    float x2 = x * x;
    float series = -2.50521084e-08f;
    series = series * x2 + 2.75573192e-06f;
    series = series * x2 - 1.98412698e-04f;
    series = series * x2 + 8.33333333e-03f;
    series = series * x2 - 1.66666667e-01f;
    series = series * x2 + 1.0f;
    return x * series;
}

float ScalarCos(float radians)
{
    float x = ScalarWrap(radians);
    float x2 = x * x;
    float series = -2.75573192e-07f;
    series = series * x2 + 2.48015873e-05f;
    series = series * x2 - 1.38888889e-03f;
    series = series * x2 + 4.16666667e-02f;
    series = series * x2 - 5.00000000e-01f;
    series = series * x2 + 1.0f;
    return series;
}

float ScalarTan(float radians)
{
    return ScalarSin(radians) / ScalarCos(radians);
}

float ScalarClamp(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

// Приводит угол к диапазону [-pi, pi].
float ScalarWrap(float radians)
{
    float revolutions = radians * 0.1591549430918953f;
    int32_t wholeRevolutions = (int32_t)(revolutions + (revolutions >= 0.0f ? 0.5f : -0.5f));
    return radians - (float)wholeRevolutions * 6.283185307179586f;
}

void Matrix4Multiply(const float left[16], const float right[16], float out[16])
{
    for (int32_t row = 0; row < 4; ++row)
    {
        for (int32_t column = 0; column < 4; ++column)
        {
            float sum = 0.0f;
            for (int32_t element = 0; element < 4; ++element)
            {
                sum += left[row * 4 + element] * right[element * 4 + column];
            }
            out[row * 4 + column] = sum;
        }
    }
}

// Комбинация столбца j: dot(вершина, столбец j) = x*m[j] + y*m[4+j] + z*m[8+j] + m[12+j].
static void FrustumPlaneFromColumns(const float m[16], int32_t column, float sign, float outPlane[4])
{
    outPlane[0] = m[3]  + sign * m[column];
    outPlane[1] = m[7]  + sign * m[4 + column];
    outPlane[2] = m[11] + sign * m[8 + column];
    outPlane[3] = m[15] + sign * m[12 + column];
}

void Matrix4ExtractFrustumPlanes(const float viewProjection[16], float outPlanes[6][4])
{
    FrustumPlaneFromColumns(viewProjection, 0,  1.0f, outPlanes[0]);  // левая
    FrustumPlaneFromColumns(viewProjection, 0, -1.0f, outPlanes[1]);  // правая
    FrustumPlaneFromColumns(viewProjection, 1,  1.0f, outPlanes[2]);  // нижняя
    FrustumPlaneFromColumns(viewProjection, 1, -1.0f, outPlanes[3]);  // верхняя
    FrustumPlaneFromColumns(viewProjection, 2, -1.0f, outPlanes[5]);  // дальняя: col3 - col2

    // Ближняя плоскость (глубина D3D 0..1): сам столбец 2.
    outPlanes[4][0] = viewProjection[2];
    outPlanes[4][1] = viewProjection[6];
    outPlanes[4][2] = viewProjection[10];
    outPlanes[4][3] = viewProjection[14];
}

bool FrustumIntersectsBox(const float planes[6][4], const float minimum[3], const float maximum[3])
{
    for (int32_t plane = 0; plane < 6; ++plane)
    {
        // Ближайшая к положительному полупространству вершина AABB.
        float x = planes[plane][0] >= 0.0f ? maximum[0] : minimum[0];
        float y = planes[plane][1] >= 0.0f ? maximum[1] : minimum[1];
        float z = planes[plane][2] >= 0.0f ? maximum[2] : minimum[2];

        if (planes[plane][0] * x + planes[plane][1] * y + planes[plane][2] * z + planes[plane][3] < 0.0f)
        {
            return false;
        }
    }

    return true;
}
