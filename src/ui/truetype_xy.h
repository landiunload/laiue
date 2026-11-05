#pragma once

#include "truetype_phantoms.h"
#include "truetype_plan.h"

enum
{
    TRUE_TYPE_XY_MAX_PIXELS_PER_UNIT = 64,
    TRUE_TYPE_XY_MAX_MAGNITUDE = 1048576,
};

typedef struct TrueTypePixelPoint
{
    float x;
    float y;
    bool onCurve;
} TrueTypePixelPoint;

typedef struct TrueTypePixelMetricPoint
{
    float x;
    float y;
} TrueTypePixelMetricPoint;

typedef struct TrueTypeXYFrame
{
    uint16_t nodeIndex;
    uint16_t nextChild;
    float m00;
    float m01;
    float m10;
    float m11;
    float tx;
    float ty;
} TrueTypeXYFrame;

typedef struct TrueTypeXYWorkspace
{
    TrueTypePlanWorkspace plan;
    TrueTypeDesignPoint designPoints[TRUE_TYPE_OUTLINE_MAX_POINTS];
    uint16_t designEnds[TRUE_TYPE_OUTLINE_MAX_CONTOURS];
    TrueTypePixelPoint points[TRUE_TYPE_OUTLINE_MAX_POINTS];
    uint16_t contourEnds[TRUE_TYPE_OUTLINE_MAX_CONTOURS];
    TrueTypeXYFrame frames[TRUE_TYPE_PLAN_MAX_DEPTH];
} TrueTypeXYWorkspace;

typedef struct TrueTypeXYStorage
{
    TrueTypePixelPoint *points;
    uint16_t *contourEnds;
    uint32_t pointCapacity;
    uint32_t contourCapacity;
} TrueTypeXYStorage;

typedef struct TrueTypeXYGlyph
{
    uint32_t pointCount;
    uint32_t contourCount;
    float xMin;
    float yMin;
    float xMax;
    float yMax;
    TrueTypePixelMetricPoint phantoms[4];
    uint32_t phantomMask;
} TrueTypeXYGlyph;

// Private XY-only geometry: pixels/Y-up, исходные on/off-curve points и ends.
// Нет implied points, hints, edges или raster. Point attachment и USE_MY_METRICS
// возвращают false. Default offset unscaled; ROUND ties toward +infinity.
// Root initial phantoms масштабируются из helper85, bounds относятся к points,
// ещё не к tight quadratic/ink bounds. Empty glyph имеет нулевые bounds.
// 0 < pixelsPerUnit <= 64; abs basis, offsets, translations и output <= 2^20.
// Это implementation limits, не гарантия всех fonts при maximum scale.
// Требуется precise FP. Font успешно Initialize и immutable. Font/bytes,
// workspace, оба storage arrays и out не перекрываются. Workspace heap/static,
// каждый вызов инвалидирует прежний scratch. При false storage/out сохраняются;
// при success пишутся только count elements. NULL arrays допустимы для count0.
// Нет allocation, I/O, libm или C recursion.
bool TrueTypeGlyphResolveXY(const TrueTypeFont *font, uint16_t glyph, float pixelsPerUnit,
                            TrueTypeXYWorkspace *workspace, TrueTypeXYStorage storage,
                            TrueTypeXYGlyph *out);
