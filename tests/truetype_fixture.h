#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Собственный минимальный SFNT: пять глифов, из них .notdef и третий пустые.
// Непустые glyf содержат заголовок без контуров и нулевую instructionLength.
// Структура таблиц: https://learn.microsoft.com/typography/opentype/spec/otff
enum
{
    TRUE_TYPE_FIXTURE_CAPACITY = 448,
    TRUE_TYPE_FIXTURE_HEAD = 124,
    TRUE_TYPE_FIXTURE_HHEA = 180,
    TRUE_TYPE_FIXTURE_MAXP = 216,
    TRUE_TYPE_FIXTURE_CMAP = 248,
    TRUE_TYPE_FIXTURE_CMAP_RECORD = 0,
    TRUE_TYPE_FIXTURE_HEAD_RECORD = 2,
    TRUE_TYPE_FIXTURE_HMTX_RECORD = 4,
};

typedef struct TrueTypeFixture
{
    uint8_t bytes[TRUE_TYPE_FIXTURE_CAPACITY];
    size_t size;
    uint32_t locaOffset;
    uint32_t glyfOffset;
    uint32_t hmtxOffset;
} TrueTypeFixture;

static void TrueTypeFixturePut16(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 8u);
    bytes[1] = (uint8_t)value;
}

static void TrueTypeFixturePut32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static void TrueTypeFixtureTable(uint8_t *bytes, uint32_t index, const char *tag, uint32_t offset,
                                 uint32_t size)
{
    uint8_t *record = bytes + 12u + index * 16u;
    for (uint32_t i = 0u; i < 4u; ++i)
        record[i] = (uint8_t)tag[i];
    TrueTypeFixturePut32(record + 8u, offset);
    TrueTypeFixturePut32(record + 12u, size);
}

static void TrueTypeFixtureCmap4(uint8_t *bytes)
{
    TrueTypeFixturePut16(bytes, 4u);
    TrueTypeFixturePut16(bytes + 2u, 52u);
    TrueTypeFixturePut16(bytes + 6u, 8u);
    TrueTypeFixturePut16(bytes + 8u, 8u);
    TrueTypeFixturePut16(bytes + 10u, 2u);
    // endCode, reservedPad, startCode, idDelta, idRangeOffset, glyphIdArray.
    const uint16_t values[] = {
        0x0042u, 0x00D8u, 0x0410u, 0xFFFFu, 0u, 0x0041u, 0x00D7u, 0x0410u, 0xFFFFu, 0xFFC0u,
        1u,      0xFBF3u, 1u,      0u,      6u, 0u,      0u,      2u,      0u,
    };
    for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
        TrueTypeFixturePut16(bytes + 14u + i * 2u, values[i]);
}

static void TrueTypeFixtureCmap12(uint8_t *bytes)
{
    TrueTypeFixturePut16(bytes, 12u);
    TrueTypeFixturePut32(bytes + 4u, 52u);
    TrueTypeFixturePut32(bytes + 12u, 3u);
    const uint32_t groups[] = {
        0x0041u, 0x0042u, 1u, 0x0410u, 0x0410u, 3u, 0x1F600u, 0x1F600u, 4u,
    };
    for (size_t i = 0u; i < sizeof(groups) / sizeof(groups[0]); ++i)
        TrueTypeFixturePut32(bytes + 16u + i * 4u, groups[i]);
}

