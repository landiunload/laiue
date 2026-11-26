#include "ui/truetype_plan.h"
#include "test_runtime.h"

enum
{
    FONT_CAPACITY = 65536,
    FONT_MAX_GLYPHS = 32
};
static uint8_t fontBytes[FONT_CAPACITY];
static TrueTypePlanWorkspace workspace;
static TrueTypeComponent records[TRUE_TYPE_PLAN_MAX_COMPONENTS];
static uint32_t failures;
static uint32_t glyphCount;
static uint32_t locaOffset;
static uint32_t glyfOffset;
static uint32_t position;
static uint32_t nextGlyph;

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

// Собственный SFNT fixture с long loca и одним cmap12 mapping на glyph0.
static void Begin(uint32_t count)
{
    for (size_t i = 0u; i < sizeof(fontBytes); ++i)
        fontBytes[i] = 0u;
    glyphCount = count;
    nextGlyph = 0u;
    locaOffset = (288u + 4u + (count - 1u) * 2u + 3u) & ~3u;
    glyfOffset = locaOffset + (count + 1u) * 4u;
    position = glyfOffset;
    Put32(0u, 0x00010000u);
    Put16(4u, 7u);
    Put16(6u, 64u);
    Put16(8u, 2u);
    Put16(10u, 48u);
    Table(0u, "head", 124u, 54u);
    Table(1u, "hhea", 180u, 36u);
    Table(2u, "maxp", 216u, 32u);
    Table(3u, "cmap", 248u, 40u);
    Table(4u, "hmtx", 288u, 4u + (count - 1u) * 2u);
    Table(5u, "loca", locaOffset, (count + 1u) * 4u);
    Put32(124u, 0x00010000u);
    Put32(136u, 0x5F0F3CF5u);
    Put16(142u, 1000u);
    Put16(174u, 1u);
    Put32(180u, 0x00010000u);
    Put16(214u, 1u);
    Put32(216u, 0x00010000u);
    Put16(220u, count);
    Put16(250u, 1u);
    Put16(252u, 3u);
    Put16(254u, 10u);
    Put32(256u, 12u);
    Put16(260u, 12u);
    Put32(264u, 28u);
    Put32(272u, 1u);
    Put16(288u, 500u);
}

static void StartGlyph(void)
{
    Expect(nextGlyph < glyphCount && position + 16384u < FONT_CAPACITY,
           "fixture has room for the next glyph");
    Put32(locaOffset + nextGlyph++ * 4u, position - glyfOffset);
}

static void Empty(void)
{
    StartGlyph();
}

static void Simple(uint32_t points, uint32_t contours)
{
    StartGlyph();
    Put16(position, contours);
    position += 10u;
    for (uint32_t i = 0u; i < contours; ++i)
    {
        Put16(position, i + 1u == contours ? points - 1u : i);
        position += 2u;
    }
    position += 2u; // zero instructionLength
    for (uint32_t i = 0u; i < points;)
    {
        uint32_t run = points - i < 256u ? points - i : 256u;
        fontBytes[position++] = 0x39u; // repeated on-curve, both coordinates unchanged
        fontBytes[position++] = (uint8_t)(run - 1u);
        i += run;
    }
}

static void Records(uint32_t count, uint16_t glyph)
{
    for (uint32_t i = 0u; i < count; ++i)
        records[i] = (TrueTypeComponent){.glyphIndex = glyph,
                                         .flags = TRUE_TYPE_COMPONENT_XY_ARGUMENTS,
                                         .xScale = 16384,
                                         .yScale = 16384};
}

