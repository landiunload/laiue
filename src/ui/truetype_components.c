#include "truetype_components.h"

// Component Glyph records: Microsoft OpenType, glyf.
// https://learn.microsoft.com/typography/opentype/spec/glyf
static uint32_t TtcU16(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 8u) | bytes[1];
}

static int32_t TtcSigned(uint32_t value, uint32_t sign)
{
    return value < sign ? (int32_t)value : (int32_t)value - (int32_t)(sign * 2u);
}

static bool TtcHas(TrueTypeSpan span, size_t offset, size_t count)
{
    return offset <= span.size && count <= span.size - offset;
}

static size_t TtcTransformBytes(uint32_t flags)
{
    return (flags & TRUE_TYPE_COMPONENT_SCALE) != 0u      ? 2u
           : (flags & TRUE_TYPE_COMPONENT_XY_SCALE) != 0u ? 4u
           : (flags & TRUE_TYPE_COMPONENT_MATRIX) != 0u   ? 8u
                                                          : 0u;
}

static bool TtcMeasure(TrueTypeSpan glyph, TrueTypeCompositeInfo *info)
{
    if (glyph.bytes == NULL || !TtcHas(glyph, 0u, 10u) || TtcU16(glyph.bytes) < 0x8000u)
        return false;
    size_t position = 10u;
    uint32_t flags;
    uint32_t instructions = 0u;
    do
    {
        if (info->componentCount == TRUE_TYPE_COMPOSITE_MAX_COMPONENTS ||
            !TtcHas(glyph, position, 4u))
            return false;
        flags = TtcU16(glyph.bytes + position);
        uint32_t transform = flags & (TRUE_TYPE_COMPONENT_SCALE | TRUE_TYPE_COMPONENT_XY_SCALE |
                                      TRUE_TYPE_COMPONENT_MATRIX);
        uint32_t offsets =
            flags & (TRUE_TYPE_COMPONENT_SCALED_OFFSET | TRUE_TYPE_COMPONENT_UNSCALED_OFFSET);
        if ((flags & 0xE010u) != 0u || (transform & (transform - 1u)) != 0u ||
            offsets == (TRUE_TYPE_COMPONENT_SCALED_OFFSET | TRUE_TYPE_COMPONENT_UNSCALED_OFFSET) ||
            (info->componentCount == 0u && (flags & TRUE_TYPE_COMPONENT_XY_ARGUMENTS) == 0u) ||
            (info->componentCount != 0u && (flags & TRUE_TYPE_COMPONENT_OVERLAP) != 0u))
            return false;
        size_t recordBytes = 4u + ((flags & TRUE_TYPE_COMPONENT_WORD_ARGUMENTS) != 0u ? 4u : 2u) +
                             TtcTransformBytes(flags);
        if (!TtcHas(glyph, position, recordBytes))
            return false;
        position += recordBytes;
        ++info->componentCount;
        instructions |= flags & TRUE_TYPE_COMPONENT_INSTRUCTIONS;
    } while ((flags & TRUE_TYPE_COMPONENT_MORE) != 0u);
    if (instructions != 0u)
    {
        if (!TtcHas(glyph, position, 2u))
            return false;
        uint32_t bytes = TtcU16(glyph.bytes + position);
        position += 2u;
        if (!TtcHas(glyph, position, bytes))
            return false;
        info->instructions = (TrueTypeSpan){glyph.bytes + position, bytes};
    }
    return true;
}

// Вызывается только после TtcMeasure: все offsets и widths уже проверены.
static TrueTypeComponent TtcDecode(const uint8_t *bytes, size_t *position)
{
    const uint8_t *record = bytes + *position;
    uint32_t flags = TtcU16(record);
    TrueTypeComponent result = {.glyphIndex = (uint16_t)TtcU16(record + 2u),
                                .flags = (uint16_t)flags,
                                .xScale = 16384,
                                .yScale = 16384};
    size_t argumentBytes = (flags & TRUE_TYPE_COMPONENT_WORD_ARGUMENTS) != 0u ? 2u : 1u;
    record += 4u;
    uint32_t first = argumentBytes == 2u ? TtcU16(record) : record[0];
    record += argumentBytes;
    uint32_t second = argumentBytes == 2u ? TtcU16(record) : record[0];
    record += argumentBytes;
    result.argument1 = (flags & TRUE_TYPE_COMPONENT_XY_ARGUMENTS) != 0u
                           ? TtcSigned(first, argumentBytes == 2u ? 32768u : 128u)
                           : (int32_t)first;
    result.argument2 = (flags & TRUE_TYPE_COMPONENT_XY_ARGUMENTS) != 0u
                           ? TtcSigned(second, argumentBytes == 2u ? 32768u : 128u)
                           : (int32_t)second;
    if ((flags & TRUE_TYPE_COMPONENT_SCALE) != 0u)
    {
        result.xScale = result.yScale = (int16_t)TtcSigned(TtcU16(record), 32768u);
    }
    else if ((flags & TRUE_TYPE_COMPONENT_XY_SCALE) != 0u)
    {
        result.xScale = (int16_t)TtcSigned(TtcU16(record), 32768u);
        result.yScale = (int16_t)TtcSigned(TtcU16(record + 2u), 32768u);
    }
    else if ((flags & TRUE_TYPE_COMPONENT_MATRIX) != 0u)
    {
        result.xScale = (int16_t)TtcSigned(TtcU16(record), 32768u);
        result.scale01 = (int16_t)TtcSigned(TtcU16(record + 2u), 32768u);
        result.scale10 = (int16_t)TtcSigned(TtcU16(record + 4u), 32768u);
        result.yScale = (int16_t)TtcSigned(TtcU16(record + 6u), 32768u);
    }
    *position += 4u + argumentBytes * 2u + TtcTransformBytes(flags);
    return result;
}

bool TrueTypeCompositeRead(TrueTypeSpan glyph, TrueTypeComponentStorage storage,
                           TrueTypeCompositeInfo *out)
{
    TrueTypeCompositeInfo info = {0};
    if (out == NULL || !TtcMeasure(glyph, &info) || info.componentCount > storage.capacity ||
        storage.components == NULL)
        return false;
    size_t position = 10u;
    for (uint32_t index = 0u; index < info.componentCount; ++index)
        storage.components[index] = TtcDecode(glyph.bytes, &position);
    *out = info;
    return true;
}
