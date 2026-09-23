#include "voxel/voxel_service.h"

#include "platform/system.h"

#include <limits.h>

typedef struct VoxelEntry
{
    uint64_t hash;
    LaiueVoxelCoordV1 coordinate;
    LaiueVoxelBlockV1 block;
    uint32_t used;
} VoxelEntry;

struct LaiueVoxelWorldV1
{
    VoxelEntry *entries;
    uint32_t capacity;
    uint32_t count;
    LaiueVoxelBlockV1 defaultBlock;
    uint64_t revision;
    LaiueVoxelProviderV1 provider;
};

static uint64_t Mix64(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static uint64_t CoordinateHash(const LaiueVoxelCoordV1 *coordinate)
{
    uint64_t hash = Mix64((uint64_t)coordinate->x);
    hash ^= Mix64((uint64_t)coordinate->y + UINT64_C(0x9e3779b97f4a7c15));
    hash ^= Mix64((uint64_t)(int64_t)coordinate->z + UINT64_C(0x243f6a8885a308d3));
    return Mix64(hash);
}

static bool SameCoordinate(const VoxelEntry *entry, const LaiueVoxelCoordV1 *coordinate)
{
    return entry->coordinate.x == coordinate->x && entry->coordinate.y == coordinate->y &&
           entry->coordinate.z == coordinate->z;
}

static uint32_t FindSlot(const LaiueVoxelWorldV1 *world,
                         const LaiueVoxelCoordV1 *coordinate, uint64_t hash,
                         uint32_t *outFound)
{
    uint32_t slot = (uint32_t)hash & (world->capacity - 1U);
    for (uint32_t probe = 0U; probe < world->capacity; ++probe)
    {
        const VoxelEntry *entry = &world->entries[slot];
        if (!entry->used)
        {
            *outFound = 0U;
            return slot;
        }
        if (entry->hash == hash && SameCoordinate(entry, coordinate))
        {
            *outFound = 1U;
            return slot;
        }
        slot = (slot + 1U) & (world->capacity - 1U);
    }
    *outFound = 0U;
    return UINT32_MAX;
}

static bool InsertEntry(VoxelEntry *entries, uint32_t capacity, const VoxelEntry *source)
{
    uint32_t slot = (uint32_t)source->hash & (capacity - 1U);
    for (uint32_t probe = 0U; probe < capacity; ++probe)
    {
        if (!entries[slot].used)
        {
            entries[slot] = *source;
            return true;
        }
        slot = (slot + 1U) & (capacity - 1U);
    }
    return false;
}

static bool Resize(VoxelEntry **entries, uint32_t *capacity, uint32_t newCapacity)
{
    VoxelEntry *newEntries =
        (VoxelEntry *)PlatformAllocate((size_t)newCapacity * sizeof(*newEntries), true);
    if (newEntries == NULL)
        return false;
    VoxelEntry *oldEntries = *entries;
    uint32_t oldCapacity = *capacity;
    for (uint32_t index = 0U; index < oldCapacity; ++index)
    {
        if (oldEntries[index].used && !InsertEntry(newEntries, newCapacity, &oldEntries[index]))
        {
            PlatformFree(newEntries);
            return false;
        }
    }
    PlatformFree(oldEntries);
    *entries = newEntries;
    *capacity = newCapacity;
    return true;
}

static uint32_t VoxelGetBlock(const LaiueVoxelProviderV1 *provider,
                              const LaiueVoxelCoordV1 *coordinate,
                              LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL || provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    *outBlock = world->defaultBlock;
    const uint64_t hash = CoordinateHash(coordinate);
    uint32_t found = 0U;
    uint32_t slot = FindSlot(world, coordinate, hash, &found);
    if (slot != UINT32_MAX && found != 0U)
        *outBlock = world->entries[slot].block;
    return 1u;
}

static uint32_t VoxelGetBlockState(const LaiueVoxelProviderV1 *provider,
                                   const LaiueVoxelCoordV1 *coordinate,
                                   LaiueVoxelBlockV1 *outBlock,
                                   uint32_t *outExplicit)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL || outExplicit == NULL ||
        provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    *outBlock = world->defaultBlock;
    *outExplicit = 0u;
    const uint64_t hash = CoordinateHash(coordinate);
    uint32_t found = 0U;
    const uint32_t slot = FindSlot(world, coordinate, hash, &found);
    if (slot != UINT32_MAX && found != 0U)
    {
        *outBlock = world->entries[slot].block;
        *outExplicit = 1u;
    }
    return 1u;
}

