#pragma once

#include <stdbool.h>

// Скалярная математика без CRT: полиномиальные аппроксимации
// вместо библиотечных sinf/cosf.

float ScalarSin(float radians);
float ScalarCos(float radians);
float ScalarTan(float radians);
float ScalarClamp(float value, float minimum, float maximum);
float ScalarWrap(float radians);

// out = left * right (матрицы 4x4, row-major).
void Matrix4Multiply(const float left[16], const float right[16], float out[16]);

// Извлекает 6 плоскостей пирамиды видимости из матрицы view-projection
// (row-major, соглашение вектор-строка v * M, глубина D3D 0..1).
// Каждая плоскость — (a, b, c, d): точка внутри, если a*x+b*y+c*z+d >= 0.
void Matrix4ExtractFrustumPlanes(const float viewProjection[16], float outPlanes[6][4]);

// Пересекается ли AABB с пирамидой видимости (консервативный тест).
bool FrustumIntersectsBox(const float planes[6][4], const float minimum[3], const float maximum[3]);
