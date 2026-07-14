#include "core/ui_font.h"

#include <windows.h>
#include <string.h>

// Диапазоны запекаемых кодовых точек (включительно).
typedef struct GlyphRange
{
    uint16_t first;
    uint16_t last;
} GlyphRange;

static const GlyphRange GLYPH_RANGES[] = {
    { 0x0020, 0x007E },  // ASCII
    { 0x00B0, 0x00B0 },  // знак градуса
    { 0x0401, 0x0401 },  // Ё
    { 0x0410, 0x044F },  // А..я
    { 0x0451, 0x0451 },  // ё
};
#define GLYPH_RANGE_COUNT (sizeof(GLYPH_RANGES) / sizeof(GLYPH_RANGES[0]))

#define GLYPH_PADDING 1

static uint32_t CountGlyphs(void)
{
    uint32_t count = 0;
    for (uint32_t range = 0; range < GLYPH_RANGE_COUNT; ++range)
    {
        count += (uint32_t)(GLYPH_RANGES[range].last - GLYPH_RANGES[range].first + 1);
    }
    return count;
}

void UiFontRelease(UiFont* font)
{
    if (font->atlas != NULL)
    {
        HeapFree(GetProcessHeap(), 0, font->atlas);
        font->atlas = NULL;
    }
    if (font->glyphs != NULL)
    {
        HeapFree(GetProcessHeap(), 0, font->glyphs);
        font->glyphs = NULL;
    }
    font->glyphCount = 0;
    font->atlasWidth = 0;
    font->atlasHeight = 0;
    font->pixelSize = 0;
}

