#include "truetype_reader.h"

// Собственный reader по Microsoft OpenType, без выполнения font bytecode:
// https://learn.microsoft.com/typography/opentype/spec/otff
// https://learn.microsoft.com/typography/opentype/spec/cmap
// https://learn.microsoft.com/typography/opentype/spec/loca
// https://learn.microsoft.com/typography/opentype/spec/hmtx
// https://learn.microsoft.com/typography/opentype/spec/glyf
// https://learn.microsoft.com/typography/opentype/spec/vhea
// https://learn.microsoft.com/typography/opentype/spec/vmtx
// Пределы ограничивают проход по directory и объём входных данных.
#define TT_MAX_FONT_BYTES ((size_t)33554432u)
#define TT_MAX_TABLES 4096u
#define TT_TAG(a, b, c, d)                                                                         \
    (((uint32_t)(a) << 24u) | ((uint32_t)(b) << 16u) | ((uint32_t)(c) << 8u) | (uint32_t)(d))
static bool TtHas(TrueTypeSpan span, size_t offset, size_t count)
{
    return offset <= span.size && count <= span.size - offset;
}

// Побайтное big-endian чтение не зависит от выравнивания и порядка байтов CPU.
static uint32_t TtU16(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 8u) | bytes[1];
}

static int32_t TtI16(const uint8_t *bytes)
{
    uint32_t value = TtU16(bytes);
    return value < 32768u ? (int32_t)value : (int32_t)value - 65536;
}

static uint32_t TtU32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) |
           bytes[3];
}

static bool TtFindTable(TrueTypeSpan file, uint32_t tag, bool required, TrueTypeSpan *out)
{
    uint32_t count = TtU16(file.bytes + 4u);
    bool found = false;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint8_t *entry = file.bytes + 12u + (size_t)index * 16u;
        if (TtU32(entry) == tag)
        {
            if (found)
            {
                return false;
            }
            size_t offset = TtU32(entry + 8u);
            size_t length = TtU32(entry + 12u);
            if (!TtHas(file, offset, length))
            {
                return false;
            }
            out->bytes = file.bytes + offset;
            out->size = length;
            found = true;
        }
    }
    return found || !required;
}

static bool TtVerticalTables(TrueTypeSpan file, TrueTypeFont *font)
{
    TrueTypeSpan vhea = {0};
    if (!TtFindTable(file, TT_TAG('v', 'h', 'e', 'a'), false, &vhea) ||
        !TtFindTable(file, TT_TAG('v', 'm', 't', 'x'), false, &font->vmtx) ||
        (vhea.bytes == NULL) != (font->vmtx.bytes == NULL))
    {
        return false;
    }
    if (vhea.bytes == NULL)
    {
        return true;
    }
    if (!TtHas(vhea, 0u, 36u))
    {
        return false;
    }
    uint32_t version = TtU32(vhea.bytes);
    // В 1.0 lineGap зарезервирован, в 1.1 vertTypoLineGap имеет значение.
    if ((version != 0x00010000u && version != 0x00011000u) ||
        (version == 0x00010000u && TtU16(vhea.bytes + 8u) != 0u) || TtU32(vhea.bytes + 24u) != 0u ||
        TtU32(vhea.bytes + 28u) != 0u || TtU16(vhea.bytes + 32u) != 0u)
    {
        return false;
    }
    font->numVMetrics = TtU16(vhea.bytes + 34u);
    return font->numVMetrics != 0u && font->numVMetrics <= font->numGlyphs &&
           font->vmtx.size >=
               (size_t)font->numVMetrics * 4u + (size_t)(font->numGlyphs - font->numVMetrics) * 2u;
}

static bool TtValidateCmap4(TrueTypeSpan map)
{
    if (!TtHas(map, 0u, 16u))
    {
        return false;
    }
    uint32_t doubled = TtU16(map.bytes + 6u);
    uint32_t count = doubled / 2u;
    if (count == 0u || (doubled & 1u) != 0u || !TtHas(map, 16u, (size_t)count * 8u) ||
        TtU16(map.bytes + 14u + (size_t)count * 2u) != 0u)
    {
        return false;
    }
    uint32_t previous = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t end = TtU16(map.bytes + 14u + (size_t)index * 2u);
        uint32_t start = TtU16(map.bytes + 16u + (size_t)(count + index) * 2u);
        size_t rangePosition = 16u + (size_t)(3u * count + index) * 2u;
        uint32_t range = TtU16(map.bytes + rangePosition);
        if (start > end || (index != 0u && start <= previous) || (range & 1u) != 0u)
        {
            return false;
        }
        if (range != 0u && (rangePosition + range < 16u + (size_t)count * 8u ||
                            !TtHas(map, rangePosition + range, ((size_t)end - start + 1u) * 2u)))
        {
            return false;
        }
        previous = end;
    }
    return previous == 65535u;
}

