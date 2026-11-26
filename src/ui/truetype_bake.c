#include "truetype_bake.h"

#include "ui_font_internal.h"

#include <string.h>

_Static_assert(TRUE_TYPE_ATLAS_MAX_GLYPHS == UI_GLYPH_INDEX_EM_DASH + 1u,
               "atlas workspace covers the fixed glyph set");

static int32_t FloorPixel(double x)
{
    const int32_t truncated = (int32_t)x;
    return truncated - (int32_t)((double)truncated > x);
}

static int32_t CeilPixel(double x)
{
    const int32_t truncated = (int32_t)x;
    return truncated + (int32_t)((double)truncated < x);
}

static bool Resolve(const TrueTypeFont *font, uint16_t glyph, float scale,
                    TrueTypeBakeGeometry *geometry, TrueTypeXYGlyph *shape)
{
    const TrueTypeXYStorage storage = {geometry->points, geometry->ends,
                                       TRUE_TYPE_OUTLINE_MAX_POINTS,
                                       TRUE_TYPE_OUTLINE_MAX_CONTOURS};
    return TrueTypeGlyphResolveXY(font, glyph, scale, &geometry->xy, storage, shape);
}

static bool BuildEdges(TrueTypeBakeGeometry *geometry, const TrueTypeXYGlyph *shape,
                       uint32_t *outCount)
{
    TrueTypeEdgeSummary edges;
    if (!TrueTypeEdgesBuild(geometry->points, shape->pointCount, geometry->ends,
                            shape->contourCount, &geometry->edges, &edges))
        return false;
    *outCount = edges.edgeCount;
    return true;
}

static bool PrepareGlyph(const TrueTypeFont *font, uint16_t code, float scale,
                         TrueTypeBakeGeometry *geometry, TrueTypeBakeGlyph *glyph)
{
    uint16_t index;
    TrueTypeXYGlyph shape;
    if (!TrueTypeReaderGlyphIndex(font, code, &index) ||
        !Resolve(font, index, scale, geometry, &shape))
        return false;

    const float pen = shape.phantoms[0].x;
    *glyph = (TrueTypeBakeGlyph){
        .index = index,
        .penX = pen,
        .metrics = {.codepoint = code, .advance = shape.phantoms[1].x - pen},
    };
    if (shape.pointCount == 0u)
        return true;

    // Предел XY сохраняет преобразования к int32_t представимыми.
    const int32_t left = FloorPixel((double)shape.xMin - (double)pen);
    const int32_t right = CeilPixel((double)shape.xMax - (double)pen);
    const int32_t top = CeilPixel(shape.yMax);
    const int32_t bottom = FloorPixel(shape.yMin);
    const int32_t width = right - left;
    const int32_t height = top - bottom;
    if (width == 0 || height == 0)
        return true;
    if (left < INT16_MIN || left > INT16_MAX || top < INT16_MIN || top > INT16_MAX ||
        width > UINT16_MAX || height > UINT16_MAX)
        return false;
    glyph->metrics.offsetX = (int16_t)left;
    glyph->metrics.offsetY = (int16_t)top;
    glyph->metrics.width = (uint16_t)width;
    glyph->metrics.height = (uint16_t)height;
    return BuildEdges(geometry, &shape, &glyph->edgeCount);
}

