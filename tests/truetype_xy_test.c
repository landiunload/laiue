#include "ui/truetype_xy.h"
#include "test_runtime.h"

enum
{
    FONT_CAPACITY = 4096,
    FONT_MAX_GLYPHS = 16,
    HEAD_OFFSET = 160,
    HHEA_OFFSET = 216,
    MAXP_OFFSET = 252,
    CMAP_OFFSET = 284,
    HMTX_OFFSET = 324,
};

static uint8_t fontBytes[FONT_CAPACITY];
static TrueTypeXYWorkspace workspace;
static TrueTypePixelPoint outputPoints[8];
static uint16_t outputEnds[8];
static uint32_t vheaOffset;
static uint32_t vmtxOffset;
static uint32_t locaOffset;
static uint32_t glyfOffset;
static uint32_t failures;
static uint32_t glyphCount;
static uint32_t horizontalMetrics;
static uint32_t verticalMetrics;
static uint32_t nextGlyph;
static uint32_t position;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failures;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static void Put16(uint32_t offset, uint32_t value)
{
    fontBytes[offset] = (uint8_t)(value >> 8u);
    fontBytes[offset + 1u] = (uint8_t)value;
}

static void Put32(uint32_t offset, uint32_t value)
{
    fontBytes[offset] = (uint8_t)(value >> 24u);
    fontBytes[offset + 1u] = (uint8_t)(value >> 16u);
    fontBytes[offset + 2u] = (uint8_t)(value >> 8u);
    fontBytes[offset + 3u] = (uint8_t)value;
}

static void Table(uint32_t index, const char *tag, uint32_t offset, uint32_t size)
{
    uint32_t entry = 12u + index * 16u;
    for (uint32_t i = 0u; i < 4u; ++i)
        fontBytes[entry + i] = (uint8_t)tag[i];
    Put32(entry + 8u, offset);
    Put32(entry + 12u, size);
}

static void Bearing(bool vertical, uint32_t glyph, int16_t value)
{
    uint32_t count = vertical ? verticalMetrics : horizontalMetrics;
    uint32_t offset = vertical ? vmtxOffset : HMTX_OFFSET;
    offset += glyph < count ? glyph * 4u + 2u : count * 4u + (glyph - count) * 2u;
    Put16(offset, (uint16_t)value);
}

static void Metric(bool vertical, uint32_t index, uint16_t advance, int16_t bearing)
{
    Expect(index < (vertical ? verticalMetrics : horizontalMetrics),
           "fixture metric has its own long record");
    Put16((vertical ? vmtxOffset : HMTX_OFFSET) + index * 4u, advance);
    Bearing(vertical, index, bearing);
}

