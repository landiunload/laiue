#include "ui/truetype_reader.h"
#include "test_runtime.h"
#include "truetype_fixture.h"

static uint32_t failureCount;
static TrueTypeFixture fixture;
static uint8_t unalignedBytes[TRUE_TYPE_FIXTURE_CAPACITY + 1u];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failureCount;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static bool SameSpan(TrueTypeSpan left, TrueTypeSpan right)
{
    return left.bytes == right.bytes && left.size == right.size;
}

static bool SameFont(const TrueTypeFont *left, const TrueTypeFont *right)
{
    return SameSpan(left->cmap, right->cmap) && SameSpan(left->glyf, right->glyf) &&
           SameSpan(left->loca, right->loca) && SameSpan(left->hmtx, right->hmtx) &&
           SameSpan(left->vmtx, right->vmtx) && left->numVMetrics == right->numVMetrics &&
           left->numGlyphs == right->numGlyphs && left->numHMetrics == right->numHMetrics &&
           left->locaWidth == right->locaWidth && left->cmapFormat == right->cmapFormat &&
           left->unitsPerEm == right->unitsPerEm && left->ascent == right->ascent &&
           left->descent == right->descent && left->lineGap == right->lineGap;
}

static void ExpectGlyph(const TrueTypeFont *font, uint32_t codepoint, uint16_t expected)
{
    uint16_t glyph = UINT16_MAX;
    Expect(TrueTypeReaderGlyphIndex(font, codepoint, &glyph) && glyph == expected,
           "Unicode lookup возвращает ожидаемый индекс глифа");
}

static void CheckMetricsAndSpans(const TrueTypeFont *font, const uint8_t *bytes)
{
    Expect(font->numGlyphs == 5u && font->numHMetrics == 2u && font->unitsPerEm == 1000u &&
               font->ascent == 800 && font->descent == -200 && font->lineGap == 100,
           "reader сохраняет целые метрики и число глифов");
    Expect(font->numVMetrics == 0u && font->vmtx.bytes == NULL && font->vmtx.size == 0u,
           "отсутствующая пара vertical tables явно представлена нулевым numVMetrics");
    Expect(font->glyf.bytes == bytes + fixture.glyfOffset && font->glyf.size == 36u &&
               font->hmtx.bytes == bytes + fixture.hmtxOffset && font->hmtx.size == 14u &&
               font->loca.bytes == bytes + fixture.locaOffset,
           "табличные spans ссылаются на исходный буфер");
    const int16_t bearings[] = {0, -20, 30, -40, 50};
    const uint32_t starts[] = {0u, 0u, 12u, 24u, 24u};
    const uint32_t sizes[] = {0u, 12u, 12u, 0u, 12u};
    for (uint16_t glyph = 0u; glyph < 5u; ++glyph)
    {
        TrueTypeHorizontalMetric metric = {0};
        Expect(TrueTypeReaderHorizontalMetric(font, glyph, &metric) &&
                   metric.advance == (glyph == 0u ? 400u : 600u) &&
                   metric.bearing == bearings[glyph],
               "hmtx повторяет последний advance и читает отдельные signed bearings");
        TrueTypeSpan span = {0};
        Expect(TrueTypeReaderGlyphSpan(font, glyph, &span) &&
                   span.bytes == bytes + fixture.glyfOffset + starts[glyph] &&
                   span.size == sizes[glyph],
               "loca возвращает точный span, включая пустой глиф");
    }
}

