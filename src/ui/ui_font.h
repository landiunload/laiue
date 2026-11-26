#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Растровый атлас шрифта, 8-битная альфа-карта. Набор: ASCII, кириллица,
// °, × и —. LAIUE_UI_FONT задаёт путь к файлу TrueType. Без него Windows
// выбирает системный шрифт через GDI, другие платформы ищут системный TTF.

typedef struct UiGlyph
{
    uint16_t codepoint;
    int16_t offsetX;    // от пера до левого края бокса, px
    int16_t offsetY;    // от базовой линии вверх до верха бокса, px
    uint16_t width;
    uint16_t height;
    float advance;
    float u0, v0, u1, v1;
} UiGlyph;

typedef struct UiFont
{
    uint8_t* atlas;      // альфа 8 бит, atlasWidth * atlasHeight
    uint32_t atlasWidth;
    uint32_t atlasHeight;
    UiGlyph* glyphs;     // отсортированы по codepoint
    uint32_t glyphCount;
    float ascent;        // от верха строки до базовой линии
    float lineHeight;
    int32_t pixelSize;
} UiFont;

// Печёт атлас под размер шрифта в пикселях и заменяет текущий при успехе.
// При false ранее подготовленный атлас остаётся доступен.
LAIUE_UI_API bool UiFontBake(UiFont* font, int32_t pixelSize);
LAIUE_UI_API void UiFontRelease(UiFont* font);

LAIUE_UI_API const UiGlyph* UiFontFindGlyph(
    const UiFont* font, uint16_t codepoint);
LAIUE_UI_API float UiFontMeasure(
    const UiFont* font, const wchar_t* text);
