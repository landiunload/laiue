#include "ui/truetype_outline.h"
#include "test_runtime.h"

static uint32_t failures;
static TrueTypeDesignPoint points[16384];
static uint16_t ends[4096];
static uint8_t largeGlyph[65680];

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

static TrueTypeOutlineStorage Storage(void)
{
    return (TrueTypeOutlineStorage){points, ends, 16384u, 4096u};
}

static void BuildGlyph(uint8_t *bytes)
{
    for (uint32_t index = 0u; index < 40u; ++index)
        bytes[index] = 0u;
    Put16(bytes, 2u);
    Put16(bytes + 10u, 2u);
    Put16(bytes + 12u, 5u);
    Put16(bytes + 14u, 2u);
    bytes[16] = 0xAAu;
    bytes[17] = 0xBBu;
    // Шесть точек: positive/negative short, unchanged и signed16 deltas.
    const uint8_t flags[] = {0x17u, 0x26u, 0x31u, 0x09u, 1u, 0x20u};
    for (uint32_t index = 0u; index < 6u; ++index)
        bytes[18u + index] = flags[index];
    bytes[24] = 10u;
    bytes[25] = 5u;
    Put16(bytes + 26u, 300u);
    Put16(bytes + 28u, 0x8000u);
    Put16(bytes + 30u, 0x7FFFu);
    bytes[32] = 20u;
    bytes[33] = 40u;
    Put16(bytes + 34u, 0xFED4u);
    Put16(bytes + 36u, 0x7FFFu);
}

static void TestCoordinates(void)
{
    uint8_t bytes[40];
    BuildGlyph(bytes);
    TrueTypeOutlineCounts result = {0};
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){bytes, sizeof(bytes)}, Storage(), &result),
           "Два контура с instructions и padding читаются");
    const int32_t x[] = {10, 5, 5, 305, -32463, 304};
    const int32_t y[] = {-20, 20, 20, -280, 32487, 32487};
    Expect(result.pointCount == 6u && result.contourCount == 2u && ends[0] == 2u && ends[1] == 5u,
           "Контуры сохраняют последовательные индексы точек");
    for (uint32_t index = 0u; index < 6u; ++index)
        Expect(points[index].x == x[index] && points[index].y == y[index] &&
                   points[index].onCurve == (index != 1u && index != 5u),
               "Packing восстанавливает design coordinates и on/off-curve");
    // Начальный OVERLAP_SIMPLE не меняет порядок и координаты.
    bytes[18] |= 0x40u;
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){bytes, 38u}, Storage(), &result) &&
               points[0].x == 10 && points[5].y == 32487,
           "OVERLAP_SIMPLE и отсутствие padding допустимы");
    uint8_t unaligned[41];
    for (uint32_t index = 0u; index < 40u; ++index)
        unaligned[index + 1u] = bytes[index];
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){unaligned + 1u, 40u}, Storage(), &result) &&
               points[3].x == 305,
           "Чтение big-endian не требует выравнивания");
}

static void ExpectRejected(TrueTypeSpan glyph, TrueTypeOutlineStorage storage)
{
    TrueTypeOutlineCounts result = {71u, 73u};
    points[0] = (TrueTypeDesignPoint){79, 83, true};
    points[5] = (TrueTypeDesignPoint){89, 97, false};
    ends[0] = 101u;
    ends[1] = 103u;
    Expect(!TrueTypeSimpleOutlineRead(glyph, storage, &result), "Некорректный outline отклоняется");
    Expect(result.pointCount == 71u && result.contourCount == 73u && points[0].x == 79 &&
               points[0].y == 83 && points[0].onCurve && points[5].x == 89 && points[5].y == 97 &&
               !points[5].onCurve && ends[0] == 101u && ends[1] == 103u,
           "Ошибка сохраняет counts и оба caller-owned buffers");
}

