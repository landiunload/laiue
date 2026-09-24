#include "voxel/voxel_service.h"

#include "platform/system.h"
#include "mod/module_service.h"
#include "world/world.h"
#include "world/world_service.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define VOXEL_PALETTE_CAPACITY 256u

typedef struct LaiueVoxelModuleState LaiueVoxelModuleState;

struct LaiueVoxelModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueWorldServiceV1 *worldService;
    uint32_t worldServiceSize;
    uint32_t worldServiceVersion;
    LaiueVoxelServiceV1 service;
    LaiueVoxelServiceV2 serviceV2;
};

/* World owns the sparse coordinate/chunk storage.  The voxel adapter only
 * adds a palette so the public block keeps its material and flags without
 * leaking that representation into the generic world runtime. */
struct LaiueVoxelWorldV1
{
    World *world;
    const LaiueWorldServiceV1 *worldService;
    uint32_t worldServiceSize;
    const LaiueModuleHostV1 *host;
    PlatformMutex paletteLock;
    LaiueVoxelBlockV1 palette[VOXEL_PALETTE_CAPACITY];
    uint32_t paletteCount;
    uint8_t defaultPaletteId;
    LaiueVoxelProviderV1 provider;
};

static bool ServiceFieldPresent(uint32_t actualSize, uint32_t declaredSize,
                                size_t offset, size_t size)
{
    return (size_t)actualSize >= offset && (size_t)actualSize - offset >= size &&
           (size_t)declaredSize >= offset && (size_t)declaredSize - offset >= size;
}

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
    if (world == NULL)
        return;
    PlatformMutexLock((PlatformMutex *)&world->paletteLock);
    if ((uint32_t)id < world->paletteCount)
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

static uint32_t VoxelGetBlockV2(const LaiueVoxelProviderV2 *provider,
                                const LaiueVoxelCoordV2 *coordinate,
                                LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL ||
        provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world =
        (const LaiueVoxelWorldV1 *)provider->context;
    if (world->worldService == NULL || world->worldService->getBlock == NULL)
        return 0u;
    const BlockType id = world->worldService->getBlock(
        world->world, coordinate->x, coordinate->y, coordinate->z);
    VoxelDecodeBlock(world, id, outBlock);
    return 1u;
}

static uint32_t VoxelGetBlockStateV2(const LaiueVoxelProviderV2 *provider,
                                     const LaiueVoxelCoordV2 *coordinate,
                                     LaiueVoxelBlockV1 *outBlock,
                                     uint32_t *outExplicit)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL ||
        outExplicit == NULL || provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world =
        (const LaiueVoxelWorldV1 *)provider->context;
    BlockType id = BLOCK_AIR;
    bool explicitEdit = false;
    if (world->worldService == NULL || world->worldService->getBlockState == NULL ||
        !world->worldService->getBlockState(world->world, coordinate->x,
                                            coordinate->y, coordinate->z, &id,
                                            &explicitEdit))
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

typedef struct VoxelEnumerationContextV2
{
    const LaiueVoxelWorldV1 *world;
    uint32_t (*visitor)(void *, const LaiueVoxelCoordV2 *,
                        const LaiueVoxelBlockV1 *);
    void *visitorContext;
} VoxelEnumerationContextV2;

static bool VoxelEnumerationVisitorV2(void *context, int64_t x, int64_t y,
                                      int64_t z, BlockType id)
{
    VoxelEnumerationContextV2 *state = (VoxelEnumerationContextV2 *)context;
    if (state == NULL || state->world == NULL || state->visitor == NULL)
        return false;
    LaiueVoxelBlockV1 block;
    VoxelDecodeBlock(state->world, id, &block);
    if (block.material == 0u)
        return true;
    const LaiueVoxelCoordV2 coordinate = {.x = x, .y = y, .z = z};
    return state->visitor(state->visitorContext, &coordinate, &block) != 0u;
}