static void Composite(uint32_t count)
{
    StartGlyph();
    Put16(position, 65535u);
    position += 10u;
    for (uint32_t i = 0u; i < count; ++i)
    {
        const TrueTypeComponent *record = &records[i];
        uint32_t flags = record->flags | TRUE_TYPE_COMPONENT_WORD_ARGUMENTS |
                         (i + 1u < count ? TRUE_TYPE_COMPONENT_MORE : 0u);
        Put16(position, flags);
        Put16(position + 2u, record->glyphIndex);
        Put16(position + 4u, (uint16_t)record->argument1);
        Put16(position + 6u, (uint16_t)record->argument2);
        position += 8u;
        if ((flags & TRUE_TYPE_COMPONENT_SCALE) != 0u)
        {
            Put16(position, (uint16_t)record->xScale);
            position += 2u;
        }
        else if ((flags & TRUE_TYPE_COMPONENT_XY_SCALE) != 0u)
        {
            Put16(position, (uint16_t)record->xScale);
            Put16(position + 2u, (uint16_t)record->yScale);
            position += 4u;
        }
        else if ((flags & TRUE_TYPE_COMPONENT_MATRIX) != 0u)
        {
            Put16(position, (uint16_t)record->xScale);
            Put16(position + 2u, (uint16_t)record->scale01);
            Put16(position + 4u, (uint16_t)record->scale10);
            Put16(position + 6u, (uint16_t)record->yScale);
            position += 8u;
        }
    }
}

static bool Finish(TrueTypeFont *font)
{
    Expect(nextGlyph == glyphCount && glyphCount <= FONT_MAX_GLYPHS,
           "fixture emitted every declared glyph");
    Put32(locaOffset + glyphCount * 4u, position - glyfOffset);
    Table(6u, "glyf", glyfOffset, position - glyfOffset);
    bool valid = TrueTypeReaderInitialize(fontBytes, position, font);
    Expect(valid, "fixture initializes with the real SFNT reader");
    return valid;
}

static bool Build(uint16_t glyph, TrueTypePlanSummary *out)
{
    TrueTypeFont font = {0};
    return Finish(&font) && TrueTypeGlyphPlanBuild(&font, glyph, &workspace, out);
}

static bool SameSummary(TrueTypePlanSummary left, TrueTypePlanSummary right)
{
    return left.nodeCount == right.nodeCount && left.componentCount == right.componentCount &&
           left.pointCount == right.pointCount && left.contourCount == right.contourCount &&
           left.maxDepth == right.maxDepth;
}

static void Rejected(uint16_t glyph)
{
    TrueTypePlanSummary previous = {11u, 12u, 13u, 14u, 15u};
    TrueTypePlanSummary out = previous;
    Expect(!Build(glyph, &out) && SameSummary(out, previous),
           "plan failure preserves every published summary field");
}

static void TestSimpleAndEmpty(void)
{
    Begin(2u);
    Empty();
    Simple(3u, 1u);
    TrueTypePlanSummary out = {0};
    Expect(Build(0u, &out) && SameSummary(out, (TrueTypePlanSummary){1u, 0u, 0u, 0u, 1u}) &&
               workspace.nodes[0].firstChild == TRUE_TYPE_PLAN_NO_INDEX &&
               workspace.nodes[0].parentIndex == TRUE_TYPE_PLAN_NO_INDEX &&
               workspace.nodes[0].componentIndex == TRUE_TYPE_PLAN_NO_INDEX,
           "empty root has one leaf and zero counts");
    Expect(Build(1u, &out) && SameSummary(out, (TrueTypePlanSummary){1u, 0u, 3u, 1u, 1u}),
           "simple root measures packed outline without point arrays");
    Rejected(2u);
    TrueTypeOutlineCounts counts = {17u, 18u};
    Expect(TrueTypeSimpleOutlineMeasure((TrueTypeSpan){0}, &counts) && counts.pointCount == 0u &&
               counts.contourCount == 0u,
           "measure shares empty span convention");
    TrueTypeFont font = {0};
    if (Finish(&font))
    {
        TrueTypePlanSummary previous = out;
        Expect(!TrueTypeGlyphPlanBuild(NULL, 0u, &workspace, &out) && SameSummary(out, previous) &&
                   !TrueTypeGlyphPlanBuild(&font, 0u, NULL, &out) && SameSummary(out, previous) &&
                   !TrueTypeGlyphPlanBuild(&font, 0u, &workspace, NULL),
               "null parameters fail without publishing a partial summary");
    }
}

