#include "ui/truetype_coverage.h"
#include "test_runtime.h"

#include <float.h>

static uint32_t failures;
static TrueTypeCoverageEvent events[TRUE_TYPE_EDGE_MAX_COUNT];
static double coverages[UINT16_MAX];
static uint8_t row[UINT16_MAX];
static TrueTypeEdgeWorkspace edgeWorkspace;
static TrueTypeEdge manyEdges[TRUE_TYPE_EDGE_MAX_COUNT];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failures;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static TrueTypeCoverageStorage Storage(void)
{
    return (TrueTypeCoverageStorage){
        events, TRUE_TYPE_EDGE_MAX_COUNT, coverages, UINT16_MAX, row, UINT16_MAX};
}

static void RectangleFixture(TrueTypeEdge edges[4], float left, float bottom, float right,
                             float top, bool reversed)
{
    edges[0] = (TrueTypeEdge){left, bottom, right, bottom};
    edges[1] = (TrueTypeEdge){right, bottom, right, top};
    edges[2] = (TrueTypeEdge){right, top, left, top};
    edges[3] = (TrueTypeEdge){left, top, left, bottom};
    if (reversed)
    {
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            const TrueTypeEdge edge = edges[index];
            edges[index] = (TrueTypeEdge){edge.x1, edge.y1, edge.x0, edge.y0};
        }
    }
}

static void ExpectRow(const TrueTypeEdge *edges, uint32_t edgeCount, double left, double top,
                      const uint8_t *expected, uint32_t width, const char *message)
{
    uint32_t bytes = UINT32_MAX;
    const bool success = TrueTypeCoverageRow(edges, edgeCount, left, top, width, Storage(), &bytes);
    Expect(success && bytes == width, message);
    if (success && bytes == width)
    {
        for (uint32_t index = 0u; index < width; ++index)
            Expect(row[index] == expected[index], message);
    }
}

static void TestAnalyticCoverage(void)
{
    TrueTypeEdge rectangle[4];
    RectangleFixture(rectangle, 0.25f, 0.0f, 2.75f, 1.0f, false);
    const uint8_t quarterPixels[] = {191u, 255u, 191u, 0u};
    ExpectRow(rectangle, 4u, 0.0, 1.0, quarterPixels, 4u,
              "horizontal coverage includes fractional pixels");
    const uint8_t shiftedTop[] = {143u, 191u, 143u};
    ExpectRow(rectangle, 4u, 0.0, 0.75, shiftedTop, 3u,
              "fractional top selects six centered samples");

    RectangleFixture(rectangle, -1.25f, -1.25f, 1.25f, -0.75f, false);
    const uint8_t negativeOrigin[] = {96u, 128u, 96u};
    ExpectRow(rectangle, 4u, -1.5, -0.5, negativeOrigin, 3u,
              "negative fractional origins preserve coverage");

    RectangleFixture(rectangle, -10.0f, 0.0f, 10.0f, 1.0f, true);
    const uint8_t clipped[] = {255u, 255u, 255u};
    ExpectRow(rectangle, 4u, -0.75, 1.0, clipped, 3u,
              "an enclosing reversed contour covers the clipped row");

    // Возрастающая длина пересечения треугольника: 0.25, 0.75, ..., 3.75.
    const TrueTypeEdge triangle[] = {
        {4.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}};
    const uint8_t slopes[] = {223u, 159u, 96u, 32u};
    ExpectRow(triangle, 3u, 0.0, 1.0, slopes, 4u, "sloped edges use analytical horizontal lengths");

    const float limit = (float)TRUE_TYPE_XY_MAX_MAGNITUDE;
    RectangleFixture(rectangle, limit - 0.25f, 0.0f, limit, 1.0f, false);
    const uint8_t nearLimit[] = {32u, 0u};
    ExpectRow(rectangle, 4u, (double)limit - 0.125, 1.0, nearLimit, 2u,
              "large X coordinates preserve a fractional pixel");
}

static void TestWinding(void)
{
    TrueTypeEdge contours[8];
    RectangleFixture(contours, 0.0f, 0.0f, 4.0f, 1.0f, false);
    RectangleFixture(contours + 4u, 1.0f, 0.0f, 3.0f, 1.0f, true);
    const uint8_t hole[] = {255u, 0u, 0u, 255u};
    ExpectRow(contours, 8u, 0.0, 1.0, hole, 4u, "opposite winding cuts a hole");

    RectangleFixture(contours + 4u, 1.0f, 0.0f, 3.0f, 1.0f, false);
    const uint8_t full[] = {255u, 255u, 255u, 255u};
    ExpectRow(contours, 8u, 0.0, 1.0, full, 4u,
              "overlapping same-direction contours do not use even-odd");

    RectangleFixture(contours, 0.0f, 0.0f, 2.0f, 1.0f, false);
    RectangleFixture(contours + 4u, 2.0f, 0.0f, 4.0f, 1.0f, true);
    ExpectRow(contours, 8u, 0.0, 1.0, full, 4u,
              "simultaneous crossings can change winding sign without a gap");

    const TrueTypeEdge oppositeTriangles[] = {{0.0f, 0.0f, 3.0f, 1.0f}, {3.0f, 1.0f, 5.0f, 0.0f},
                                              {5.0f, 0.0f, 0.0f, 0.0f}, {3.0f, 1.0f, 0.0f, 0.0f},
                                              {0.0f, 0.0f, 5.0f, 0.0f}, {5.0f, 0.0f, 3.0f, 1.0f}};
    const uint8_t none[] = {0u, 0u, 0u, 0u};
    ExpectRow(oppositeTriangles, 6u, 0.25, 0.96, none, 4u,
              "coincident reversed edges use identical intersection coordinates");

    RectangleFixture(contours, 0.0f, 0.0625f, 1.0f, 0.125f, false);
    RectangleFixture(contours + 4u, 1.0f, 0.0f, 2.0f, 0.0625f, false);
    const uint8_t halfOpen[] = {32u, 0u};
    ExpectRow(contours, 8u, 0.0, 1.0, halfOpen, 2u,
              "a sample at an edge minimum is included, at its maximum excluded");
}

