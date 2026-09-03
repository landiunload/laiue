#pragma once

#include "api.h"

#include <stdbool.h>

// Матрицы и пирамида видимости остаются частью публичного SDK; скалярная
// математика вынесена во внутреннюю math/scalar.h и не экспортируется.

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
