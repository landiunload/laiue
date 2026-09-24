#include "world/world_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"
#include "numeric/numeric_service.h"
#include "world/numeric_provider.h"

#include <string.h>

typedef struct LaiueWorldModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueNumericServiceV1 *numeric;
    LaiueWorldServiceV1 service;
} LaiueWorldModuleState;

static const LaiueWorldServiceV1 compatibilityService = {
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
    .getBlockState = WorldGetBlockState,
    .enumerateOverrides = WorldEnumerateOverrides,
    .trySetBlockExplicit = WorldTrySetBlockExplicit,
};

static World *CreateWithContext(void *moduleContext,
    const WorldBaseProvider *provider)
{
    LaiueWorldModuleState *state = moduleContext;
    if (state == NULL || state->numeric == NULL || state->host == NULL)
        return NULL;
    WorldAllocator allocator = {
        .context = state->host->context,
        .allocate = state->host->allocate,
        .reallocate = state->host->reallocate,
        .free = state->host->free,
    };
    return WorldCreateWithNumericServiceAndAllocator(provider, state->numeric, &allocator);
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    if (host->allocate == NULL || host->free == NULL)
        return 0u;
    LaiueWorldModuleState *state = host->allocate(host->context,
        sizeof(*state));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->service.structSize = sizeof(state->service);
    state->service.abiVersion = LAIUE_WORLD_SERVICE_ABI_VERSION_1;
    state->service.create = WorldCreate;
    state->service.destroy = WorldDestroy;
    state->service.rebase = WorldRebase;
    state->service.getBlock = WorldGetBlock;
    state->service.trySetBlock = WorldTrySetBlock;
    state->service.setBlock = WorldSetBlock;
    state->service.applyBlockBatch = WorldApplyBlockBatch;
    state->service.getRevision = WorldGetRevision;
    state->service.fillRegion = WorldFillRegion;
    state->service.getBlockState = WorldGetBlockState;
    state->service.enumerateOverrides = WorldEnumerateOverrides;
    state->service.trySetBlockExplicit = WorldTrySetBlockExplicit;
    state->service.createWithContext = CreateWithContext;
    state->service.context = state;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueWorldModuleState *state = (LaiueWorldModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    state->numeric = NULL;
    state->numeric = (const LaiueNumericServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_NUMERIC_SERVICE_NAME,
        LAIUE_NUMERIC_SERVICE_ABI_VERSION_1, sizeof(LaiueNumericServiceV1));
    if (state->numeric == NULL)
        return 0u;
    if (!WorldTryAcquireNumericService(state, state->numeric))
    {
        state->numeric = NULL;
        return 0u;
    }
    LaiueModuleServiceV1 published = {
        .name = LAIUE_WORLD_SERVICE_NAME,
        .version = LAIUE_WORLD_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(state->service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->numeric = NULL;
        WorldReleaseNumericService(state);
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
        WorldReleaseNumericService(state);
    }
}

static void ModuleDestroy(void *context)
{
    LaiueWorldModuleState *state = context;
    WorldReleaseNumericService(state);
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    state->numeric = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
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

const LaiueWorldServiceV1 *LaiueWorldGetStaticServiceV1(void)
{
    return &compatibilityService;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