// Собственный SFNT: long loca, cmap12 на glyph0, optional vhea/vmtx.
// Непустой simple имеет один настоящий point и точно соответствующий ему bbox.
static void Begin(uint32_t count, uint32_t hMetrics, uint32_t vMetrics)
{
    Expect(count != 0u && count <= FONT_MAX_GLYPHS && hMetrics != 0u && hMetrics <= count &&
               vMetrics <= count,
           "fixture dimensions fit the reserved table storage");
    for (size_t i = 0u; i < sizeof(fontBytes); ++i)
        fontBytes[i] = 0u;
    glyphCount = count;
    horizontalMetrics = hMetrics;
    verticalMetrics = vMetrics;
    nextGlyph = 0u;
    uint32_t hBytes = hMetrics * 4u + (count - hMetrics) * 2u;
    vheaOffset = (HMTX_OFFSET + hBytes + 3u) & ~3u;
    vmtxOffset = vheaOffset + 36u;
    uint32_t vBytes = vMetrics != 0u ? vMetrics * 4u + (count - vMetrics) * 2u : 0u;
    locaOffset = (vmtxOffset + vBytes + 3u) & ~3u;
    glyfOffset = locaOffset + (count + 1u) * 4u;
    position = glyfOffset;
    Put32(0u, 0x00010000u);
    Put16(4u, vMetrics != 0u ? 9u : 7u);
    Put16(6u, vMetrics != 0u ? 128u : 64u);
    Put16(8u, vMetrics != 0u ? 3u : 2u);
    Put16(10u, vMetrics != 0u ? 16u : 48u);
    Table(0u, "cmap", CMAP_OFFSET, 40u);
    Table(2u, "head", HEAD_OFFSET, 54u);
    Table(3u, "hhea", HHEA_OFFSET, 36u);
    Table(4u, "hmtx", HMTX_OFFSET, hMetrics * 4u + (count - hMetrics) * 2u);
    Table(5u, "loca", locaOffset, (count + 1u) * 4u);
    Table(6u, "maxp", MAXP_OFFSET, 32u);
    Put32(HEAD_OFFSET, 0x00010000u);
    Put32(HEAD_OFFSET + 12u, 0x5F0F3CF5u);
    Put16(HEAD_OFFSET + 18u, 1000u);
    Put16(HEAD_OFFSET + 50u, 1u);
    Put32(HHEA_OFFSET, 0x00010000u);
    Put16(HHEA_OFFSET + 4u, 800u);
    Put16(HHEA_OFFSET + 6u, (uint16_t)-200);
    Put16(HHEA_OFFSET + 8u, 100u);
    Put16(HHEA_OFFSET + 34u, hMetrics);
    Put32(MAXP_OFFSET, 0x00010000u);
    Put16(MAXP_OFFSET + 4u, count);
    Put16(CMAP_OFFSET + 2u, 1u);
    Put16(CMAP_OFFSET + 4u, 3u);
    Put16(CMAP_OFFSET + 6u, 10u);
    Put32(CMAP_OFFSET + 8u, 12u);
    Put16(CMAP_OFFSET + 12u, 12u);
    Put32(CMAP_OFFSET + 16u, 28u);
    Put32(CMAP_OFFSET + 24u, 1u);
    for (uint32_t i = 0u; i < hMetrics; ++i)
        Metric(false, i, 500u, 0);
    if (vMetrics != 0u)
    {
        Table(7u, "vhea", vheaOffset, 36u);
        Table(8u, "vmtx", vmtxOffset, vMetrics * 4u + (count - vMetrics) * 2u);
        Put32(vheaOffset, 0x00010000u);
        Put16(vheaOffset + 34u, vMetrics);
        for (uint32_t i = 0u; i < vMetrics; ++i)
            Metric(true, i, 1000u, 0);
    }
}

static void StartGlyph(void)
{
    Expect(nextGlyph < glyphCount && position + 64u <= FONT_CAPACITY,
           "fixture has room for the next glyph");
    Put32(locaOffset + nextGlyph++ * 4u, position - glyfOffset);
}

static void Header(uint16_t contours, int16_t xMin, int16_t yMin, int16_t xMax, int16_t yMax)
{
    StartGlyph();
    Put16(position, contours);
    Put16(position + 2u, (uint16_t)xMin);
    Put16(position + 4u, (uint16_t)yMin);
    Put16(position + 6u, (uint16_t)xMax);
    Put16(position + 8u, (uint16_t)yMax);
    position += 10u;
}

static void Empty(void)
{
    StartGlyph();
}

static void Point(int16_t x, int16_t y, bool onCurve)
{
    Header(1u, x, y, x, y);
    Put16(position, 0u);                          // endPtsOfContours[0]
    Put16(position + 2u, 0u);                     // instructionLength
    fontBytes[position + 4u] = onCurve ? 1u : 0u; // two signed word deltas
    Put16(position + 5u, (uint16_t)x);
    Put16(position + 7u, (uint16_t)y);
    position = (position + 9u + 3u) & ~3u;
}

static void ZeroContourBox(int16_t xMin, int16_t yMin, int16_t xMax, int16_t yMax)
{
    Header(0u, xMin, yMin, xMax, yMax);
    Put16(position, 0u); // instructionLength
    position += 2u;
}

static void EmptyComposite(void)
{
    Header(UINT16_MAX, 20, 600, 40, 700);
    Put16(position, TRUE_TYPE_COMPONENT_WORD_ARGUMENTS | TRUE_TYPE_COMPONENT_XY_ARGUMENTS);
    Put16(position + 2u, 1u);
    position = (position + 8u + 3u) & ~3u; // component1, zero XY arguments
}

static bool Initialize(TrueTypeFont *font)
{
    Expect(nextGlyph == glyphCount, "fixture emitted every declared glyph");
    Put32(locaOffset + glyphCount * 4u, position - glyfOffset);
    Table(1u, "glyf", glyfOffset, position - glyfOffset);
    bool valid = TrueTypeReaderInitialize(fontBytes, position, font);
    Expect(valid, "functional fixture initializes with the real SFNT reader");
    return valid;
}

static void Line(void)
{
    Header(1u, 0, 0, 8, 16);
    Put16(position, 1u);
    fontBytes[position + 4u] = fontBytes[position + 5u] = 1u;
    Put16(position + 6u, 0u);
    Put16(position + 8u, 8u);
    Put16(position + 10u, 0u);
    Put16(position + 12u, 16u);
    position = (position + 14u + 3u) & ~3u;
}

