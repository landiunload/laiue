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
    TrueTypeSpan vmtx;
    uint32_t numGlyphs;
    uint32_t numHMetrics;
    uint32_t numVMetrics;
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

typedef struct TrueTypeVerticalMetric
{
    uint16_t advance;
    int16_t bearing;
} TrueTypeVerticalMetric;

typedef struct TrueTypeGlyphHeader
{
    int16_t contourCount;
    int16_t xMin;
    int16_t yMin;
    int16_t xMax;
    int16_t yMax;
} TrueTypeGlyphHeader;

// SFNT 1.0, glyf/loca и Unicode cmap 4/12; format 12 предпочтительнее 4.
// Метрики и bbox возвращаются в единицах шрифта. Checksums и тело outlines не читаются.
// vhea 1.0/1.1 и vmtx необязательны только вместе: numVMetrics == 0 означает
// отсутствие обеих таблиц; неполная или некорректная пара отклоняется Initialize.
// Ошибка любой функции сохраняет прежний output. Accessors принимают только
// успешно инициализированный font. Отсутствующий символ даёт glyph 0;
// неверный Unicode scalar или glyph id за numGlyphs возвращает false.
bool TrueTypeReaderInitialize(const uint8_t *bytes, size_t size, TrueTypeFont *out);
bool TrueTypeReaderGlyphIndex(const TrueTypeFont *font, uint32_t codepoint, uint16_t *out);
bool TrueTypeReaderGlyphSpan(const TrueTypeFont *font, uint16_t glyph, TrueTypeSpan *out);
// Пустой loca span даёт нулевые count/bbox. Непустой требует полного заголовка
// с xMin <= xMax и yMin <= yMax; отрицательный contourCount означает composite.
bool TrueTypeReaderGlyphHeader(const TrueTypeFont *font, uint16_t glyph, TrueTypeGlyphHeader *out);
bool TrueTypeReaderHorizontalMetric(const TrueTypeFont *font, uint16_t glyph,
                                    TrueTypeHorizontalMetric *out);
// advanceHeight и topSideBearing; при отсутствии vertical tables возвращает false.
bool TrueTypeReaderVerticalMetric(const TrueTypeFont *font, uint16_t glyph,
                                  TrueTypeVerticalMetric *out);
