#pragma once

#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Построение кадра широкого угла (внутренний компонент ядра).
//
// Классическая перспектива геометрически не может показать 180 и больше,
// а её растяжение по краям делает бесполезной уже к ~150. Поэтому широкий
// угол рисуется через кубмапу: сцена растеризуется на грани куба,
// ориентированного по осям вида, затем полноэкранный резолв переводит
// пиксель экрана в направление и читает кубмапу.
//
// Оптимизация — держать стоимость на уровне обычного рендера:
//  * активны только грани, куда реально смотрит проекция;
//  * на каждой грани рисуется только задействованный прямоугольник
//    (off-center пирамида + viewport/scissor), поэтому суммарная площадь
//    растеризации близка к площади экрана;
//  * разрешение грани подбирается под плотность пикселей экрана
//    (чем шире угол, тем меньше грань);
//  * прямоугольники и проекции пересчитываются только при смене поля
//    зрения, размера окна или проекции — не каждый кадр.

typedef enum RenderProjection
{
    RENDER_PROJECTION_AUTO = 0,     // перспектива до порога, дальше рыбий глаз
    RENDER_PROJECTION_PERSPECTIVE,  // классический однопроходный рендер
    RENDER_PROJECTION_FISHEYE,      // равноудалённая панорама (0..360)
    RENDER_PROJECTION_CYLINDER,     // цилиндрическая панорама (0..360)
    RENDER_PROJECTION_COUNT,
} RenderProjection;

// Порог режима «Авто»: до него — обычная перспектива без накладных расходов.
#define PANORAMA_AUTO_THRESHOLD_DEGREES 120.0f
// Предел явной перспективы (к 180 она вырождается).
#define PANORAMA_PERSPECTIVE_MAX_DEGREES 170.0f

// Кеш геометрии панорамы. Обнулить при создании.
typedef struct PanoramaCache
{
    bool valid;
    RendererResolveMapping mapping;
    float fovHorizontalRadians;
    int32_t width;
    int32_t height;
    uint32_t faceResolution;

    uint32_t passCount;
    uint32_t faceIndex[RENDERER_MAX_SCENE_PASSES];
    uint32_t rect[RENDERER_MAX_SCENE_PASSES][4];        // тексели: minX, minY, maxX, maxY
    float faceProjection[RENDERER_MAX_SCENE_PASSES][16]; // off-center проекция грани
    float verticalScale;
} PanoramaCache;

// Панорамный ли режим при данных настройках (для подписи в меню).
bool PanoramaIsActive(RenderProjection projection, float fovHorizontalDegrees);

// Собирает описание кадра для рендерера. view — матрица вида камеры.
// fovHorizontalDegrees — горизонтальное поле зрения 0..360 (значения < 1
// поднимаются до 1, перспектива дополнительно ограничивается сверху).
void PanoramaBuildFrameSetup(PanoramaCache* cache,
    RenderProjection projection, float fovHorizontalDegrees,
    int32_t width, int32_t height, float nearPlane, float farPlane,
    const float view[16], RendererFrameSetup* outSetup);