static TrueTypeComponent Record(uint16_t glyph, int32_t x, int32_t y, uint16_t flags)
{
    return (TrueTypeComponent){.glyphIndex = glyph,
                               .flags = flags | TRUE_TYPE_COMPONENT_WORD_ARGUMENTS |
                                        TRUE_TYPE_COMPONENT_XY_ARGUMENTS,
                               .argument1 = x,
                               .argument2 = y,
                               .xScale = 16384,
                               .yScale = 16384};
}

static void Composite(const TrueTypeComponent *components, uint32_t count, int16_t xMin,
                      int16_t yMin, int16_t xMax, int16_t yMax)
{
    Header(UINT16_MAX, xMin, yMin, xMax, yMax);
    for (uint32_t index = 0u; index < count; ++index)
    {
        TrueTypeComponent record = components[index];
        uint16_t flags = record.flags | TRUE_TYPE_COMPONENT_WORD_ARGUMENTS |
                         (index + 1u < count ? TRUE_TYPE_COMPONENT_MORE : 0u);
        Put16(position, flags);
        Put16(position + 2u, record.glyphIndex);
        Put16(position + 4u, (uint16_t)record.argument1);
        Put16(position + 6u, (uint16_t)record.argument2);
        position += 8u;
        if ((flags & TRUE_TYPE_COMPONENT_SCALE) != 0u)
        {
            Put16(position, (uint16_t)record.xScale);
            position += 2u;
        }
        else if ((flags & TRUE_TYPE_COMPONENT_XY_SCALE) != 0u)
        {
            Put16(position, (uint16_t)record.xScale);
            Put16(position + 2u, (uint16_t)record.yScale);
            position += 4u;
        }
        else if ((flags & TRUE_TYPE_COMPONENT_MATRIX) != 0u)
        {
            Put16(position, (uint16_t)record.xScale);
            Put16(position + 2u, (uint16_t)record.scale01);
            Put16(position + 4u, (uint16_t)record.scale10);
            Put16(position + 6u, (uint16_t)record.yScale);
            position += 8u;
        }
    }
    position = (position + 3u) & ~3u;
}

static TrueTypeXYStorage Storage(void)
{
    return (TrueTypeXYStorage){outputPoints, outputEnds, 8u, 8u};
}

static bool Resolve(float scale, TrueTypeXYGlyph *result)
{
    TrueTypeFont font = {0};
    return Initialize(&font) &&
           TrueTypeGlyphResolveXY(&font, 0u, scale, &workspace, Storage(), result);
}

static bool PointEquals(uint32_t index, float x, float y, bool onCurve)
{
    return outputPoints[index].x == x && outputPoints[index].y == y &&
           outputPoints[index].onCurve == onCurve;
}

static bool PhantomsEqual(const TrueTypeXYGlyph *result, uint32_t mask, float left, float right,
                          float top, float bottom)
{
    return result->phantomMask == mask && result->phantoms[0].x == left &&
           result->phantoms[0].y == 0.0f && result->phantoms[1].x == right &&
           result->phantoms[1].y == 0.0f && result->phantoms[2].x == 0.0f &&
           result->phantoms[2].y == top && result->phantoms[3].x == 0.0f &&
           result->phantoms[3].y == bottom;
}

static void TestSimpleAndEmpty(void)
{
    Begin(1u, 1u, 1u);
    Point(20, 700, true);
    Metric(false, 0u, 512u, 10);
    Metric(true, 0u, 1024u, 100);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 1u && result.contourCount == 1u &&
               PointEquals(0u, 0.625f, 21.875f, true) && outputEnds[0] == 0u &&
               result.phantomMask == 15u && result.phantoms[0].x == 0.3125f &&
               result.phantoms[1].x == 16.3125f && result.phantoms[2].y == 25.0f &&
               result.phantoms[3].y == -7.0f && result.xMin == 0.625f && result.yMax == 21.875f,
           "simple geometry and initial H/V phantoms share pixel scale");
    Begin(1u, 1u, 0u);
    Empty();
    Metric(false, 0u, 400u, 10);
    TrueTypeFont font = {0};
    Expect(Initialize(&font) &&
               TrueTypeGlyphResolveXY(&font, 0u, 1.0f / 32.0f, &workspace, (TrueTypeXYStorage){0},
                                      &result) &&
               result.pointCount == 0u && result.contourCount == 0u && result.xMin == 0.0f &&
               result.yMax == 0.0f && result.phantomMask == 3u &&
               result.phantoms[0].x == -0.3125f && result.phantoms[1].x == 12.1875f &&
               result.phantoms[2].x == 0.0f && result.phantoms[2].y == 0.0f,
           "empty result permits absent arrays and preserves horizontal advance");
    Begin(1u, 1u, 0u);
    ZeroContourBox(20, 600, 40, 700);
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 0u && result.xMax == 0.0f,
           "zero-contour metadata does not invent geometry");
    Begin(2u, 1u, 0u);
    EmptyComposite();
    Empty();
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 0u && result.contourCount == 0u &&
               result.xMin == 0.0f && result.yMin == 0.0f,
           "all-empty composite traverses without publishing bbox as ink");
}