static bool TtValidateCmap12(TrueTypeSpan map, uint32_t numGlyphs)
{
    if (!TtHas(map, 0u, 16u) || TtU16(map.bytes + 2u) != 0u)
    {
        return false;
    }
    uint32_t count = TtU32(map.bytes + 12u);
    if (count > (map.size - 16u) / 12u)
    {
        return false;
    }
    uint32_t previous = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint8_t *group = map.bytes + 16u + (size_t)index * 12u;
        uint32_t start = TtU32(group);
        uint32_t end = TtU32(group + 4u);
        uint32_t glyph = TtU32(group + 8u);
        if (start > end || end > 0x10FFFFu || (index != 0u && start <= previous) ||
            glyph >= numGlyphs || end - start >= numGlyphs - glyph)
        {
            return false;
        }
        previous = end;
    }
    return true;
}

static bool TtSelectCmap(TrueTypeSpan table, uint32_t numGlyphs, TrueTypeFont *font)
{
    if (!TtHas(table, 0u, 4u) || TtU16(table.bytes) != 0u)
    {
        return false;
    }
    uint32_t count = TtU16(table.bytes + 2u);
    if (!TtHas(table, 4u, (size_t)count * 8u))
    {
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint8_t *record = table.bytes + 4u + (size_t)index * 8u;
        uint32_t platform = TtU16(record);
        uint32_t encoding = TtU16(record + 2u);
        if (platform != 0u && (platform != 3u || (encoding != 1u && encoding != 10u)))
        {
            continue;
        }
        size_t offset = TtU32(record + 4u);
        if (!TtHas(table, offset, 4u))
        {
            return false;
        }
        uint32_t format = TtU16(table.bytes + offset);
        size_t length;
        if (format == 4u)
        {
            length = TtU16(table.bytes + offset + 2u);
        }
        else if (format == 12u)
        {
            if (!TtHas(table, offset, 16u))
            {
                return false;
            }
            length = TtU32(table.bytes + offset + 4u);
        }
        else
        {
            continue;
        }
        if (!TtHas(table, offset, length))
        {
            return false;
        }
        if (format > font->cmapFormat)
        {
            font->cmap = (TrueTypeSpan){table.bytes + offset, length};
            font->cmapFormat = format;
        }
    }
    // Records могут ссылаться на один map. Содержимое выбранного map проверяется
    // ровно один раз: повторные ссылки не умножают проход по segments/groups.
    return font->cmapFormat == 12u ? TtValidateCmap12(font->cmap, numGlyphs)
                                   : font->cmapFormat == 4u && TtValidateCmap4(font->cmap);
}

