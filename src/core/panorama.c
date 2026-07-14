#include "core/panorama.h"
#include "core/math.h"
#include "game/camera.h"

#include <string.h>

#define DEGREES_TO_RADIANS 0.0174532925199433f

// Плотность выборки при поиске задействованных прямоугольников граней.
#define BORDER_SAMPLES 256
#define EDGE_SAMPLES 48

// Запас прямоугольника: разреженность выборки + билинейная фильтрация.
#define RECT_MARGIN_UV 0.02f
#define RECT_MARGIN_TEXELS 4.0f

// Базисы граней кубмапы в пространстве вида (соглашение D3D:
// порядок +X, -X, +Y, -Y, +Z, -Z; up и right дают совпадение
// растеризации с выборкой TextureCube по направлению).
static const float FACE_RIGHT[6][3] = {
    {  0.0f, 0.0f, -1.0f },
    {  0.0f, 0.0f,  1.0f },
    {  1.0f, 0.0f,  0.0f },
    {  1.0f, 0.0f,  0.0f },
    {  1.0f, 0.0f,  0.0f },
    { -1.0f, 0.0f,  0.0f },
};
static const float FACE_UP[6][3] = {
    { 0.0f, 1.0f,  0.0f },
    { 0.0f, 1.0f,  0.0f },
    { 0.0f, 0.0f, -1.0f },
    { 0.0f, 0.0f,  1.0f },
    { 0.0f, 1.0f,  0.0f },
    { 0.0f, 1.0f,  0.0f },
};
static const float FACE_FORWARD[6][3] = {
    {  1.0f,  0.0f,  0.0f },
    { -1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f },
    {  0.0f, -1.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f },
    {  0.0f,  0.0f, -1.0f },
};

static float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Пиксель экрана (NDC, y вверх) -> единичное направление в пространстве
// вида. Держать в синхроне с shaders/panorama.hlsl.
static void ScreenToDirection(RendererResolveMapping mapping, float fovHalf,
    float verticalScale, float ndcX, float ndcY, float outDirection[3])
{
    if (mapping == RENDERER_RESOLVE_FISHEYE)
    {
        float qx = ndcX;
        float qy = ndcY * verticalScale;
        float radius = ScalarSqrt(qx * qx + qy * qy);
        float theta = radius * fovHalf;
        float sinTheta = ScalarSin(theta);
        float cosTheta = ScalarCos(theta);
        if (radius > 1e-6f)
        {
            outDirection[0] = qx / radius * sinTheta;
            outDirection[1] = qy / radius * sinTheta;
        }
        else
        {
            outDirection[0] = 0.0f;
            outDirection[1] = 0.0f;
        }
        outDirection[2] = cosTheta;
    }
    else
    {
        float psi = ndcX * fovHalf;
        float tangent = ndcY * verticalScale;
        float sinPsi = ScalarSin(psi);
        float cosPsi = ScalarCos(psi);
        float inverseLength = 1.0f / ScalarSqrt(1.0f + tangent * tangent);
        outDirection[0] = sinPsi * inverseLength;
        outDirection[1] = tangent * inverseLength;
        outDirection[2] = cosPsi * inverseLength;
    }
}

// Попадает ли единичное направление в отображаемую область экрана.
static bool DirectionOnScreen(RendererResolveMapping mapping, float fovHalf,
    float verticalScale, const float direction[3])
{
    const float tolerance = 1.0f + 1e-3f;

    if (mapping == RENDERER_RESOLVE_FISHEYE)
    {
        float theta = ScalarAcos(ScalarClamp(direction[2], -1.0f, 1.0f));
        float radius = theta / fovHalf;
        float planar = ScalarSqrt(
            direction[0] * direction[0] + direction[1] * direction[1]);
        if (planar < 1e-6f)
        {
            // Полюс (прямо вперёд или назад): назад виден только при
            // полном горизонтальном обороте.
            return radius <= tolerance;
        }
        float ndcX = direction[0] / planar * radius;
        float ndcY = direction[1] / planar * radius / verticalScale;
        return ndcX >= -tolerance && ndcX <= tolerance
            && ndcY >= -tolerance && ndcY <= tolerance;
    }

    float planar = ScalarSqrt(
        direction[0] * direction[0] + direction[2] * direction[2]);
    if (planar < 1e-6f)
    {
        return false; // полюса цилиндр не отображает
    }
    float ndcX = ScalarAtan2(direction[0], direction[2]) / fovHalf;
    float ndcY = direction[1] / planar / verticalScale;
    return ndcX >= -tolerance && ndcX <= tolerance
        && ndcY >= -tolerance && ndcY <= tolerance;
}