static void TestNestedAndRawRecords(void)
{
    Begin(4u);
    Records(2u, 1u);
    records[0].flags |= TRUE_TYPE_COMPONENT_MATRIX;
    records[0].xScale = 8192;
    records[0].scale01 = -2048;
    records[0].scale10 = 4096;
    records[0].yScale = 24576;
    records[0].argument1 = -100;
    records[1].glyphIndex = 2u;
    records[1].flags |= TRUE_TYPE_COMPONENT_USE_METRICS;
    records[1].argument2 = 37;
    Composite(2u);
    Records(2u, 2u);
    records[1].glyphIndex = 3u;
    Composite(2u);
    Simple(3u, 1u);
    Simple(4u, 2u);
    TrueTypePlanSummary out = {0};
    Expect(Build(0u, &out) && SameSummary(out, (TrueTypePlanSummary){5u, 4u, 10u, 4u, 3u}),
           "nested composite counts repeated DAG leaves independently");
    const TrueTypePlanNode *nodes = workspace.nodes;
    Expect(nodes[0].firstChild == 1u && nodes[0].childCount == 2u && nodes[1].firstChild == 3u &&
               nodes[1].childCount == 2u && nodes[2].pointBase == 7u &&
               nodes[2].contourBase == 3u && nodes[3].pointBase == 0u &&
               nodes[3].contourBase == 0u && nodes[4].pointBase == 3u &&
               nodes[4].contourBase == 1u && nodes[2].glyphIndex == nodes[3].glyphIndex &&
               nodes[3].parentIndex == 1u,
           "BFS node storage preserves component output order and final bases");
    Expect(workspace.components[0].argument1 == -100 && workspace.components[0].xScale == 8192 &&
               workspace.components[0].scale01 == -2048 &&
               workspace.components[0].scale10 == 4096 && workspace.components[0].yScale == 24576 &&
               workspace.components[1].argument2 == 37 &&
               (workspace.components[1].flags & TRUE_TYPE_COMPONENT_USE_METRICS) != 0u,
           "preflight preserves raw nonsymmetric matrix and metric flag without applying them");
    TrueTypeOutlineCounts counts = {17u, 18u};
    Expect(!TrueTypeSimpleOutlineMeasure((TrueTypeSpan){fontBytes + glyfOffset, 42u}, &counts) &&
               counts.pointCount == 17u && counts.contourCount == 18u,
           "simple measure rejects composite without changing counts");
}

static void ReferenceFont(uint32_t parent, uint32_t child)
{
    Begin(3u);
    Records(2u, 1u);
    records[1].glyphIndex = 2u;
    records[1].flags = 0u;
    records[1].argument1 = (int32_t)parent;
    records[1].argument2 = (int32_t)child;
    Composite(2u);
    Simple(3u, 1u);
    Simple(4u, 1u);
}

static void TestReferences(void)
{
    for (uint32_t child = 0u; child < 8u; ++child)
    {
        ReferenceFont(2u, child);
        TrueTypePlanSummary out = {0};
        Expect(Build(0u, &out), "child ordinary points and all four phantom slots are distinct");
    }
    const uint32_t parent[] = {0u, 2u, 7u, 8u, 9u, 10u};
    for (uint32_t i = 0u; i < sizeof(parent) / sizeof(parent[0]); ++i)
    {
        ReferenceFont(parent[i], 7u);
        TrueTypePlanSummary out = {0};
        Expect(Build(0u, &out), "parent prefix or final phantom and child phantom are structural");
    }
    const uint32_t future[] = {3u, 6u, 11u};
    for (uint32_t i = 0u; i < sizeof(future) / sizeof(future[0]); ++i)
    {
        ReferenceFont(future[i], 0u);
        Rejected(0u);
    }
    ReferenceFont(0u, 8u);
    Rejected(0u);
    Begin(2u);
    Records(2u, 1u);
    records[1].flags = 0u;
    records[1].argument1 = 3;
    records[1].argument2 = 1;
    Composite(2u);
    Empty();
    TrueTypePlanSummary out = {0};
    Expect(Build(0u, &out) && out.pointCount == 0u,
           "empty child and parent expose four structural phantom indices");
}