static bool TtInitialize(const uint8_t *bytes, size_t size, TrueTypeFont *font)
{
    TrueTypeSpan file = {bytes, size};
    if (bytes == NULL || size > TT_MAX_FONT_BYTES || !TtHas(file, 0u, 12u) ||
        TtU32(bytes) != 0x00010000u)
    {
        return false;
    }
    uint32_t count = TtU16(bytes + 4u);
    if (count == 0u || count > TT_MAX_TABLES || !TtHas(file, 12u, (size_t)count * 16u))
    {
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint8_t *entry = bytes + 12u + (size_t)index * 16u;
        if (!TtHas(file, TtU32(entry + 8u), TtU32(entry + 12u)))
        {
            return false;
        }
    }
    TrueTypeSpan head = {0};
    TrueTypeSpan hhea = {0};
    TrueTypeSpan maxp = {0};
    TrueTypeSpan cmap = {0};
    if (!TtFindTable(file, TT_TAG('h', 'e', 'a', 'd'), true, &head) || head.size < 54u ||
        !TtFindTable(file, TT_TAG('h', 'h', 'e', 'a'), true, &hhea) || hhea.size < 36u ||
        !TtFindTable(file, TT_TAG('m', 'a', 'x', 'p'), true, &maxp) || maxp.size < 32u ||
        !TtFindTable(file, TT_TAG('c', 'm', 'a', 'p'), true, &cmap) ||
        !TtFindTable(file, TT_TAG('l', 'o', 'c', 'a'), true, &font->loca) ||
        !TtFindTable(file, TT_TAG('g', 'l', 'y', 'f'), true, &font->glyf) ||
        !TtFindTable(file, TT_TAG('h', 'm', 't', 'x'), true, &font->hmtx))
    {
        return false;
    }
    uint32_t units = TtU16(head.bytes + 18u);
    uint32_t locaFormat = TtU16(head.bytes + 50u);
    font->numGlyphs = TtU16(maxp.bytes + 4u);
    font->numHMetrics = TtU16(hhea.bytes + 34u);
    if (TtU32(head.bytes) != 0x00010000u || TtU32(head.bytes + 12u) != 0x5F0F3CF5u ||
        TtU32(hhea.bytes) != 0x00010000u || TtU32(maxp.bytes) != 0x00010000u || units < 16u ||
        units > 16384u || locaFormat > 1u || TtU16(head.bytes + 52u) != 0u ||
        TtU16(hhea.bytes + 32u) != 0u || font->numGlyphs == 0u || font->numHMetrics == 0u ||
        font->numHMetrics > font->numGlyphs)
    {
        return false;
    }
    font->locaWidth = locaFormat == 0u ? 2u : 4u;
    if (font->loca.size < (size_t)(font->numGlyphs + 1u) * font->locaWidth ||
        font->hmtx.size <
            (size_t)font->numHMetrics * 4u + (size_t)(font->numGlyphs - font->numHMetrics) * 2u)
    {
        return false;
    }
    uint32_t previous = 0u;
    for (uint32_t index = 0u; index <= font->numGlyphs; ++index)
    {
        const uint8_t *entry = font->loca.bytes + (size_t)index * font->locaWidth;
        uint32_t offset = font->locaWidth == 2u ? TtU16(entry) * 2u : TtU32(entry);
        if (offset < previous || offset > font->glyf.size)
        {
            return false;
        }
        previous = offset;
    }
    font->unitsPerEm = (uint16_t)units;
    font->ascent = (int16_t)TtI16(hhea.bytes + 4u);
    font->descent = (int16_t)TtI16(hhea.bytes + 6u);
    font->lineGap = (int16_t)TtI16(hhea.bytes + 8u);
    return TtVerticalTables(file, font) && TtSelectCmap(cmap, font->numGlyphs, font);
}

bool TrueTypeReaderInitialize(const uint8_t *bytes, size_t size, TrueTypeFont *out)
{
    TrueTypeFont font = {0};
    if (out == NULL || !TtInitialize(bytes, size, &font))
    {
        return false;
    }
    *out = font;
    return true;
}