// Направление -> UV на грани; false, если направление вне пирамиды грани.
static bool DirectionToFaceUv(uint32_t face, const float direction[3],
    float* outU, float* outV)
{
    float x = Dot3(direction, FACE_RIGHT[face]);
    float y = Dot3(direction, FACE_UP[face]);
    float z = Dot3(direction, FACE_FORWARD[face]);
    if (z < 1e-6f)
    {
        return false;
    }

    const float tolerance = 1.0f + 2e-2f;
    float ndcX = x / z;
    float ndcY = y / z;
    if (ndcX < -tolerance || ndcX > tolerance
        || ndcY < -tolerance || ndcY > tolerance)
    {
        return false;
    }

    ndcX = ScalarClamp(ndcX, -1.0f, 1.0f);
    ndcY = ScalarClamp(ndcY, -1.0f, 1.0f);
    *outU = 0.5f + 0.5f * ndcX;
    *outV = 0.5f - 0.5f * ndcY;
    return true;
}

static void FaceUvToDirection(uint32_t face, float u, float v,
    float outDirection[3])
{
    float ndcX = u * 2.0f - 1.0f;
    float ndcY = 1.0f - v * 2.0f;
    float raw[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        raw[axis] = FACE_RIGHT[face][axis] * ndcX
            + FACE_UP[face][axis] * ndcY
            + FACE_FORWARD[face][axis];
    }
    float inverseLength = 1.0f / ScalarSqrt(Dot3(raw, raw));
    outDirection[0] = raw[0] * inverseLength;
    outDirection[1] = raw[1] * inverseLength;
    outDirection[2] = raw[2] * inverseLength;
}

typedef struct FaceBounds
{
    float minU, minV, maxU, maxV;
    bool used;
} FaceBounds;

static void AccumulateDirection(FaceBounds bounds[6], const float direction[3])
{
    for (uint32_t face = 0; face < 6; ++face)
    {
        float u;
        float v;
        if (!DirectionToFaceUv(face, direction, &u, &v))
        {
            continue;
        }
        FaceBounds* box = &bounds[face];
        if (!box->used)
        {
            box->used = true;
            box->minU = box->maxU = u;
            box->minV = box->maxV = v;
        }
        else
        {
            if (u < box->minU) box->minU = u;
            if (u > box->maxU) box->maxU = u;
            if (v < box->minV) box->minV = v;
            if (v > box->maxV) box->maxV = v;
        }
    }
}

// Задействованные прямоугольники граней. Область экрана связна, поэтому
// её границы на гранях покрываются образом периметра экрана плюс участками
// рёбер граней, попавшими внутрь области; выборка обоих множеств с запасом
// RECT_MARGIN даёт консервативный прямоугольник.
static void ComputeFaceBounds(RendererResolveMapping mapping, float fovHalf,
    float verticalScale, FaceBounds bounds[6])
{
    memset(bounds, 0, sizeof(FaceBounds) * 6);

    // Периметр экрана в NDC (параметр 0..4 — четыре стороны) и центр.
    for (uint32_t i = 0; i < BORDER_SAMPLES; ++i)
    {
        float t = (float)i * (4.0f / (float)BORDER_SAMPLES);
        float ndcX;
        float ndcY;
        if (t < 1.0f)      { ndcX = t * 2.0f - 1.0f;          ndcY = -1.0f; }
        else if (t < 2.0f) { ndcX = 1.0f;                     ndcY = (t - 1.0f) * 2.0f - 1.0f; }
        else if (t < 3.0f) { ndcX = 1.0f - (t - 2.0f) * 2.0f; ndcY = 1.0f; }
        else               { ndcX = -1.0f;                    ndcY = 1.0f - (t - 3.0f) * 2.0f; }

        float direction[3];
        ScreenToDirection(mapping, fovHalf, verticalScale, ndcX, ndcY, direction);
        AccumulateDirection(bounds, direction);
    }

    float center[3];
    ScreenToDirection(mapping, fovHalf, verticalScale, 0.0f, 0.0f, center);
    AccumulateDirection(bounds, center);

    // Рёбра граней: участки, попавшие в отображаемую область.
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t edge = 0; edge < 4; ++edge)
        {
            for (uint32_t i = 0; i <= EDGE_SAMPLES; ++i)
            {
                float t = (float)i / (float)EDGE_SAMPLES;
                float u;
                float v;
                switch (edge)
                {
                    case 0:  u = t;    v = 0.0f; break;
                    case 1:  u = t;    v = 1.0f; break;
                    case 2:  u = 0.0f; v = t;    break;
                    default: u = 1.0f; v = t;    break;
                }

                float direction[3];
                FaceUvToDirection(face, u, v, direction);
                if (DirectionOnScreen(mapping, fovHalf, verticalScale, direction))
                {
                    AccumulateDirection(bounds, direction);
                }
            }
        }
    }
}