static void TestLimits(void)
{
    TrueTypeEdge rectangle[4];
    RectangleFixture(rectangle, 0.0f, 0.0f, 4.0f, 1.0f, false);
    TrueTypeCoverageStorage storage = Storage();
    uint32_t written = 91u;
    storage.eventCapacity = 1u;
    Expect(!TrueTypeCoverageRow(rectangle, 4u, 0.0, 1.0, 4u, storage, &written) && written == 91u,
           "crossing capacity exhaustion preserves the published byte count");
    storage = Storage();
    storage.coverageCapacity = 3u;
    Expect(!TrueTypeCoverageRow(rectangle, 4u, 0.0, 1.0, 4u, storage, &written) && written == 91u,
           "short coverage storage fails before publishing");
    storage = Storage();
    storage.rowCapacity = 3u;
    Expect(!TrueTypeCoverageRow(rectangle, 4u, 0.0, 1.0, 4u, storage, &written) && written == 91u,
           "short row storage fails before publishing");
    storage = Storage();
    Expect(!TrueTypeCoverageRow(rectangle + 1u, 1u, 0.0, 1.0, 4u, storage, &written) &&
               written == 91u,
           "unbalanced winding is rejected even at the clip boundary");
    Expect(!TrueTypeCoverageRow(rectangle, TRUE_TYPE_EDGE_MAX_COUNT + 1u, 0.0, 1.0, 4u, storage,
                                &written) &&
               written == 91u,
           "out-of-contract counts are rejected before reading the input");
    Expect(!TrueTypeCoverageRow(rectangle, 4u, 0.0, 1.0, (uint32_t)UINT16_MAX + 1u, storage,
                                &written) &&
               written == 91u,
           "row width cannot exceed UiGlyph");
    Expect(!TrueTypeCoverageRow(rectangle, 4u, -DBL_MAX, 1.0, 4u, storage, &written) &&
               written == 91u,
           "row origin is bounded");
    rectangle[0].x0 = FLT_MAX;
    Expect(!TrueTypeCoverageRow(rectangle, 4u, 0.0, 1.0, 4u, storage, &written) && written == 91u,
           "invalid endpoints are rejected even for horizontal edges");
    Expect(TrueTypeCoverageRow(NULL, 0u, 0.0, 1.0, 0u, (TrueTypeCoverageStorage){0}, &written) &&
               written == 0u,
           "zero width needs no workspace");

    storage.events = NULL;
    storage.eventCapacity = 0u;
    row[2] = 91u;
    Expect(TrueTypeCoverageRow(NULL, 0u, 0.0, 1.0, 2u, storage, &written) && written == 2u &&
               row[0] == 0u && row[1] == 0u && row[2] == 91u,
           "blank rows need no events and write exactly their width");

    storage = Storage();
    for (uint32_t index = 0u; index < TRUE_TYPE_EDGE_MAX_COUNT / 2u; ++index)
    {
        manyEdges[index * 2u] = (TrueTypeEdge){1.0f, 0.0f, 1.0f, 1.0f};
        manyEdges[index * 2u + 1u] = (TrueTypeEdge){0.0f, 1.0f, 0.0f, 0.0f};
    }
    Expect(TrueTypeCoverageRow(manyEdges, TRUE_TYPE_EDGE_MAX_COUNT, 0.0, 1.0, UINT16_MAX, storage,
                               &written) &&
               written == UINT16_MAX && row[0] == 255u && row[1] == 0u &&
               row[UINT16_MAX - 1u] == 0u,
           "maximal counts keep winding and every row index representable");
}

static void TestEdgeProducer(void)
{
    const TrueTypePixelPoint points[] = {
        {0.0f, 0.0f, true}, {2.0f, 2.0f, false}, {4.0f, 0.0f, true}};
    const uint16_t ends[] = {2u};
    TrueTypeEdgeSummary edges = {0};
    if (!TrueTypeEdgesBuild(points, 3u, ends, 1u, &edgeWorkspace, &edges))
    {
        Expect(false, "quadratic contour reaches the coverage stage");
        return;
    }
    uint32_t bytes = 0u;
    Expect(TrueTypeCoverageRow(edgeWorkspace.edges, edges.edgeCount, 0.0, 1.0, 4u, Storage(),
                               &bytes) &&
               bytes == 4u && row[0] > 0u && row[0] < row[1] && row[0] == row[3] &&
               row[1] == row[2],
           "a flattened quadratic arch produces symmetric coverage");
}

LAIUE_TEST_ENTRY(TrueTypeCoverageTestEntryPoint)
{
    TestAnalyticCoverage();
    TestWinding();
    TestLimits();
    TestEdgeProducer();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType row coverage OK\n");
    LAIUE_TEST_SUCCESS();
}