static void TestValidFonts(void)
{
    for (uint32_t combination = 0u; combination < 4u; ++combination)
    {
        bool fullUnicode = (combination & 1u) != 0u;
        bool longLoca = (combination & 2u) != 0u;
        TrueTypeFixtureBuild(&fixture, fullUnicode, longLoca);
        TrueTypeFont font = {0};
        if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
        {
            Expect(false, "корректный fixture принимается во всех сочетаниях cmap и loca");
            continue;
        }
        Expect(font.cmapFormat == (fullUnicode ? 12u : 4u) &&
                   font.locaWidth == (longLoca ? 4u : 2u),
               "format12 имеет приоритет, ширина loca соответствует head");
        CheckMetricsAndSpans(&font, fixture.bytes);
        ExpectGlyph(&font, 0x0041u, 1u);
        ExpectGlyph(&font, 0x0042u, 2u);
        ExpectGlyph(&font, 0x0410u, 3u);
        ExpectGlyph(&font, 0x0040u, 0u);
        ExpectGlyph(&font, 0x0043u, 0u);
        ExpectGlyph(&font, 0xFFFFu, 0u);
        ExpectGlyph(&font, 0x10FFFFu, 0u);
        ExpectGlyph(&font, 0x00D7u, fullUnicode ? 0u : 3u);
        ExpectGlyph(&font, 0x00D8u, 0u);
        ExpectGlyph(&font, 0x1F600u, fullUnicode ? 4u : 0u);
    }

    TrueTypeFixtureBuild(&fixture, false, false);
    // Unicode platform 0 принимается без Microsoft encoding record.
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 4u, 0u);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 6u, 3u);
    TrueTypeFont font = {0};
    Expect(TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font),
           "Unicode platform 0 поддерживается");
    ExpectGlyph(&font, 0x0410u, 3u);

    TrueTypeFixtureBuild(&fixture, true, true);
    for (size_t i = 0u; i < fixture.size; ++i)
        unalignedBytes[i + 1u] = fixture.bytes[i];
    Expect(TrueTypeReaderInitialize(unalignedBytes + 1u, fixture.size, &font),
           "big-endian reader допускает невыровненный исходный буфер");
    CheckMetricsAndSpans(&font, unalignedBytes + 1u);
    ExpectGlyph(&font, 0x1F600u, 4u);
}

static void ExpectRejected(const TrueTypeFont *previous)
{
    TrueTypeFont output = *previous;
    Expect(!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &output) &&
               SameFont(&output, previous),
           "некорректный fixture отклоняется без изменения прежнего reader");
}

static void TestUnsupportedAndTruncated(void)
{
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFont previous = {0};
    if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &previous))
    {
        Expect(false, "fixture для отказов изначально корректен");
        return;
    }
    for (size_t size = 0u; size < fixture.size; ++size)
    {
        TrueTypeFont output = previous;
        if (TrueTypeReaderInitialize(fixture.bytes, size, &output) || !SameFont(&output, &previous))
        {
            Expect(false, "усечённый SFNT отклоняется без частичного результата");
            break;
        }
    }
    TrueTypeFont output = previous;
    Expect(!TrueTypeReaderInitialize(NULL, fixture.size, &output) && SameFont(&output, &previous),
           "NULL-буфер отклоняется без изменения результата");
    Expect(!TrueTypeReaderInitialize(fixture.bytes, fixture.size, NULL),
           "NULL-результат отклоняется");

    TrueTypeFixturePut32(fixture.bytes, 0x4F54544Fu);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 12u, 6u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 4u, 1u);
    ExpectRejected(&previous);
}

static void TestInvalidTables(void)
{
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFont previous = {0};
    if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &previous))
    {
        Expect(false, "fixture для табличных ошибок изначально корректен");
        return;
    }

    TrueTypeFixturePut32(fixture.bytes + 12u + (size_t)TRUE_TYPE_FIXTURE_HMTX_RECORD * 16u + 12u,
                         13u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut32(fixture.bytes + 12u + (size_t)TRUE_TYPE_FIXTURE_CMAP_RECORD * 16u + 8u,
                         UINT32_MAX);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    fixture.bytes[12u + TRUE_TYPE_FIXTURE_HEAD_RECORD * 16u] = (uint8_t)'x';
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_HEAD + 18u, 0u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_HEAD + 50u, 2u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_HHEA + 34u, 0u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_HHEA + 34u, 6u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + fixture.locaOffset + 6u, 5u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixturePut32(fixture.bytes + fixture.locaOffset + 20u, 37u);
    ExpectRejected(&previous);

    TrueTypeFixtureBuild(&fixture, false, false);
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 12u + 42u, 0xFFFEu);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixturePut32(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 72u + 4u, 51u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixturePut32(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 72u + 28u, 0x0042u);
    ExpectRejected(&previous);
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixturePut32(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 72u + 24u, 4u);
    ExpectRejected(&previous);
}

