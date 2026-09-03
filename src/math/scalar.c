#include "math/scalar.h"

#include <stdint.h>

// Аппаратный sqrt берётся интринсиком, чтобы не тянуть CRT/libm в no-CRT
// сборку. У x86_64 это SSE, у ARM64 — скалярный fsqrt.
#if defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#else
#include <xmmintrin.h>
#endif

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

float ScalarSqrt(float value)
{
    if (value <= 0.0f)
    {
        return 0.0f;
    }
#if defined(_M_ARM64) || defined(__aarch64__)
    // Скалярного vsqrts_f32 нет в общем наборе заголовков; однополосный
    // вектор компилируется в ту же одну инструкцию fsqrt.
    return vget_lane_f32(vsqrt_f32(vdup_n_f32(value)), 0);
#else
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(value)));
#endif
}

// Схема Cephes atanf: два порога редукции диапазона
// (tan(pi/8) и tan(3*pi/8)), затем полином 4-й степени по x^2.
float ScalarAtan(float value)
{
    float sign = 1.0f;
    float x = value;
    if (x < 0.0f)
    {
        sign = -1.0f;
        x = -x;
    }

    float offset = 0.0f;
    if (x > 2.414213562f)        // tan(3*pi/8)
    {
        offset = 1.570796327f;   // pi/2
        x = -1.0f / x;
    }
    else if (x > 0.414213562f)   // tan(pi/8)
    {
        offset = 0.785398163f;   // pi/4
        x = (x - 1.0f) / (x + 1.0f);
    }

    float x2 = x * x;
    float series = 8.05374449538e-2f;
    series = series * x2 - 1.38776856032e-1f;
    series = series * x2 + 1.99777106478e-1f;
    series = series * x2 - 3.33329491539e-1f;
    return sign * (series * x2 * x + x + offset);
}

float ScalarAtan2(float y, float x)
{
    if (x == 0.0f)
    {
        if (y > 0.0f) return 1.570796327f;
        if (y < 0.0f) return -1.570796327f;
        return 0.0f;
    }

    float angle = ScalarAtan(y / x);
    if (x < 0.0f)
    {
        angle += y >= 0.0f ? 3.141592654f : -3.141592654f;
    }
    return angle;
}

float ScalarAcos(float value)
{
    float x = ScalarClamp(value, -1.0f, 1.0f);
    return ScalarAtan2(ScalarSqrt(1.0f - x * x), x);
}

// Приводит угол к диапазону [-pi, pi].
float ScalarWrap(float radians)
{
    float revolutions = radians * 0.1591549430918953f;
    int32_t wholeRevolutions = (int32_t)(revolutions + (revolutions >= 0.0f ? 0.5f : -0.5f));
    return radians - (float)wholeRevolutions * 6.283185307179586f;
}
