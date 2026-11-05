#pragma once

#include "truetype_components.h"
#include "truetype_outline.h"

enum
{
    TRUE_TYPE_PLAN_MAX_COMPONENTS = TRUE_TYPE_COMPOSITE_MAX_COMPONENTS,
    TRUE_TYPE_PLAN_MAX_NODES = TRUE_TYPE_PLAN_MAX_COMPONENTS + 1,
    TRUE_TYPE_PLAN_MAX_DEPTH = 16,
    TRUE_TYPE_PLAN_NO_INDEX = 65535,
};

typedef struct TrueTypePlanNode
{
    uint32_t pointCount;
    uint32_t contourCount;
    uint32_t pointBase;
    uint32_t contourBase;
    uint16_t glyphIndex;
    uint16_t parentIndex;
    uint16_t firstChild;
    uint16_t childCount;
    uint16_t componentIndex;
    uint8_t depth;
} TrueTypePlanNode;

typedef struct TrueTypePlanWorkspace
{
    TrueTypePlanNode nodes[TRUE_TYPE_PLAN_MAX_NODES];
    TrueTypeComponent components[TRUE_TYPE_PLAN_MAX_COMPONENTS];
} TrueTypePlanWorkspace;

typedef struct TrueTypePlanSummary
{
    uint32_t nodeCount;
    uint32_t componentCount;
    uint32_t pointCount;
    uint32_t contourCount;
    uint32_t maxDepth;
} TrueTypePlanSummary;

// Внутренний structural preflight, не resolver геометрии или метрик.
// Node описывает occurrence; children идут подряд, parentIndex < child index.
// Counts относятся к subtree, bases — к окончательному outline root.
// componentIndex указывает на входящий raw record; root parent/component и
// leaf firstChild равны NO_INDEX. Root depth=1, максимум 16 вместе с root.
// Parent point references допускают только prefix уже включённых компонентов
// либо четыре final phantom indices; child — свои final points + четыре phantoms.
// Vertical availability, USE_MY_METRICS и transforms здесь не применяются.
// Font должен быть успешно Initialize; исходные bytes неизменны и живут дольше
// плана. Font, его bytes, workspace и out не перекрываются. Workspace — scratch:
// каждый Build инвалидирует прежний plan, после false его содержимое invalid.
// Ошибка сохраняет scalar out. Нет allocation, I/O, float или C recursion.
bool TrueTypeGlyphPlanBuild(const TrueTypeFont *font, uint16_t glyph,
                            TrueTypePlanWorkspace *workspace, TrueTypePlanSummary *out);