static void TestInvalidLookups(void)
{
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFont font = {0};
    if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
    {
        Expect(false, "fixture для границ lookup изначально корректен");
        return;
    }
    uint16_t glyph = 17u;
    Expect(!TrueTypeReaderGlyphIndex(&font, 0x110000u, &glyph) && glyph == 17u &&
               !TrueTypeReaderGlyphIndex(&font, 0xD800u, &glyph) && glyph == 17u &&
               !TrueTypeReaderGlyphIndex(&font, 0xDFFFu, &glyph) && glyph == 17u,
           "невалидные Unicode scalar values не меняют результат lookup");
    TrueTypeSpan span = {fixture.bytes, 7u};
    Expect(!TrueTypeReaderGlyphSpan(&font, 5u, &span) && span.bytes == fixture.bytes &&
               span.size == 7u,
           "индекс за концом loca не меняет span");
    TrueTypeHorizontalMetric metric = {17u, -17};
    Expect(!TrueTypeReaderHorizontalMetric(&font, 5u, &metric) && metric.advance == 17u &&
               metric.bearing == -17,
           "индекс за концом hmtx не меняет метрики");

    TrueTypeFixtureBuild(&fixture, false, false);
    // Структура cmap4 корректна, но delta для A даёт индекс 5 при пяти глифах.
    TrueTypeFixturePut16(fixture.bytes + TRUE_TYPE_FIXTURE_CMAP + 12u + 32u, 0xFFC4u);
    if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
    {
        Expect(false, "структурно корректный cmap4 принимается до адресного lookup");
        return;
    }
    glyph = 17u;
    Expect(!TrueTypeReaderGlyphIndex(&font, 0x0041u, &glyph) && glyph == 17u,
           "повреждённый mapping отклоняется, сохраняя результат lookup");
}

static bool SameHeader(TrueTypeGlyphHeader left, TrueTypeGlyphHeader right)
{
    return left.contourCount == right.contourCount && left.xMin == right.xMin &&
           left.yMin == right.yMin && left.xMax == right.xMax && left.yMax == right.yMax;
}

static void ExpectHeaderRejected(const TrueTypeFont *font, uint16_t glyph)
{
    TrueTypeGlyphHeader previous = {17, 18, 19, 20, 21};
    TrueTypeGlyphHeader output = previous;
    Expect(!TrueTypeReaderGlyphHeader(font, glyph, &output) && SameHeader(output, previous),
           "отказ чтения заголовка сохраняет все поля результата");
}

static void TestGlyphHeaders(void)
{
    const TrueTypeGlyphHeader headers[] = {
        {0, INT16_MIN, -200, INT16_MAX, 800},
        {1, 7, -17, 7, -17},
        {INT16_MIN, -500, INT16_MIN, 300, INT16_MAX},
    };
    const uint16_t glyphs[] = {1u, 2u, 4u};
    for (uint32_t combination = 0u; combination < 4u; ++combination)
    {
        TrueTypeFixtureBuild(&fixture, (combination & 1u) != 0u, (combination & 2u) != 0u);
        for (size_t index = 0u; index < 3u; ++index)
        {
            uint8_t *bytes = fixture.bytes + fixture.glyfOffset + index * 12u;
            TrueTypeFixturePut16(bytes, (uint16_t)headers[index].contourCount);
            TrueTypeFixturePut16(bytes + 2u, (uint16_t)headers[index].xMin);
            TrueTypeFixturePut16(bytes + 4u, (uint16_t)headers[index].yMin);
            TrueTypeFixturePut16(bytes + 6u, (uint16_t)headers[index].xMax);
            TrueTypeFixturePut16(bytes + 8u, (uint16_t)headers[index].yMax);
        }
        TrueTypeFont font = {0};
        if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
        {
            Expect(false, "шрифт с signed glyph headers принимается");
            continue;
        }
        for (size_t index = 0u; index < 3u; ++index)
        {
            TrueTypeGlyphHeader header = {0};
            Expect(TrueTypeReaderGlyphHeader(&font, glyphs[index], &header) &&
                       SameHeader(header, headers[index]),
                   "glyph header сохраняет signed contourCount и bbox без чтения тела");
        }
        for (uint16_t glyph = 0u; glyph <= 3u; glyph += 3u)
        {
            TrueTypeGlyphHeader header = {17, 18, 19, 20, 21};
            Expect(TrueTypeReaderGlyphHeader(&font, glyph, &header) &&
                       SameHeader(header, (TrueTypeGlyphHeader){0}),
                   "пустой loca span возвращает нулевой заголовок");
        }
        ExpectHeaderRejected(&font, 5u);
        ExpectHeaderRejected(NULL, 0u);
        Expect(!TrueTypeReaderGlyphHeader(&font, 1u, NULL), "NULL-заголовок отклоняется");
    }

    for (uint32_t longLoca = 0u; longLoca <= 1u; ++longLoca)
    {
        for (uint32_t truncated = 0u; truncated <= 1u; ++truncated)
        {
            TrueTypeFixtureBuild(&fixture, false, longLoca != 0u);
            uint32_t size = truncated == 0u ? 10u : (longLoca != 0u ? 9u : 8u);
            if (longLoca != 0u)
                TrueTypeFixturePut32(fixture.bytes + fixture.locaOffset + 8u, size);
            else
                TrueTypeFixturePut16(fixture.bytes + fixture.locaOffset + 4u, size / 2u);
            TrueTypeFont font = {0};
            if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
            {
                Expect(false, "loca допускает glyph span короче заголовка до его чтения");
                continue;
            }
            if (truncated != 0u)
                ExpectHeaderRejected(&font, 1u);
            else
            {
                TrueTypeGlyphHeader header = {17, 18, 19, 20, 21};
                Expect(TrueTypeReaderGlyphHeader(&font, 1u, &header) &&
                           SameHeader(header, (TrueTypeGlyphHeader){0}),
                       "ровно десяти байтов достаточно для заголовка");
            }
        }
    }
    for (size_t axis = 0u; axis < 2u; ++axis)
    {
        TrueTypeFixtureBuild(&fixture, false, false);
        TrueTypeFixturePut16(fixture.bytes + fixture.glyfOffset + 2u + axis * 2u, 1u);
        TrueTypeFont font = {0};
        Expect(TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font),
               "bbox проверяется при адресном чтении заголовка");
        ExpectHeaderRejected(&font, 1u);
    }
}