static void TestMatricesAndOffsets(void)
{
    Begin(3u, 1u, 0u);
    TrueTypeComponent outer = Record(1u, 0, 0, TRUE_TYPE_COMPONENT_XY_SCALE);
    outer.xScale = 24576;
    Composite(&outer, 1u, 12, 23, 12, 23);
    TrueTypeComponent inner = Record(2u, 0, 0, TRUE_TYPE_COMPONENT_MATRIX);
    inner.xScale = 8192;
    inner.scale01 = -2048;
    inner.scale10 = 4096;
    inner.yScale = 24576;
    Composite(&inner, 1u, 8, 23, 8, 23);
    Point(8, 16, true);
    TrueTypeXYGlyph nested = {0};
    Expect(Resolve(1.0f / 32.0f, &nested) && PointEquals(0u, 0.375f, 0.71875f, true),
           "nested noncommuting matrices multiply parent basis before child matrix");
    for (uint32_t mode = 0u; mode < 3u; ++mode)
    {
        Begin(2u, 1u, 0u);
        TrueTypeComponent record = Record(1u, 96, -48, TRUE_TYPE_COMPONENT_MATRIX);
        record.xScale = 8192;
        record.scale01 = -2048;
        record.scale10 = 4096;
        record.yScale = 24576;
        if (mode == 1u)
            record.flags |= TRUE_TYPE_COMPONENT_UNSCALED_OFFSET;
        if (mode == 2u)
            record.flags |= TRUE_TYPE_COMPONENT_SCALED_OFFSET;
        int16_t x = mode == 2u ? 44 : 104;
        int16_t y = mode == 2u ? -61 : -25;
        Composite(&record, 1u, x, y, x, y);
        Point(8, 16, true);
        Metric(false, 0u, 512u, x);
        TrueTypeXYGlyph result = {0};
        Expect(Resolve(1.0f / 32.0f, &result) &&
                   PointEquals(0u, mode == 2u ? 1.375f : 3.25f, mode == 2u ? -1.90625f : -0.78125f,
                               true),
               "nonsymmetric matrix and default/explicit offset policy are distinct");
    }
    Begin(2u, 1u, 0u);
    TrueTypeComponent record = Record(1u, 16, -8, TRUE_TYPE_COMPONENT_XY_SCALE);
    record.xScale = -16384;
    record.yScale = 24576;
    Composite(&record, 1u, 8, 16, 8, 16);
    Point(8, 16, false);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(0.5f, &result) && PointEquals(0u, 4.0f, 8.0f, false),
           "reflection and nonuniform scale preserve off-curve flags");
    Begin(2u, 1u, 0u);
    record = Record(1u, 32, -16, TRUE_TYPE_COMPONENT_SCALE);
    record.xScale = record.yScale = 0;
    Composite(&record, 1u, 32, -16, 32, -16);
    Point(123, -25, true);
    Expect(Resolve(1.0f / 32.0f, &result) && PointEquals(0u, 1.0f, -0.5f, true),
           "singular matrix needs no inverse and retains unscaled placement");
}

