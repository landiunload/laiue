#include "core/math.h"

float ScalarSin(float x)
{
    x = ScalarWrap(x);
    float x2 = x * x;
    float s = -2.50521084e-08f;
    s = s * x2 + 2.75573192e-06f;
    s = s * x2 - 1.98412698e-04f;
    s = s * x2 + 8.33333333e-03f;
    s = s * x2 - 1.66666667e-01f;
    s = s * x2 + 1.0f;
    return x * s;
}

float ScalarCos(float x)
{
    x = ScalarWrap(x);
    float x2 = x * x;
    float c = -2.75573192e-07f;
    c = c * x2 + 2.48015873e-05f;
    c = c * x2 - 1.38888889e-03f;
    c = c * x2 + 4.16666667e-02f;
    c = c * x2 - 5.00000000e-01f;
    c = c * x2 + 1.0f;
    return c;
}

float ScalarTan(float radians)
{
    return ScalarSin(radians) / ScalarCos(radians);
}

float ScalarClamp(float value, float min, float max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

float ScalarWrap(float radians)
{
    float rev = radians * 0.1591549430918953f;
    int whole = (int)(rev + (rev >= 0.0f ? 0.5f : -0.5f));
    return radians - (float)whole * 6.283185307179586f;
}