static void TrueTypeFixtureBuild(TrueTypeFixture *fixture, bool fullUnicode, bool longLoca)
{
    for (size_t i = 0u; i < sizeof(fixture->bytes); ++i)
        fixture->bytes[i] = 0u;
    uint8_t *bytes = fixture->bytes;
    uint32_t cmapBytes = fullUnicode ? 124u : 64u;
    uint32_t locaBytes = longLoca ? 24u : 12u;
    fixture->locaOffset = TRUE_TYPE_FIXTURE_CMAP + cmapBytes;
    fixture->glyfOffset = fixture->locaOffset + locaBytes;
    fixture->hmtxOffset = fixture->glyfOffset + 36u;
    fixture->size = fixture->hmtxOffset + 14u;

    TrueTypeFixturePut32(bytes, 0x00010000u);
    TrueTypeFixturePut16(bytes + 4u, 7u);
    TrueTypeFixturePut16(bytes + 6u, 64u);
    TrueTypeFixturePut16(bytes + 8u, 2u);
    TrueTypeFixturePut16(bytes + 10u, 48u);
    TrueTypeFixtureTable(bytes, 0u, "cmap", TRUE_TYPE_FIXTURE_CMAP, cmapBytes);
    TrueTypeFixtureTable(bytes, 1u, "glyf", fixture->glyfOffset, 36u);
    TrueTypeFixtureTable(bytes, 2u, "head", TRUE_TYPE_FIXTURE_HEAD, 54u);
    TrueTypeFixtureTable(bytes, 3u, "hhea", TRUE_TYPE_FIXTURE_HHEA, 36u);
    TrueTypeFixtureTable(bytes, 4u, "hmtx", fixture->hmtxOffset, 14u);
    TrueTypeFixtureTable(bytes, 5u, "loca", fixture->locaOffset, locaBytes);
    TrueTypeFixtureTable(bytes, 6u, "maxp", TRUE_TYPE_FIXTURE_MAXP, 32u);

    uint8_t *head = bytes + TRUE_TYPE_FIXTURE_HEAD;
    TrueTypeFixturePut32(head, 0x00010000u);
    TrueTypeFixturePut32(head + 12u, 0x5F0F3CF5u);
    TrueTypeFixturePut16(head + 18u, 1000u);
    TrueTypeFixturePut16(head + 50u, longLoca ? 1u : 0u);
    uint8_t *hhea = bytes + TRUE_TYPE_FIXTURE_HHEA;
    TrueTypeFixturePut32(hhea, 0x00010000u);
    TrueTypeFixturePut16(hhea + 4u, 800u);
    TrueTypeFixturePut16(hhea + 6u, 0xFF38u);
    TrueTypeFixturePut16(hhea + 8u, 100u);
    TrueTypeFixturePut16(hhea + 34u, 2u);
    TrueTypeFixturePut32(bytes + TRUE_TYPE_FIXTURE_MAXP, 0x00010000u);
    TrueTypeFixturePut16(bytes + TRUE_TYPE_FIXTURE_MAXP + 4u, 5u);

    uint8_t *cmap = bytes + TRUE_TYPE_FIXTURE_CMAP;
    TrueTypeFixturePut16(cmap + 2u, fullUnicode ? 2u : 1u);
    TrueTypeFixturePut16(cmap + 4u, 3u);
    TrueTypeFixturePut16(cmap + 6u, 1u);
    TrueTypeFixturePut32(cmap + 8u, fullUnicode ? 20u : 12u);
    TrueTypeFixtureCmap4(cmap + (fullUnicode ? 20u : 12u));
    if (fullUnicode)
    {
        // BMP mappings дополнительных Unicode subtables должны совпадать.
        TrueTypeFixturePut16(cmap + 20u + 48u, 0u);
        TrueTypeFixturePut16(cmap + 12u, 3u);
        TrueTypeFixturePut16(cmap + 14u, 10u);
        TrueTypeFixturePut32(cmap + 16u, 72u);
        TrueTypeFixtureCmap12(cmap + 72u);
    }

    const uint32_t offsets[] = {0u, 0u, 12u, 24u, 24u, 36u};
    for (size_t i = 0u; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
    {
        if (longLoca)
            TrueTypeFixturePut32(bytes + fixture->locaOffset + i * 4u, offsets[i]);
        else
            TrueTypeFixturePut16(bytes + fixture->locaOffset + i * 2u, offsets[i] / 2u);
    }
    uint8_t *hmtx = bytes + fixture->hmtxOffset;
    TrueTypeFixturePut16(hmtx, 400u);
    TrueTypeFixturePut16(hmtx + 4u, 600u);
    TrueTypeFixturePut16(hmtx + 6u, 0xFFECu);
    TrueTypeFixturePut16(hmtx + 8u, 30u);
    TrueTypeFixturePut16(hmtx + 10u, 0xFFD8u);
    TrueTypeFixturePut16(hmtx + 12u, 50u);
}
