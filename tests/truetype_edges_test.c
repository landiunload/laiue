#include "ui/truetype_edges.h"
#include "test_runtime.h"

static TrueTypeEdgeWorkspace workspace;
static TrueTypeEdgeSummary summary;
static uint32_t failures;

typedef struct ReferencePoint
{
    double x;
    double y;
} ReferencePoint;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failures;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static bool Build(const TrueTypePixelPoint *points, uint32_t pointCount, const uint16_t *ends,
                  uint32_t contourCount)
{
    bool success = TrueTypeEdgesBuild(points, pointCount, ends, contourCount, &workspace, &summary);
    Expect(success, "ordinary contour conversion succeeds");
    return success;
}

static void Edge(uint32_t index, float x0, float y0, float x1, float y1)
{
    if (index >= summary.edgeCount)
    {
        Expect(false, "expected directed edge exists");
        return;
    }
    TrueTypeEdge edge = workspace.edges[index];
    Expect(edge.x0 == x0 && edge.y0 == y0 && edge.x1 == x1 && edge.y1 == y1,
           "directed edge coordinates and source order match");
}

static void Closed(void)
{
    Expect(summary.edgeCount != 0u, "nondegenerate contour has edges");
    for (uint32_t index = 0u; index < summary.edgeCount; ++index)
    {
        TrueTypeEdge first = workspace.edges[index];
        TrueTypeEdge second = workspace.edges[(index + 1u) % summary.edgeCount];
        Expect(first.x1 == second.x0 && first.y1 == second.y0,
               "consecutive edges share the same rounded endpoint including closure");
    }
}

// Independent geometric reference: sample the polynomial and measure distance
// to the resulting segments. This does not repeat the converter's flatness test.
static void CurveNearEdges(ReferencePoint first, ReferencePoint control, ReferencePoint last)
{
    for (uint32_t sample = 0u; sample <= 1024u; ++sample)
    {
        double t = (double)sample / 1024.0;
        double s = 1.0 - t;
        double x = s * s * first.x + 2.0 * s * t * control.x + t * t * last.x;
        double y = s * s * first.y + 2.0 * s * t * control.y + t * t * last.y;
        double nearest = 1.0e30;
        for (uint32_t index = 0u; index < summary.edgeCount; ++index)
        {
            TrueTypeEdge edge = workspace.edges[index];
            double dx = (double)edge.x1 - (double)edge.x0;
            double dy = (double)edge.y1 - (double)edge.y0;
            double length = dx * dx + dy * dy;
            double u = ((x - (double)edge.x0) * dx + (y - (double)edge.y0) * dy) / length;
            if (u < 0.0)
                u = 0.0;
            if (u > 1.0)
                u = 1.0;
            double ex = x - ((double)edge.x0 + u * dx);
            double ey = y - ((double)edge.y0 + u * dy);
            double distance = ex * ex + ey * ey;
            if (distance < nearest)
                nearest = distance;
        }
        Expect(nearest <= 1.0 / 64.0, "sampled quadratic lies within 1/8 pixel of edges");
    }
}

