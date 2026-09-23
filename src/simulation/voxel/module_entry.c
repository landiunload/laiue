#include "voxel/voxel_service.h"

#include "platform/system.h"
#include "mod/module_service.h"
#include "world/world.h"
#include "world/world_service.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define VOXEL_PALETTE_CAPACITY 256u

typedef struct LaiueVoxelModuleState LaiueVoxelModuleState;

struct LaiueVoxelModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueWorldServiceV1 *worldService;
    LaiueVoxelServiceV1 service;
};

/* World owns the sparse coordinate/chunk storage.  The voxel adapter only
 * adds a palette so the public block keeps its material and flags without
 * leaking that representation into the generic world runtime. */
struct LaiueVoxelWorldV1
{
    World *world;
    const LaiueWorldServiceV1 *worldService;
    PlatformMutex paletteLock;
    LaiueVoxelBlockV1 palette[VOXEL_PALETTE_CAPACITY];
    uint32_t paletteCount;
    uint8_t defaultPaletteId;
    LaiueVoxelProviderV1 provider;
};

static bool VoxelBlocksEqual(const LaiueVoxelBlockV1 *left,
                             const LaiueVoxelBlockV1 *right)
{
    return left != NULL && right != NULL && left->material == right->material &&
           left->flags == right->flags;
}

static BlockType VoxelBaseGetBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)x;
    (void)y;
    (void)z;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)context;
    return world == NULL ? BLOCK_AIR : world->defaultPaletteId;
}

static bool VoxelEncodeBlock(LaiueVoxelWorldV1 *world,
                             const LaiueVoxelBlockV1 *block, BlockType *outId)
{
    if (world == NULL || block == NULL || outId == NULL)
        return false;
    PlatformMutexLock(&world->paletteLock);
    for (uint32_t index = 0u; index < world->paletteCount; ++index)
        if (VoxelBlocksEqual(&world->palette[index], block))
        {
            *outId = (BlockType)index;
            PlatformMutexUnlock(&world->paletteLock);
            return true;
        }
    if (world->paletteCount >= VOXEL_PALETTE_CAPACITY)
    {
        PlatformMutexUnlock(&world->paletteLock);
        return false;
    }
    const uint32_t index = world->paletteCount++;
    world->palette[index] = *block;
    *outId = (BlockType)index;
    PlatformMutexUnlock(&world->paletteLock);
    return true;
}

static void VoxelDecodeBlock(const LaiueVoxelWorldV1 *world,
                             BlockType id, LaiueVoxelBlockV1 *outBlock)
{
    if (outBlock == NULL)
        return;
    *outBlock = (LaiueVoxelBlockV1){0};
    if (world == NULL || (uint32_t)id >= VOXEL_PALETTE_CAPACITY)
        return;
    PlatformMutexLock((PlatformMutex *)&world->paletteLock);
    *outBlock = world->palette[id];
    PlatformMutexUnlock((PlatformMutex *)&world->paletteLock);
}

static uint32_t VoxelGetBlock(const LaiueVoxelProviderV1 *provider,
                              const LaiueVoxelCoordV1 *coordinate,
                              LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL ||
        provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    if (world->worldService == NULL || world->worldService->getBlock == NULL)
        return 0u;
    const BlockType id = world->worldService->getBlock(
        world->world, coordinate->x, coordinate->y, (int64_t)coordinate->z);
    VoxelDecodeBlock(world, id, outBlock);
    return 1u;
}

static uint32_t VoxelGetBlockState(const LaiueVoxelProviderV1 *provider,
                                   const LaiueVoxelCoordV1 *coordinate,
                                   LaiueVoxelBlockV1 *outBlock,
                                   uint32_t *outExplicit)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL ||
        outExplicit == NULL || provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    BlockType id = BLOCK_AIR;
    bool explicitEdit = false;
    if (world->worldService == NULL || world->worldService->getBlockState == NULL ||
        !world->worldService->getBlockState(world->world, coordinate->x, coordinate->y,
                                            (int64_t)coordinate->z, &id, &explicitEdit))
        return 0u;
    VoxelDecodeBlock(world, id, outBlock);
    *outExplicit = explicitEdit ? 1u : 0u;
    return 1u;
}