static void TestOrderAndRepeatedNodes(void)
{
    Begin(4u, 1u, 0u);
    TrueTypeComponent records[2] = {Record(1u, 32, 0, 0u), Record(3u, -16, 32, 0u)};
    Composite(records, 2u, -24, 0, 40, 32);
    TrueTypeComponent child = Record(2u, 0, 0, 0u);
    Composite(&child, 1u, 0, 0, 8, 16);
    Line();
    Point(-8, 0, false);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 3u && result.contourCount == 2u &&
               PointEquals(0u, 1.0f, 0.0f, true) && PointEquals(1u, 1.25f, 0.5f, true) &&
               PointEquals(2u, -0.75f, 1.0f, false) && outputEnds[0] == 1u && outputEnds[1] == 2u &&
               result.xMin == -0.75f && result.xMax == 1.25f && result.yMin == 0.0f &&
               result.yMax == 1.0f,
           "DFS honors plan order with distinct point and contour bases");
    Begin(3u, 1u, 0u);
    records[0] = Record(1u, 0, 0, 0u);
    records[1] = Record(1u, 32, 0, 0u);
    Composite(records, 2u, 8, 16, 40, 16);
    child = Record(2u, 0, 0, 0u);
    Composite(&child, 1u, 8, 16, 8, 16);
    Point(8, 16, true);
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 2u &&
               PointEquals(0u, 0.25f, 0.5f, true) && PointEquals(1u, 1.25f, 0.5f, true) &&
               outputEnds[0] == 0u && outputEnds[1] == 1u,
           "repeated DAG subtree is transformed per occurrence");
}

static float FloatBits(uint32_t bits)
{
    union
    {
        uint32_t bits;
        float value;
    } number = {.bits = bits};
    return number.value;
}

static void TestRounding(void)
{
    Begin(3u, 1u, 0u);
    TrueTypeComponent outer = Record(1u, 24, 0, TRUE_TYPE_COMPONENT_XY_SCALE);
    outer.xScale = 24576;
    Composite(&outer, 1u, 42, 0, 42, 0);
    TrueTypeComponent inner = Record(2u, 12, 0, TRUE_TYPE_COMPONENT_ROUND_XY);
    Composite(&inner, 1u, 12, 0, 12, 0);
    Point(0, 0, true);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(1.0f / 32.0f, &result) && PointEquals(0u, 1.75f, 0.0f, true),
           "nested ROUND uses effective basis and adds ancestor translation once");
    Begin(2u, 1u, 0u);
    inner = Record(1u, 1, -1, TRUE_TYPE_COMPONENT_ROUND_XY);
    Composite(&inner, 1u, 1, -1, 1, -1);
    Point(0, 0, true);
    const uint32_t scales[] = {0x3EFFFFFFu, 0x3F000000u, 0x3F000001u};
    for (uint32_t index = 0u; index < 3u; ++index)
        Expect(Resolve(FloatBits(scales[index]), &result) &&
                   PointEquals(0u, index == 0u ? 0.0f : 1.0f, index == 2u ? -1.0f : 0.0f, true),
               "half ties go positive without double rounding a neighboring float");
}