static void TestLinesAndOrientation(void)
{
    Expect(Build(NULL, 0u, NULL, 0u) && summary.edgeCount == 0u, "empty outline has no edges");
    const TrueTypePixelPoint one[] = {{2.0f, -3.0f, true}};
    const uint16_t oneEnd[] = {0u};
    Expect(Build(one, 1u, oneEnd, 1u) && summary.edgeCount == 0u,
           "a single on-curve point contributes no directed edge");
    const TrueTypePixelPoint off[] = {{2.0f, -3.0f, false}};
    Expect(Build(off, 1u, oneEnd, 1u) && summary.edgeCount == 0u,
           "a single off-curve point closes degenerately");

    const TrueTypePixelPoint repeated[] = {
        {0.0f, 0.0f, true}, {0.0f, 0.0f, true}, {2.0f, 0.0f, true}, {2.0f, 0.0f, true}};
    const uint16_t repeatedEnd[] = {3u};
    if (Build(repeated, 4u, repeatedEnd, 1u))
    {
        Expect(summary.edgeCount == 2u, "exact repeated line points are skipped");
        Edge(0u, 0.0f, 0.0f, 2.0f, 0.0f);
        Edge(1u, 2.0f, 0.0f, 0.0f, 0.0f);
        Closed();
    }

    const TrueTypePixelPoint rings[] = {{0.0f, 0.0f, true}, {4.0f, 0.0f, true}, {4.0f, 4.0f, true},
                                        {0.0f, 4.0f, true}, {1.0f, 1.0f, true}, {1.0f, 3.0f, true},
                                        {3.0f, 3.0f, true}, {3.0f, 1.0f, true}};
    const uint16_t ringEnds[] = {3u, 7u};
    if (!Build(rings, 8u, ringEnds, 2u))
        return;
    Expect(summary.edgeCount == 8u, "outer and inner contours each retain four edges");
    Edge(0u, 0.0f, 0.0f, 4.0f, 0.0f);
    Edge(3u, 0.0f, 4.0f, 0.0f, 0.0f);
    Edge(4u, 1.0f, 1.0f, 1.0f, 3.0f);
    Edge(7u, 3.0f, 1.0f, 1.0f, 1.0f);
    double areas[2] = {0};
    for (uint32_t index = 0u; index < summary.edgeCount; ++index)
    {
        TrueTypeEdge edge = workspace.edges[index];
        areas[index / 4u] += (double)edge.x0 * edge.y1 - (double)edge.x1 * edge.y0;
    }
    Expect(areas[0] == 32.0 && areas[1] == -8.0,
           "source winding for the outer ring and hole is preserved");
}

static void TestQuadraticsAndStarts(void)
{
    const uint16_t end[] = {2u};
    const TrueTypePixelPoint firstOn[] = {
        {0.0f, 0.0f, true}, {1.0f, 2.0f, false}, {2.0f, 0.0f, true}};
    const TrueTypePixelPoint lastOn[] = {
        {1.0f, 2.0f, false}, {2.0f, 0.0f, true}, {0.0f, 0.0f, true}};
    for (uint32_t order = 0u; order < 2u; ++order)
    {
        if (!Build(order == 0u ? firstOn : lastOn, 3u, end, 1u))
            return;
        Expect(summary.edgeCount == 5u, "analytic quadratic has four chords and a closing line");
        Edge(0u, 0.0f, 0.0f, 0.5f, 0.75f);
        Edge(1u, 0.5f, 0.75f, 1.0f, 1.0f);
        Edge(2u, 1.0f, 1.0f, 1.5f, 0.75f);
        Edge(3u, 1.5f, 0.75f, 2.0f, 0.0f);
        Edge(4u, 2.0f, 0.0f, 0.0f, 0.0f);
        Closed();
        CurveNearEdges((ReferencePoint){0.0, 0.0}, (ReferencePoint){1.0, 2.0},
                       (ReferencePoint){2.0, 0.0});
    }

    const TrueTypePixelPoint closingOff[] = {
        {0.0f, 0.0f, true}, {1.0f, 2.0f, false}, {2.0f, 0.0f, true}, {1.0f, -2.0f, false}};
    const uint16_t fourEnd[] = {3u};
    if (Build(closingOff, 4u, fourEnd, 1u))
    {
        Expect(summary.edgeCount == 8u, "last off-curve control closes a second quadratic");
        Edge(4u, 2.0f, 0.0f, 1.5f, -0.75f);
        Edge(7u, 0.5f, -0.75f, 0.0f, 0.0f);
        Closed();
    }

    const TrueTypePixelPoint endsOff[] = {
        {0.0f, 2.0f, false}, {2.0f, 2.0f, true}, {2.0f, 0.0f, false}};
    if (Build(endsOff, 3u, end, 1u))
    {
        Expect(workspace.edges[0].x0 == 1.0f && workspace.edges[0].y0 == 1.0f,
               "off-curve endpoints imply the starting midpoint");
        Closed();
        CurveNearEdges((ReferencePoint){1.0, 1.0}, (ReferencePoint){0.0, 2.0},
                       (ReferencePoint){2.0, 2.0});
        CurveNearEdges((ReferencePoint){2.0, 2.0}, (ReferencePoint){2.0, 0.0},
                       (ReferencePoint){1.0, 1.0});
    }

    const TrueTypePixelPoint allOff[] = {
        {0.0f, 0.0f, false}, {2.0f, 0.0f, false}, {2.0f, 2.0f, false}, {0.0f, 2.0f, false}};
    if (Build(allOff, 4u, fourEnd, 1u))
    {
        Expect(summary.edgeCount == 16u, "four implied endpoints each receive four curve chords");
        Expect(workspace.edges[0].x0 == 0.0f && workspace.edges[0].y0 == 1.0f,
               "all-off contour begins between last and first controls");
        Edge(3u, 0.5625f, 0.0625f, 1.0f, 0.0f);
        Closed();
        CurveNearEdges((ReferencePoint){0.0, 1.0}, (ReferencePoint){0.0, 0.0},
                       (ReferencePoint){1.0, 0.0});
        CurveNearEdges((ReferencePoint){1.0, 0.0}, (ReferencePoint){2.0, 0.0},
                       (ReferencePoint){2.0, 1.0});
        CurveNearEdges((ReferencePoint){2.0, 1.0}, (ReferencePoint){2.0, 2.0},
                       (ReferencePoint){1.0, 2.0});
        CurveNearEdges((ReferencePoint){1.0, 2.0}, (ReferencePoint){0.0, 2.0},
                       (ReferencePoint){0.0, 1.0});
    }
}

