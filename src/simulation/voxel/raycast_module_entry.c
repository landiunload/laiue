#include "voxel/raycast_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"
#include "platform/system.h"
#include "world/world_service.h"

#include <string.h>

typedef struct LaiueVoxelRaycastModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueWorldServiceV1 *world;
    LaiueVoxelRaycastServiceV1 service;
} LaiueVoxelRaycastModuleState;

/* Compatibility calls predate the context-aware tail and therefore cannot
 * carry a module instance. Keep the fallback deliberately narrow; new code
 * must call raycastWithContext and pass the published context. */
static const LaiueWorldServiceV1 *compatWorldService;

typedef struct RaycastQueryContext
{
    const LaiueWorldServiceV1 *worldService;
    World *world;
} RaycastQueryContext;

static BlockType ModuleGetBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    const RaycastQueryContext *query = (const RaycastQueryContext *)context;
    return query != NULL && query->worldService != NULL &&
                   query->worldService->getBlock != NULL && query->world != NULL
               ? query->worldService->getBlock(query->world, x, y, z)
               : BLOCK_AIR;
}

static bool ModuleRaycastWithState(const LaiueVoxelRaycastModuleState *state,
                                   World *world, const double origin[3],
                                   const float direction[3], float maximumDistance,
                                   VoxelRaycastHit *outHit)
{
    if (state == NULL || world == NULL || state->world == NULL ||
        state->world->getBlock == NULL)
        return false;
    RaycastQueryContext query = {state->world, world};
    return VoxelRaycastWithBlockQuery(&query, ModuleGetBlock, origin, direction,
                                      maximumDistance, outHit);
}

static bool ModuleRaycast(World *world, const double origin[3], const float direction[3],
                          float maximumDistance, VoxelRaycastHit *outHit)
{
    LaiueVoxelRaycastModuleState compatibility = {
        .world = compatWorldService,
    };
    return ModuleRaycastWithState(&compatibility, world, origin, direction,
                                  maximumDistance, outHit);
}

static bool ModuleRaycastWithContext(void *moduleContext, World *world,
                                     const double origin[3], const float direction[3],
                                     float maximumDistance, VoxelRaycastHit *outHit)
{
    return ModuleRaycastWithState((const LaiueVoxelRaycastModuleState *)moduleContext,
                                  world, origin, direction, maximumDistance, outHit);
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    LaiueVoxelRaycastModuleState *state =
        (LaiueVoxelRaycastModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->service = (LaiueVoxelRaycastServiceV1){
        .structSize = sizeof(LaiueVoxelRaycastServiceV1),
        .abiVersion = LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1,
        .raycast = ModuleRaycast,
        .raycastWithContext = ModuleRaycastWithContext,
        .context = state,
    };
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelRaycastModuleState *state = (LaiueVoxelRaycastModuleState *)context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return 0u;
    state->world = NULL;
    uint32_t version = 0u;
    uint32_t size = 0u;
    state->world = (const LaiueWorldServiceV1 *)state->host->queryService(
        state->host->context, LAIUE_WORLD_SERVICE_NAME,
        LAIUE_WORLD_SERVICE_ABI_VERSION_1, sizeof(LaiueWorldServiceV1),
        &version, &size);
    if (state->world == NULL || version < LAIUE_WORLD_SERVICE_ABI_VERSION_1 ||
        size < sizeof(*state->world) || state->world->getBlock == NULL)
    {
        state->world = NULL;
        return 0u;
    }
    compatWorldService = state->world;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_VOXEL_RAYCAST_SERVICE_NAME,
        .version = LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(state->service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->world = NULL;
        compatWorldService = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueVoxelRaycastModuleState *state = (LaiueVoxelRaycastModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_VOXEL_RAYCAST_SERVICE_NAME);
    if (state != NULL)
        state->world = NULL;
    compatWorldService = NULL;
}

static void ModuleDestroy(void *context)
{
    LaiueVoxelRaycastModuleState *state = (LaiueVoxelRaycastModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        const LaiueWorldServiceV1 *world = state->world;
        state->host = NULL;
        state->world = NULL;
        if (compatWorldService == world)
            compatWorldService = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
        else
            PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_VOXEL_RAYCAST_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.voxel_raycast",
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

const LaiueModuleApiV1 *LaiueVoxelRaycastGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