typedef struct VoxelEnumerationContext
{
    const LaiueVoxelWorldV1 *world;
    uint32_t (*visitor)(void *, const LaiueVoxelCoordV1 *,
                        const LaiueVoxelBlockV1 *);
    void *visitorContext;
} VoxelEnumerationContext;

static bool VoxelEnumerationVisitor(void *context, int64_t x, int64_t y,
                                    int64_t z, BlockType id)
{
    VoxelEnumerationContext *state = (VoxelEnumerationContext *)context;
    if (state == NULL || state->world == NULL || state->visitor == NULL ||
        z < INT32_MIN || z > INT32_MAX)
        return false;
    LaiueVoxelBlockV1 block;
    VoxelDecodeBlock(state->world, id, &block);
    if (block.material == 0u)
        return true;
    const LaiueVoxelCoordV1 coordinate = {.x = x, .y = y, .z = (int32_t)z};
    return state->visitor(state->visitorContext, &coordinate, &block) != 0u;
}

static uint32_t VoxelEnumerateSolid(const LaiueVoxelProviderV1 *provider,
                                    const LaiueVoxelAabbV1 *bounds,
                                    uint32_t (*visitor)(void *, const LaiueVoxelCoordV1 *,
                                                        const LaiueVoxelBlockV1 *),
                                    void *visitorContext)
{
    if (provider == NULL || bounds == NULL || visitor == NULL ||
        provider->context == NULL || bounds->minimum.x > bounds->maximum.x ||
        bounds->minimum.y > bounds->maximum.y || bounds->minimum.z > bounds->maximum.z)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    VoxelEnumerationContext enumeration = {
        .world = world,
        .visitor = visitor,
        .visitorContext = visitorContext,
    };
    if (world->worldService == NULL || world->worldService->enumerateOverrides == NULL)
        return 0u;
    return world->worldService->enumerateOverrides(world->world,
                                   bounds->minimum.x, bounds->minimum.y,
                                   (int64_t)bounds->minimum.z,
                                   bounds->maximum.x, bounds->maximum.y,
                                   (int64_t)bounds->maximum.z,
                                   VoxelEnumerationVisitor, &enumeration)
               ? 1u
               : 0u;
}

static uint32_t VoxelBuildMesh(const LaiueVoxelProviderV1 *provider,
                               const LaiueVoxelAabbV1 *bounds,
                               void *vertexOutput, uint64_t vertexCapacity,
                               void *indexOutput, uint64_t indexCapacity,
                               LaiueVoxelMeshRangeV1 *outRange)
{
    (void)provider;
    (void)bounds;
    (void)vertexOutput;
    (void)vertexCapacity;
    (void)indexOutput;
    (void)indexCapacity;
    if (outRange != NULL)
        *outRange = (LaiueVoxelMeshRangeV1){0};
    /* Meshing remains a separate voxel_render/mesher technology. */
    return 0u;
}

static uint32_t VoxelGetProvider(LaiueVoxelWorldV1 *world,
                                 LaiueVoxelProviderV1 *outProvider)
{
    if (world == NULL || outProvider == NULL)
        return 0u;
    *outProvider = world->provider;
    return 1u;
}

static uint32_t VoxelSetBlock(LaiueVoxelWorldV1 *world,
                              const LaiueVoxelCoordV1 *coordinate,
                              const LaiueVoxelBlockV1 *block)
{
    if (world == NULL || coordinate == NULL || block == NULL)
        return 0u;
    BlockType id = BLOCK_AIR;
    if (!VoxelEncodeBlock(world, block, &id))
        return 0u;
    if (world->worldService == NULL || world->worldService->trySetBlockExplicit == NULL)
        return 0u;
    return world->worldService->trySetBlockExplicit(
               world->world, coordinate->x, coordinate->y,
               (int64_t)coordinate->z, id)
               ? 1u
               : 0u;
}

