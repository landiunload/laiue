#include "voxel/raycast_service.h"

#include "mod/module_api.h"
#include "world/world_service.h"

typedef struct LaiueVoxelRaycastModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueWorldServiceV1 *world;
} LaiueVoxelRaycastModuleState;

static LaiueVoxelRaycastModuleState moduleState;

static BlockType ModuleGetBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    return moduleState.world->getBlock((World *)context, x, y, z);
}

static bool ModuleRaycast(World *world, const double origin[3], const float direction[3],
    float maximumDistance, VoxelRaycastHit *outHit)
{
    if (world == NULL || moduleState.world == NULL || moduleState.world->getBlock == NULL)
        return false;
    /* The service table is resolved once at start; no string lookup occurs in
     * the hot ray traversal. The callback context is the host-owned World. */
    return VoxelRaycastWithBlockQuery(
        (void *)world, ModuleGetBlock, origin, direction, maximumDistance, outHit);
}

static const LaiueVoxelRaycastServiceV1 service = {
    .structSize = sizeof(LaiueVoxelRaycastServiceV1),
    .abiVersion = LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1,
    .raycast = ModuleRaycast,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.world = NULL;
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelRaycastModuleState *state = (LaiueVoxelRaycastModuleState *)context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return 0u;
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
    LaiueModuleServiceV1 published = {
        .name = LAIUE_VOXEL_RAYCAST_SERVICE_NAME,
        .version = LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1,
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
    LaiueVoxelRaycastModuleState *state = (LaiueVoxelRaycastModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_VOXEL_RAYCAST_SERVICE_NAME);
    if (state != NULL)
        state->world = NULL;
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    moduleState.world = NULL;
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
