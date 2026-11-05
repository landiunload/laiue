#include "ui/truetype_phantoms.h"
#include "ui/truetype_plan.h"
#include "test_runtime.h"

enum
{
    FONT_CAPACITY = 1024,
    FONT_MAX_GLYPHS = 4,
    HEAD_OFFSET = 160,
    HHEA_OFFSET = 216,
    MAXP_OFFSET = 252,
    CMAP_OFFSET = 284,
    HMTX_OFFSET = 324,
    VHEA_OFFSET = 340,
    VMTX_OFFSET = 376,
    LOCA_OFFSET = 392,
    GLYF_OFFSET = 412,
};

static uint8_t fontBytes[FONT_CAPACITY];
static TrueTypePlanWorkspace workspace;
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
    uint32_t offset = vertical ? VMTX_OFFSET : HMTX_OFFSET;
    offset += glyph < count ? glyph * 4u + 2u : count * 4u + (glyph - count) * 2u;
    Put16(offset, (uint16_t)value);
}

static void Metric(bool vertical, uint32_t index, uint16_t advance, int16_t bearing)
{
    Expect(index < (vertical ? verticalMetrics : horizontalMetrics),
           "fixture metric has its own long record");
    Put16((vertical ? VMTX_OFFSET : HMTX_OFFSET) + index * 4u, advance);
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
    position = GLYF_OFFSET;
    Put32(0u, 0x00010000u);
    Put16(4u, vMetrics != 0u ? 9u : 7u);
    Put16(6u, vMetrics != 0u ? 128u : 64u);
    Put16(8u, vMetrics != 0u ? 3u : 2u);
    Put16(10u, vMetrics != 0u ? 16u : 48u);
    Table(0u, "cmap", CMAP_OFFSET, 40u);
    Table(2u, "head", HEAD_OFFSET, 54u);
    Table(3u, "hhea", HHEA_OFFSET, 36u);
    Table(4u, "hmtx", HMTX_OFFSET, hMetrics * 4u + (count - hMetrics) * 2u);
    Table(5u, "loca", LOCA_OFFSET, (count + 1u) * 4u);
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
        Table(7u, "vhea", VHEA_OFFSET, 36u);
        Table(8u, "vmtx", VMTX_OFFSET, vMetrics * 4u + (count - vMetrics) * 2u);
        Put32(VHEA_OFFSET, 0x00010000u);
        Put16(VHEA_OFFSET + 34u, vMetrics);
        for (uint32_t i = 0u; i < vMetrics; ++i)
            Metric(true, i, 1000u, 0);
    }
}

