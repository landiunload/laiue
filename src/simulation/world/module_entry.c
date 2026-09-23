#include "world/world_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"
#include "numeric/numeric_service.h"
#include "world/numeric_provider.h"

typedef struct LaiueWorldModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueNumericServiceV1 *numeric;
} LaiueWorldModuleState;

static LaiueWorldModuleState moduleState;

static const LaiueWorldServiceV1 service = {
    .structSize = sizeof(LaiueWorldServiceV1),
    .abiVersion = LAIUE_WORLD_SERVICE_ABI_VERSION_1,
    .create = WorldCreate,
    .destroy = WorldDestroy,
    .rebase = WorldRebase,
    .getBlock = WorldGetBlock,
    .trySetBlock = WorldTrySetBlock,
    .setBlock = WorldSetBlock,
    .applyBlockBatch = WorldApplyBlockBatch,
    .getRevision = WorldGetRevision,
    .fillRegion = WorldFillRegion,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.numeric = NULL;
    WorldSetNumericService(NULL);
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueWorldModuleState *state = (LaiueWorldModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    WorldSetNumericService(NULL);
    state->numeric = (const LaiueNumericServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_NUMERIC_SERVICE_NAME,
        LAIUE_NUMERIC_SERVICE_ABI_VERSION_1, sizeof(LaiueNumericServiceV1));
    if (state->numeric == NULL)
        return 0u;
    WorldSetNumericService(state->numeric);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_WORLD_SERVICE_NAME,
        .version = LAIUE_WORLD_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->numeric = NULL;
        WorldSetNumericService(NULL);
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueWorldModuleState *state = (LaiueWorldModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_WORLD_SERVICE_NAME);
    if (state != NULL)
    {
        state->numeric = NULL;
        WorldSetNumericService(NULL);
    }
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    moduleState.numeric = NULL;
    WorldSetNumericService(NULL);
}

static const char *const provides[] = {LAIUE_WORLD_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_NUMERIC_SERVICE_NAME, LAIUE_NUMERIC_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.world",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = sizeof(requiresServices) / sizeof(requiresServices[0]),
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueWorldGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
