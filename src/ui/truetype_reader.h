#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TrueTypeSpan
{
    const uint8_t *bytes;
    size_t size;
} TrueTypeSpan;

// Внутренний reader SFNT с TrueType outlines. Данные принадлежат вызывающему:
// весь исходный буфер должен оставаться неизменным, пока используется font.
// Структуру заполняет только Initialize; память, I/O и масштабирование здесь не нужны.
typedef struct TrueTypeFont
{
    TrueTypeSpan cmap;
    TrueTypeSpan glyf;
    TrueTypeSpan loca;
    TrueTypeSpan hmtx;
    uint32_t numGlyphs;
    uint32_t numHMetrics;
    uint32_t locaWidth;
    uint32_t cmapFormat;
    uint16_t unitsPerEm;
    int16_t ascent;
    int16_t descent;
    int16_t lineGap;
} TrueTypeFont;

typedef struct TrueTypeHorizontalMetric
{
    uint16_t advance;
    int16_t bearing;
} TrueTypeHorizontalMetric;

// SFNT 1.0, glyf/loca и Unicode cmap 4/12; format 12 предпочтительнее 4.
// Метрики возвращаются в единицах шрифта. Checksums и glyph outlines не читаются.
// Ошибка любой функции сохраняет прежний output. Accessors принимают только
// успешно инициализированный font. Отсутствующий символ даёт glyph 0;
// неверный Unicode scalar или glyph id за numGlyphs возвращает false.
bool TrueTypeReaderInitialize(const uint8_t *bytes, size_t size, TrueTypeFont *out);
bool TrueTypeReaderGlyphIndex(const TrueTypeFont *font, uint32_t codepoint, uint16_t *out);
bool TrueTypeReaderGlyphSpan(const TrueTypeFont *font, uint16_t glyph, TrueTypeSpan *out);
bool TrueTypeReaderHorizontalMetric(const TrueTypeFont *font, uint16_t glyph,
                                    TrueTypeHorizontalMetric *out);
