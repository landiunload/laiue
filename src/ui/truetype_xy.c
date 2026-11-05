#include "truetype_xy.h"

static bool XyBound(float value)
{
    return value >= -(float)TRUE_TYPE_XY_MAX_MAGNITUDE &&
           value <= (float)TRUE_TYPE_XY_MAX_MAGNITUDE;
}

// Bounded truncation avoids double rounding in floor(value + 0.5f).
static float XyRound(float value)
{
    int32_t integral = (int32_t)value;
    float remainder = value - (float)integral;
    if (remainder >= 0.5f)
        ++integral;
    else if (remainder < -0.5f)
        --integral;
    return (float)integral;
}

static bool XyChild(const TrueTypeXYFrame *parent, const TrueTypeComponent *component,
                    uint16_t nodeIndex, TrueTypeXYFrame *out)
{
    const float unit = 1.0f / 16384.0f;
    float a = (float)component->xScale * unit;
    float b = (float)component->scale01 * unit;
    float c = (float)component->scale10 * unit;
    float d = (float)component->yScale * unit;
    TrueTypeXYFrame child = {.nodeIndex = nodeIndex,
                             .m00 = parent->m00 * a + parent->m01 * b,
                             .m01 = parent->m00 * c + parent->m01 * d,
                             .m10 = parent->m10 * a + parent->m11 * b,
                             .m11 = parent->m10 * c + parent->m11 * d};
    if (!XyBound(child.m00) || !XyBound(child.m01) || !XyBound(child.m10) || !XyBound(child.m11))
        return false;
    const TrueTypeXYFrame *basis =
        (component->flags & TRUE_TYPE_COMPONENT_SCALED_OFFSET) != 0u ? &child : parent;
    float x = (float)component->argument1;
    float y = (float)component->argument2;
    float dx = basis->m00 * x + basis->m01 * y;
    float dy = basis->m10 * x + basis->m11 * y;
    if (!XyBound(dx) || !XyBound(dy))
        return false;
    if ((component->flags & TRUE_TYPE_COMPONENT_ROUND_XY) != 0u)
    {
        dx = XyRound(dx);
        dy = XyRound(dy);
    }
    child.tx = parent->tx + dx;
    child.ty = parent->ty + dy;
    if (!XyBound(child.tx) || !XyBound(child.ty))
        return false;
    *out = child;
    return true;
}

static bool XyPhantoms(const TrueTypeFont *font, uint16_t glyph, uint32_t contours, float scale,
                       TrueTypeXYGlyph *result)
{
    TrueTypeInitialPhantoms initial = {0};
    if (!TrueTypeGlyphInitialPhantomsRead(font, glyph, contours, &initial))
        return false;
    result->phantomMask = initial.validMask;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        if ((initial.validMask & (1u << index)) == 0u)
            continue;
        float x = (float)initial.points[index].x * scale;
        float y = (float)initial.points[index].y * scale;
        if (!XyBound(x) || !XyBound(y))
            return false;
        result->phantoms[index] = (TrueTypePixelMetricPoint){x, y};
    }
    return true;
}