// Off-center проекция, накрывающая прямоугольник [u0,u1]x[v0,v1] грани
// (полный размах грани — тангенс 45 градусов). Row-major, вектор-строка,
// глубина D3D 0..1 — совпадает с CameraGetProjectionMatrix.
static void FaceRectProjection(float u0, float v0, float u1, float v1,
    float nearPlane, float farPlane, float outMatrix[16])
{
    float left   = (u0 * 2.0f - 1.0f) * nearPlane;
    float right  = (u1 * 2.0f - 1.0f) * nearPlane;
    float top    = (1.0f - v0 * 2.0f) * nearPlane;
    float bottom = (1.0f - v1 * 2.0f) * nearPlane;
    float depth  = farPlane / (farPlane - nearPlane);

    for (int32_t i = 0; i < 16; ++i)
    {
        outMatrix[i] = 0.0f;
    }
    outMatrix[0]  = 2.0f * nearPlane / (right - left);
    outMatrix[5]  = 2.0f * nearPlane / (top - bottom);
    outMatrix[8]  = (left + right) / (left - right);
    outMatrix[9]  = (top + bottom) / (bottom - top);
    outMatrix[10] = depth;
    outMatrix[11] = 1.0f;
    outMatrix[14] = -nearPlane * depth;
}

// Матрица перехода из пространства вида в пространство грани
// (вектор-строка: столбцы — базис грани).
static void FaceBasisMatrix(uint32_t face, float outMatrix[16])
{
    for (int32_t i = 0; i < 16; ++i)
    {
        outMatrix[i] = 0.0f;
    }
    for (int32_t row = 0; row < 3; ++row)
    {
        outMatrix[row * 4 + 0] = FACE_RIGHT[face][row];
        outMatrix[row * 4 + 1] = FACE_UP[face][row];
        outMatrix[row * 4 + 2] = FACE_FORWARD[face][row];
    }
    outMatrix[15] = 1.0f;
}

// Разрешение грани: плотность текселей в центре грани не ниже плотности
// пикселей в центре экрана (шире угол — меньше грань), с разумным пределом.
static uint32_t ChooseFaceResolution(int32_t width, float fovHorizontalRadians)
{
    float needed = 2.0f * (float)width / fovHorizontalRadians;
    if (needed < 256.0f)
    {
        return 256;
    }
    if (needed >= 2048.0f)
    {
        return 2048;
    }
    return ((uint32_t)needed + 255u) & ~255u;
}

static float EffectiveFovDegrees(RenderProjection projection, float fovDegrees)
{
    float fov = ScalarClamp(fovDegrees, 1.0f, 360.0f);
    if (projection == RENDER_PROJECTION_PERSPECTIVE
        || (projection == RENDER_PROJECTION_AUTO
            && fov <= PANORAMA_AUTO_THRESHOLD_DEGREES))
    {
        fov = ScalarClamp(fov, 1.0f, PANORAMA_PERSPECTIVE_MAX_DEGREES);
    }
    return fov;
}

bool PanoramaIsActive(RenderProjection projection, float fovHorizontalDegrees)
{
    switch (projection)
    {
        case RENDER_PROJECTION_PERSPECTIVE:
            return false;
        case RENDER_PROJECTION_FISHEYE:
        case RENDER_PROJECTION_CYLINDER:
            return true;
        default:
            return ScalarClamp(fovHorizontalDegrees, 1.0f, 360.0f)
                > PANORAMA_AUTO_THRESHOLD_DEGREES;
    }
}

static void RebuildCache(PanoramaCache* cache, RendererResolveMapping mapping,
    float fovHalf, float verticalScale, uint32_t faceResolution,
    float nearPlane, float farPlane)
{
    FaceBounds bounds[6];
    ComputeFaceBounds(mapping, fovHalf, verticalScale, bounds);

    cache->passCount = 0;
    float resolution = (float)faceResolution;
    float margin = RECT_MARGIN_UV + RECT_MARGIN_TEXELS / resolution;

    for (uint32_t face = 0; face < 6; ++face)
    {
        if (!bounds[face].used || cache->passCount == RENDERER_MAX_SCENE_PASSES)
        {
            continue;
        }

        float minU = ScalarClamp(bounds[face].minU - margin, 0.0f, 1.0f);
        float maxU = ScalarClamp(bounds[face].maxU + margin, 0.0f, 1.0f);
        float minV = ScalarClamp(bounds[face].minV - margin, 0.0f, 1.0f);
        float maxV = ScalarClamp(bounds[face].maxV + margin, 0.0f, 1.0f);

        uint32_t texMinX = (uint32_t)(minU * resolution);
        uint32_t texMinY = (uint32_t)(minV * resolution);
        uint32_t texMaxX = (uint32_t)(maxU * resolution) + 1u;
        uint32_t texMaxY = (uint32_t)(maxV * resolution) + 1u;
        if (texMaxX > faceResolution) texMaxX = faceResolution;
        if (texMaxY > faceResolution) texMaxY = faceResolution;
        if (texMinX >= texMaxX || texMinY >= texMaxY)
        {
            continue;
        }

        uint32_t slot = cache->passCount++;
        cache->faceIndex[slot] = face;
        cache->rect[slot][0] = texMinX;
        cache->rect[slot][1] = texMinY;
        cache->rect[slot][2] = texMaxX;
        cache->rect[slot][3] = texMaxY;

        // Проекция строится по текселям, чтобы точно совпасть с viewport.
        FaceRectProjection(
            (float)texMinX / resolution, (float)texMinY / resolution,
            (float)texMaxX / resolution, (float)texMaxY / resolution,
            nearPlane, farPlane, cache->faceProjection[slot]);
    }
}

