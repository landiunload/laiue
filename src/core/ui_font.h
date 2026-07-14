#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Растровый атлас шрифта (внутренний компонент ядра): печётся из
// системного шрифта через GDI (GetGlyphOutlineW, серый антиалиасинг)
// в 8-битную альфа-карту. Набор глифов: ASCII, кириллица, знак градуса.

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

// Печёт атлас под размер шрифта в пикселях; повторный вызов перепекает,
// освобождая старый. false — шрифт или память недоступны.
bool UiFontBake(UiFont* font, int32_t pixelSize);
void UiFontRelease(UiFont* font);

const UiGlyph* UiFontFindGlyph(const UiFont* font, uint16_t codepoint);
float UiFontMeasure(const UiFont* font, const wchar_t* text);