static void TestMetricSources(void)
{
    Begin(2u, 2u, 2u);
    TrueTypeComponent record = Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(&record, 1u, 20, 700, 20, 700);
    Point(20, 700, true);
    Metric(false, 0u, 900u, -30);
    Metric(false, 1u, 512u, 10);
    Metric(true, 0u, 1600u, -50);
    Metric(true, 1u, 1024u, 100);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(1.0f / 32.0f, &result) &&
               PhantomsEqual(&result, 15u, 0.3125f, 16.3125f, 25.0f, -7.0f) &&
               PointEquals(0u, 0.625f, 21.875f, true) && result.xMin == 0.625f &&
               result.xMax == 0.625f && result.yMin == 21.875f && result.yMax == 21.875f,
           "identity USE selects all child phantoms without shifting geometry or bounds");

    Begin(3u, 3u, 3u);
    record = Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(&record, 1u, 4, 8, 4, 8);
    record = Record(2u, 0, 0, TRUE_TYPE_COMPONENT_SCALE);
    record.xScale = record.yScale = 8192;
    Composite(&record, 1u, 4, 8, 4, 8);
    Point(8, 16, true);
    Metric(false, 0u, 900u, -30);
    Metric(false, 1u, 700u, 1);
    Metric(false, 2u, 512u, 10);
    Metric(true, 0u, 1600u, -50);
    Metric(true, 1u, 1000u, 20);
    Metric(true, 2u, 1024u, 100);
    Expect(Resolve(1.0f / 8.0f, &result) && result.pointCount == 1u && result.contourCount == 1u &&
               PhantomsEqual(&result, 15u, 0.375f, 87.875f, 3.5f, -121.5f) &&
               PointEquals(0u, 0.5f, 1.0f, true) && result.xMin == 0.5f && result.xMax == 0.5f &&
               result.yMin == 1.0f && result.yMax == 1.0f,
           "metric selection stops at a composite whose scaled child has no USE flag");

    Begin(4u, 4u, 0u);
    TrueTypeComponent records[3] = {
        Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS | TRUE_TYPE_COMPONENT_MATRIX),
        Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS | TRUE_TYPE_COMPONENT_XY_SCALE),
        Record(3u, 0, 0, 0u)};
    Composite(records, 3u, -10, 5, 20, 40);
    Point(10, 30, true);
    Point(20, 40, false);
    Point(-10, 5, true);
    Metric(false, 0u, 900u, -10);
    Metric(false, 1u, 200u, 1);
    Metric(false, 2u, 300u, 5);
    Metric(false, 3u, 400u, -3);
    Expect(Resolve(1.0f / 8.0f, &result) && result.pointCount == 3u && result.contourCount == 3u &&
               PhantomsEqual(&result, 3u, 1.875f, 39.375f, 0.0f, 0.0f) &&
               PointEquals(0u, 1.25f, 3.75f, true) && PointEquals(1u, 2.5f, 5.0f, false) &&
               PointEquals(2u, -1.25f, 0.625f, true),
           "last flagged child wins rather than last child, and absent V clears all V slots");

    Begin(3u, 3u, 3u);
    record = Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(&record, 1u, 20, 700, 20, 700);
    record = Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(&record, 1u, 20, 700, 20, 700);
    Point(20, 700, true);
    Metric(false, 0u, 900u, -30);
    Metric(false, 1u, 700u, -10);
    Metric(false, 2u, 512u, 10);
    Metric(true, 0u, 1600u, -50);
    Metric(true, 1u, 1400u, 30);
    Metric(true, 2u, 1024u, 100);
    Expect(Resolve(1.0f / 32.0f, &result) &&
               PhantomsEqual(&result, 15u, 0.3125f, 16.3125f, 25.0f, -7.0f),
           "nested USE chain inherits the selected child's resolved metric source");

    Begin(3u, 3u, 3u);
    records[0] = Record(1u, 0, 0, 0u);
    records[1] = Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(records, 2u, 20, 700, 20, 700);
    Point(20, 700, true);
    Empty();
    Metric(false, 2u, 400u, 10);
    Metric(true, 2u, 600u, 20);
    Expect(Resolve(1.0f / 32.0f, &result) && result.pointCount == 1u && result.contourCount == 1u &&
               PhantomsEqual(&result, 15u, -0.3125f, 12.1875f, 0.625f, -18.125f) &&
               PointEquals(0u, 0.625f, 21.875f, true),
           "selected empty glyph uses its own zero contour count while root remains nonempty");

    Begin(3u, 3u, 0u);
    record = Record(1u, 0, 0, 0u);
    Composite(&record, 1u, 20, 700, 20, 700);
    record = Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(&record, 1u, 20, 700, 20, 700);
    Point(20, 700, true);
    Metric(false, 0u, 900u, -30);
    Metric(false, 2u, 512u, 10);
    Expect(Resolve(1.0f / 32.0f, &result) &&
               PhantomsEqual(&result, 3u, 1.5625f, 29.6875f, 0.0f, 0.0f),
           "USE inside an unflagged subtree does not replace root initial metrics");

    const uint16_t encodings[] = {TRUE_TYPE_COMPONENT_SCALE, TRUE_TYPE_COMPONENT_XY_SCALE,
                                  TRUE_TYPE_COMPONENT_MATRIX};
    for (uint32_t index = 0u; index < sizeof(encodings) / sizeof(encodings[0]); ++index)
    {
        Begin(2u, 2u, 0u);
        record = Record(1u, 0, 0,
                        encodings[index] | TRUE_TYPE_COMPONENT_USE_METRICS |
                            TRUE_TYPE_COMPONENT_ROUND_XY | TRUE_TYPE_COMPONENT_SCALED_OFFSET);
        Composite(&record, 1u, 8, 16, 8, 16);
        Point(8, 16, true);
        Metric(false, 1u, 256u, 4);
        Expect(Resolve(1.0f / 8.0f, &result) &&
                   PhantomsEqual(&result, 3u, 0.5f, 32.5f, 0.0f, 0.0f) &&
                   PointEquals(0u, 1.0f, 2.0f, true),
               "explicit identity encodings and zero ROUND/scaled placement support USE");
    }

    Begin(16u, 1u, 0u);
    for (uint32_t glyph = 0u; glyph < 15u; ++glyph)
    {
        record = Record((uint16_t)(glyph + 1u), 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
        Composite(&record, 1u, 0, 0, 0, 0);
    }
    Point(0, 0, true);
    Bearing(false, 15u, 10);
    Expect(Resolve(0.5f, &result) && PhantomsEqual(&result, 3u, -5.0f, 245.0f, 0.0f, 0.0f),
           "metric selection accepts the full sixteen-level chain without extra scratch");
}