static uint32_t VoxelEnumerateSolidV2(
    const LaiueVoxelProviderV2 *provider, const LaiueVoxelAabbV2 *bounds,
    uint32_t (*visitor)(void *, const LaiueVoxelCoordV2 *,
                        const LaiueVoxelBlockV1 *),
    void *visitorContext)
{
    if (provider == NULL || bounds == NULL || visitor == NULL ||
        provider->context == NULL || bounds->minimum.x > bounds->maximum.x ||
        bounds->minimum.y > bounds->maximum.y || bounds->minimum.z > bounds->maximum.z)
        return 0u;
    const LaiueVoxelWorldV1 *world =
        (const LaiueVoxelWorldV1 *)provider->context;
    VoxelEnumerationContextV2 enumeration = {
        .world = world,
        .visitor = visitor,
        .visitorContext = visitorContext,
    };
    if (world->worldService == NULL || world->worldService->enumerateOverrides == NULL)
        return 0u;
    return world->worldService->enumerateOverrides(
               world->world, bounds->minimum.x, bounds->minimum.y,
               bounds->minimum.z, bounds->maximum.x, bounds->maximum.y,
               bounds->maximum.z, VoxelEnumerationVisitorV2, &enumeration)
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

static uint32_t VoxelBuildMeshV2(const LaiueVoxelProviderV2 *provider,
                                 const LaiueVoxelAabbV2 *bounds,
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
    return 0u;
}

/* The V1 create signature has no service context. Keep one narrow bridge for
 * legacy callers while all new code uses createWithContext. */
static const LaiueWorldServiceV1 *compatWorldService;

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

static uint32_t VoxelGetProviderV2(LaiueVoxelWorldV2 *world,
                                   LaiueVoxelProviderV2 *outProvider)
{
    if (world == NULL || outProvider == NULL)
        return 0u;
    *outProvider = (LaiueVoxelProviderV2){
        .structSize = sizeof(*outProvider),
        .abiVersion = LAIUE_VOXEL_ABI_VERSION_2,
        .context = world,
        .getBlock = VoxelGetBlockV2,
        .getBlockState = VoxelGetBlockStateV2,
        .enumerateSolid = VoxelEnumerateSolidV2,
        .buildMesh = VoxelBuildMeshV2,
    };
    return 1u;
}

static uint32_t VoxelSetBlockV2(LaiueVoxelWorldV2 *world,
                                const LaiueVoxelCoordV2 *coordinate,
                                const LaiueVoxelBlockV1 *block)
{
    if (world == NULL || coordinate == NULL || block == NULL)
        return 0u;
    BlockType id = BLOCK_AIR;
    if (!VoxelEncodeBlock(world, block, &id) || world->worldService == NULL ||
        world->worldService->trySetBlockExplicit == NULL)
        return 0u;
    return world->worldService->trySetBlockExplicit(
               world->world, coordinate->x, coordinate->y, coordinate->z, id)
               ? 1u
               : 0u;
}

static void *VoxelAllocate(const LaiueModuleHostV1 *host, size_t size, bool zero)
{
    void *memory = host != NULL && host->allocate != NULL
                       ? host->allocate(host->context, (uint64_t)size)
                       : PlatformAllocate(size, false);
    if (memory != NULL && zero)
        memset(memory, 0, size);
    return memory;
}

static void VoxelFree(const LaiueModuleHostV1 *host, void *memory)
{
    if (memory == NULL)
        return;
    if (host != NULL && host->free != NULL)
        host->free(host->context, memory);
    else
        PlatformFree(memory);
}

static uint32_t VoxelCreateWithWorldService(
    const LaiueWorldServiceV1 *worldService,
    uint32_t worldServiceSize,
    const LaiueVoxelWorldConfigV1 *config,
    const LaiueModuleHostV1 *host,
    LaiueVoxelWorldV1 **outWorld)
{
    if (outWorld == NULL || worldService == NULL ||
        !ServiceFieldPresent(worldServiceSize, worldService->structSize,
                             offsetof(LaiueWorldServiceV1, create),
                             sizeof(worldService->create)) ||
        worldService->create == NULL)
        return 0u;
    *outWorld = NULL;
    LaiueVoxelWorldV1 *world =
        (LaiueVoxelWorldV1 *)VoxelAllocate(host, sizeof(*world), true);
    if (world == NULL || !PlatformMutexInitialize(&world->paletteLock))
    {
        VoxelFree(host, world);
        return 0u;
    }
    world->worldService = worldService;
    world->worldServiceSize = worldServiceSize;
    world->host = host;
    world->paletteCount = 1u;
    world->palette[0] = (LaiueVoxelBlockV1){0};
    LaiueVoxelBlockV1 defaultBlock = {0};
    if (config != NULL && config->structSize >= sizeof(*config) &&
        config->abiVersion == LAIUE_VOXEL_SERVICE_ABI_VERSION_1)
        defaultBlock = config->defaultBlock;
    if (!VoxelEncodeBlock(world, &defaultBlock, &world->defaultPaletteId))
    {
        PlatformMutexDestroy(&world->paletteLock);
        VoxelFree(host, world);
        return 0u;
    }
    WorldBaseProvider base = {
        .context = world,
        .getBlock = VoxelBaseGetBlock,
    };
    if (ServiceFieldPresent(world->worldServiceSize, world->worldService->structSize,
            offsetof(LaiueWorldServiceV1, createWithContext),
            sizeof(worldService->createWithContext)) &&
        worldService->createWithContext != NULL &&
        ServiceFieldPresent(world->worldServiceSize, world->worldService->structSize,
            offsetof(LaiueWorldServiceV1, context), sizeof(worldService->context)) &&
        worldService->context != NULL)
    {
        world->world = worldService->createWithContext(worldService->context, &base);
    }
    else
    {
        world->world = worldService->create(&base);
    }
    if (world->world == NULL)
    {
        PlatformMutexDestroy(&world->paletteLock);
        VoxelFree(host, world);
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
    return VoxelCreateWithWorldService(compatWorldService,
                                       compatWorldService == NULL
                                           ? 0u
                                           : (uint32_t)sizeof(*compatWorldService),
                                       config, NULL, outWorld);
}

static uint32_t VoxelCreateWithContext(void *serviceContext,
                                       const LaiueVoxelWorldConfigV1 *config,
                                       LaiueVoxelWorldV1 **outWorld)
{
    const LaiueVoxelModuleState *state =
        (const LaiueVoxelModuleState *)serviceContext;
    return state == NULL ? 0u
                         : VoxelCreateWithWorldService(state->worldService,
                                                        state->worldServiceSize, config,
                                                        state->host, outWorld);
}

static void VoxelDestroy(LaiueVoxelWorldV1 *world)
{
    if (world == NULL)
        return;
    if (world->worldService != NULL && world->worldService->destroy != NULL)
        world->worldService->destroy(world->world);
    PlatformMutexDestroy(&world->paletteLock);
    VoxelFree(world->host, world);
}

static uint64_t VoxelGetRevision(const LaiueVoxelWorldV1 *world)
{
    return world == NULL || world->worldService == NULL ||
                   world->worldService->getRevision == NULL
               ? 0u
               : world->worldService->getRevision(world->world);
}

static uint32_t VoxelRebase(LaiueVoxelWorldV1 *world, int64_t blockShiftX,
                            int64_t blockShiftY, int64_t blockShiftZ)
{
    if (world == NULL || world->worldService == NULL ||
        !ServiceFieldPresent(world->worldServiceSize,
                             world->worldService->structSize,
                             offsetof(LaiueWorldServiceV1, rebase),
                             sizeof(world->worldService->rebase)) ||
        world->worldService->rebase == NULL)
        return 0u;
    return world->worldService->rebase(world->world, blockShiftX, blockShiftY,
                                       blockShiftZ)
               ? 1u
               : 0u;
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    LaiueVoxelModuleState *state =
        (LaiueVoxelModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->worldService = NULL;
    state->worldServiceSize = 0u;
    state->worldServiceVersion = 0u;
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
    state->service.rebase = VoxelRebase;
    state->serviceV2.structSize = sizeof(state->serviceV2);
    state->serviceV2.abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_2;
    state->serviceV2.createWithContext = VoxelCreateWithContext;
    state->serviceV2.destroy = VoxelDestroy;
    state->serviceV2.getProvider = VoxelGetProviderV2;
    state->serviceV2.setBlock = VoxelSetBlockV2;
    state->serviceV2.getRevision = VoxelGetRevision;
    state->serviceV2.context = state;
    state->serviceV2.rebase = VoxelRebase;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    LaiueModuleServiceViewV1 worldView;
    state->worldService = NULL;
    state->worldServiceSize = 0u;
    state->worldServiceVersion = 0u;
    if (!LaiueModuleQueryServiceView(
            state->host, LAIUE_WORLD_SERVICE_NAME,
            LAIUE_WORLD_SERVICE_ABI_VERSION_1,
            LAIUE_WORLD_SERVICE_V1_LEGACY_SIZE, &worldView))
    {
        compatWorldService = NULL;
        return 0u;
    }
    state->worldService = (const LaiueWorldServiceV1 *)worldView.table;
    state->worldServiceSize = worldView.tableSize;
    state->worldServiceVersion = worldView.version;
    if (state->worldService == NULL || state->worldService->create == NULL ||
        state->worldService->destroy == NULL || state->worldService->getBlock == NULL ||
        !ServiceFieldPresent(state->worldServiceSize, state->worldService->structSize,
            offsetof(LaiueWorldServiceV1, getBlockState),
            sizeof(state->worldService->getBlockState)) ||
        !ServiceFieldPresent(state->worldServiceSize, state->worldService->structSize,
            offsetof(LaiueWorldServiceV1, enumerateOverrides),
            sizeof(state->worldService->enumerateOverrides)) ||
        !ServiceFieldPresent(state->worldServiceSize, state->worldService->structSize,
            offsetof(LaiueWorldServiceV1, trySetBlockExplicit),
            sizeof(state->worldService->trySetBlockExplicit)) ||
        state->worldService->getBlockState == NULL ||
        state->worldService->enumerateOverrides == NULL ||
        state->worldService->trySetBlockExplicit == NULL)
    {
        state->worldService = NULL;
        state->worldServiceSize = 0u;
        state->worldServiceVersion = 0u;
        compatWorldService = NULL;
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
        state->worldServiceSize = 0u;
        state->worldServiceVersion = 0u;
        return 0u;
    }
    LaiueModuleServiceV1 publishedV2 = {
        .name = LAIUE_VOXEL_SERVICE_NAME_V2,
        .version = LAIUE_VOXEL_SERVICE_ABI_VERSION_2,
        .table = &state->serviceV2,
        .tableSize = sizeof(state->serviceV2),
    };
    if (state->host->publishService(state->host->context, &publishedV2) !=
        LAIUE_MODULE_OK)
    {
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_VOXEL_SERVICE_NAME);
        state->worldService = NULL;
        state->worldServiceSize = 0u;
        state->worldServiceVersion = 0u;
        return 0u;
    }
    /* Publish the legacy bridge only after both service tables are live. */
    compatWorldService = state->worldService;
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
    {
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_VOXEL_SERVICE_NAME_V2);
        (void)state->host->unpublishService(state->host->context, LAIUE_VOXEL_SERVICE_NAME);
    }
    if (state != NULL)
    {
        if (compatWorldService == state->worldService)
            compatWorldService = NULL;
        state->worldService = NULL;
        state->worldServiceSize = 0u;
        state->worldServiceVersion = 0u;
    }
}

static void ModuleDestroy(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        if (compatWorldService == state->worldService)
            compatWorldService = NULL;
        state->worldService = NULL;
        state->worldServiceSize = 0u;
        state->worldServiceVersion = 0u;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {
    LAIUE_VOXEL_SERVICE_NAME,
    LAIUE_VOXEL_SERVICE_NAME_V2,
};
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
        .providesCount = sizeof(provides) / sizeof(provides[0]),
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
