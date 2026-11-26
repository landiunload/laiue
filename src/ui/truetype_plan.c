#include "truetype_plan.h"

// Порядок component records и point numbering: Microsoft OpenType glyf.
// https://learn.microsoft.com/typography/opentype/spec/glyf
static TrueTypePlanNode TtpNode(uint16_t glyph, uint16_t parent, uint16_t component, uint8_t depth)
{
    return (TrueTypePlanNode){.glyphIndex = glyph,
                              .parentIndex = parent,
                              .firstChild = TRUE_TYPE_PLAN_NO_INDEX,
                              .componentIndex = component,
                              .depth = depth};
}

static bool TtpContainsAncestor(const TrueTypePlanWorkspace *workspace, uint32_t node,
                                uint16_t glyph)
{
    while (node != TRUE_TYPE_PLAN_NO_INDEX)
    {
        if (workspace->nodes[node].glyphIndex == glyph)
            return true;
        node = workspace->nodes[node].parentIndex;
    }
    return false;
}

static bool TtpAppend(const TrueTypeFont *font, TrueTypePlanWorkspace *workspace, uint32_t index,
                      TrueTypeSpan span, TrueTypePlanSummary *summary)
{
    TrueTypePlanNode *node = &workspace->nodes[index];
    if (node->depth == TRUE_TYPE_PLAN_MAX_DEPTH)
        return false;
    uint32_t firstComponent = summary->componentCount;
    TrueTypeCompositeInfo info = {0};
    TrueTypeComponentStorage storage = {workspace->components + firstComponent,
                                        TRUE_TYPE_PLAN_MAX_COMPONENTS - firstComponent};
    if (!TrueTypeCompositeRead(span, storage, &info) ||
        info.componentCount > TRUE_TYPE_PLAN_MAX_NODES - summary->nodeCount)
        return false;
    node->firstChild = (uint16_t)summary->nodeCount;
    node->childCount = (uint16_t)info.componentCount;
    for (uint32_t offset = 0u; offset < info.componentCount; ++offset)
    {
        uint32_t component = firstComponent + offset;
        uint16_t glyph = workspace->components[component].glyphIndex;
        if (glyph >= font->numGlyphs || TtpContainsAncestor(workspace, index, glyph))
            return false;
        workspace->nodes[summary->nodeCount++] =
            TtpNode(glyph, (uint16_t)index, (uint16_t)component, (uint8_t)(node->depth + 1u));
    }
    summary->componentCount += info.componentCount;
    if (summary->maxDepth < (uint32_t)node->depth + 1u)
        summary->maxDepth = (uint32_t)node->depth + 1u;
    return true;
}

static bool TtpReference(uint32_t point, uint32_t prefix, uint32_t finalCount)
{
    return point < prefix || (point >= finalCount && point - finalCount < 4u);
}

static bool TtpCount(TrueTypePlanWorkspace *workspace, uint32_t count)
{
    while (count != 0u)
    {
        TrueTypePlanNode *node = &workspace->nodes[--count];
        for (uint32_t offset = 0u; offset < node->childCount; ++offset)
        {
            const TrueTypePlanNode *child = &workspace->nodes[node->firstChild + offset];
            if (child->pointCount > TRUE_TYPE_OUTLINE_MAX_POINTS - node->pointCount ||
                child->contourCount > TRUE_TYPE_OUTLINE_MAX_CONTOURS - node->contourCount)
                return false;
            node->pointCount += child->pointCount;
            node->contourCount += child->contourCount;
        }
        uint32_t prefix = 0u;
        for (uint32_t offset = 0u; offset < node->childCount; ++offset)
        {
            const TrueTypePlanNode *child = &workspace->nodes[node->firstChild + offset];
            const TrueTypeComponent *component = &workspace->components[child->componentIndex];
            if ((component->flags & TRUE_TYPE_COMPONENT_XY_ARGUMENTS) == 0u &&
                (!TtpReference((uint32_t)component->argument1, prefix, node->pointCount) ||
                 !TtpReference((uint32_t)component->argument2, child->pointCount,
                               child->pointCount)))
                return false;
            prefix += child->pointCount;
        }
    }
    return true;
}

static void TtpBases(TrueTypePlanWorkspace *workspace, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        const TrueTypePlanNode *node = &workspace->nodes[index];
        uint32_t point = node->pointBase;
        uint32_t contour = node->contourBase;
        for (uint32_t offset = 0u; offset < node->childCount; ++offset)
        {
            TrueTypePlanNode *child = &workspace->nodes[node->firstChild + offset];
            child->pointBase = point;
            child->contourBase = contour;
            point += child->pointCount;
            contour += child->contourCount;
        }
    }
}

bool TrueTypeGlyphPlanBuild(const TrueTypeFont *font, uint16_t glyph,
                            TrueTypePlanWorkspace *workspace, TrueTypePlanSummary *out)
{
    if (font == NULL || workspace == NULL || out == NULL || glyph >= font->numGlyphs)
        return false;
    TrueTypePlanSummary summary = {.nodeCount = 1u, .maxDepth = 1u};
    workspace->nodes[0] = TtpNode(glyph, TRUE_TYPE_PLAN_NO_INDEX, TRUE_TYPE_PLAN_NO_INDEX, 1u);
    for (uint32_t index = 0u; index < summary.nodeCount; ++index)
    {
        TrueTypePlanNode *node = &workspace->nodes[index];
        TrueTypeSpan span = {0};
        TrueTypeGlyphHeader header = {0};
        if (!TrueTypeReaderGlyphSpan(font, node->glyphIndex, &span) ||
            !TrueTypeReaderGlyphHeader(font, node->glyphIndex, &header))
            return false;
        if (header.contourCount < 0)
        {
            if (!TtpAppend(font, workspace, index, span, &summary))
                return false;
        }
        else
        {
            TrueTypeOutlineCounts counts = {0};
            if (!TrueTypeSimpleOutlineMeasure(span, &counts))
                return false;
            // Сумма leaf occurrences равна counts раскрытого root: не измерять
            // остальные leaves после превышения итогового лимита.
            if (counts.pointCount > TRUE_TYPE_OUTLINE_MAX_POINTS - summary.pointCount ||
                counts.contourCount > TRUE_TYPE_OUTLINE_MAX_CONTOURS - summary.contourCount)
                return false;
            summary.pointCount += counts.pointCount;
            summary.contourCount += counts.contourCount;
            node->pointCount = counts.pointCount;
            node->contourCount = counts.contourCount;
        }
    }
    if (!TtpCount(workspace, summary.nodeCount))
        return false;
    TtpBases(workspace, summary.nodeCount);
    summary.pointCount = workspace->nodes[0].pointCount;
    summary.contourCount = workspace->nodes[0].contourCount;
    *out = summary;
    return true;
}
