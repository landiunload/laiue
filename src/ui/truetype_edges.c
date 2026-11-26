#include "truetype_edges.h"

#include <float.h>

_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG >= 53,
               "TrueType edges need binary32 and binary64 precision");

typedef struct EdgePoint
{
    double x;
    double y;
} EdgePoint;

static EdgePoint EdgeMidpoint(EdgePoint first, EdgePoint second)
{
    return (EdgePoint){(first.x + second.x) * 0.5, (first.y + second.y) * 0.5};
}

static bool EdgeAppend(TrueTypeEdgeWorkspace *workspace, uint32_t *count, EdgePoint first,
                       EdgePoint second)
{
    TrueTypeEdge edge = {(float)first.x, (float)first.y, (float)second.x, (float)second.y};
    if (edge.x0 == edge.x1 && edge.y0 == edge.y1)
        return true;
    if (*count == TRUE_TYPE_EDGE_MAX_COUNT)
        return false;
    workspace->edges[(*count)++] = edge;
    return true;
}

static bool EdgeCurve(TrueTypeEdgeWorkspace *workspace, uint32_t *count, EdgePoint first,
                      EdgePoint control, EdgePoint last)
{
    workspace->segments[0] =
        (TrueTypeEdgeQuadratic){first.x, first.y, control.x, control.y, last.x, last.y, 0u};
    uint32_t pending = 1u;
    while (pending != 0u)
    {
        TrueTypeEdgeQuadratic curve = workspace->segments[--pending];
        double dx = 2.0 * curve.x1 - curve.x0 - curve.x2;
        double dy = 2.0 * curve.y1 - curve.y0 - curve.y2;
        if (dx * dx + dy * dy <= 0.0625)
        {
            if (!EdgeAppend(workspace, count, (EdgePoint){curve.x0, curve.y0},
                            (EdgePoint){curve.x2, curve.y2}))
                return false;
            continue;
        }
        if (curve.depth == TRUE_TYPE_EDGE_MAX_DEPTH || pending + 2u > TRUE_TYPE_EDGE_MAX_DEPTH + 1u)
            return false;
        EdgePoint left =
            EdgeMidpoint((EdgePoint){curve.x0, curve.y0}, (EdgePoint){curve.x1, curve.y1});
        EdgePoint right =
            EdgeMidpoint((EdgePoint){curve.x1, curve.y1}, (EdgePoint){curve.x2, curve.y2});
        EdgePoint middle = EdgeMidpoint(left, right);
        uint32_t depth = curve.depth + 1u;
        workspace->segments[pending++] = (TrueTypeEdgeQuadratic){
            middle.x, middle.y, right.x, right.y, curve.x2, curve.y2, depth};
        workspace->segments[pending++] =
            (TrueTypeEdgeQuadratic){curve.x0, curve.y0, left.x, left.y, middle.x, middle.y, depth};
    }
    return true;
}

static bool EdgeContour(const TrueTypePixelPoint *points, uint32_t first, uint32_t last,
                        TrueTypeEdgeWorkspace *workspace, uint32_t *count)
{
    EdgePoint start = {(double)points[first].x, (double)points[first].y};
    uint32_t index = first;
    uint32_t end = last + 1u;
    if (points[first].onCurve)
        ++index;
    else if (points[last].onCurve)
    {
        start = (EdgePoint){(double)points[last].x, (double)points[last].y};
        end = last;
    }
    else
        start = EdgeMidpoint(start, (EdgePoint){(double)points[last].x, (double)points[last].y});
    EdgePoint current = start;
    EdgePoint control = {0};
    bool pending = false;
    for (; index < end; ++index)
    {
        EdgePoint point = {(double)points[index].x, (double)points[index].y};
        if (points[index].onCurve)
        {
            if (pending ? !EdgeCurve(workspace, count, current, control, point)
                        : !EdgeAppend(workspace, count, current, point))
                return false;
            current = point;
            pending = false;
        }
        else
        {
            if (pending)
            {
                EdgePoint middle = EdgeMidpoint(control, point);
                if (!EdgeCurve(workspace, count, current, control, middle))
                    return false;
                current = middle;
            }
            control = point;
            pending = true;
        }
    }
    return pending ? EdgeCurve(workspace, count, current, control, start)
                   : EdgeAppend(workspace, count, current, start);
}

bool TrueTypeEdgesBuild(const TrueTypePixelPoint *points, uint32_t pointCount,
                        const uint16_t *contourEnds, uint32_t contourCount,
                        TrueTypeEdgeWorkspace *workspace, TrueTypeEdgeSummary *out)
{
    if (workspace == NULL || out == NULL || pointCount > TRUE_TYPE_OUTLINE_MAX_POINTS ||
        contourCount > TRUE_TYPE_OUTLINE_MAX_CONTOURS)
        return false;
    if (pointCount == 0u || contourCount == 0u)
    {
        if (pointCount != 0u || contourCount != 0u)
            return false;
        *out = (TrueTypeEdgeSummary){0};
        return true;
    }
    if (points == NULL || contourEnds == NULL)
        return false;
    uint32_t first = 0u;
    for (uint32_t index = 0u; index < contourCount; ++index)
    {
        uint32_t last = contourEnds[index];
        if (last < first || last >= pointCount)
            return false;
        first = last + 1u;
    }
    if (first != pointCount)
        return false;
    for (uint32_t index = 0u; index < pointCount; ++index)
    {
        float x = points[index].x;
        float y = points[index].y;
        if (!(x >= -(float)TRUE_TYPE_XY_MAX_MAGNITUDE && x <= (float)TRUE_TYPE_XY_MAX_MAGNITUDE &&
              y >= -(float)TRUE_TYPE_XY_MAX_MAGNITUDE && y <= (float)TRUE_TYPE_XY_MAX_MAGNITUDE))
            return false;
    }
    TrueTypeEdgeSummary result = {0};
    first = 0u;
    for (uint32_t index = 0u; index < contourCount; ++index)
    {
        uint32_t last = contourEnds[index];
        if (!EdgeContour(points, first, last, workspace, &result.edgeCount))
            return false;
        first = last + 1u;
    }
    *out = result;
    return true;
}
