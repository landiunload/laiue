#include "truetype_atlas.h"

bool TrueTypeAtlasPlan(const TrueTypeAtlasGlyphSize *sizes, uint32_t count,
                       TrueTypeAtlasLimits limits, TrueTypeAtlasPlacement *placements,
                       uint32_t placementCapacity, TrueTypeAtlasSummary *out)
{
    if (out == NULL || count > TRUE_TYPE_ATLAS_MAX_GLYPHS || count > placementCapacity ||
        (count != 0u && (sizes == NULL || placements == NULL)) || limits.width < 2u)
        return false;

    const size_t affordableHeight = limits.byteBudget / limits.width;
    if (affordableHeight < limits.height)
        limits.height = (uint32_t)affordableHeight;
    if (limits.height < 2u)
        return false;

    uint32_t x = 1u;
    uint32_t y = 1u;
    uint32_t shelfHeight = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const TrueTypeAtlasGlyphSize glyph = sizes[index];
        if (glyph.width == 0u || glyph.height == 0u)
        {
            placements[index] = (TrueTypeAtlasPlacement){0u, 0u};
            continue;
        }
        const uint32_t paddedWidth = (uint32_t)glyph.width + 1u;
        const uint32_t paddedHeight = (uint32_t)glyph.height + 1u;
        if (paddedWidth >= limits.width || paddedHeight >= limits.height)
            return false;
        if (x > limits.width - paddedWidth)
        {
            x = 1u;
            y += shelfHeight + 1u;
            shelfHeight = 0u;
        }
        if (y > limits.height - paddedHeight)
            return false;
        placements[index] = (TrueTypeAtlasPlacement){x, y};
        x += paddedWidth;
        if (glyph.height > shelfHeight)
            shelfHeight = glyph.height;
    }

    const uint32_t height = y + shelfHeight + 1u;
    *out = (TrueTypeAtlasSummary){limits.width, height, count, (size_t)limits.width * height};
    return true;
}
