#include "truetype_outline.h"

// Packed flags и coordinate deltas: Microsoft OpenType, glyf.
// https://learn.microsoft.com/typography/opentype/spec/glyf
typedef struct TrueTypeSimpleLayout
{
    TrueTypeOutlineCounts counts;
    size_t flags;
    size_t x;
    size_t y;
} TrueTypeSimpleLayout;

static uint32_t TtoU16(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 8u) | bytes[1];
}

static bool TtoHas(TrueTypeSpan span, size_t offset, size_t count)
{
    return offset <= span.size && count <= span.size - offset;
}

static uint32_t TtoCoordinateBytes(uint8_t flags, uint32_t shortMask, uint32_t sameMask)
{
    return ((uint32_t)flags & shortMask) != 0u ? 1u : ((uint32_t)flags & sameMask) != 0u ? 0u : 2u;
}

static bool TtoMeasure(TrueTypeSpan glyph, TrueTypeSimpleLayout *layout)
{
    if (glyph.size == 0u)
        return true;
    if (glyph.bytes == NULL || !TtoHas(glyph, 0u, 10u))
        return false;
    uint32_t contours = TtoU16(glyph.bytes);
    if (contours > TRUE_TYPE_OUTLINE_MAX_CONTOURS || !TtoHas(glyph, 10u, (size_t)contours * 2u))
        return false;
    layout->counts.contourCount = contours;
    if (contours == 0u && glyph.size == 10u)
        return true;
    uint32_t points = 0u;
    for (uint32_t index = 0u; index < contours; ++index)
    {
        uint32_t end = TtoU16(glyph.bytes + 10u + (size_t)index * 2u);
        if (end + 1u <= points)
            return false;
        points = end + 1u;
    }
    if (points > TRUE_TYPE_OUTLINE_MAX_POINTS)
        return false;
    layout->counts.pointCount = points;
    size_t position = 10u + (size_t)contours * 2u;
    if (!TtoHas(glyph, position, 2u))
        return false;
    uint32_t instructions = TtoU16(glyph.bytes + position);
    position += 2u;
    if (!TtoHas(glyph, position, instructions))
        return false;
    position += instructions;
    layout->flags = position;
    size_t xBytes = 0u;
    size_t yBytes = 0u;
    for (uint32_t index = 0u; index < points;)
    {
        if (!TtoHas(glyph, position, 1u))
            return false;
        uint8_t flags = glyph.bytes[position++];
        uint32_t run = 1u;
        if ((flags & 8u) != 0u)
        {
            if (!TtoHas(glyph, position, 1u))
                return false;
            run += glyph.bytes[position++];
        }
        if ((flags & 0x80u) != 0u || run > points - index)
            return false;
        xBytes += (size_t)run * TtoCoordinateBytes(flags, 2u, 16u);
        yBytes += (size_t)run * TtoCoordinateBytes(flags, 4u, 32u);
        index += run;
    }
    if (!TtoHas(glyph, position, xBytes + yBytes))
        return false;
    layout->x = position;
    layout->y = position + xBytes;
    return true;
}

static int32_t TtoDelta(const uint8_t *bytes, size_t *position, uint8_t flags, uint32_t shortMask,
                        uint32_t sameMask)
{
    if (((uint32_t)flags & shortMask) != 0u)
    {
        int32_t delta = bytes[(*position)++];
        return ((uint32_t)flags & sameMask) != 0u ? delta : -delta;
    }
    if (((uint32_t)flags & sameMask) != 0u)
        return 0;
    uint32_t value = TtoU16(bytes + *position);
    *position += 2u;
    return value < 32768u ? (int32_t)value : (int32_t)value - 65536;
}

bool TrueTypeSimpleOutlineRead(TrueTypeSpan glyph, TrueTypeOutlineStorage storage,
                               TrueTypeOutlineCounts *out)
{
    TrueTypeSimpleLayout layout = {0};
    if (out == NULL || !TtoMeasure(glyph, &layout) ||
        layout.counts.pointCount > storage.pointCapacity ||
        layout.counts.contourCount > storage.contourCapacity ||
        (layout.counts.pointCount != 0u && storage.points == NULL) ||
        (layout.counts.contourCount != 0u && storage.contourEnds == NULL))
        return false;
    // Первый проход проверил все длины и runs. После него запись не отказывает.
    // 16384 signed16 deltas суммируются в int32 без переполнения.
    for (uint32_t index = 0u; index < layout.counts.contourCount; ++index)
        storage.contourEnds[index] = (uint16_t)TtoU16(glyph.bytes + 10u + (size_t)index * 2u);
    size_t position = layout.flags;
    int32_t x = 0;
    int32_t y = 0;
    for (uint32_t index = 0u; index < layout.counts.pointCount;)
    {
        uint8_t flags = glyph.bytes[position++];
        uint32_t run = 1u;
        if ((flags & 8u) != 0u)
            run += glyph.bytes[position++];
        for (uint32_t repeated = 0u; repeated < run; ++repeated)
        {
            x += TtoDelta(glyph.bytes, &layout.x, flags, 2u, 16u);
            y += TtoDelta(glyph.bytes, &layout.y, flags, 4u, 32u);
            storage.points[index++] = (TrueTypeDesignPoint){x, y, (flags & 1u) != 0u};
        }
    }
    *out = layout.counts;
    return true;
}
