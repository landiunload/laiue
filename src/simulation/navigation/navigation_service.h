#pragma once

/* Таблица сервиса laiue.navigation для загрузки модуля по имени. Функции
 * совпадают с одноимёнными из navigation.h. */

#include "api.h"
#include "mod/module_api.h"
#include "navigation/navigation.h"

#include <stdint.h>

#define LAIUE_NAVIGATION_SERVICE_NAME "laiue.navigation"
#define LAIUE_NAVIGATION_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueNavigationServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueNavStatus (*graphCreate)(const LaiueNavGraphDesc *desc, LaiueNavGraph **outGraph);
    void (*graphDestroy)(LaiueNavGraph *graph);
    LaiueNavStatus (*graphAddNode)(LaiueNavGraph *graph, const LaiueNavPosition *position,
                                   uint32_t userData, LaiueNavNode *outNode);
    LaiueNavStatus (*graphRemoveNode)(LaiueNavGraph *graph, LaiueNavNode node);
    LaiueNavStatus (*graphGetNode)(const LaiueNavGraph *graph, LaiueNavNode node,
                                   LaiueNavPosition *outPosition, uint32_t *outUserData);
    LaiueNavStatus (*graphSetNodePosition)(LaiueNavGraph *graph, LaiueNavNode node,
                                           const LaiueNavPosition *position);
    LaiueNavStatus (*graphAddEdge)(LaiueNavGraph *graph, LaiueNavNode from, LaiueNavNode to,
                                   const LaiueNavEdgeDesc *desc, LaiueNavEdge *outEdge);
    LaiueNavStatus (*graphRemoveEdge)(LaiueNavGraph *graph, LaiueNavEdge edge);
    LaiueNavStatus (*graphGetEdge)(const LaiueNavGraph *graph, LaiueNavEdge edge,
                                   LaiueNavEdgeInfo *outInfo);
    LaiueNavStatus (*graphSetEdgeCost)(LaiueNavGraph *graph, LaiueNavEdge edge, uint32_t cost);
    LaiueNavStatus (*graphSetEdgeModes)(LaiueNavGraph *graph, LaiueNavEdge edge, uint32_t modes);
    LaiueNavStatus (*graphPrepare)(LaiueNavGraph *graph);
    bool (*graphIsPrepared)(const LaiueNavGraph *graph);
    uint32_t (*graphNodeCount)(const LaiueNavGraph *graph);
    uint32_t (*graphEdgeCount)(const LaiueNavGraph *graph);
    uint64_t (*graphRevision)(const LaiueNavGraph *graph);
    LaiueNavStatus (*queryCreate)(LaiueNavQuery **outQuery);
    void (*queryDestroy)(LaiueNavQuery *query);
    void (*findPath)(const LaiueNavGraph *graph, LaiueNavQuery *query,
                     const LaiueNavPathRequest *request, LaiueNavEdge *edges, uint32_t capacity,
                     LaiueNavPathResult *outResult);
    LaiueNavStatus (*queryBegin)(LaiueNavQuery *query, const LaiueNavGraph *graph,
                                 const LaiueNavPathRequest *request);
    void (*queryContinue)(LaiueNavQuery *query, uint32_t maxExpansions,
                          LaiueNavPathResult *outResult);
    void (*queryGetPath)(LaiueNavQuery *query, LaiueNavEdge *edges, uint32_t capacity,
                         LaiueNavPathResult *outResult);
    LaiueNavStatus (*findPathsBatch)(const LaiueNavGraph *graph, const LaiueTaskExecutor *executor,
                                     LaiueNavQuery *const *queries, uint32_t queryCount,
                                     LaiueNavBatchItem *items, uint32_t itemCount);
    LaiueNavStatus (*gridCreate)(const LaiueNavGridDesc *desc, LaiueNavGrid **outGrid);
    void (*gridDestroy)(LaiueNavGrid *grid);
    LaiueNavStatus (*gridSetCell)(LaiueNavGrid *grid, int64_t x, int64_t y, uint8_t cost);
    uint8_t (*gridGetCell)(const LaiueNavGrid *grid, int64_t x, int64_t y);
    LaiueNavStatus (*gridFillRect)(LaiueNavGrid *grid, const LaiueNavRect *rect, uint8_t cost);
    uint64_t (*gridRevision)(const LaiueNavGrid *grid);
    void (*gridFindPath)(const LaiueNavGrid *grid, LaiueNavQuery *query,
                         const LaiueNavGridPathRequest *request, LaiueNavCell *cells,
                         uint32_t capacity, LaiueNavPathResult *outResult);
    uint32_t (*gridSmoothPath)(const LaiueNavGrid *grid, LaiueNavCell *cells, uint32_t count);
    LaiueNavStatus (*flowFieldCreate)(LaiueNavFlowField **outField);
    void (*flowFieldDestroy)(LaiueNavFlowField *field);
    LaiueNavStatus (*flowFieldBuild)(LaiueNavFlowField *field, const LaiueNavGrid *grid,
                                     const LaiueNavRect *bounds, const LaiueNavCell *goals,
                                     uint32_t goalCount);
    uint8_t (*flowFieldDirection)(const LaiueNavFlowField *field, int64_t x, int64_t y);
    uint32_t (*flowFieldCost)(const LaiueNavFlowField *field, int64_t x, int64_t y);
    uintptr_t reserved[8];
} LaiueNavigationServiceV1;

LAIUE_NAVIGATION_API const LaiueNavigationServiceV1 *LaiueNavigationGetStaticServiceV1(void);
LAIUE_NAVIGATION_API const LaiueModuleApiV1 *LaiueNavigationGetStaticModuleApiV1(void);
