#include "scene/math.h"
#include "math/scalar.h"

#include <stdint.h>

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

static void Matrix4Identity(float out[16])
{
    for (int32_t index = 0; index < 16; ++index)
    {
        out[index] = (index % 5 == 0) ? 1.0f : 0.0f;
    }
}

bool Matrix4Perspective(float verticalFovRadians, float aspectRatio, float nearPlane,
                        float farPlane, float out[16])
{
    Matrix4Identity(out);
    if (!(verticalFovRadians > 0.0f && verticalFovRadians < 3.14159f) || !(aspectRatio > 0.0f) ||
        !(nearPlane > 0.0f) || !(farPlane > nearPlane))
    {
        return false;
    }
    const float yScale = 1.0f / ScalarTan(verticalFovRadians * 0.5f);
    const float depth = farPlane / (farPlane - nearPlane);
    out[0] = yScale / aspectRatio;
    out[5] = yScale;
    out[10] = depth;
    out[11] = 1.0f;
    out[14] = -nearPlane * depth;
    out[15] = 0.0f;
    return true;
}

static float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool Normalize3(float vector[3])
{
    const float length = ScalarSqrt(Dot3(vector, vector));
    if (!(length > 1e-20f))
    {
        return false;
    }
    vector[0] /= length;
    vector[1] /= length;
    vector[2] /= length;
    return true;
}

bool Matrix4LookAt(const float eye[3], const float target[3], const float up[3], float out[16])
{
    Matrix4Identity(out);
    float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float right[3];
    float trueUp[3];
    if (!Normalize3(forward))
    {
        return false;
    }
    // Тот же базис, что у CameraGetViewMatrix: right = forward x up,
    // верх кадра = right x forward.
    Cross3(forward, up, right);
    if (!Normalize3(right))
    {
        return false;
    }
    Cross3(right, forward, trueUp);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        out[axis * 4 + 0] = right[axis];
        out[axis * 4 + 1] = trueUp[axis];
        out[axis * 4 + 2] = forward[axis];
        out[axis * 4 + 3] = 0.0f;
    }
    out[12] = -Dot3(right, eye);
    out[13] = -Dot3(trueUp, eye);
    out[14] = -Dot3(forward, eye);
    out[15] = 1.0f;
    return true;
}

static double AbsDouble(double value)
{
    return value < 0.0 ? -value : value;
}

bool Matrix4Invert(const float matrix[16], float out[16])
{
    // Гаусс-Жордан с выбором ведущего элемента: [M | I] -> [I | M^-1].
    double work[4][8];
    for (int32_t row = 0; row < 4; ++row)
    {
        for (int32_t column = 0; column < 4; ++column)
        {
            work[row][column] = matrix[row * 4 + column];
            work[row][4 + column] = row == column ? 1.0 : 0.0;
        }
    }
    for (int32_t column = 0; column < 4; ++column)
    {
        int32_t pivot = column;
        for (int32_t row = column + 1; row < 4; ++row)
        {
            if (AbsDouble(work[row][column]) > AbsDouble(work[pivot][column]))
            {
                pivot = row;
            }
        }
        if (!(AbsDouble(work[pivot][column]) > 1e-30))
        {
            Matrix4Identity(out);
            return false;
        }
        for (int32_t element = 0; element < 8; ++element)
        {
            const double swap = work[column][element];
            work[column][element] = work[pivot][element];
            work[pivot][element] = swap;
        }
        const double scale = 1.0 / work[column][column];
        for (int32_t element = 0; element < 8; ++element)
        {
            work[column][element] *= scale;
        }
        for (int32_t row = 0; row < 4; ++row)
        {
            const double factor = work[row][column];
            if (row == column || factor == 0.0)
            {
                continue;
            }
            for (int32_t element = 0; element < 8; ++element)
            {
                work[row][element] -= factor * work[column][element];
            }
        }
    }
    for (int32_t row = 0; row < 4; ++row)
    {
        for (int32_t column = 0; column < 4; ++column)
        {
            out[row * 4 + column] = (float)work[row][4 + column];
        }
    }
    return true;
}

// Точка экрана на глубине depth обратно в пространство мира.
static bool Unproject(const float inverse[16], float ndcX, float ndcY, float depth, float out[3])
{
    float point[4];
    for (int32_t column = 0; column < 4; ++column)
    {
        point[column] = ndcX * inverse[column] + ndcY * inverse[4 + column] +
                        depth * inverse[8 + column] + inverse[12 + column];
    }
    if (!(point[3] > 1e-20f || point[3] < -1e-20f))
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        out[axis] = point[axis] / point[3];
    }
    return true;
}

bool ViewRayThroughScreenPoint(const float viewProjection[16], float ndcX, float ndcY,
                               float outOrigin[3], float outDirection[3])
{
    float inverse[16];
    float nearPoint[3];
    float deeperPoint[3];
    // Вторая точка берётся на глубине 0.5, а не на дальней плоскости: при
    // дальней плоскости в десятки километров глубина около 1 теряет точность
    // во float, а направлению хватает любой второй точки луча.
    if (!Matrix4Invert(viewProjection, inverse) ||
        !Unproject(inverse, ndcX, ndcY, 0.0f, nearPoint) ||
        !Unproject(inverse, ndcX, ndcY, 0.5f, deeperPoint))
    {
        return false;
    }
    float direction[3] = {deeperPoint[0] - nearPoint[0], deeperPoint[1] - nearPoint[1],
                          deeperPoint[2] - nearPoint[2]};
    if (!Normalize3(direction))
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outOrigin[axis] = nearPoint[axis];
        outDirection[axis] = direction[axis];
    }
    return true;
}