static void TestRejected(void)
{
    uint8_t bytes[40];
    BuildGlyph(bytes);
    for (size_t size = 1u; size < 38u; ++size)
        ExpectRejected((TrueTypeSpan){bytes, size}, Storage());
    ExpectRejected((TrueTypeSpan){NULL, 40u}, Storage());
    TrueTypeOutlineStorage storage = Storage();
    storage.pointCapacity = 5u;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, storage);
    storage = Storage();
    storage.contourCapacity = 1u;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, storage);
    storage = Storage();
    storage.points = NULL;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, storage);
    storage = Storage();
    storage.contourEnds = NULL;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, storage);
    Expect(!TrueTypeSimpleOutlineRead((TrueTypeSpan){bytes, 40u}, Storage(), NULL),
           "Null counts отклоняется");
    Put16(bytes, 0xFFFFu);
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    Put16(bytes, 4097u);
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    BuildGlyph(bytes);
    Put16(bytes + 12u, 2u);
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    Put16(bytes + 12u, 16384u);
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    BuildGlyph(bytes);
    bytes[18] |= 0x80u;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    BuildGlyph(bytes);
    bytes[22] = 255u;
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
    BuildGlyph(bytes);
    Put16(bytes + 14u, 40u);
    ExpectRejected((TrueTypeSpan){bytes, 40u}, Storage());
}

static void TestEmpty(void)
{
    uint8_t bytes[14] = {0};
    TrueTypeOutlineCounts result = {1u, 1u};
    TrueTypeOutlineStorage empty = {0};
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){NULL, 0u}, empty, &result) &&
               result.pointCount == 0u && result.contourCount == 0u,
           "Пустой loca span не требует storage");
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){bytes, 10u}, empty, &result) &&
               result.pointCount == 0u && result.contourCount == 0u,
           "Нулевые контуры могут не содержать instructionLength");
    Put16(bytes + 10u, 2u);
    bytes[12] = 0xAAu;
    bytes[13] = 0xBBu;
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){bytes, 14u}, empty, &result),
           "Нулевые контуры могут содержать instructions");
    ExpectRejected((TrueTypeSpan){bytes, 11u}, empty);
    ExpectRejected((TrueTypeSpan){bytes, 13u}, empty);
}

static void TestMaximumPoints(void)
{
    for (size_t index = 0u; index < sizeof(largeGlyph); ++index)
        largeGlyph[index] = 0u;
    Put16(largeGlyph, 4096u);
    for (uint32_t index = 0u; index < 4096u; ++index)
        Put16(largeGlyph + 10u + (size_t)index * 2u, index);
    for (uint32_t run = 0u; run < 16u; ++run)
    {
        largeGlyph[8204u + run * 2u] = 0x39u;
        largeGlyph[8205u + run * 2u] = 255u;
    }
    TrueTypeOutlineCounts result = {0};
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){largeGlyph, 8236u}, Storage(), &result) &&
               result.pointCount == 4096u && result.contourCount == 4096u && ends[4095] == 4095u &&
               points[4095].x == 0 && points[4095].y == 0,
           "Максимальное число одноточечных контуров читается");
    for (size_t index = 0u; index < sizeof(largeGlyph); ++index)
        largeGlyph[index] = 0u;
    Put16(largeGlyph, 1u);
    Put16(largeGlyph + 10u, 16383u);
    for (uint32_t run = 0u; run < 64u; ++run)
    {
        largeGlyph[14u + run * 2u] = 9u;
        largeGlyph[15u + run * 2u] = 255u;
    }
    for (uint32_t index = 0u; index < 32768u; ++index)
        Put16(largeGlyph + 142u + (size_t)index * 2u, 0x8000u);
    Expect(TrueTypeSimpleOutlineRead((TrueTypeSpan){largeGlyph, 65678u}, Storage(), &result) &&
               result.pointCount == 16384u && result.contourCount == 1u && ends[0] == 16383u &&
               points[0].x == -32768 && points[16383].x == -536870912 &&
               points[16383].y == -536870912 && points[16383].onCurve,
           "Максимальные repeat runs и сумма signed deltas помещаются в int32");
}

LAIUE_TEST_ENTRY(TrueTypeOutlineTestEntryPoint)
{
    TestCoordinates();
    TestRejected();
    TestEmpty();
    TestMaximumPoints();
    if (failures == 0u)
        LaiueTestRuntimeWrite("TrueType simple outlines OK\n");
    if (failures != 0u)
        LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
