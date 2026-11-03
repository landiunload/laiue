#pragma once

#include "truetype_reader.h"

enum
{
    TRUE_TYPE_OUTLINE_MAX_POINTS = 16384,
    TRUE_TYPE_OUTLINE_MAX_CONTOURS = 4096,
};

typedef struct TrueTypeDesignPoint
{
    int32_t x;
    int32_t y;
    bool onCurve;
} TrueTypeDesignPoint;

typedef struct TrueTypeOutlineStorage
{
    TrueTypeDesignPoint *points;
    uint16_t *contourEnds;
    uint32_t pointCapacity;
    uint32_t contourCapacity;
} TrueTypeOutlineStorage;

typedef struct TrueTypeOutlineCounts
{
    uint32_t pointCount;
    uint32_t contourCount;
} TrueTypeOutlineCounts;

// Внутренний decoder простого glyf: целые font design units, без hinting.
// Пустой span и нулевые контуры допустимы; composite возвращает false.
// Максимум 16384 точек и 4096 контуров. Буферы принадлежат вызывающему;
// glyph, оба буфера и out не перекрываются и живут до конца вызова.
// При false содержимое буферов и out сохраняется. Дополнительные байты
// выравнивания допустимы; instructions пропускаются без выполнения.
bool TrueTypeSimpleOutlineRead(TrueTypeSpan glyph, TrueTypeOutlineStorage storage,
                               TrueTypeOutlineCounts *out);