static void TestFlatnessAndCoordinates(void)
{
    const TrueTypePixelPoint zeroChord[] = {{0.0f, 0.0f, true}, {0.0f, 2.0f, false}};
    const uint16_t twoEnd[] = {1u};
    if (Build(zeroChord, 2u, twoEnd, 1u))
    {
        Expect(summary.edgeCount == 4u, "zero chord length does not erase a nonconstant quadratic");
        Edge(0u, 0.0f, 0.0f, 0.0f, 0.75f);
        Edge(1u, 0.0f, 0.75f, 0.0f, 1.0f);
        Edge(2u, 0.0f, 1.0f, 0.0f, 0.75f);
        Edge(3u, 0.0f, 0.75f, 0.0f, 0.0f);
        Closed();
    }
    TrueTypePixelPoint flat[] = {{-1.0f, 0.0f, true}, {0.0f, 0.125f, false}, {1.0f, 0.0f, true}};
    const uint16_t threeEnd[] = {2u};
    Expect(Build(flat, 3u, threeEnd, 1u) && summary.edgeCount == 2u,
           "a quadratic exactly at the chord threshold is accepted");
    flat[1].y = 0.126f;
    Expect(Build(flat, 3u, threeEnd, 1u) && summary.edgeCount == 3u,
           "a quadratic above the chord threshold subdivides");

    const TrueTypePixelPoint translated[] = {{1048574.0f, 1048574.0f, true},
                                             {1048575.0f, 1048576.0f, false},
                                             {1048576.0f, 1048574.0f, true}};
    if (Build(translated, 3u, threeEnd, 1u))
    {
        Expect(summary.edgeCount == 5u,
               "large translated analytic curve retains its segment count");
        Closed();
        CurveNearEdges((ReferencePoint){1048574.0, 1048574.0},
                       (ReferencePoint){1048575.0, 1048576.0},
                       (ReferencePoint){1048576.0, 1048574.0});
    }
    const TrueTypePixelPoint rounded[] = {{1048574.0f, 1048574.0f, true},
                                          {1048575.0625f, 1048575.9375f, false},
                                          {1048576.0f, 1048574.0f, true}};
    if (Build(rounded, 3u, threeEnd, 1u))
    {
        Expect(summary.edgeCount == 5u, "large fractional control still fits four curve chords");
        Edge(0u, 1048574.0f, 1048574.0f, 1048574.5f, 1048574.75f);
        Edge(1u, 1048574.5f, 1048574.75f, 1048575.0f, 1048575.0f);
        // Exact Bx(.25)=1048574.5234375 rounds to the asserted float endpoint.
        Closed();
        CurveNearEdges((ReferencePoint){1048574.0, 1048574.0},
                       (ReferencePoint){1048575.0625, 1048575.9375},
                       (ReferencePoint){1048576.0, 1048574.0});
    }
}

