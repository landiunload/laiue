#include "ui/truetype_components.h"
#include "test_runtime.h"

static uint32_t failures;
static TrueTypeComponent components[1024];
static uint8_t maximumGlyph[6200];

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        ++failures;
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
    }
}

static void Put16(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static TrueTypeComponentStorage Storage(void)
{
    return (TrueTypeComponentStorage){components, 1024u};
}

static bool ComponentEqual(TrueTypeComponent left, TrueTypeComponent right)
{
    return left.glyphIndex == right.glyphIndex && left.flags == right.flags &&
           left.argument1 == right.argument1 && left.argument2 == right.argument2 &&
           left.xScale == right.xScale && left.scale01 == right.scale01 &&
           left.scale10 == right.scale10 && left.yScale == right.yScale;
}

static void TestRecords(void)
{
    // -2 contours; three records, instructions flag only on the first one.
    static const uint8_t glyph[] = {
        0xFF, 0xFE, 0,    0,    0,    0,  0,    0,    0, 0,    0x07, 0x27, 0,    31, 0x80, 0,
        0x7F, 0xFF, 0,    0x20, 0,    47, 0x80, 0xFF, 0, 0x81, 0xFF, 0xFF, 0x80, 0,  0xFF, 0xFF,
        0x20, 0,    0xE0, 0,    0x40, 0,  0x7F, 0xFF, 0, 3,    0xAA, 0xBB, 0xCC, 0,  0,
    };
    TrueTypeCompositeInfo info = {0};
    components[3].glyphIndex = 79u;
    Expect(TrueTypeCompositeRead((TrueTypeSpan){glyph, sizeof(glyph)}, Storage(), &info),
           "Three components with instructions and padding read");
    Expect(info.componentCount == 3u && info.instructions.bytes == glyph + 42u &&
               info.instructions.size == 3u && info.instructions.bytes[2] == 0xCCu,
           "Instructions from an early component borrow the input span");
    Expect(ComponentEqual(components[0],
                          (TrueTypeComponent){31, 0x727, -32768, 32767, 16384, 0, 0, 16384}),
           "Word XY arguments are signed and absent transform is identity");
    Expect(
        ComponentEqual(components[1], (TrueTypeComponent){47, 0x20, 128, 255, 16384, 0, 0, 16384}),
        "Byte point numbers are unsigned");
    Expect(ComponentEqual(components[2], (TrueTypeComponent){65535, 0x81, 32768, 65535, 8192, -8192,
                                                             16384, 32767}),
           "Word point numbers and nonsymmetric F2Dot14 matrix preserve wire order");
    Expect(components[3].glyphIndex == 79u, "Only declared components are written");
    uint8_t unaligned[48];
    for (uint32_t index = 0u; index < sizeof(glyph); ++index)
        unaligned[index + 1u] = glyph[index];
    Expect(TrueTypeCompositeRead((TrueTypeSpan){unaligned + 1u, sizeof(glyph)}, Storage(), &info) &&
               components[2].scale01 == -8192 && info.instructions.bytes == unaligned + 43u,
           "Packed records do not require alignment");
}

static void TestTransforms(void)
{
    uint8_t glyph[26] = {0xFF, 0xFF};
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_SCALE);
    Put16(glyph + 12u, 5u);
    glyph[14] = 0x80u;
    glyph[15] = 0x7Fu;
    Put16(glyph + 16u, 0x8000u);
    TrueTypeCompositeInfo info = {0};
    Expect(TrueTypeCompositeRead((TrueTypeSpan){glyph, 18u}, Storage(), &info) &&
               components[0].argument1 == -128 && components[0].argument2 == 127 &&
               components[0].xScale == -32768 && components[0].yScale == -32768 &&
               components[0].scale01 == 0 && components[0].scale10 == 0 &&
               info.instructions.bytes == NULL && info.instructions.size == 0u,
           "Byte XY and one raw negative scale read");
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_WORD_ARGUMENTS | TRUE_TYPE_COMPONENT_XY_ARGUMENTS |
                           TRUE_TYPE_COMPONENT_XY_SCALE | TRUE_TYPE_COMPONENT_INSTRUCTIONS);
    Put16(glyph + 14u, 0x8000u);
    Put16(glyph + 16u, 0xFFFFu);
    Put16(glyph + 18u, 0x7FFFu);
    Put16(glyph + 20u, 0u);
    Expect(TrueTypeCompositeRead((TrueTypeSpan){glyph, 24u}, Storage(), &info) &&
               components[0].argument1 == -32768 && components[0].argument2 == -1 &&
               components[0].xScale == 32767 && components[0].yScale == 0 &&
               components[0].scale01 == 0 && components[0].scale10 == 0 &&
               info.instructions.bytes == glyph + 24u && info.instructions.size == 0u,
           "Separate x/y scales and zero-length instructions read");
    Put16(glyph, 0x8000u);
    Expect(TrueTypeCompositeRead((TrueTypeSpan){glyph, 24u}, Storage(), &info),
           "Every negative contour count denotes a composite");
}