static uint32_t VoxelCreateWithWorldService(
    const LaiueWorldServiceV1 *worldService,
    const LaiueVoxelWorldConfigV1 *config, LaiueVoxelWorldV1 **outWorld)
{
    if (outWorld == NULL || worldService == NULL || worldService->create == NULL)
        return 0u;
    *outWorld = NULL;
    LaiueVoxelWorldV1 *world =
        (LaiueVoxelWorldV1 *)PlatformAllocate(sizeof(*world), true);
    if (world == NULL || !PlatformMutexInitialize(&world->paletteLock))
    {
        PlatformFree(world);
        return 0u;
    }
    world->worldService = worldService;
    world->paletteCount = 1u;
    world->palette[0] = (LaiueVoxelBlockV1){0};
    LaiueVoxelBlockV1 defaultBlock = {0};
    if (config != NULL && config->structSize >= sizeof(*config) &&
        config->abiVersion == LAIUE_VOXEL_SERVICE_ABI_VERSION_1)
        defaultBlock = config->defaultBlock;
    if (!VoxelEncodeBlock(world, &defaultBlock, &world->defaultPaletteId))
    {
        PlatformMutexDestroy(&world->paletteLock);
        PlatformFree(world);
        return 0u;
    }
    WorldBaseProvider base = {
        .context = world,
        .getBlock = VoxelBaseGetBlock,
    };
    world->world = worldService->create(&base);
    if (world->world == NULL)
    {
        PlatformMutexDestroy(&world->paletteLock);
        PlatformFree(world);
        return 0u;
    }
    world->provider.structSize = sizeof(world->provider);
    world->provider.abiVersion = LAIUE_VOXEL_ABI_VERSION_1;
    world->provider.context = world;
    world->provider.getBlock = VoxelGetBlock;
    world->provider.getBlockState = VoxelGetBlockState;
    world->provider.enumerateSolid = VoxelEnumerateSolid;
    world->provider.buildMesh = VoxelBuildMesh;
    *outWorld = world;
    return 1u;
}

static uint32_t VoxelCreateLegacy(const LaiueVoxelWorldConfigV1 *config,
                                  LaiueVoxelWorldV1 **outWorld)
{
    (void)config;
    if (outWorld != NULL)
        *outWorld = NULL;
    /* The old signature has no service context. New applications must use
     * createWithContext; returning failure is safer than importing a hidden
     * world implementation or selecting a global provider. */
    return 0u;
}

static uint32_t VoxelCreateWithContext(void *serviceContext,
                                       const LaiueVoxelWorldConfigV1 *config,
                                       LaiueVoxelWorldV1 **outWorld)
{
    const LaiueVoxelModuleState *state =
        (const LaiueVoxelModuleState *)serviceContext;
    return state == NULL ? 0u
                         : VoxelCreateWithWorldService(state->worldService,
                                                        config, outWorld);
}

static void VoxelDestroy(LaiueVoxelWorldV1 *world)
{
    if (world == NULL)
        return;
    if (world->worldService != NULL && world->worldService->destroy != NULL)
        world->worldService->destroy(world->world);
    PlatformMutexDestroy(&world->paletteLock);
    PlatformFree(world);
}

static uint64_t VoxelGetRevision(const LaiueVoxelWorldV1 *world)
{
    return world == NULL || world->worldService == NULL ||
                   world->worldService->getRevision == NULL
               ? 0u
               : world->worldService->getRevision(world->world);
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    *outContext = NULL;
    LaiueVoxelModuleState *state =
        (LaiueVoxelModuleState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    state->host = host;
    state->worldService = NULL;
    state->service.structSize = sizeof(state->service);
    state->service.abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1;
    state->service.create = VoxelCreateLegacy;
    state->service.destroy = VoxelDestroy;
    state->service.getProvider = VoxelGetProvider;
    state->service.setBlock = VoxelSetBlock;
    state->service.getRevision = VoxelGetRevision;
    state->service.createWithContext = VoxelCreateWithContext;
    state->service.context = state;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    state->worldService = (const LaiueWorldServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1,
        sizeof(LaiueWorldServiceV1));
    if (state->worldService == NULL || state->worldService->create == NULL ||
        state->worldService->destroy == NULL || state->worldService->getBlock == NULL ||
        state->worldService->getBlockState == NULL ||
        state->worldService->enumerateOverrides == NULL ||
        state->worldService->trySetBlockExplicit == NULL)
    {
        state->worldService = NULL;
        return 0u;
    }
    LaiueModuleServiceV1 published = {
        .name = LAIUE_VOXEL_SERVICE_NAME,
        .version = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(state->service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->worldService = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_VOXEL_SERVICE_NAME);
    if (state != NULL)
        state->worldService = NULL;
}

static void ModuleDestroy(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL)
    {
        state->host = NULL;
        state->worldService = NULL;
        PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_VOXEL_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.voxel",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = 1u,
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueVoxelGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