static void Rejected(const TrueTypePixelPoint *points, uint32_t pointCount, const uint16_t *ends,
                     uint32_t contourCount)
{
    TrueTypeEdgeSummary out = {123u};
    Expect(!TrueTypeEdgesBuild(points, pointCount, ends, contourCount, &workspace, &out) &&
               out.edgeCount == 123u,
           "conversion refusal preserves the scalar summary");
}

static void TestExplicitCaps(void)
{
    const TrueTypePixelPoint point[] = {{1.0f, 1.0f, true}};
    const uint16_t end[] = {0u};
    Rejected(point, 1u, end, 0u);
    Rejected(NULL, 1u, end, 1u);
    Rejected(point, 1u, NULL, 1u);
    const TrueTypePixelPoint two[] = {{0.0f, 0.0f, true}, {1.0f, 1.0f, true}};
    Rejected(two, 2u, end, 1u);
    const float magnitude = (float)TRUE_TYPE_XY_MAX_MAGNITUDE;
    const TrueTypePixelPoint deep[] = {{-magnitude, -magnitude, true},
                                       {magnitude, magnitude, false}};
    const uint16_t twoEnd[] = {1u};
    Rejected(deep, 2u, twoEnd, 1u);

    TrueTypePixelPoint curves[34];
    uint16_t ends[17];
    for (uint32_t index = 0u; index < 17u; ++index)
    {
        size_t pointIndex = (size_t)index * 2u;
        curves[pointIndex] = (TrueTypePixelPoint){-magnitude, 0.0f, true};
        curves[pointIndex + 1u] = (TrueTypePixelPoint){magnitude, 0.0f, false};
        ends[index] = (uint16_t)(pointIndex + 1u);
    }
    Expect(Build(curves, 32u, ends, 16u) && summary.edgeCount == TRUE_TYPE_EDGE_MAX_COUNT,
           "the edge capacity is inclusive when sixteen long curves fit exactly");
    Rejected(curves, 34u, ends, 17u);
}

static void WriteSize(size_t value)
{
    char text[32];
    size_t count = 0u;
    do
    {
        text[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (size_t index = 0u; index < count / 2u; ++index)
    {
        char swap = text[index];
        text[index] = text[count - index - 1u];
        text[count - index - 1u] = swap;
    }
    text[count] = '\0';
    LaiueTestRuntimeWrite(text);
}

LAIUE_TEST_ENTRY(TrueTypeEdgesTestEntryPoint)
{
    TestLinesAndOrientation();
    TestQuadraticsAndStarts();
    TestFlatnessAndCoordinates();
    TestExplicitCaps();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType edges OK; workspace=");
    WriteSize(sizeof(TrueTypeEdgeWorkspace));
    LaiueTestRuntimeWrite(" segment=");
    WriteSize(sizeof(TrueTypeEdgeQuadratic));
    LaiueTestRuntimeWrite(" edge=");
    WriteSize(sizeof(TrueTypeEdge));
    LaiueTestRuntimeWrite(" result=");
    WriteSize(sizeof(TrueTypeEdgeSummary));
    LaiueTestRuntimeWrite("\n");
    LAIUE_TEST_SUCCESS();
}