static void StartGlyph(void)
{
    Expect(nextGlyph < glyphCount && position + 20u <= FONT_CAPACITY,
           "fixture has room for the next glyph");
    Put32(LOCA_OFFSET + nextGlyph++ * 4u, position - GLYF_OFFSET);
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

static void Point(int16_t x, int16_t y)
{
    Header(1u, x, y, x, y);
    Put16(position, 0u);           // endPtsOfContours[0]
    Put16(position + 2u, 0u);      // instructionLength
    fontBytes[position + 4u] = 1u; // on-curve, two signed word deltas
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
    Put32(LOCA_OFFSET + glyphCount * 4u, position - GLYF_OFFSET);
    Table(1u, "glyf", GLYF_OFFSET, position - GLYF_OFFSET);
    bool valid = TrueTypeReaderInitialize(fontBytes, position, font);
    Expect(valid, "functional fixture initializes with the real SFNT reader");
    return valid;
}

static bool ReadPlanned(uint16_t glyph, TrueTypeInitialPhantoms *out)
{
    TrueTypeFont font = {0};
    TrueTypePlanSummary summary = {0};
    if (!Initialize(&font) || !TrueTypeGlyphPlanBuild(&font, glyph, &workspace, &summary))
    {
        Expect(false, "functional glyph obtains its contour count from the real plan");
        return false;
    }
    return TrueTypeGlyphInitialPhantomsRead(&font, glyph, summary.contourCount, out);
}

static TrueTypeInitialPhantoms Expected(int32_t left, int32_t right, int32_t top, int32_t bottom,
                                        uint32_t mask)
{
    return (TrueTypeInitialPhantoms){.points = {{left, 0}, {right, 0}, {0, top}, {0, bottom}},
                                     .validMask = mask};
}

static bool Same(const TrueTypeInitialPhantoms *left, const TrueTypeInitialPhantoms *right)
{
    if (left->validMask != right->validMask)
        return false;
    for (uint32_t i = 0u; i < 4u; ++i)
        if (left->points[i].x != right->points[i].x || left->points[i].y != right->points[i].y)
            return false;
    return true;
}

static TrueTypeInitialPhantoms Sentinel(void)
{
    return (TrueTypeInitialPhantoms){.points = {{11, 12}, {13, 14}, {15, 16}, {17, 18}},
                                     .validMask = 71u};
}

static void ExpectPlanned(uint16_t glyph, TrueTypeInitialPhantoms expected, const char *message)
{
    TrueTypeInitialPhantoms result = Sentinel();
    Expect(ReadPlanned(glyph, &result) && Same(&result, &expected), message);
}

static void TestHorizontalAndVertical(void)
{
    Begin(1u, 1u, 1u);
    Point(20, 700);
    Metric(false, 0u, 500u, 10);
    Metric(true, 0u, 1000u, 100);
    ExpectPlanned(0u, Expected(10, 510, 800, -200, 0xFu),
                  "H/V initial points use raw bearings and downward vertical advance");

    Begin(1u, 1u, 0u);
    Point(20, 700);
    Metric(false, 0u, 500u, 10);
    ExpectPlanned(0u, Expected(10, 510, 0, 0, 0x3u),
                  "H-only success overwrites all V slots without synthesizing hhea metrics");
}

static void TestEmptyContours(void)
{
    Begin(1u, 1u, 1u);
    Empty();
    Metric(false, 0u, 400u, 10);
    Metric(true, 0u, 900u, 20);
    ExpectPlanned(0u, Expected(-10, 390, 20, -880, 0xFu),
                  "empty loca retains raw bearings and nonzero advances");

    Begin(1u, 1u, 1u);
    ZeroContourBox(20, 600, 40, 700);
    Metric(false, 0u, 400u, 10);
    Metric(true, 0u, 900u, 20);
    ExpectPlanned(0u, Expected(-10, 390, 20, -880, 0xFu),
                  "zero-contour ordered bbox uses the explicit effective-zero convention");

    Begin(2u, 1u, 1u);
    EmptyComposite();
    Empty();
    Metric(false, 0u, 400u, 10);
    Metric(true, 0u, 900u, 20);
    Bearing(false, 1u, -7);
    Bearing(true, 1u, -30);
    ExpectPlanned(0u, Expected(-10, 390, 20, -880, 0xFu),
                  "all-empty composite uses resolved count instead of negative raw count");
    ExpectPlanned(1u, Expected(7, 407, -30, -930, 0xFu),
                  "empty child has independent bearings with repeated long advances");
}

static void TestCompressedMetrics(void)
{
    Begin(3u, 2u, 2u);
    Point(20, 700);
    Point(-30, 50);
    Point(100, -20);
    Metric(false, 0u, 400u, 10);
    Metric(false, 1u, 500u, -5);
    Bearing(false, 2u, 25);
    Metric(true, 0u, 1000u, 100);
    Metric(true, 1u, 900u, -10);
    Bearing(true, 2u, 20);
    ExpectPlanned(0u, Expected(10, 410, 800, -200, 0xFu),
                  "first long metric retains its own horizontal and vertical advance");
    ExpectPlanned(1u, Expected(-25, 475, 40, -860, 0xFu), "last long metric reads signed bearings");
    ExpectPlanned(2u, Expected(75, 575, 0, -900, 0xFu),
                  "compressed hmtx/vmtx repeat the last advance with separate bearings");
}

static void TestZeroAndExtrema(void)
{
    Begin(1u, 1u, 1u);
    Point(-12, 70);
    Metric(false, 0u, 0u, 5);
    Metric(true, 0u, 0u, -20);
    ExpectPlanned(0u, Expected(-17, -17, 50, 50, 0xFu),
                  "zero advances remain available metric points");

    Begin(1u, 1u, 1u);
    Point(INT16_MAX, INT16_MIN);
    Metric(false, 0u, UINT16_MAX, INT16_MIN);
    Metric(true, 0u, UINT16_MAX, INT16_MIN);
    ExpectPlanned(0u, Expected(65535, 131070, -65536, -131071, 0xFu),
                  "int32 arithmetic retains maximum positive right and negative bottom");

    Begin(1u, 1u, 1u);
    Point(INT16_MIN, INT16_MAX);
    Metric(false, 0u, 0u, INT16_MAX);
    Metric(true, 0u, 0u, INT16_MAX);
    ExpectPlanned(0u, Expected(-65535, -65535, 65534, 65534, 0xFu),
                  "opposite signed extrema do not narrow the initial phantom coordinates");
}

static void TestOutputPreservation(void)
{
    Begin(1u, 1u, 1u);
    Point(20, 700);
    TrueTypeFont font = {0};
    TrueTypePlanSummary summary = {0};
    if (!Initialize(&font) || !TrueTypeGlyphPlanBuild(&font, 0u, &workspace, &summary))
    {
        Expect(false, "context fixture obtains its successful plan");
        return;
    }
    TrueTypeInitialPhantoms previous = Sentinel();
    TrueTypeInitialPhantoms result = previous;
    Expect(!TrueTypeGlyphInitialPhantomsRead(&font, 0u, 0u, &result) && Same(&result, &previous),
           "simple count mismatch preserves every output coordinate and mask");
    Expect(!TrueTypeGlyphInitialPhantomsRead(&font, 0u, TRUE_TYPE_OUTLINE_MAX_CONTOURS + 1u,
                                             &result) &&
               Same(&result, &previous),
           "context count above the plan contour limit preserves the output");
    Expect(!TrueTypeGlyphInitialPhantomsRead(&font, 1u, summary.contourCount, &result) &&
               Same(&result, &previous),
           "glyph id outside the font preserves the output");
    Expect(!TrueTypeGlyphInitialPhantomsRead(NULL, 0u, summary.contourCount, &result) &&
               Same(&result, &previous) &&
               !TrueTypeGlyphInitialPhantomsRead(&font, 0u, summary.contourCount, NULL),
           "null context or output is rejected without publishing partial values");

    Begin(1u, 1u, 0u);
    ZeroContourBox(20, 0, 10, 0);
    if (Initialize(&font))
        Expect(!TrueTypeGlyphInitialPhantomsRead(&font, 0u, 0u, &result) &&
                   Same(&result, &previous),
               "header accessor failure is propagated before applying the empty convention");
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
    for (size_t i = 0u; i < count / 2u; ++i)
    {
        char swap = digits[i];
        digits[i] = digits[count - 1u - i];
        digits[count - 1u - i] = swap;
    }
    digits[count] = '\0';
    LaiueTestRuntimeWrite(digits);
}

LAIUE_TEST_ENTRY(TrueTypePhantomsTestEntryPoint)
{
    TestHorizontalAndVertical();
    TestEmptyContours();
    TestCompressedMetrics();
    TestZeroAndExtrema();
    TestOutputPreservation();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType initial phantoms OK; point=");
    WriteSize(sizeof(TrueTypePhantomPoint));
    LaiueTestRuntimeWrite(" result=");
    WriteSize(sizeof(TrueTypeInitialPhantoms));
    LaiueTestRuntimeWrite("\n");
    LAIUE_TEST_SUCCESS();
}
