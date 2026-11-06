#pragma once

#include "truetype_xy.h"

enum
{
    TRUE_TYPE_EDGE_MAX_COUNT = 65536,
    TRUE_TYPE_EDGE_MAX_DEPTH = 12,
};

typedef struct TrueTypeEdge
{
    float x0;
    float y0;
    float x1;
    float y1;
} TrueTypeEdge;

typedef struct TrueTypeEdgeQuadratic
{
    double x0;
    double y0;
    double x1;
    double y1;
    double x2;
    double y2;
    uint32_t depth;
} TrueTypeEdgeQuadratic;

typedef struct TrueTypeEdgeWorkspace
{
    TrueTypeEdge edges[TRUE_TYPE_EDGE_MAX_COUNT];
    TrueTypeEdgeQuadratic segments[TRUE_TYPE_EDGE_MAX_DEPTH + 1u];
} TrueTypeEdgeWorkspace;

typedef struct TrueTypeEdgeSummary
{
    uint32_t edgeCount;
} TrueTypeEdgeSummary;

// Private directed edges from original on/off points in pixels/Y-up. Contours
// retain their order and orientation; implied points never change input indices.
// Double De Casteljau subdivision has a 1/16-pixel exact-arithmetic chord
// threshold; output endpoints are rounded to float. No raster or ink bounds.
// Precise binary32/binary64 FP with default round-to-nearest is required.
// Input counts obey outline limits; ends increase and finish at pointCount-1.
// Every coordinate is finite with abs <= 2^20. Both zero counts allow NULL input.
// Workspace is caller-owned heap/static scratch. Inputs/workspace/out do not
// overlap. Every call invalidates previous edges; read edges[0..edgeCount) only
// after success and before the next call. False preserves scalar out; scratch
// is invalid. Caps return false instead of truncating unfinished geometry.
// No allocation, I/O, libm, CRT or C recursion.
bool TrueTypeEdgesBuild(const TrueTypePixelPoint *points, uint32_t pointCount,
                        const uint16_t *contourEnds, uint32_t contourCount,
                        TrueTypeEdgeWorkspace *workspace, TrueTypeEdgeSummary *out);
