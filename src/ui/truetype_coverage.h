#pragma once

#include "truetype_edges.h"

typedef struct TrueTypeCoverageEvent
{
    double x;
    int32_t direction;
} TrueTypeCoverageEvent;

typedef struct TrueTypeCoverageStorage
{
    TrueTypeCoverageEvent *events;
    uint32_t eventCapacity;
    double *coverage;
    uint32_t coverageCapacity;
    uint8_t *row;
    uint32_t rowCapacity;
} TrueTypeCoverageStorage;

// Alpha8 для одной строки в координатах edges, Y направлен вверх.
// Пиксель x покрывает [left+x, left+x+1), строка — [top-1, top).
// На восьми подстроках в центрах равных полос используется nonzero winding.
// По X считается длина пересечения, результат округляется к ближайшему байту
// (при половине — вверх). Требуется precise FP.
//
// Edges должны образовывать замкнутые контуры в пределах TrueTypeEdgesBuild.
// left/top могут быть дробными; модуль каждого не больше 2^20 + UINT16_MAX.
// width не больше UINT16_MAX, как у UiGlyph. Ширина 0 не требует scratch.
// Для остальных строк coverage и row вмещают width элементов, events —
// все пересечения одной подстроки. NULL edges допустим при edgeCount == 0.
// Память источника, scratch и outRowBytes не перекрывается. Строка принадлежит
// вызывающему; читать первые *outRowBytes байтов можно после успеха. При false
// *outRowBytes сохраняется, scratch не гарантируется. Нет allocation, I/O, libm
// или рекурсии.
bool TrueTypeCoverageRow(const TrueTypeEdge *edges, uint32_t edgeCount, double left, double top,
                         uint32_t width, TrueTypeCoverageStorage storage, uint32_t *outRowBytes);