void PanoramaBuildFrameSetup(PanoramaCache* cache,
    RenderProjection projection, float fovHorizontalDegrees,
    int32_t width, int32_t height, float nearPlane, float farPlane,
    const float view[16], RendererFrameSetup* outSetup)
{
    memset(outSetup, 0, sizeof(*outSetup));

    float fovDegrees = EffectiveFovDegrees(projection, fovHorizontalDegrees);
    float fovRadians = fovDegrees * DEGREES_TO_RADIANS;
    float aspectHeightOverWidth = (float)height / (float)width;

    if (!PanoramaIsActive(projection, fovHorizontalDegrees))
    {
        // Классический однопроходный кадр: вертикальное поле зрения
        // выводится из горизонтального через отношение сторон.
        float tanHalfVertical =
            ScalarTan(fovRadians * 0.5f) * aspectHeightOverWidth;
        float fovVertical = 2.0f * ScalarAtan(tanHalfVertical);

        float perspective[16];
        CameraGetProjectionMatrix(1.0f / aspectHeightOverWidth, fovVertical,
            nearPlane, farPlane, perspective);

        outSetup->panorama = false;
        outSetup->passCount = 1;
        outSetup->passes[0].faceIndex = 0;
        outSetup->passes[0].rectMinX = 0;
        outSetup->passes[0].rectMinY = 0;
        outSetup->passes[0].rectMaxX = (uint32_t)width;
        outSetup->passes[0].rectMaxY = (uint32_t)height;
        Matrix4Multiply(view, perspective, outSetup->passes[0].viewProjection);
        return;
    }

    RendererResolveMapping mapping = projection == RENDER_PROJECTION_CYLINDER
        ? RENDERER_RESOLVE_CYLINDER : RENDERER_RESOLVE_FISHEYE;

    float fovHalf = fovRadians * 0.5f;
    float verticalScale;
    if (mapping == RENDERER_RESOLVE_FISHEYE)
    {
        verticalScale = aspectHeightOverWidth;
    }
    else
    {
        // Цилиндр: вертикальный масштаб уравнивает плотность пикселей
        // в центре экрана по горизонтали и вертикали.
        verticalScale = ScalarClamp(
            aspectHeightOverWidth * fovHalf, 0.01f, 5.67f); // tan(80)
    }

    uint32_t faceResolution = ChooseFaceResolution(width, fovRadians);

    if (!cache->valid
        || cache->mapping != mapping
        || cache->fovHorizontalRadians != fovRadians
        || cache->width != width
        || cache->height != height
        || cache->faceResolution != faceResolution)
    {
        RebuildCache(cache, mapping, fovHalf, verticalScale,
            faceResolution, nearPlane, farPlane);
        cache->valid = true;
        cache->mapping = mapping;
        cache->fovHorizontalRadians = fovRadians;
        cache->width = width;
        cache->height = height;
        cache->faceResolution = faceResolution;
        cache->verticalScale = verticalScale;
    }

    outSetup->panorama = true;
    outSetup->faceResolution = faceResolution;
    outSetup->resolveMapping = mapping;
    outSetup->fovHalfRadians = fovHalf;
    outSetup->resolveVerticalScale = verticalScale;
    outSetup->passCount = cache->passCount;

    for (uint32_t slot = 0; slot < cache->passCount; ++slot)
    {
        RendererScenePass* pass = &outSetup->passes[slot];
        pass->faceIndex = cache->faceIndex[slot];
        pass->rectMinX = cache->rect[slot][0];
        pass->rectMinY = cache->rect[slot][1];
        pass->rectMaxX = cache->rect[slot][2];
        pass->rectMaxY = cache->rect[slot][3];

        float faceBasis[16];
        float faceView[16];
        FaceBasisMatrix(cache->faceIndex[slot], faceBasis);
        Matrix4Multiply(view, faceBasis, faceView);
        Matrix4Multiply(faceView, cache->faceProjection[slot],
            pass->viewProjection);
    }
}