static TrueTypeXYGlyph Sentinel(void)
{
    return (TrueTypeXYGlyph){
        .pointCount = 91u,
        .contourCount = 92u,
        .xMin = 1.0f,
        .yMin = 2.0f,
        .xMax = 3.0f,
        .yMax = 4.0f,
        .phantoms = {{5.0f, 6.0f}, {7.0f, 8.0f}, {9.0f, 10.0f}, {11.0f, 12.0f}},
        .phantomMask = 93u};
}

static bool SameGlyph(const TrueTypeXYGlyph *left, const TrueTypeXYGlyph *right)
{
    if (left->pointCount != right->pointCount || left->contourCount != right->contourCount ||
        left->xMin != right->xMin || left->yMin != right->yMin || left->xMax != right->xMax ||
        left->yMax != right->yMax || left->phantomMask != right->phantomMask)
        return false;
    for (uint32_t index = 0u; index < 4u; ++index)
        if (left->phantoms[index].x != right->phantoms[index].x ||
            left->phantoms[index].y != right->phantoms[index].y)
            return false;
    return true;
}

static void Rejected(float scale, TrueTypeXYStorage storage)
{
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        outputPoints[index] =
            (TrueTypePixelPoint){100.0f + (float)index, 200.0f + (float)index, (index & 1u) != 0u};
        outputEnds[index] = (uint16_t)(700u + index);
    }
    TrueTypeFont font = {0};
    TrueTypeXYGlyph previous = Sentinel();
    TrueTypeXYGlyph result = previous;
    Expect(Initialize(&font) &&
               !TrueTypeGlyphResolveXY(&font, 0u, scale, &workspace, storage, &result) &&
               SameGlyph(&result, &previous),
           "failed geometry preserves every scalar and phantom field");
    for (uint32_t index = 0u; index < 8u; ++index)
        Expect(
            PointEquals(index, 100.0f + (float)index, 200.0f + (float)index, (index & 1u) != 0u) &&
                outputEnds[index] == 700u + index,
            "failed geometry preserves complete published arrays");
}

static void TestLimitsAndUnsupportedModes(void)
{
    Begin(1u, 1u, 0u);
    Point(8, 16, true);
    const float badScales[] = {0.0f, -1.0f, 65.0f, FloatBits(0x7F800000u), FloatBits(0x7FC00000u)};
    for (uint32_t index = 0u; index < sizeof(badScales) / sizeof(badScales[0]); ++index)
        Rejected(badScales[index], Storage());
    Rejected(0.5f, (TrueTypeXYStorage){outputPoints, outputEnds, 0u, 8u});
    Rejected(0.5f, (TrueTypeXYStorage){outputPoints, outputEnds, 8u, 0u});
    Rejected(0.5f, (TrueTypeXYStorage){NULL, outputEnds, 8u, 8u});
    Begin(2u, 1u, 0u);
    TrueTypeComponent records[2] = {Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS)};
    Composite(records, 1u, 8, 16, 8, 16);
    Point(8, 16, true);
    TrueTypeXYGlyph identity = {0};
    Expect(Resolve(0.5f, &identity) && PointEquals(0u, 4.0f, 8.0f, true) &&
               PhantomsEqual(&identity, 3u, 4.0f, 254.0f, 0.0f, 0.0f),
           "the former identity USE rejection is now supported");
    Begin(2u, 1u, 0u);
    records[0] = Record(1u, 0, 0, 0u);
    records[1] = Record(1u, 0, 0, 0u);
    records[1].flags = TRUE_TYPE_COMPONENT_WORD_ARGUMENTS;
    Composite(records, 2u, 8, 16, 8, 16);
    Point(8, 16, true);
    Rejected(0.5f, Storage());
    Begin(1u, 1u, 0u);
    Point(16384, 0, true);
    Metric(false, 0u, 0u, 16384);
    TrueTypeXYGlyph result = {0};
    Expect(Resolve(64.0f, &result) && PointEquals(0u, 1048576.0f, 0.0f, true),
           "pixel coordinate limit is inclusive");
    Begin(1u, 1u, 0u);
    Point(INT16_MAX, 0, true);
    Metric(false, 0u, 0u, INT16_MAX);
    Rejected(64.0f, Storage());
    Begin(3u, 1u, 0u);
    records[0] = Record(1u, 0, 0, 0u);
    records[1] = Record(2u, 0, 0, 0u);
    Composite(records, 2u, 0, 0, INT16_MAX, 0);
    Point(0, 0, true);
    Point(INT16_MAX, 0, true);
    Metric(false, 0u, 0u, 0);
    Rejected(64.0f, Storage());
    for (uint32_t excessive = 0u; excessive < 2u; ++excessive)
    {
        Begin(16u, 1u, 0u);
        for (uint32_t glyph = 0u; glyph < 15u; ++glyph)
        {
            TrueTypeComponent record = Record((uint16_t)(glyph + 1u), 0, 0,
                                              excessive != 0u ? TRUE_TYPE_COMPONENT_MATRIX : 0u);
            if (excessive != 0u)
                record.xScale = record.scale01 = record.scale10 = record.yScale = INT16_MAX;
            Composite(&record, 1u, 0, 0, 0, 0);
        }
        Point(0, 0, true);
        Metric(false, 0u, 0u, 0);
        if (excessive != 0u)
            Rejected(64.0f, Storage());
        else
            Expect(Resolve(0.5f, &result) && PointEquals(0u, 0.0f, 0.0f, true),
                   "explicit DFS accepts all sixteen planned levels");
    }
}