bool TrueTypeBakePrepare(const TrueTypeFont *font, int32_t pixelSize, TrueTypeAtlasLimits limits,
                         uint64_t edgeRowBudget, TrueTypeBakeGeometry *geometry,
                         TrueTypeBakePlan *plan)
{
    if (font == NULL || font->unitsPerEm == 0u || pixelSize <= 0 || geometry == NULL ||
        plan == NULL)
        return false;
    const float scale = (float)pixelSize / (float)font->unitsPerEm;
    const float metricHeight = (float)font->ascent - (float)font->descent + (float)font->lineGap;
    if (scale > TRUE_TYPE_XY_MAX_PIXELS_PER_UNIT || metricHeight <= 0.0f)
        return false;

    TrueTypeAtlasGlyphSize sizes[TRUE_TYPE_ATLAS_MAX_GLYPHS];
    uint32_t output = 0u;
    uint32_t maxEdges = 0u;
    uint32_t maxWidth = 0u;
    for (size_t range = 0u; range < UI_GLYPH_RANGE_COUNT; ++range)
    {
        const UiGlyphRange codes = UI_GLYPH_RANGES[range];
        for (uint32_t code = codes.first; code <= codes.last; ++code, ++output)
        {
            TrueTypeBakeGlyph *glyph = &plan->glyphs[output];
            if (!PrepareGlyph(font, (uint16_t)code, scale, geometry, glyph))
                return false;
            const uint64_t work = (uint64_t)glyph->edgeCount * glyph->metrics.height;
            if (work > edgeRowBudget)
                return false;
            edgeRowBudget -= work;
            if (glyph->edgeCount > maxEdges)
                maxEdges = glyph->edgeCount;
            if (glyph->metrics.width > maxWidth)
                maxWidth = glyph->metrics.width;
            sizes[output] = (TrueTypeAtlasGlyphSize){glyph->metrics.width, glyph->metrics.height};
        }
    }

    if (!TrueTypeAtlasPlan(sizes, output, limits, plan->placements, TRUE_TYPE_ATLAS_MAX_GLYPHS,
                           &plan->atlas))
        return false;
    plan->pixelsPerUnit = scale;
    plan->ascent = (float)font->ascent * scale;
    plan->lineHeight = metricHeight * scale;
    plan->maxEdges = maxEdges;
    plan->maxWidth = maxWidth;
    return true;
}

bool TrueTypeBakePaint(const TrueTypeFont *font, const TrueTypeBakePlan *plan,
                       TrueTypeBakeGeometry *geometry, TrueTypeCoverageStorage scratch,
                       uint8_t *atlas, size_t atlasCapacity, UiGlyph *glyphs,
                       uint32_t glyphCapacity)
{
    if (font == NULL || plan == NULL || geometry == NULL || atlas == NULL || glyphs == NULL ||
        atlasCapacity < plan->atlas.bytes || glyphCapacity < plan->atlas.glyphCount ||
        plan->maxEdges > scratch.eventCapacity || plan->maxWidth > scratch.coverageCapacity ||
        (plan->maxEdges != 0u && scratch.events == NULL) ||
        (plan->maxWidth != 0u && scratch.coverage == NULL))
        return false;

    memset(atlas, 0, plan->atlas.bytes);
    const float inverseWidth = 1.0f / (float)plan->atlas.width;
    const float inverseHeight = 1.0f / (float)plan->atlas.height;
    for (uint32_t index = 0u; index < plan->atlas.glyphCount; ++index)
    {
        const TrueTypeBakeGlyph prepared = plan->glyphs[index];
        UiGlyph *glyph = &glyphs[index];
        *glyph = prepared.metrics;
        if (glyph->width == 0u || glyph->height == 0u)
            continue;
        const TrueTypeAtlasPlacement placement = plan->placements[index];
        TrueTypeXYGlyph shape;
        uint32_t edgeCount;
        if (!Resolve(font, prepared.index, plan->pixelsPerUnit, geometry, &shape) ||
            !BuildEdges(geometry, &shape, &edgeCount) || edgeCount != prepared.edgeCount)
            return false;

        const double left = (double)glyph->offsetX + (double)prepared.penX;
        scratch.rowCapacity = glyph->width;
        for (uint32_t row = 0u; row < glyph->height; ++row)
        {
            scratch.row = atlas + (size_t)(placement.y + row) * plan->atlas.width + placement.x;
            uint32_t written;
            if (!TrueTypeCoverageRow(geometry->edges.edges, edgeCount, left,
                                     (double)glyph->offsetY - (double)row, glyph->width, scratch,
                                     &written))
                return false;
        }
        glyph->u0 = (float)placement.x * inverseWidth;
        glyph->v0 = (float)placement.y * inverseHeight;
        glyph->u1 = (float)(placement.x + glyph->width) * inverseWidth;
        glyph->v1 = (float)(placement.y + glyph->height) * inverseHeight;
    }
    return true;
}