static uint32_t VoxelEnumerateSolid(const LaiueVoxelProviderV1 *provider,
                                    const LaiueVoxelAabbV1 *bounds,
                                    uint32_t (*visitor)(void *, const LaiueVoxelCoordV1 *,
                                                        const LaiueVoxelBlockV1 *),
                                    void *visitorContext)
{
    if (provider == NULL || bounds == NULL || visitor == NULL || provider->context == NULL)
        return 0u;
    const LaiueVoxelWorldV1 *world = (const LaiueVoxelWorldV1 *)provider->context;
    for (uint32_t index = 0U; index < world->capacity; ++index)
    {
        const VoxelEntry *entry = &world->entries[index];
        if (!entry->used || entry->block.material == 0U ||
            entry->coordinate.x < bounds->minimum.x || entry->coordinate.x > bounds->maximum.x ||
            entry->coordinate.y < bounds->minimum.y || entry->coordinate.y > bounds->maximum.y ||
            entry->coordinate.z < bounds->minimum.z || entry->coordinate.z > bounds->maximum.z)
            continue;
        if (visitor(visitorContext, &entry->coordinate, &entry->block) == 0U)
            return 0u;
    }
    return 1u;
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
    {
        outRange->vertexOffset = 0U;
        outRange->vertexCount = 0U;
        outRange->indexOffset = 0U;
        outRange->indexCount = 0U;
    }
    /* Meshing is intentionally a separate voxel_render/mesher technology. */
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
    const uint64_t hash = CoordinateHash(coordinate);
    uint32_t found = 0U;
    uint32_t slot = FindSlot(world, coordinate, hash, &found);
    if (slot == UINT32_MAX)
        return 0u;
    if (found != 0U)
    {
        /* Keep default/air writes as explicit entries. This lets a sparse
         * edit mask a game-owned infinite terrain provider. */
        world->entries[slot].block = *block;
        ++world->revision;
        return 1u;
    }
    if (world->count > world->capacity - world->capacity / 3U)
    {
        if (world->capacity > UINT32_MAX / 2U ||
            !Resize(&world->entries, &world->capacity, world->capacity * 2U))
            return 0u;
        slot = FindSlot(world, coordinate, hash, &found);
        if (slot == UINT32_MAX || found != 0U)
            return 0u;
    }
    world->entries[slot].hash = hash;
    world->entries[slot].coordinate = *coordinate;
    world->entries[slot].block = *block;
    world->entries[slot].used = 1U;
    ++world->count;
    ++world->revision;
    return 1u;
}

static uint32_t VoxelCreate(const LaiueVoxelWorldConfigV1 *config,
                            LaiueVoxelWorldV1 **outWorld)
{
    if (outWorld == NULL)
        return 0u;
    *outWorld = NULL;
    LaiueVoxelWorldV1 *world =
        (LaiueVoxelWorldV1 *)PlatformAllocate(sizeof(*world), true);
    if (world == NULL)
        return 0u;
    world->capacity = 64U;
    world->entries = (VoxelEntry *)PlatformAllocate(
        (size_t)world->capacity * sizeof(*world->entries), true);
    if (world->entries == NULL)
    {
        PlatformFree(world);
        return 0u;
    }
    if (config != NULL && config->structSize >= sizeof(*config) &&
        config->abiVersion == LAIUE_VOXEL_SERVICE_ABI_VERSION_1)
        world->defaultBlock = config->defaultBlock;
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

static void VoxelDestroy(LaiueVoxelWorldV1 *world)
{
    if (world == NULL)
        return;
    PlatformFree(world->entries);
    PlatformFree(world);
}

static uint64_t VoxelGetRevision(const LaiueVoxelWorldV1 *world)
{
    return world == NULL ? 0U : world->revision;
}

static const LaiueVoxelServiceV1 service = {
    .structSize = sizeof(LaiueVoxelServiceV1),
    .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
    .create = VoxelCreate,
    .destroy = VoxelDestroy,
    .getProvider = VoxelGetProvider,
    .setBlock = VoxelSetBlock,
    .getRevision = VoxelGetRevision,
};

typedef struct LaiueVoxelModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueVoxelModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL)
        return 0u;
    *outContext = NULL;
    LaiueVoxelModuleState *state =
        (LaiueVoxelModuleState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_VOXEL_SERVICE_NAME,
        .version = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
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
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_VOXEL_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueVoxelModuleState *state = (LaiueVoxelModuleState *)context;
    if (state != NULL)
    {
        state->host = NULL;
        PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_VOXEL_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.voxel",
        .version = "1.0.0",
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