static void TestComplexMetricsRejected(void)
{
    for (uint32_t mode = 0u; mode < 7u; ++mode)
    {
        Begin(2u, 1u, 0u);
        TrueTypeComponent record =
            Record(1u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS | TRUE_TYPE_COMPONENT_MATRIX);
        int16_t x = 8;
        int16_t y = 16;
        if (mode == 0u)
        {
            record.xScale = 8192;
            x = 4;
        }
        if (mode == 1u)
        {
            record.yScale = 8192;
            y = 8;
        }
        if (mode == 2u)
        {
            record.scale01 = 4096;
            y = 18;
        }
        if (mode == 3u)
        {
            record.scale10 = 4096;
            x = 12;
        }
        if (mode == 4u)
        {
            record.argument1 = 1;
            x = 9;
        }
        if (mode == 5u)
        {
            record.argument2 = -1;
            y = 15;
        }
        if (mode == 6u)
        {
            record.xScale = -16384;
            x = -8;
        }
        Composite(&record, 1u, x, y, x, y);
        Point(8, 16, true);
        Rejected(0.5f, Storage());
    }

    Begin(3u, 1u, 0u);
    TrueTypeComponent records[2] = {Record(1u, 1, 0, TRUE_TYPE_COMPONENT_USE_METRICS),
                                    Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS)};
    Composite(records, 2u, 9, 16, 12, 20);
    Point(8, 16, true);
    Point(12, 20, true);
    Rejected(0.5f, Storage());

    Begin(4u, 1u, 0u);
    records[0] = Record(1u, 0, 0, 0u);
    records[1] = Record(3u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS);
    Composite(records, 2u, 4, 16, 12, 20);
    TrueTypeComponent complex =
        Record(2u, 0, 0, TRUE_TYPE_COMPONENT_USE_METRICS | TRUE_TYPE_COMPONENT_MATRIX);
    complex.xScale = 8192;
    Composite(&complex, 1u, 4, 16, 4, 16);
    Point(8, 16, true);
    Point(12, 20, true);
    Rejected(0.5f, Storage());
}

static void WriteSize(size_t value)
{
    char digits[32];
    size_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (size_t index = 0u; index < count / 2u; ++index)
    {
        char swap = digits[index];
        digits[index] = digits[count - index - 1u];
        digits[count - index - 1u] = swap;
    }
    digits[count] = '\0';
    LaiueTestRuntimeWrite(digits);
}

LAIUE_TEST_ENTRY(TrueTypeXYTestEntryPoint)
{
    TestSimpleAndEmpty();
    TestMatricesAndOffsets();
    TestOrderAndRepeatedNodes();
    TestRounding();
    TestMetricSources();
    TestLimitsAndUnsupportedModes();
    TestComplexMetricsRejected();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType XY metrics OK; workspace=");
    WriteSize(sizeof(TrueTypeXYWorkspace));
    LaiueTestRuntimeWrite(" frame=");
    WriteSize(sizeof(TrueTypeXYFrame));
    LaiueTestRuntimeWrite(" point=");
    WriteSize(sizeof(TrueTypePixelPoint));
    LaiueTestRuntimeWrite(" result=");
    WriteSize(sizeof(TrueTypeXYGlyph));
    LaiueTestRuntimeWrite("\n");
    LAIUE_TEST_SUCCESS();
}
