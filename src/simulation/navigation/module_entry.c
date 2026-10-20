#include "navigation/navigation_service.h"

static const LaiueNavigationServiceV1 service = {
    .structSize = sizeof(LaiueNavigationServiceV1),
    .abiVersion = LAIUE_NAVIGATION_SERVICE_ABI_VERSION_1,
    .graphCreate = LaiueNavGraphCreate,
    .graphDestroy = LaiueNavGraphDestroy,
    .graphAddNode = LaiueNavGraphAddNode,
    .graphRemoveNode = LaiueNavGraphRemoveNode,
    .graphGetNode = LaiueNavGraphGetNode,
    .graphSetNodePosition = LaiueNavGraphSetNodePosition,
    .graphAddEdge = LaiueNavGraphAddEdge,
    .graphRemoveEdge = LaiueNavGraphRemoveEdge,
    .graphGetEdge = LaiueNavGraphGetEdge,
    .graphSetEdgeCost = LaiueNavGraphSetEdgeCost,
    .graphSetEdgeModes = LaiueNavGraphSetEdgeModes,
    .graphPrepare = LaiueNavGraphPrepare,
    .graphIsPrepared = LaiueNavGraphIsPrepared,
    .graphNodeCount = LaiueNavGraphNodeCount,
    .graphEdgeCount = LaiueNavGraphEdgeCount,
    .graphRevision = LaiueNavGraphRevision,
    .queryCreate = LaiueNavQueryCreate,
    .queryDestroy = LaiueNavQueryDestroy,
    .findPath = LaiueNavFindPath,
    .queryBegin = LaiueNavQueryBegin,
    .queryContinue = LaiueNavQueryContinue,
    .queryGetPath = LaiueNavQueryGetPath,
    .findPathsBatch = LaiueNavFindPathsBatch,
    .gridCreate = LaiueNavGridCreate,
    .gridDestroy = LaiueNavGridDestroy,
    .gridSetCell = LaiueNavGridSetCell,
    .gridGetCell = LaiueNavGridGetCell,
    .gridFillRect = LaiueNavGridFillRect,
    .gridRevision = LaiueNavGridRevision,
    .gridFindPath = LaiueNavGridFindPath,
    .gridSmoothPath = LaiueNavGridSmoothPath,
    .flowFieldCreate = LaiueNavFlowFieldCreate,
    .flowFieldDestroy = LaiueNavFlowFieldDestroy,
    .flowFieldBuild = LaiueNavFlowFieldBuild,
    .flowFieldDirection = LaiueNavFlowFieldDirection,
    .flowFieldCost = LaiueNavFlowFieldCost,
};

typedef struct NavigationModuleState
{
    const LaiueModuleHostV1 *host;
} NavigationModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    NavigationModuleState *state = host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    NavigationModuleState *state = context;
    const LaiueModuleServiceV1 published = {
        .name = LAIUE_NAVIGATION_SERVICE_NAME,
        .version = LAIUE_NAVIGATION_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    NavigationModuleState *state = context;
    if (state != NULL && state->host != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_NAVIGATION_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    NavigationModuleState *state = context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_NAVIGATION_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.navigation",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueNavigationServiceV1 *LaiueNavigationGetStaticServiceV1(void)
{
    return &service;
}

const LaiueModuleApiV1 *LaiueNavigationGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
