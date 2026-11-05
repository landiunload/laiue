#include "truetype_phantoms.h"
#include "truetype_outline.h"

// Initial horizontal phantom formulas: Microsoft OpenType hmtx.
// https://learn.microsoft.com/typography/opentype/spec/hmtx
// Vertical origin and downward advance: Microsoft OpenType vmtx.
// https://learn.microsoft.com/typography/opentype/spec/vmtx
bool TrueTypeGlyphInitialPhantomsRead(const TrueTypeFont *font, uint16_t glyph,
                                      uint32_t resolvedContourCount, TrueTypeInitialPhantoms *out)
{
    if (font == NULL || out == NULL || resolvedContourCount > TRUE_TYPE_OUTLINE_MAX_CONTOURS)
        return false;
    TrueTypeGlyphHeader header = {0};
    TrueTypeHorizontalMetric horizontal = {0};
    if (!TrueTypeReaderGlyphHeader(font, glyph, &header) ||
        (header.contourCount >= 0 && (uint32_t)header.contourCount != resolvedContourCount) ||
        !TrueTypeReaderHorizontalMetric(font, glyph, &horizontal))
        return false;

    int32_t xMin = resolvedContourCount != 0u ? (int32_t)header.xMin : 0;
    int32_t yMax = resolvedContourCount != 0u ? (int32_t)header.yMax : 0;
    TrueTypeInitialPhantoms result = {.validMask = TRUE_TYPE_PHANTOM_HORIZONTAL_MASK};
    result.points[0].x = xMin - (int32_t)horizontal.bearing;
    result.points[1].x = result.points[0].x + (int32_t)horizontal.advance;
    if (font->numVMetrics != 0u)
    {
        TrueTypeVerticalMetric vertical = {0};
        if (!TrueTypeReaderVerticalMetric(font, glyph, &vertical))
            return false;
        result.points[2].y = yMax + (int32_t)vertical.bearing;
        result.points[3].y = result.points[2].y - (int32_t)vertical.advance;
        result.validMask |= TRUE_TYPE_PHANTOM_VERTICAL_MASK;
    }
    *out = result;
    return true;
}