static void TestDepthAndCycles(void)
{
    for (uint32_t depth = 16u; depth <= 17u; ++depth)
    {
        Begin(depth);
        for (uint32_t glyph = 0u; glyph + 1u < depth; ++glyph)
        {
            Records(1u, (uint16_t)(glyph + 1u));
            Composite(1u);
        }
        Simple(3u, 1u);
        TrueTypePlanSummary out = {0};
        if (depth == 16u)
            Expect(Build(0u, &out) && out.maxDepth == 16u && out.pointCount == 3u,
                   "depth includes root and accepts exactly sixteen nodes");
        else
            Rejected(0u);
    }
    Begin(2u);
    Records(1u, 1u);
    Composite(1u);
    Records(1u, 0u);
    Composite(1u);
    Rejected(0u);
    Begin(2u);
    Records(1u, 2u);
    Composite(1u);
    Empty();
    Rejected(0u);
}

static void TestLimitsAndReuse(void)
{
    Begin(2u);
    Records(1024u, 1u);
    Composite(1024u);
    Empty();
    TrueTypePlanSummary out = {0};
    Expect(Build(0u, &out) && out.nodeCount == 1025u && out.componentCount == 1024u &&
               workspace.nodes[1024].parentIndex == 0u,
           "exact total component and node capacities are accepted");
    for (uint32_t children = 511u; children <= 512u; ++children)
    {
        Begin(3u);
        Records(2u, 1u);
        Composite(2u);
        Records(children, 2u);
        Composite(children);
        Empty();
        if (children == 511u)
            Expect(Build(0u, &out) && out.componentCount == 1024u,
                   "component capacity counts expanded repeated subtrees");
        else
            Rejected(0u);
    }
    for (uint32_t count = 2u; count <= 3u; ++count)
    {
        Begin(2u);
        Records(count, 1u);
        Composite(count);
        Simple(8192u, 1u);
        if (count == 2u)
            Expect(Build(0u, &out) && out.pointCount == 16384u,
                   "flattened point capacity is inclusive");
        else
            Rejected(0u);
        Begin(2u);
        Records(count, 1u);
        Composite(count);
        Simple(2048u, 2048u);
        if (count == 2u)
            Expect(Build(0u, &out) && out.contourCount == 4096u,
                   "flattened contour capacity is inclusive");
        else
            Rejected(0u);
    }
    Begin(1u);
    Empty();
    Expect(Build(0u, &out) && SameSummary(out, (TrueTypePlanSummary){1u, 0u, 0u, 0u, 1u}) &&
               workspace.nodes[0].pointBase == 0u && workspace.nodes[0].contourBase == 0u,
           "workspace reuses clean logical counts after failed larger calls");
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

LAIUE_TEST_ENTRY(TrueTypePlanTestEntryPoint)
{
    TestSimpleAndEmpty();
    TestNestedAndRawRecords();
    TestReferences();
    TestDepthAndCycles();
    TestLimitsAndReuse();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType plan OK; node=");
    WriteSize(sizeof(TrueTypePlanNode));
    LaiueTestRuntimeWrite(" component=");
    WriteSize(sizeof(TrueTypeComponent));
    LaiueTestRuntimeWrite(" workspace=");
    WriteSize(sizeof(TrueTypePlanWorkspace));
    LaiueTestRuntimeWrite("\n");
    LAIUE_TEST_SUCCESS();
}