static void TestIgnoredPointFlags(void)
{
    uint8_t glyph[22] = {0xFF, 0xFF};
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_MORE);
    Put16(glyph + 12u, 1u);
    Put16(glyph + 18u, 2u);
    glyph[20] = 0x80u;
    glyph[21] = 0xFFu;
    TrueTypeCompositeInfo info = {0};
    for (uint32_t bit = TRUE_TYPE_COMPONENT_SCALED_OFFSET;
         bit <= TRUE_TYPE_COMPONENT_UNSCALED_OFFSET; bit *= 2u)
    {
        uint32_t flags = bit | TRUE_TYPE_COMPONENT_ROUND_XY;
        Put16(glyph + 16u, flags);
        Expect(TrueTypeCompositeRead((TrueTypeSpan){glyph, sizeof(glyph)}, Storage(), &info) &&
                   components[1].flags == flags && components[1].argument1 == 128 &&
                   components[1].argument2 == 255,
               "Point records preserve flags whose effect is ignored by the resolver");
    }
}

static void FillSentinels(void)
{
    for (uint32_t index = 0u; index < 1024u; ++index)
        components[index] = (TrueTypeComponent){9, 17, 23, 29, 31, 37, 41, 43};
}

static void CheckSentinels(void)
{
    bool unchanged = true;
    for (uint32_t index = 0u; index < 1024u; ++index)
        unchanged = unchanged && ComponentEqual(components[index],
                                                (TrueTypeComponent){9, 17, 23, 29, 31, 37, 41, 43});
    Expect(unchanged, "Rejected input preserves every caller component");
}

static void ExpectRejected(TrueTypeSpan glyph, TrueTypeComponentStorage storage)
{
    static const uint8_t marker = 0u;
    TrueTypeCompositeInfo info = {71u, {&marker, 73u}};
    FillSentinels();
    Expect(!TrueTypeCompositeRead(glyph, storage, &info), "Unsupported composite is rejected");
    Expect(info.componentCount == 71u && info.instructions.bytes == &marker &&
               info.instructions.size == 73u,
           "Rejected input preserves component count and instructions span");
    CheckSentinels();
}

static void TestRejected(void)
{
    uint8_t glyph[32] = {0xFF, 0xFF};
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_MORE);
    Put16(glyph + 16u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_INSTRUCTIONS);
    Put16(glyph + 22u, 2u);
    for (size_t size = 0u; size < 26u; ++size)
        ExpectRejected((TrueTypeSpan){glyph, size}, Storage());
    ExpectRejected((TrueTypeSpan){NULL, 26u}, Storage());
    ExpectRejected((TrueTypeSpan){glyph, 26u}, (TrueTypeComponentStorage){components, 1u});
    ExpectRejected((TrueTypeSpan){glyph, 26u}, (TrueTypeComponentStorage){NULL, 2u});
    FillSentinels();
    Expect(!TrueTypeCompositeRead((TrueTypeSpan){glyph, 26u}, Storage(), NULL),
           "Null output is rejected");
    CheckSentinels();
    Put16(glyph, 0u);
    ExpectRejected((TrueTypeSpan){glyph, 26u}, Storage());
    Put16(glyph, 1u);
    ExpectRejected((TrueTypeSpan){glyph, 26u}, Storage());
    Put16(glyph, 0xFFFFu);
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_MORE);
    ExpectRejected((TrueTypeSpan){glyph, 26u}, Storage());
    Put16(glyph + 10u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_MORE);
    const uint32_t rejected[] = {
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | 0x10u,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | 0x2000u,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | 0x4000u,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | 0x8000u,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_SCALE | TRUE_TYPE_COMPONENT_XY_SCALE,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_SCALE | TRUE_TYPE_COMPONENT_MATRIX,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_XY_SCALE |
            TRUE_TYPE_COMPONENT_MATRIX,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_SCALED_OFFSET |
            TRUE_TYPE_COMPONENT_UNSCALED_OFFSET,
        TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_OVERLAP,
    };
    for (uint32_t index = 0u; index < sizeof(rejected) / sizeof(rejected[0]); ++index)
    {
        Put16(glyph + 16u, rejected[index]);
        ExpectRejected((TrueTypeSpan){glyph, sizeof(glyph)}, Storage());
    }
}

static void TestMaximumComponents(void)
{
    for (uint32_t index = 0u; index < sizeof(maximumGlyph); ++index)
        maximumGlyph[index] = 0u;
    Put16(maximumGlyph, 0xFFFFu);
    for (uint32_t index = 0u; index < 1024u; ++index)
    {
        size_t position = 10u + (size_t)index * 6u;
        Put16(maximumGlyph + position,
              TRUE_TYPE_COMPONENT_XY_ARGUMENTS | (index == 1023u ? 0u : TRUE_TYPE_COMPONENT_MORE));
        Put16(maximumGlyph + position + 2u, index);
    }
    TrueTypeCompositeInfo info = {0};
    Expect(TrueTypeCompositeRead((TrueTypeSpan){maximumGlyph, 6154u}, Storage(), &info) &&
               info.componentCount == 1024u && components[1023].glyphIndex == 1023u,
           "The exact component limit reads");
    Put16(maximumGlyph + 6148u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS | TRUE_TYPE_COMPONENT_MORE);
    Put16(maximumGlyph + 6154u, TRUE_TYPE_COMPONENT_XY_ARGUMENTS);
    ExpectRejected((TrueTypeSpan){maximumGlyph, 6160u}, Storage());
}

LAIUE_TEST_ENTRY(TrueTypeComponentsTestEntryPoint)
{
    TestRecords();
    TestTransforms();
    TestIgnoredPointFlags();
    TestRejected();
    TestMaximumComponents();
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LaiueTestRuntimeWrite("TrueType component records OK\n");
    LAIUE_TEST_SUCCESS();
}
