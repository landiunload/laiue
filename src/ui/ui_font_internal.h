#pragma once

// Внутренние горячие помощники раскладки глифов. Раскладка запекания строго
// фиксирована таблицей диапазонов ниже, поэтому индекс глифа по кодпоинту
// вычисляется формулой на месте. Раньше ui.c вызывал экспортируемый
// UiFontFindGlyph на каждый символ, и тот повторно проверял принадлежность
// диапазону и раскладку шрифта. Вынесено в общий заголовок, чтобы ui.c и
// ui_font.c использовали один и тот же путь без вызова на символ.

#include "ui/ui_font.h"

#include <stdbool.h>
#include <stdint.h>

// Границы запекаемых диапазонов (включительно). Держать в синхроне с
// UI_GLYPH_RANGES: базы индексов ниже выводятся из этих же значений.
#define UI_GLYPH_ASCII_FIRST 0x0020u
#define UI_GLYPH_ASCII_LAST 0x007Eu
#define UI_GLYPH_DEGREE 0x00B0u
#define UI_GLYPH_YO_BIG 0x0401u
#define UI_GLYPH_CYR_FIRST 0x0410u
#define UI_GLYPH_CYR_LAST 0x044Fu
#define UI_GLYPH_YO_SMALL 0x0451u

typedef struct UiGlyphRange
{
    uint16_t first;
    uint16_t last;
} UiGlyphRange;

static const UiGlyphRange UI_GLYPH_RANGES[] = {
    {UI_GLYPH_ASCII_FIRST, UI_GLYPH_ASCII_LAST}, // ASCII
    {UI_GLYPH_DEGREE, UI_GLYPH_DEGREE},          // знак градуса
    {UI_GLYPH_YO_BIG, UI_GLYPH_YO_BIG},          // Ё
    {UI_GLYPH_CYR_FIRST, UI_GLYPH_CYR_LAST},     // А..я
    {UI_GLYPH_YO_SMALL, UI_GLYPH_YO_SMALL},      // ё
};
#define UI_GLYPH_RANGE_COUNT (sizeof(UI_GLYPH_RANGES) / sizeof(UI_GLYPH_RANGES[0]))

// Последовательные индексы внутриатласной раскладки UiFontBake. Значения
// выводятся из тех же границ, поэтому совпадают с порядком запекания.
#define UI_GLYPH_INDEX_NONE UINT32_MAX
#define UI_GLYPH_ASCII_COUNT (UI_GLYPH_ASCII_LAST - UI_GLYPH_ASCII_FIRST + 1u)
#define UI_GLYPH_CYR_COUNT (UI_GLYPH_CYR_LAST - UI_GLYPH_CYR_FIRST + 1u)
#define UI_GLYPH_INDEX_DEGREE (UI_GLYPH_ASCII_COUNT)
#define UI_GLYPH_INDEX_YO_BIG (UI_GLYPH_ASCII_COUNT + 1u)
#define UI_GLYPH_INDEX_CYR_BASE (UI_GLYPH_ASCII_COUNT + 2u)
#define UI_GLYPH_INDEX_YO_SMALL (UI_GLYPH_INDEX_CYR_BASE + UI_GLYPH_CYR_COUNT)

// Число глифов фиксированного набора; используется и при запекании, и для
// проверки, что конкретный шрифт запечён именно по этой таблице.
static inline uint32_t UiGlyphCount(void)
{
    uint32_t count = 0u;
    for (uint32_t range = 0u; range < UI_GLYPH_RANGE_COUNT; ++range)
    {
        count += (uint32_t)(UI_GLYPH_RANGES[range].last - UI_GLYPH_RANGES[range].first + 1u);
    }
    return count;
}

// Индекс глифа по кодпоинту для раскладки UiFontBake. UI_GLYPH_INDEX_NONE —
// кодпоинт не входит ни в один запекаемый диапазон.
static inline uint32_t UiGlyphLayoutIndex(uint16_t codepoint)
{
    uint32_t ascii = (uint32_t)codepoint - UI_GLYPH_ASCII_FIRST;
    if (ascii < UI_GLYPH_ASCII_COUNT)
    {
        return ascii;
    }
    uint32_t cyrillic = (uint32_t)codepoint - UI_GLYPH_CYR_FIRST;
    if (cyrillic < UI_GLYPH_CYR_COUNT)
    {
        return UI_GLYPH_INDEX_CYR_BASE + cyrillic;
    }
    if (codepoint == UI_GLYPH_DEGREE)
    {
        return UI_GLYPH_INDEX_DEGREE;
    }
    if (codepoint == UI_GLYPH_YO_BIG)
    {
        return UI_GLYPH_INDEX_YO_BIG;
    }
    if (codepoint == UI_GLYPH_YO_SMALL)
    {
        return UI_GLYPH_INDEX_YO_SMALL;
    }
    return UI_GLYPH_INDEX_NONE;
}

// Совпадает ли форма таблицы с фиксированным набором: число глифов и крайние
// точки. Внутреннее содержимое подтверждается отдельно при каждом прямом
// индексировании, поэтому эта проверка безопасна и для пользовательского
// шрифта, у которого случайно совпали count и границы.
static inline bool UiFontHasBakedGlyphShape(const UiFont *font)
{
    return font != NULL && font->glyphs != NULL && font->glyphCount != 0u &&
           font->glyphCount == UiGlyphCount() &&
           font->glyphs[0].codepoint == UI_GLYPH_RANGES[0].first &&
           font->glyphs[font->glyphCount - 1u].codepoint ==
               UI_GLYPH_RANGES[UI_GLYPH_RANGE_COUNT - 1u].last;
}

// Поиск без вызова экспортируемой функции для подходящего прямого индекса.
// hasBakedShape вычисляется один раз на строку; для пользовательской
// раскладки или несовпадения конкретного индекса используется бинарный поиск.
static inline const UiGlyph *UiFontLookupGlyph(const UiFont *font, uint16_t codepoint,
                                               bool hasBakedShape)
{
    if (font == NULL)
    {
        return NULL;
    }
    if (hasBakedShape)
    {
        uint32_t index = UiGlyphLayoutIndex(codepoint);
        if (index < font->glyphCount && font->glyphs[index].codepoint == codepoint)
        {
            return &font->glyphs[index];
        }
    }
    return UiFontFindGlyph(font, codepoint);
}
