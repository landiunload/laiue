#pragma once

#include "truetype_atlas.h"
#include "truetype_coverage.h"
#include "ui_font.h"

typedef struct TrueTypeBakeGeometry
{
    TrueTypeXYWorkspace xy;
    TrueTypeEdgeWorkspace edges;
    TrueTypePixelPoint points[TRUE_TYPE_OUTLINE_MAX_POINTS];
    uint16_t ends[TRUE_TYPE_OUTLINE_MAX_CONTOURS];
} TrueTypeBakeGeometry;

typedef struct TrueTypeBakeGlyph
{
    UiGlyph metrics;
    uint16_t index;
    float penX;
    uint32_t edgeCount;
} TrueTypeBakeGlyph;

typedef struct TrueTypeBakePlan
{
    TrueTypeBakeGlyph glyphs[TRUE_TYPE_ATLAS_MAX_GLYPHS];
    TrueTypeAtlasPlacement placements[TRUE_TYPE_ATLAS_MAX_GLYPHS];
    TrueTypeAtlasSummary atlas;
    float pixelsPerUnit;
    float ascent;
    float lineHeight;
    uint32_t maxEdges;
    uint32_t maxWidth;
} TrueTypeBakePlan;

// Font и его байты неизменны до конца Paint. Геометрия и план принадлежат
// вызывающему и не перекрываются. При ошибке plan — рабочая память.
// edgeRowBudget ограничивает сумму (edges × height) для всего атласа.
bool TrueTypeBakePrepare(const TrueTypeFont *font, int32_t pixelSize, TrueTypeAtlasLimits limits,
                         uint64_t edgeRowBudget, TrueTypeBakeGeometry *geometry,
                         TrueTypeBakePlan *plan);

// Заполняет новые буферы по успешному плану. scratch.events и scratch.coverage
// нужны для строки, row указывает прямо в новый атлас. При false буферы
// недействительны. План не меняется, источники и выходы не перекрываются.
bool TrueTypeBakePaint(const TrueTypeFont *font, const TrueTypeBakePlan *plan,
                       TrueTypeBakeGeometry *geometry, TrueTypeCoverageStorage scratch,
                       uint8_t *atlas, size_t atlasCapacity, UiGlyph *glyphs,
                       uint32_t glyphCapacity);

// Замена существующего шрифта только после успешной подготовки и растеризации.
bool UiFontBakeFromSfnt(UiFont *font, int32_t pixelSize, const uint8_t *bytes, size_t size,
                        TrueTypeAtlasLimits limits, uint64_t edgeRowBudget);
