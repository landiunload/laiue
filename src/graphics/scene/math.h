#pragma once

#include "api.h"

#include <stdbool.h>

// Матрицы и пирамида видимости остаются частью публичного SDK; скалярная
// математика вынесена во внутреннюю math/scalar.h и не экспортируется.

// out = left * right (матрицы 4x4, row-major).
LAIUE_SCENE_MATH_API void Matrix4Multiply(
    const float left[16], const float right[16], float out[16]);

// Извлекает 6 плоскостей пирамиды видимости из матрицы view-projection
// (row-major, соглашение вектор-строка v * M, глубина D3D 0..1).
// Каждая плоскость — (a, b, c, d): точка внутри, если a*x+b*y+c*z+d >= 0.
LAIUE_SCENE_MATH_API void Matrix4ExtractFrustumPlanes(
    const float viewProjection[16], float outPlanes[6][4]);

// Пересекается ли AABB с пирамидой видимости (консервативный тест).
LAIUE_SCENE_MATH_API bool FrustumIntersectsBox(
    const float planes[6][4], const float minimum[3],
    const float maximum[3]);

// Перспектива в соглашениях рендера: вид смотрит вдоль +Z, вектор-строка,
// глубина D3D 0..1. Угол — вертикальное поле зрения в радианах. false и
// единичная матрица при неположительных размерах или nearPlane >= farPlane.
LAIUE_SCENE_MATH_API bool Matrix4Perspective(
    float verticalFovRadians, float aspectRatio, float nearPlane,
    float farPlane, float out[16]);

// Матрица вида из eye на target; up задаёт верх мира ({0, 0, 1} для мира с
// осью Z вверх). Координаты — относительно близкого начала рендера, не
// абсолютные. false и единичная матрица, если eye совпадает с target или
// взгляд параллелен up.
LAIUE_SCENE_MATH_API bool Matrix4LookAt(
    const float eye[3], const float target[3], const float up[3],
    float out[16]);

// Обратная матрица 4x4, посчитанная в двойной точности. false и единичная
// матрица, если матрица вырождена.
LAIUE_SCENE_MATH_API bool Matrix4Invert(const float matrix[16], float out[16]);

// Луч из камеры через точку экрана для выбора курсором. ndcX и ndcY — от -1
// (левый и нижний край кадра) до 1 (правый и верхний); viewProjection — та же
// матрица, что рисует кадр (вектор-строка, глубина D3D 0..1). Начало луча
// лежит на ближней плоскости, направление единичное. false, если матрица
// вырождена.
LAIUE_SCENE_MATH_API bool ViewRayThroughScreenPoint(
    const float viewProjection[16], float ndcX, float ndcY, float outOrigin[3],
    float outDirection[3]);
