#pragma once

#include "api.h"

#include <stdbool.h>

// Скалярная математика без CRT: полиномиальные аппроксимации
// вместо библиотечных sinf/cosf.

LAIUE_SCENE_API float ScalarSin(float radians);
LAIUE_SCENE_API float ScalarCos(float radians);
LAIUE_SCENE_API float ScalarTan(float radians);
LAIUE_SCENE_API float ScalarClamp(
    float value, float minimum, float maximum);
LAIUE_SCENE_API float ScalarWrap(float radians);
LAIUE_SCENE_API float ScalarSqrt(float value);

// Арктангенсы — минимаксный полином, точность ~1e-6 рад.
// ScalarAtan2 повторяет соглашения atan2f: результат в (-pi, pi],
// учитывает знаки обоих аргументов.
LAIUE_SCENE_API float ScalarAtan(float value);
LAIUE_SCENE_API float ScalarAtan2(float y, float x);
LAIUE_SCENE_API float ScalarAcos(float value);

// out = left * right (матрицы 4x4, row-major).
LAIUE_SCENE_API void Matrix4Multiply(
    const float left[16], const float right[16], float out[16]);

// Извлекает 6 плоскостей пирамиды видимости из матрицы view-projection
// (row-major, соглашение вектор-строка v * M, глубина D3D 0..1).
// Каждая плоскость — (a, b, c, d): точка внутри, если a*x+b*y+c*z+d >= 0.
LAIUE_SCENE_API void Matrix4ExtractFrustumPlanes(
    const float viewProjection[16], float outPlanes[6][4]);

// Пересекается ли AABB с пирамидой видимости (консервативный тест).
LAIUE_SCENE_API bool FrustumIntersectsBox(
    const float planes[6][4], const float minimum[3],
    const float maximum[3]);