static void TestVerticalMetrics(void)
{
    const uint32_t counts[] = {1u, 2u, 5u};
    const int16_t bearings[] = {INT16_MIN, -20, 30, -40, INT16_MAX};
    for (uint32_t combination = 0u; combination < 4u; ++combination)
    {
        for (uint32_t version = 0u; version < 2u; ++version)
        {
            for (size_t index = 0u; index < 3u; ++index)
            {
                TrueTypeFixtureBuild(&fixture, (combination & 1u) != 0u, (combination & 2u) != 0u);
                uint32_t count = counts[index];
                TrueTypeFixtureAddVertical(&fixture, version == 0u ? 0x00010000u : 0x00011000u,
                                           count);
                if (version != 0u)
                    TrueTypeFixturePut16(fixture.bytes + fixture.vheaOffset + 8u, 0xFF9Cu);
                TrueTypeFont font = {0};
                if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font))
                {
                    Expect(false, "обе версии vhea и compressed vmtx принимаются");
                    continue;
                }
                Expect(font.numVMetrics == count &&
                           font.vmtx.bytes == fixture.bytes + fixture.vmtxOffset &&
                           font.vmtx.size == (size_t)count * 4u + (size_t)(5u - count) * 2u,
                       "vmtx span заимствован и число long metrics сохранено");
                for (uint16_t glyph = 0u; glyph < 5u; ++glyph)
                {
                    TrueTypeVerticalMetric metric = {0};
                    uint32_t expected = 1000u + (glyph < count ? glyph : count - 1u) * 100u;
                    Expect(TrueTypeReaderVerticalMetric(&font, glyph, &metric) &&
                               metric.advance == expected && metric.bearing == bearings[glyph],
                           "vmtx читает signed bearings и повторяет последний advanceHeight");
                }
                TrueTypeVerticalMetric metric = {17u, -17};
                Expect(!TrueTypeReaderVerticalMetric(&font, 5u, &metric) && metric.advance == 17u &&
                           metric.bearing == -17 &&
                           !TrueTypeReaderVerticalMetric(NULL, 0u, &metric) &&
                           metric.advance == 17u && metric.bearing == -17 &&
                           !TrueTypeReaderVerticalMetric(&font, 0u, NULL),
                       "невалидный vertical lookup сохраняет результат");
            }
        }
    }
    for (size_t index = 0u; index < fixture.size; ++index)
        unalignedBytes[index + 1u] = fixture.bytes[index];
    TrueTypeFont font = {0};
    TrueTypeVerticalMetric metric = {0};
    Expect(TrueTypeReaderInitialize(unalignedBytes + 1u, fixture.size, &font) &&
               TrueTypeReaderVerticalMetric(&font, 4u, &metric) && metric.advance == 1400u &&
               metric.bearing == INT16_MAX,
           "vertical tables читаются из невыровненного буфера");

    TrueTypeFixturePut16(fixture.bytes + fixture.vmtxOffset, 0u);
    TrueTypeFixturePut16(fixture.bytes + fixture.vmtxOffset + 16u, UINT16_MAX);
    TrueTypeFixturePut16(fixture.bytes + fixture.vheaOffset + 10u, UINT16_MAX);
    Expect(TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font) &&
               TrueTypeReaderVerticalMetric(&font, 0u, &metric) && metric.advance == 0u &&
               TrueTypeReaderVerticalMetric(&font, 4u, &metric) && metric.advance == UINT16_MAX,
           "advanceHeight сохраняет весь диапазон unsigned16");

    TrueTypeFixtureBuild(&fixture, false, false);
    metric = (TrueTypeVerticalMetric){17u, -17};
    Expect(TrueTypeReaderInitialize(fixture.bytes, fixture.size, &font) &&
               !TrueTypeReaderVerticalMetric(&font, 0u, &metric) && metric.advance == 17u &&
               metric.bearing == -17,
           "отсутствие vertical tables не синтезирует метрики и сохраняет output");
}