bool TrueTypeReaderGlyphIndex(const TrueTypeFont *font, uint32_t codepoint, uint16_t *out)
{
    if (font == NULL || out == NULL || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
    {
        return false;
    }
    TrueTypeSpan map = font->cmap;
    uint32_t glyph = 0u;
    if (font->cmapFormat == 12u)
    {
        uint32_t low = 0u;
        uint32_t high = TtU32(map.bytes + 12u);
        while (low < high)
        {
            uint32_t middle = low + (high - low) / 2u;
            const uint8_t *group = map.bytes + 16u + (size_t)middle * 12u;
            if (codepoint < TtU32(group))
            {
                high = middle;
            }
            else if (codepoint > TtU32(group + 4u))
            {
                low = middle + 1u;
            }
            else
            {
                glyph = TtU32(group + 8u) + codepoint - TtU32(group);
                break;
            }
        }
    }
    else
    {
        uint32_t count = TtU16(map.bytes + 6u) / 2u;
        uint32_t low = 0u;
        uint32_t high = count;
        while (low < high)
        {
            uint32_t middle = low + (high - low) / 2u;
            if (TtU16(map.bytes + 14u + (size_t)middle * 2u) < codepoint)
            {
                low = middle + 1u;
            }
            else
            {
                high = middle;
            }
        }
        if (low < count)
        {
            uint32_t start = TtU16(map.bytes + 16u + (size_t)(count + low) * 2u);
            if (codepoint >= start)
            {
                uint32_t delta = TtU16(map.bytes + 16u + (size_t)(2u * count + low) * 2u);
                size_t rangePosition = 16u + (size_t)(3u * count + low) * 2u;
                uint32_t range = TtU16(map.bytes + rangePosition);
                if (range == 0u)
                {
                    glyph = (codepoint + delta) & 65535u;
                }
                else
                {
                    glyph =
                        TtU16(map.bytes + rangePosition + range + (size_t)(codepoint - start) * 2u);
                    if (glyph != 0u)
                    {
                        glyph = (glyph + delta) & 65535u;
                    }
                }
            }
        }
    }
    if (glyph >= font->numGlyphs)
    {
        return false;
    }
    *out = (uint16_t)glyph;
    return true;
}

static uint32_t TtLocation(const TrueTypeFont *font, uint32_t glyph)
{
    const uint8_t *entry = font->loca.bytes + (size_t)glyph * font->locaWidth;
    return font->locaWidth == 2u ? TtU16(entry) * 2u : TtU32(entry);
}

bool TrueTypeReaderGlyphSpan(const TrueTypeFont *font, uint16_t glyph, TrueTypeSpan *out)
{
    if (font == NULL || out == NULL || glyph >= font->numGlyphs)
    {
        return false;
    }
    uint32_t start = TtLocation(font, glyph);
    uint32_t end = TtLocation(font, (uint32_t)glyph + 1u);
    *out = (TrueTypeSpan){font->glyf.bytes + start, (size_t)end - start};
    return true;
}

bool TrueTypeReaderHorizontalMetric(const TrueTypeFont *font, uint16_t glyph,
                                    TrueTypeHorizontalMetric *out)
{
    if (font == NULL || out == NULL || glyph >= font->numGlyphs)
    {
        return false;
    }
    uint32_t metric = glyph < font->numHMetrics ? glyph : font->numHMetrics - 1u;
    size_t bearingOffset = glyph < font->numHMetrics ? (size_t)glyph * 4u + 2u
                                                     : (size_t)font->numHMetrics * 4u +
                                                           ((size_t)glyph - font->numHMetrics) * 2u;
    out->advance = (uint16_t)TtU16(font->hmtx.bytes + (size_t)metric * 4u);
    out->bearing = (int16_t)TtI16(font->hmtx.bytes + bearingOffset);
    return true;
}

bool TrueTypeReaderGlyphHeader(const TrueTypeFont *font, uint16_t glyph, TrueTypeGlyphHeader *out)
{
    TrueTypeSpan span = {0};
    if (out == NULL || !TrueTypeReaderGlyphSpan(font, glyph, &span))
    {
        return false;
    }
    TrueTypeGlyphHeader header = {0};
    if (span.size != 0u)
    {
        if (!TtHas(span, 0u, 10u))
        {
            return false;
        }
        header.contourCount = (int16_t)TtI16(span.bytes);
        header.xMin = (int16_t)TtI16(span.bytes + 2u);
        header.yMin = (int16_t)TtI16(span.bytes + 4u);
        header.xMax = (int16_t)TtI16(span.bytes + 6u);
        header.yMax = (int16_t)TtI16(span.bytes + 8u);
        if (header.xMin > header.xMax || header.yMin > header.yMax)
        {
            return false;
        }
    }
    *out = header;
    return true;
}

bool TrueTypeReaderVerticalMetric(const TrueTypeFont *font, uint16_t glyph,
                                  TrueTypeVerticalMetric *out)
{
    if (font == NULL || out == NULL || glyph >= font->numGlyphs || font->numVMetrics == 0u)
    {
        return false;
    }
    uint32_t metric = glyph < font->numVMetrics ? glyph : font->numVMetrics - 1u;
    size_t bearingOffset = glyph < font->numVMetrics ? (size_t)glyph * 4u + 2u
                                                     : (size_t)font->numVMetrics * 4u +
                                                           ((size_t)glyph - font->numVMetrics) * 2u;
    out->advance = (uint16_t)TtU16(font->vmtx.bytes + (size_t)metric * 4u);
    out->bearing = (int16_t)TtI16(font->vmtx.bytes + bearingOffset);
    return true;
}