static bool XyLeaf(const TrueTypeFont *font, const TrueTypePlanNode *node,
                   const TrueTypeXYFrame *frame, TrueTypeXYWorkspace *workspace,
                   TrueTypeXYGlyph *result)
{
    TrueTypeSpan span = {0};
    TrueTypeOutlineCounts counts = {0};
    TrueTypeOutlineStorage storage = {workspace->designPoints, workspace->designEnds,
                                      TRUE_TYPE_OUTLINE_MAX_POINTS, TRUE_TYPE_OUTLINE_MAX_CONTOURS};
    if (!TrueTypeReaderGlyphSpan(font, node->glyphIndex, &span) ||
        !TrueTypeSimpleOutlineRead(span, storage, &counts) ||
        counts.pointCount != node->pointCount || counts.contourCount != node->contourCount)
        return false;
    for (uint32_t index = 0u; index < counts.pointCount; ++index)
    {
        TrueTypeDesignPoint point = workspace->designPoints[index];
        float x = frame->m00 * (float)point.x + frame->m01 * (float)point.y + frame->tx;
        float y = frame->m10 * (float)point.x + frame->m11 * (float)point.y + frame->ty;
        if (!XyBound(x) || !XyBound(y))
            return false;
        uint32_t target = node->pointBase + index;
        workspace->points[target] = (TrueTypePixelPoint){x, y, point.onCurve};
        if (target == 0u)
        {
            result->xMin = result->xMax = x;
            result->yMin = result->yMax = y;
        }
        else
        {
            if (x < result->xMin)
                result->xMin = x;
            if (x > result->xMax)
                result->xMax = x;
            if (y < result->yMin)
                result->yMin = y;
            if (y > result->yMax)
                result->yMax = y;
        }
    }
    for (uint32_t index = 0u; index < counts.contourCount; ++index)
        workspace->contourEnds[node->contourBase + index] =
            (uint16_t)(node->pointBase + workspace->designEnds[index]);
    return true;
}

bool TrueTypeGlyphResolveXY(const TrueTypeFont *font, uint16_t glyph, float pixelsPerUnit,
                            TrueTypeXYWorkspace *workspace, TrueTypeXYStorage storage,
                            TrueTypeXYGlyph *out)
{
    if (font == NULL || workspace == NULL || out == NULL ||
        !(pixelsPerUnit > 0.0f && pixelsPerUnit <= (float)TRUE_TYPE_XY_MAX_PIXELS_PER_UNIT))
        return false;
    TrueTypePlanSummary summary = {0};
    if (!TrueTypeGlyphPlanBuild(font, glyph, &workspace->plan, &summary) ||
        summary.pointCount > storage.pointCapacity ||
        summary.contourCount > storage.contourCapacity ||
        (summary.pointCount != 0u && storage.points == NULL) ||
        (summary.contourCount != 0u && storage.contourEnds == NULL))
        return false;
    for (uint32_t index = 0u; index < summary.componentCount; ++index)
    {
        uint16_t flags = workspace->plan.components[index].flags;
        if ((flags & TRUE_TYPE_COMPONENT_XY_ARGUMENTS) == 0u ||
            (flags & TRUE_TYPE_COMPONENT_USE_METRICS) != 0u)
            return false;
    }
    TrueTypeXYGlyph result = {.pointCount = summary.pointCount,
                              .contourCount = summary.contourCount};
    if (!XyPhantoms(font, glyph, summary.contourCount, pixelsPerUnit, &result))
        return false;
    workspace->frames[0] = (TrueTypeXYFrame){.m00 = pixelsPerUnit, .m11 = pixelsPerUnit};
    uint32_t frameCount = 1u;
    while (frameCount != 0u)
    {
        TrueTypeXYFrame *frame = &workspace->frames[frameCount - 1u];
        const TrueTypePlanNode *node = &workspace->plan.nodes[frame->nodeIndex];
        if (node->childCount == 0u)
        {
            if (!XyLeaf(font, node, frame, workspace, &result))
                return false;
            --frameCount;
        }
        else if (frame->nextChild == node->childCount)
            --frameCount;
        else
        {
            if (frameCount == TRUE_TYPE_PLAN_MAX_DEPTH)
                return false;
            uint16_t childIndex = (uint16_t)(node->firstChild + frame->nextChild++);
            const TrueTypePlanNode *child = &workspace->plan.nodes[childIndex];
            if (!XyChild(frame, &workspace->plan.components[child->componentIndex], childIndex,
                         &workspace->frames[frameCount]))
                return false;
            ++frameCount;
        }
    }
    for (uint32_t index = 0u; index < summary.pointCount; ++index)
        storage.points[index] = workspace->points[index];
    for (uint32_t index = 0u; index < summary.contourCount; ++index)
        storage.contourEnds[index] = workspace->contourEnds[index];
    *out = result;
    return true;
}