static void TestInvalidVerticalTables(void)
{
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixtureAddVertical(&fixture, 0x00011000u, 2u);
    TrueTypeFont previous = {0};
    if (!TrueTypeReaderInitialize(fixture.bytes, fixture.size, &previous))
    {
        Expect(false, "vertical fixture для отказов изначально корректен");
        return;
    }
    for (size_t record = TRUE_TYPE_FIXTURE_VHEA_RECORD; record <= TRUE_TYPE_FIXTURE_VMTX_RECORD;
         ++record)
    {
        TrueTypeFixtureBuild(&fixture, true, true);
        TrueTypeFixtureAddVertical(&fixture, 0x00011000u, 2u);
        fixture.bytes[12u + record * 16u] = (uint8_t)'x';
        ExpectRejected(&previous);
        TrueTypeFixtureBuild(&fixture, true, true);
        TrueTypeFixtureAddVertical(&fixture, 0x00011000u, 2u);
        TrueTypeFixturePut16(fixture.bytes + 4u, 10u);
        TrueTypeFixturePut16(fixture.bytes + 10u, 32u);
        for (uint32_t byte = 0u; byte < 16u; ++byte)
            fixture.bytes[12u + 9u * 16u + byte] = fixture.bytes[12u + record * 16u + byte];
        ExpectRejected(&previous);
        TrueTypeFixtureBuild(&fixture, true, true);
        TrueTypeFixtureAddVertical(&fixture, 0x00011000u, 2u);
        TrueTypeFixturePut32(fixture.bytes + 12u + record * 16u + 12u, 0u);
        ExpectRejected(&previous);
        TrueTypeFixturePut32(fixture.bytes + 12u + record * 16u + 12u,
                             record == TRUE_TYPE_FIXTURE_VHEA_RECORD ? 35u : 13u);
        ExpectRejected(&previous);
    }
    const uint32_t offsets[] = {8u, 24u, 26u, 28u, 30u, 32u, 34u, 34u};
    const uint32_t values[] = {1u, 1u, 1u, 1u, 1u, 1u, 0u, 6u};
    for (size_t index = 0u; index < sizeof(offsets) / sizeof(offsets[0]); ++index)
    {
        TrueTypeFixtureBuild(&fixture, true, true);
        TrueTypeFixtureAddVertical(&fixture, 0x00010000u, 2u);
        TrueTypeFixturePut16(fixture.bytes + fixture.vheaOffset + offsets[index], values[index]);
        ExpectRejected(&previous);
    }
    TrueTypeFixtureBuild(&fixture, true, true);
    TrueTypeFixtureAddVertical(&fixture, 0x00010001u, 2u);
    ExpectRejected(&previous);
}

LAIUE_TEST_ENTRY(TrueTypeReaderTestEntryPoint)
{
    TestValidFonts();
    TestUnsupportedAndTruncated();
    TestInvalidTables();
    TestInvalidLookups();
    TestGlyphHeaders();
    TestVerticalMetrics();
    TestInvalidVerticalTables();
    if (failureCount != 0u)
        LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