bool UiFontBake(UiFont* font, int32_t pixelSize)
{
    if (pixelSize < 6)
    {
        pixelSize = 6;
    }

    HFONT gdiFont = CreateFontW(
        -pixelSize, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (gdiFont == NULL)
    {
        return false;
    }

    HDC deviceContext = CreateCompatibleDC(NULL);
    if (deviceContext == NULL)
    {
        DeleteObject(gdiFont);
        return false;
    }
    HGDIOBJ previousFont = SelectObject(deviceContext, gdiFont);

    TEXTMETRICW textMetrics;
    if (!GetTextMetricsW(deviceContext, &textMetrics))
    {
        SelectObject(deviceContext, previousFont);
        DeleteDC(deviceContext);
        DeleteObject(gdiFont);
        return false;
    }

    const MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    uint32_t glyphCount = CountGlyphs();
    UiGlyph* glyphs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)glyphCount * sizeof(UiGlyph));
    if (glyphs == NULL)
    {
        SelectObject(deviceContext, previousFont);
        DeleteDC(deviceContext);
        DeleteObject(gdiFont);
        return false;
    }

    // Проход 1: метрики и раскладка полками (shelf packing).
    uint32_t atlasWidth = pixelSize <= 28 ? 512 : 1024;
    uint32_t shelfX = GLYPH_PADDING;
    uint32_t shelfY = GLYPH_PADDING;
    uint32_t shelfHeight = 0;
    uint32_t maxGlyphBytes = 0;
    uint32_t glyphIndex = 0;

    for (uint32_t range = 0; range < GLYPH_RANGE_COUNT; ++range)
    {
        for (uint32_t code = GLYPH_RANGES[range].first;
             code <= GLYPH_RANGES[range].last; ++code, ++glyphIndex)
        {
            UiGlyph* glyph = &glyphs[glyphIndex];
            glyph->codepoint = (uint16_t)code;

            GLYPHMETRICS metrics;
            memset(&metrics, 0, sizeof(metrics));
            DWORD bytes = GetGlyphOutlineW(deviceContext, code,
                GGO_GRAY8_BITMAP, &metrics, 0, NULL, &identity);
            if (bytes == GDI_ERROR)
            {
                // Глиф недоступен: аванс по ширине текста, бокс пустой.
                wchar_t character = (wchar_t)code;
                SIZE extent;
                if (GetTextExtentPoint32W(deviceContext, &character, 1, &extent))
                {
                    glyph->advance = (float)extent.cx;
                }
                continue;
            }

            glyph->advance = (float)metrics.gmCellIncX;
            glyph->offsetX = (int16_t)metrics.gmptGlyphOrigin.x;
            glyph->offsetY = (int16_t)metrics.gmptGlyphOrigin.y;
            glyph->width = (uint16_t)metrics.gmBlackBoxX;
            glyph->height = (uint16_t)metrics.gmBlackBoxY;
            if (bytes > maxGlyphBytes)
            {
                maxGlyphBytes = bytes;
            }
            if (glyph->width == 0 || glyph->height == 0 || bytes == 0)
            {
                glyph->width = 0;
                glyph->height = 0;
                continue;
            }

            uint32_t paddedWidth = (uint32_t)glyph->width + GLYPH_PADDING;
            if (shelfX + paddedWidth > atlasWidth)
            {
                shelfX = GLYPH_PADDING;
                shelfY += shelfHeight + GLYPH_PADDING;
                shelfHeight = 0;
            }

            // Временно кладём позицию в UV-поля (пиксели до нормировки).
            glyph->u0 = (float)shelfX;
            glyph->v0 = (float)shelfY;
            shelfX += paddedWidth;
            if ((uint32_t)glyph->height > shelfHeight)
            {
                shelfHeight = glyph->height;
            }
        }
    }

    uint32_t atlasHeight = shelfY + shelfHeight + GLYPH_PADDING;
    uint8_t* atlas = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)atlasWidth * atlasHeight);
    uint8_t* scratch = maxGlyphBytes > 0
        ? HeapAlloc(GetProcessHeap(), 0, maxGlyphBytes) : NULL;
    if (atlas == NULL || (maxGlyphBytes > 0 && scratch == NULL))
    {
        if (atlas != NULL) HeapFree(GetProcessHeap(), 0, atlas);
        if (scratch != NULL) HeapFree(GetProcessHeap(), 0, scratch);
        HeapFree(GetProcessHeap(), 0, glyphs);
        SelectObject(deviceContext, previousFont);
        DeleteDC(deviceContext);
        DeleteObject(gdiFont);
        return false;
    }

    // Проход 2: растеризация глифов в атлас (GGO_GRAY8: 0..64 -> 0..255,
    // строки исходника выровнены по 4 байта).
    for (uint32_t i = 0; i < glyphCount; ++i)
    {
        UiGlyph* glyph = &glyphs[i];
        if (glyph->width == 0 || glyph->height == 0)
        {
            glyph->u0 = glyph->v0 = glyph->u1 = glyph->v1 = 0.0f;
            continue;
        }

        GLYPHMETRICS metrics;
        memset(&metrics, 0, sizeof(metrics));
        DWORD bytes = GetGlyphOutlineW(deviceContext, glyph->codepoint,
            GGO_GRAY8_BITMAP, &metrics, maxGlyphBytes, scratch, &identity);
        uint32_t atlasX = (uint32_t)glyph->u0;
        uint32_t atlasY = (uint32_t)glyph->v0;
        if (bytes != GDI_ERROR && bytes > 0)
        {
            uint32_t pitch = ((uint32_t)glyph->width + 3u) & ~3u;
            for (uint32_t row = 0; row < glyph->height; ++row)
            {
                const uint8_t* source = scratch + (size_t)row * pitch;
                uint8_t* destination =
                    atlas + (size_t)(atlasY + row) * atlasWidth + atlasX;
                for (uint32_t column = 0; column < glyph->width; ++column)
                {
                    uint32_t value = source[column];
                    destination[column] =
                        (uint8_t)(value >= 64 ? 255 : value * 4);
                }
            }
        }

        float inverseWidth = 1.0f / (float)atlasWidth;
        float inverseHeight = 1.0f / (float)atlasHeight;
        glyph->u0 = (float)atlasX * inverseWidth;
        glyph->v0 = (float)atlasY * inverseHeight;
        glyph->u1 = (float)(atlasX + glyph->width) * inverseWidth;
        glyph->v1 = (float)(atlasY + glyph->height) * inverseHeight;
    }

    if (scratch != NULL)
    {
        HeapFree(GetProcessHeap(), 0, scratch);
    }
    SelectObject(deviceContext, previousFont);
    DeleteDC(deviceContext);
    DeleteObject(gdiFont);

    UiFontRelease(font);
    font->atlas = atlas;
    font->atlasWidth = atlasWidth;
    font->atlasHeight = atlasHeight;
    font->glyphs = glyphs;
    font->glyphCount = glyphCount;
    font->ascent = (float)textMetrics.tmAscent;
    font->lineHeight = (float)(textMetrics.tmHeight + textMetrics.tmExternalLeading);
    font->pixelSize = pixelSize;
    return true;
}

const UiGlyph* UiFontFindGlyph(const UiFont* font, uint16_t codepoint)
{
    uint32_t low = 0;
    uint32_t high = font->glyphCount;
    while (low < high)
    {
        uint32_t middle = (low + high) / 2;
        if (font->glyphs[middle].codepoint < codepoint)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low < font->glyphCount && font->glyphs[low].codepoint == codepoint)
    {
        return &font->glyphs[low];
    }
    return NULL;
}

float UiFontMeasure(const UiFont* font, const wchar_t* text)
{
    float width = 0.0f;
    for (const wchar_t* character = text; *character != L'\0'; ++character)
    {
        const UiGlyph* glyph = UiFontFindGlyph(font, (uint16_t)*character);
        if (glyph != NULL)
        {
            width += glyph->advance;
        }
    }
    return width;
}
