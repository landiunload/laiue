#pragma once

#include "truetype_reader.h"

enum
{
    TRUE_TYPE_PHANTOM_HORIZONTAL_MASK = 0x3,
    TRUE_TYPE_PHANTOM_VERTICAL_MASK = 0xC,
};

typedef struct TrueTypePhantomPoint
{
    int32_t x;
    int32_t y;
} TrueTypePhantomPoint;

typedef struct TrueTypeInitialPhantoms
{
    TrueTypePhantomPoint points[4]; // left, right, top, bottom
    uint32_t validMask;
} TrueTypeInitialPhantoms;

// Внутренние initial metric points в целых design units, Y вверх, до transforms,
// placement, USE_MY_METRICS и hinting. Это не метрики разрешённого composite.
// resolvedContourCount взят из успешного plan того же glyph и immutable font;
// максимум 4096. Для simple glyph count должен совпадать с header.
// Осевые поперечные координаты равны 0. Без реальных контуров effective xMin/yMax
// равны 0, raw bearings сохраняются. H points доступны всегда; без vertical
// tables mask=0x3, V slots нулевые и недоступны, без синтеза из hhea.
// Font успешно Initialize, bytes неизменны во время вызова; font, bytes и out
// не перекрываются. Out содержит собственные числа и не заимствует font/plan.
// Ошибка сохраняет весь out. Нет allocation, I/O, float или C recursion.
bool TrueTypeGlyphInitialPhantomsRead(const TrueTypeFont *font, uint16_t glyph,
                                      uint32_t resolvedContourCount, TrueTypeInitialPhantoms *out);
