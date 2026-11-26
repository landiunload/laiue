#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    TRUE_TYPE_ATLAS_MAX_GLYPHS = 164
};

typedef struct TrueTypeAtlasGlyphSize
{
    uint16_t width;
    uint16_t height;
} TrueTypeAtlasGlyphSize;

typedef struct TrueTypeAtlasLimits
{
    uint32_t width;
    uint32_t height;
    size_t byteBudget;
} TrueTypeAtlasLimits;

typedef struct TrueTypeAtlasPlacement
{
    uint32_t x;
    uint32_t y;
} TrueTypeAtlasPlacement;

typedef struct TrueTypeAtlasSummary
{
    uint32_t width;
    uint32_t height;
    uint32_t glyphCount;
    size_t bytes;
} TrueTypeAtlasSummary;

// Полки в порядке входных глифов, с одним прозрачным пикселем снаружи
// и между глифами. Пустая область получает (0, 0) и оставляет полку прежней.
// У пустого атласа высота 2. Буферы не перекрываются, при count == 0 массивы
// могут быть NULL. При false итог не меняется, placements остаётся scratch.
bool TrueTypeAtlasPlan(const TrueTypeAtlasGlyphSize *sizes, uint32_t count,
                       TrueTypeAtlasLimits limits, TrueTypeAtlasPlacement *placements,
                       uint32_t placementCapacity, TrueTypeAtlasSummary *out);
