#include "world/world.h"
#include "world/block_properties.h"
#include "world/infinite_coord.h"
#include "world/terrain_noise.h"
#include "platform/system.h"

#include <limits.h>
#include <string.h>

typedef struct LocalChunkCoordinate
{
    int64_t x;
    int64_t y;
    int64_t z;
} LocalChunkCoordinate;

typedef struct CoordinateFrame
{
    InfiniteCoord chunkOrigin[3];
    uint32_t referenceCount;
} CoordinateFrame;

typedef struct GlobalChunkCoordinate
{
    uint64_t hash;
    CoordinateFrame* frame;
    LocalChunkCoordinate local;
} GlobalChunkCoordinate;

// 18 бит индекса блока в чанке + 8 бит типа блока.
typedef uint32_t DeltaEntry;
#define DELTA_INDEX_BITS (CHUNK_SIZE_LOG2 * 3)
#define DELTA_INDEX_MASK ((1u << DELTA_INDEX_BITS) - 1u)

typedef struct Chunk
{
    uint32_t deltaCount;
    uint32_t deltaCapacity;
    DeltaEntry* deltas;
    uint64_t revision;
} Chunk;

#define WORLD_INITIAL_CAPACITY 32
#define HEIGHT_GRID_SIZE (CHUNK_SIZE + 2)
#define HEIGHT_GRID_CELLS (HEIGHT_GRID_SIZE * HEIGHT_GRID_SIZE)
#define HEIGHT_CACHE_SLOTS 16

typedef struct HeightGridSlot
{
    bool valid;
    int64_t chunkX;
    int64_t chunkY;
    float minimumHeight;
    float maximumHeight;
    float* heights;
} HeightGridSlot;

struct World
{
    PlatformRwLock tableLock;
    GlobalChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    uint64_t revision;

    int64_t seed;
    InfiniteCoord blockOrigin[3];
    InfiniteCoord chunkOrigin[3];
    CoordinateFrame* editFrame;
    TerrainOrigin terrainOrigin;

    PlatformRwLock heightCacheLock;
    HeightGridSlot heightCache[HEIGHT_CACHE_SLOTS];
};

static int64_t ChunkFromBlock(int64_t block)
{
    int64_t quotient = block / CHUNK_SIZE;
    if (block % CHUNK_SIZE < 0)
    {
        --quotient;
    }
    return quotient;
}

static uint16_t LocalFromBlock(int64_t block, int64_t chunkCoordinate)
{
    return (uint16_t)(block - chunkCoordinate * CHUNK_SIZE);
}

static uint32_t PackLocalIndex(uint16_t x, uint16_t y, uint16_t z)
{
    return (uint32_t)x * CHUNK_SIZE * CHUNK_SIZE + (uint32_t)y * CHUNK_SIZE + (uint32_t)z;
}

static DeltaEntry PackDelta(uint32_t localIndex, BlockType block)
{
    return (localIndex & DELTA_INDEX_MASK)
        | ((uint32_t)block << DELTA_INDEX_BITS);
}

static uint32_t DeltaLocalIndex(DeltaEntry entry)
{
    return entry & DELTA_INDEX_MASK;
}

static BlockType DeltaBlock(DeltaEntry entry)
{
    return (BlockType)(entry >> DELTA_INDEX_BITS);
}

static bool MutableBlockTypeValid(BlockType block)
{
    return block <= BLOCK_GRASS;
}

static uint64_t HashRotateLeft64(uint64_t value, uint32_t amount)
{
    return (value << amount) | (value >> (64u - amount));
}

static uint64_t HashLocalChunkCoordinate(const World* world, LocalChunkCoordinate coordinate)
{
    uint64_t x = InfiniteCoordHashOffset(&world->chunkOrigin[0], coordinate.x);
    uint64_t y = InfiniteCoordHashOffset(&world->chunkOrigin[1], coordinate.y);
    uint64_t z = InfiniteCoordHashOffset(&world->chunkOrigin[2], coordinate.z);
    return x ^ HashRotateLeft64(y, 21) ^ HashRotateLeft64(z, 42);
}

static void CoordinateFrameRelease(CoordinateFrame* frame)
{
    if (frame == NULL) return;
    if (--frame->referenceCount != 0) return;

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&frame->chunkOrigin[axis]);
    }
    PlatformFree(frame);
}

static CoordinateFrame* WorldGetEditFrame(World* world)
{
    if (world->editFrame != NULL) return world->editFrame;

    CoordinateFrame* frame = PlatformAllocate(sizeof(*frame), true);
    if (frame == NULL) return NULL;

    frame->referenceCount = 1; // ссылка кеша World
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&frame->chunkOrigin[axis]);
        if (!InfiniteCoordTryCopyAddInt64(
                &frame->chunkOrigin[axis], &world->chunkOrigin[axis], 0))
        {
            CoordinateFrameRelease(frame);
            return NULL;
        }
    }
    world->editFrame = frame;
    return frame;
}

static bool GlobalChunkCoordinateMatchesLocal(const GlobalChunkCoordinate* global,
    const World* world, LocalChunkCoordinate local)
{
    if (global->frame == world->editFrame)
    {
        return global->local.x == local.x
            && global->local.y == local.y
            && global->local.z == local.z;
    }

    return InfiniteCoordEqualsOffsets(
            &global->frame->chunkOrigin[0], global->local.x,
            &world->chunkOrigin[0], local.x)
        && InfiniteCoordEqualsOffsets(
            &global->frame->chunkOrigin[1], global->local.y,
            &world->chunkOrigin[1], local.y)
        && InfiniteCoordEqualsOffsets(
            &global->frame->chunkOrigin[2], global->local.z,
            &world->chunkOrigin[2], local.z);
}

static void GlobalChunkCoordinateDestroy(GlobalChunkCoordinate* coordinate)
{
    CoordinateFrameRelease(coordinate->frame);
    coordinate->frame = NULL;
    coordinate->hash = 0;
}

static bool GlobalChunkCoordinateTryCreate(GlobalChunkCoordinate* out,
    World* world, LocalChunkCoordinate local)
{
    CoordinateFrame* frame = WorldGetEditFrame(world);
    if (frame == NULL || frame->referenceCount == UINT32_MAX)
    {
        return false;
    }

    frame->referenceCount++;
    out->hash = HashLocalChunkCoordinate(world, local);
    out->frame = frame;
    out->local = local;
    return true;
}

static float TerrainHeight(const World* world, int64_t localX, int64_t localY)
{
    float noise = GenerateTerrainNoise(world->seed, &world->terrainOrigin, localX, localY);
    return (noise - 0.5f) * 32.0f;
}

static int32_t ColumnCeiling(float height)
{
    int32_t ceiling = (int32_t)height;
    if ((float)ceiling < height)
    {
        ceiling++;
    }
    return ceiling;
}

static bool IsAbsoluteZBelow(const World* world, int64_t localZ, int64_t boundary)
{
    return InfiniteCoordCompareAddInt64ToInt64(&world->blockOrigin[2], localZ, boundary) < 0;
}

static BlockType GeneratedBlock(const World* world, int64_t x, int64_t y, int64_t z)
{
    int32_t boundary = ColumnCeiling(TerrainHeight(world, x, y));
    if (!IsAbsoluteZBelow(world, z, boundary))
    {
        return BLOCK_AIR;
    }
    return IsAbsoluteZBelow(world, z, boundary - 1)
        ? BLOCK_EARTH
        : BLOCK_GRASS;
}

static int32_t SolidCountInColumn(const World* world,
    int64_t minBlockZ, int32_t sizeZ, int32_t boundary)
{
    if (!IsAbsoluteZBelow(world, minBlockZ, boundary))
    {
        return 0;
    }
    if (IsAbsoluteZBelow(world, minBlockZ + sizeZ - 1, boundary))
    {
        return sizeZ;
    }

    int32_t low = 0;
    int32_t high = sizeZ;
    while (low < high)
    {
        int32_t middle = low + (high - low) / 2;
        if (IsAbsoluteZBelow(world, minBlockZ + middle, boundary))
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

static void WorldObtainHeightGrid(World* world, int64_t chunkX, int64_t chunkY,
    float* outHeights, float* outMinimum, float* outMaximum)
{
    uint32_t slotIndex = WorldHashChunkCoordinate(chunkX, chunkY, 0) & (HEIGHT_CACHE_SLOTS - 1);
    HeightGridSlot* slot = &world->heightCache[slotIndex];

    PlatformRwLockAcquireShared(&world->heightCacheLock);
    if (slot->valid && slot->chunkX == chunkX && slot->chunkY == chunkY)
    {
        memcpy(outHeights, slot->heights, HEIGHT_GRID_CELLS * sizeof(float));
        *outMinimum = slot->minimumHeight;
        *outMaximum = slot->maximumHeight;
        PlatformRwLockReleaseShared(&world->heightCacheLock);
        return;
    }
    PlatformRwLockReleaseShared(&world->heightCacheLock);

    int64_t minBlockX = chunkX * CHUNK_SIZE - 1;
    int64_t minBlockY = chunkY * CHUNK_SIZE - 1;
    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    bool first = true;

    for (int32_t y = 0; y < HEIGHT_GRID_SIZE; ++y)
    {
        for (int32_t x = 0; x < HEIGHT_GRID_SIZE; ++x)
        {
            float height = TerrainHeight(world, minBlockX + x, minBlockY + y);
            outHeights[y * HEIGHT_GRID_SIZE + x] = height;
            if (first || height < minimumHeight) minimumHeight = height;
            if (first || height > maximumHeight) maximumHeight = height;
            first = false;
        }
    }

    *outMinimum = minimumHeight;
    *outMaximum = maximumHeight;

    PlatformRwLockAcquireExclusive(&world->heightCacheLock);
    memcpy(slot->heights, outHeights, HEIGHT_GRID_CELLS * sizeof(float));
    slot->chunkX = chunkX;
    slot->chunkY = chunkY;
    slot->minimumHeight = minimumHeight;
    slot->maximumHeight = maximumHeight;
    slot->valid = true;
    PlatformRwLockReleaseExclusive(&world->heightCacheLock);
}

static void ChunkDestroy(Chunk* chunk)
{
    if (chunk->deltas != NULL)
    {
        PlatformFree(chunk->deltas);
    }
    PlatformFree(chunk);
}

static bool WorldGrow(World* world)
{
    uint32_t oldCapacity = world->capacity;
    uint32_t newCapacity = oldCapacity * 2;

    GlobalChunkCoordinate* newKeys = PlatformAllocate(
        (size_t)newCapacity * sizeof(GlobalChunkCoordinate), true);
    Chunk** newChunks = PlatformAllocate(
        (size_t)newCapacity * sizeof(Chunk*), true);
    bool* newOccupied = PlatformAllocate(
        (size_t)newCapacity * sizeof(bool), true);

    if (newKeys == NULL || newChunks == NULL || newOccupied == NULL)
    {
        PlatformFree(newKeys);
        PlatformFree(newChunks);
        PlatformFree(newOccupied);
        return false;
    }

    for (uint32_t i = 0; i < oldCapacity; ++i)
    {
        if (!world->occupied[i]) continue;

        uint32_t mask = newCapacity - 1;
        uint32_t index = (uint32_t)(world->keys[i].hash ^ (world->keys[i].hash >> 32)) & mask;
        while (newOccupied[index]) index = (index + 1) & mask;

        newKeys[index] = world->keys[i];
        newChunks[index] = world->chunks[i];
        newOccupied[index] = true;
    }

    PlatformFree(world->keys);
    PlatformFree(world->chunks);
    PlatformFree(world->occupied);

    world->keys = newKeys;
    world->chunks = newChunks;
    world->occupied = newOccupied;
    world->capacity = newCapacity;
    return true;
}

World* WorldCreate(int64_t seed)
{
    World* world = PlatformAllocate(sizeof(*world), true);
    if (world == NULL) return NULL;

    if (!PlatformRwLockInitialize(&world->tableLock))
    {
        PlatformFree(world);
        return NULL;
    }
    if (!PlatformRwLockInitialize(&world->heightCacheLock))
    {
        PlatformRwLockDestroy(&world->tableLock);
        PlatformFree(world);
        return NULL;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&world->blockOrigin[axis]);
        InfiniteCoordInit(&world->chunkOrigin[axis]);
    }

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = PlatformAllocate(
        (size_t)world->capacity * sizeof(GlobalChunkCoordinate), true);
    world->chunks = PlatformAllocate(
        (size_t)world->capacity * sizeof(Chunk*), true);
    world->occupied = PlatformAllocate(
        (size_t)world->capacity * sizeof(bool), true);

    bool heightCacheAllocated = true;
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].heights = PlatformAllocate(
            HEIGHT_GRID_CELLS * sizeof(float), false);
        if (world->heightCache[slot].heights == NULL) heightCacheAllocated = false;
    }

    if (world->keys == NULL || world->chunks == NULL || world->occupied == NULL || !heightCacheAllocated)
    {
        WorldDestroy(world);
        return NULL;
    }

    world->seed = seed;
    TerrainOriginInit(&world->terrainOrigin, &world->blockOrigin[0], &world->blockOrigin[1]);
    return world;
}

void WorldDestroy(World* world)
{
    if (world == NULL) return;

    if (world->occupied != NULL)
    {
        for (uint32_t i = 0; i < world->capacity; ++i)
        {
            if (!world->occupied[i]) continue;
            GlobalChunkCoordinateDestroy(&world->keys[i]);
            if (world->chunks != NULL && world->chunks[i] != NULL)
            {
                ChunkDestroy(world->chunks[i]);
            }
        }
    }

    if (world->editFrame != NULL)
    {
        CoordinateFrameRelease(world->editFrame);
        world->editFrame = NULL;
    }
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        if (world->heightCache[slot].heights != NULL)
        {
            PlatformFree(world->heightCache[slot].heights);
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&world->blockOrigin[axis]);
        InfiniteCoordDestroy(&world->chunkOrigin[axis]);
    }

    PlatformFree(world->keys);
    PlatformFree(world->chunks);
    PlatformFree(world->occupied);
    PlatformRwLockDestroy(&world->heightCacheLock);
    PlatformRwLockDestroy(&world->tableLock);
    PlatformFree(world);
}

bool WorldRebase(World* world, int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ)
{
    int64_t blockShift[3] = { blockShiftX, blockShiftY, blockShiftZ };
    InfiniteCoord newBlockOrigin[3];
    InfiniteCoord newChunkOrigin[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&newBlockOrigin[axis]);
        InfiniteCoordInit(&newChunkOrigin[axis]);
        if (blockShift[axis] % CHUNK_SIZE != 0
            || !InfiniteCoordTryCopyAddInt64(&newBlockOrigin[axis], &world->blockOrigin[axis], blockShift[axis])
            || !InfiniteCoordTryCopyAddInt64(&newChunkOrigin[axis], &world->chunkOrigin[axis], blockShift[axis] / CHUNK_SIZE))
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&newBlockOrigin[cleanup]);
                InfiniteCoordDestroy(&newChunkOrigin[cleanup]);
            }
            return false;
        }
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordSwap(&world->blockOrigin[axis], &newBlockOrigin[axis]);
        InfiniteCoordSwap(&world->chunkOrigin[axis], &newChunkOrigin[axis]);
        InfiniteCoordDestroy(&newBlockOrigin[axis]);
        InfiniteCoordDestroy(&newChunkOrigin[axis]);
    }

    if (world->editFrame != NULL)
    {
        CoordinateFrameRelease(world->editFrame);
        world->editFrame = NULL;
    }
    TerrainOriginInit(&world->terrainOrigin, &world->blockOrigin[0], &world->blockOrigin[1]);

    PlatformRwLockAcquireExclusive(&world->heightCacheLock);
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].valid = false;
    }
    PlatformRwLockReleaseExclusive(&world->heightCacheLock);
    return true;
}

bool WorldSquareAbsoluteX(
    World* world, int64_t localBlockX, int64_t* outLocalBlockX,
    bool* outChunkOriginDeltaFits, int64_t* outChunkOriginDeltaX)
{
    if (outLocalBlockX == NULL || outChunkOriginDeltaFits == NULL
        || outChunkOriginDeltaX == NULL)
    {
        return false;
    }

    InfiniteCoord squared;
    InfiniteCoord newBlockOrigin;
    InfiniteCoord newChunkOrigin;
    InfiniteCoordInit(&squared);
    InfiniteCoordInit(&newBlockOrigin);
    InfiniteCoordInit(&newChunkOrigin);

    if (!InfiniteCoordTryCopySquareAddInt64(
            &squared, &world->blockOrigin[0], localBlockX))
    {
        return false;
    }

    uint64_t localBlock = squared.limbCount == 0
        ? 0
        : squared.limbs[0] & (CHUNK_SIZE - 1u);

    if (!InfiniteCoordTryCopyAddInt64(
            &newBlockOrigin, &squared, -(int64_t)localBlock)
        || !InfiniteCoordTryCopyShiftRight(
            &newChunkOrigin, &squared, CHUNK_SIZE_LOG2))
    {
        InfiniteCoordDestroy(&squared);
        InfiniteCoordDestroy(&newBlockOrigin);
        InfiniteCoordDestroy(&newChunkOrigin);
        return false;
    }

    int64_t chunkOriginDeltaX;
    bool chunkOriginDeltaFits = InfiniteCoordTrySubtractToInt64(
        &newChunkOrigin, &world->chunkOrigin[0], &chunkOriginDeltaX);
    InfiniteCoordSwap(&world->blockOrigin[0], &newBlockOrigin);
    InfiniteCoordSwap(&world->chunkOrigin[0], &newChunkOrigin);
    InfiniteCoordDestroy(&squared);
    InfiniteCoordDestroy(&newBlockOrigin);
    InfiniteCoordDestroy(&newChunkOrigin);

    *outLocalBlockX = (int64_t)localBlock;
    *outChunkOriginDeltaFits = chunkOriginDeltaFits;
    *outChunkOriginDeltaX = chunkOriginDeltaFits ? chunkOriginDeltaX : 0;
    if (world->editFrame != NULL)
    {
        CoordinateFrameRelease(world->editFrame);
        world->editFrame = NULL;
    }
    TerrainOriginInit(&world->terrainOrigin, &world->blockOrigin[0], &world->blockOrigin[1]);

    PlatformRwLockAcquireExclusive(&world->heightCacheLock);
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].valid = false;
    }
    PlatformRwLockReleaseExclusive(&world->heightCacheLock);
    return true;
}

static Chunk** WorldFindEntry(World* world, LocalChunkCoordinate key)
{
    if (world->count == 0) return NULL;

    uint64_t hash = HashLocalChunkCoordinate(world, key);
    uint32_t mask = world->capacity - 1;
    uint32_t index = (uint32_t)(hash ^ (hash >> 32)) & mask;

    for (uint32_t probe = 0; probe < world->capacity; ++probe)
    {
        if (!world->occupied[index]) return NULL;
        if (world->keys[index].hash == hash
            && GlobalChunkCoordinateMatchesLocal(&world->keys[index], world, key))
        {
            return &world->chunks[index];
        }
        index = (index + 1) & mask;
    }
    return NULL;
}

static Chunk* WorldGetOrCreateChunk(World* world, LocalChunkCoordinate key)
{
    Chunk** entry = WorldFindEntry(world, key);
    if (entry != NULL) return *entry;

    if (world->count * 2 >= world->capacity && !WorldGrow(world))
    {
        return NULL;
    }

    GlobalChunkCoordinate globalKey;
    if (!GlobalChunkCoordinateTryCreate(&globalKey, world, key))
    {
        return NULL;
    }

    Chunk* chunk = PlatformAllocate(sizeof(Chunk), true);
    if (chunk == NULL)
    {
        GlobalChunkCoordinateDestroy(&globalKey);
        return NULL;
    }

    uint32_t mask = world->capacity - 1;
    uint32_t index = (uint32_t)(globalKey.hash ^ (globalKey.hash >> 32)) & mask;
    while (world->occupied[index]) index = (index + 1) & mask;

    world->keys[index] = globalKey;
    world->chunks[index] = chunk;
    world->occupied[index] = true;
    world->count++;
    return chunk;
}

static uint32_t ChunkDeltaLowerBound(const Chunk* chunk, uint32_t localIndex)
{
    uint32_t low = 0;
    uint32_t high = chunk->deltaCount;
    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2u;
        if (DeltaLocalIndex(chunk->deltas[middle]) < localIndex)
        {
            low = middle + 1u;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

static bool ChunkGetDelta(const Chunk* chunk, uint32_t localIndex, BlockType* outBlock)
{
    uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
    if (position < chunk->deltaCount
        && DeltaLocalIndex(chunk->deltas[position]) == localIndex)
    {
        *outBlock = DeltaBlock(chunk->deltas[position]);
        return true;
    }
    return false;
}

static bool ChunkSetDelta(Chunk* chunk, uint32_t localIndex, BlockType block)
{
    uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
    if (position < chunk->deltaCount
        && DeltaLocalIndex(chunk->deltas[position]) == localIndex)
    {
        chunk->deltas[position] = PackDelta(localIndex, block);
        return true;
    }

    if (chunk->deltaCount >= chunk->deltaCapacity)
    {
        uint32_t newCapacity = chunk->deltaCapacity < 4 ? 4 : chunk->deltaCapacity * 2;
        DeltaEntry* newDeltas = chunk->deltas == NULL
            ? PlatformAllocate(
                (size_t)newCapacity * sizeof(DeltaEntry), false)
            : PlatformReallocate(chunk->deltas,
                (size_t)newCapacity * sizeof(DeltaEntry), false);
        if (newDeltas == NULL) return false;
        chunk->deltas = newDeltas;
        chunk->deltaCapacity = newCapacity;
    }

    for (uint32_t index = chunk->deltaCount; index > position; --index)
    {
        chunk->deltas[index] = chunk->deltas[index - 1u];
    }
    chunk->deltas[position] = PackDelta(localIndex, block);
    chunk->deltaCount++;
    return true;
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    WorldBlockState state;
    return WorldGetBlockState(world, x, y, z, &state)
        ? state.block : BLOCK_AIR;
}

bool WorldGetBlockState(World* world, int64_t x, int64_t y, int64_t z,
    WorldBlockState* outState)
{
    if (world == NULL || outState == NULL) return false;
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };
    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z));
    BlockType block = GeneratedBlock(world, x, y, z);
    bool edited = false;

    PlatformRwLockAcquireShared(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    if (entry != NULL)
    {
        if (ChunkGetDelta(*entry, localIndex, &block))
        {
            edited = true;
        }
    }
    PlatformRwLockReleaseShared(&world->tableLock);
    outState->block = block;
    outState->edited = edited;
    return true;
}

static bool WorldBlockRangeValid(const WorldBlockRange* range)
{
    uint64_t cellCount = 1U;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (range->minimum[axis] > range->maximum[axis]) return false;
        uint64_t span = (uint64_t)range->maximum[axis]
            - (uint64_t)range->minimum[axis] + 1U;
        if (span == 0U || span > WORLD_MAX_BOUNDED_BLOCK_QUERY_CELLS
            || cellCount > WORLD_MAX_BOUNDED_BLOCK_QUERY_CELLS / span)
        {
            return false;
        }
        cellCount *= span;
    }
    return true;
}

bool WorldAnySolidBlockInRanges(World* world, const WorldBlockRange* ranges,
    uint32_t count, bool* outSolid)
{
    if (outSolid != NULL) *outSolid = false;
    if (world == NULL || outSolid == NULL ||
        count > WORLD_MAX_BOUNDED_BLOCK_RANGES ||
        (count != 0U && ranges == NULL))
    {
        return false;
    }
    for (uint32_t rangeIndex = 0U; rangeIndex < count; ++rangeIndex)
    {
        if (!WorldBlockRangeValid(&ranges[rangeIndex])) return false;
    }
    if (count == 0U) return true;

    bool solid = false;
    PlatformRwLockAcquireShared(&world->tableLock);
    for (uint32_t rangeIndex = 0U;
         rangeIndex < count && !solid; ++rangeIndex)
    {
        const WorldBlockRange* range = &ranges[rangeIndex];
        int64_t block[3] = {
            range->minimum[0], range->minimum[1], range->minimum[2]
        };
        for (;;)
        {
            LocalChunkCoordinate coordinate = {
                ChunkFromBlock(block[0]), ChunkFromBlock(block[1]),
                ChunkFromBlock(block[2])
            };
            uint32_t localIndex = PackLocalIndex(
                LocalFromBlock(block[0], coordinate.x),
                LocalFromBlock(block[1], coordinate.y),
                LocalFromBlock(block[2], coordinate.z));
            BlockType material = GeneratedBlock(
                world, block[0], block[1], block[2]);
            Chunk** entry = WorldFindEntry(world, coordinate);
            if (entry != NULL)
                (void)ChunkGetDelta(*entry, localIndex, &material);
            if (BlockGetProperties(material).solid)
            {
                solid = true;
                break;
            }

            uint32_t axis = 0U;
            for (; axis < 3U; ++axis)
            {
                if (block[axis] != range->maximum[axis])
                {
                    ++block[axis];
                    break;
                }
                block[axis] = range->minimum[axis];
            }
            if (axis == 3U) break;
        }
    }
    PlatformRwLockReleaseShared(&world->tableLock);
    *outSolid = solid;
    return true;
}

bool WorldAnySolidBlockInRange(World* world, const int64_t minimum[3],
    const int64_t maximum[3])
{
    if (minimum == NULL || maximum == NULL) return false;
    WorldBlockRange range = {
        .minimum = { minimum[0], minimum[1], minimum[2] },
        .maximum = { maximum[0], maximum[1], maximum[2] },
    };
    bool solid = false;
    return WorldAnySolidBlockInRanges(world, &range, 1U, &solid) && solid;
}

bool WorldIsBlockEdited(World* world, int64_t x, int64_t y, int64_t z)
{
    WorldBlockState state;
    return WorldGetBlockState(world, x, y, z, &state) && state.edited;
}

bool WorldTrySetBlock(World* world, int64_t x, int64_t y, int64_t z,
    BlockType block)
{
    if (world == NULL || !MutableBlockTypeValid(block)) return false;
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };
    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z));
    BlockType generated = GeneratedBlock(world, x, y, z);

    PlatformRwLockAcquireExclusive(&world->tableLock);
    Chunk** existingEntry = WorldFindEntry(world, coordinate);
    BlockType current = generated;
    if (existingEntry != NULL)
        ChunkGetDelta(*existingEntry, localIndex, &current);
    if (current == block)
    {
        PlatformRwLockReleaseExclusive(&world->tableLock);
        return true;
    }
    bool changed = false;
    Chunk* changedChunk = existingEntry == NULL ? NULL : *existingEntry;
    if (block == generated)
    {
        Chunk** entry = existingEntry;
        if (entry != NULL)
        {
            Chunk* chunk = *entry;
            uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
            if (position < chunk->deltaCount
                && DeltaLocalIndex(chunk->deltas[position]) == localIndex)
            {
                for (uint32_t index = position + 1u;
                     index < chunk->deltaCount; ++index)
                {
                    chunk->deltas[index - 1u] = chunk->deltas[index];
                }
                chunk->deltaCount--;
                changed = true;
                changedChunk = chunk;
            }
        }
    }
    else
    {
        Chunk* chunk = WorldGetOrCreateChunk(world, coordinate);
        if (chunk != NULL && ChunkSetDelta(chunk, localIndex, block))
        {
            changed = true;
            changedChunk = chunk;
        }
    }
    if (changed)
    {
        if (changedChunk != NULL && changedChunk->revision != UINT64_MAX)
            ++changedChunk->revision;
        if (world->revision != UINT64_MAX) ++world->revision;
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);
    return changed;
}

void WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z,
    BlockType block)
{
    (void)WorldTrySetBlock(world, x, y, z, block);
}

typedef struct WorldBatchChunk
{
    LocalChunkCoordinate coordinate;
    Chunk* existing;
    DeltaEntry* stagedDeltas;
    uint32_t stagedCount;
    uint32_t stagedCapacity;
    uint32_t changedCount;
    GlobalChunkCoordinate newKey;
    Chunk* newChunk;
    bool newKeyReady;
} WorldBatchChunk;

static bool LocalChunkCoordinateEqual(
    LocalChunkCoordinate left, LocalChunkCoordinate right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

static void WorldBatchCleanup(WorldBatchChunk* chunks, uint32_t count)
{
    if (chunks == NULL) return;
    for (uint32_t index = 0; index < count; ++index)
    {
        WorldBatchChunk* chunk = &chunks[index];
        if (chunk->newChunk != NULL)
        {
            PlatformFree(chunk->newChunk);
        }
        if (chunk->newKeyReady)
        {
            GlobalChunkCoordinateDestroy(&chunk->newKey);
        }
        PlatformFree(chunk->stagedDeltas);
    }
    PlatformFree(chunks);
}

static bool WorldBatchApplyToChunk(
    World* world, WorldBatchChunk* batch,
    const WorldBlockMutation* mutation)
{
    uint16_t localX = LocalFromBlock(
        mutation->block[0], batch->coordinate.x);
    uint16_t localY = LocalFromBlock(
        mutation->block[1], batch->coordinate.y);
    uint16_t localZ = LocalFromBlock(
        mutation->block[2], batch->coordinate.z);
    uint32_t localIndex = PackLocalIndex(localX, localY, localZ);
    BlockType generated = GeneratedBlock(world,
        mutation->block[0], mutation->block[1], mutation->block[2]);
    Chunk staged = {
        .deltaCount = batch->stagedCount,
        .deltaCapacity = batch->stagedCapacity,
        .deltas = batch->stagedDeltas,
        .revision = 0,
    };
    uint32_t position = ChunkDeltaLowerBound(&staged, localIndex);
    bool hasDelta = position < staged.deltaCount &&
        DeltaLocalIndex(staged.deltas[position]) == localIndex;

    if (mutation->replacement == generated)
    {
        if (hasDelta)
        {
            for (uint32_t index = position + 1U;
                 index < staged.deltaCount; ++index)
            {
                staged.deltas[index - 1U] = staged.deltas[index];
            }
            --staged.deltaCount;
        }
    }
    else if (hasDelta)
    {
        staged.deltas[position] = PackDelta(
            localIndex, mutation->replacement);
    }
    else if (!ChunkSetDelta(
                 &staged, localIndex, mutation->replacement))
    {
        return false;
    }
    batch->stagedDeltas = staged.deltas;
    batch->stagedCount = staged.deltaCount;
    batch->stagedCapacity = staged.deltaCapacity;
    ++batch->changedCount;
    return true;
}

static uint64_t SaturatingAddRevision(uint64_t value, uint32_t amount)
{
    uint64_t remaining = UINT64_MAX - value;
    return remaining < (uint64_t)amount
        ? UINT64_MAX : value + (uint64_t)amount;
}

bool WorldApplyBlockBatch(World* world,
    const WorldBlockMutation* mutations, uint32_t count)
{
    if (world == NULL || (count != 0U && mutations == NULL) ||
        count > WORLD_MAX_ATOMIC_BLOCK_MUTATIONS)
    {
        return false;
    }
    if (count == 0U) return true;

    for (uint32_t index = 0; index < count; ++index)
    {
        if (!MutableBlockTypeValid(mutations[index].expected) ||
            !MutableBlockTypeValid(mutations[index].replacement))
        {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous)
        {
            if (mutations[index].block[0] == mutations[previous].block[0] &&
                mutations[index].block[1] == mutations[previous].block[1] &&
                mutations[index].block[2] == mutations[previous].block[2])
            {
                return false;
            }
        }
    }

    WorldBatchChunk* chunks = PlatformAllocate(
        (size_t)count * sizeof(*chunks), true);
    if (chunks == NULL) return false;
    uint32_t chunkCount = 0;
    uint32_t totalChanged = 0;
    bool succeeded = true;

    PlatformRwLockAcquireExclusive(&world->tableLock);
    for (uint32_t index = 0; index < count && succeeded; ++index)
    {
        const WorldBlockMutation* mutation = &mutations[index];
        LocalChunkCoordinate coordinate = {
            ChunkFromBlock(mutation->block[0]),
            ChunkFromBlock(mutation->block[1]),
            ChunkFromBlock(mutation->block[2]),
        };
        WorldBatchChunk* batch = NULL;
        for (uint32_t chunkIndex = 0;
             chunkIndex < chunkCount; ++chunkIndex)
        {
            if (LocalChunkCoordinateEqual(
                    chunks[chunkIndex].coordinate, coordinate))
            {
                batch = &chunks[chunkIndex];
                break;
            }
        }
        if (batch == NULL)
        {
            batch = &chunks[chunkCount++];
            batch->coordinate = coordinate;
            Chunk** entry = WorldFindEntry(world, coordinate);
            batch->existing = entry == NULL ? NULL : *entry;
            uint32_t existingCount = batch->existing == NULL
                ? 0U : batch->existing->deltaCount;
            batch->stagedCapacity = existingCount + count;
            if (batch->stagedCapacity != 0U)
            {
                batch->stagedDeltas = PlatformAllocate(
                    (size_t)batch->stagedCapacity * sizeof(DeltaEntry),
                    false);
                if (batch->stagedDeltas == NULL)
                {
                    succeeded = false;
                    break;
                }
            }
            batch->stagedCount = existingCount;
            if (existingCount != 0U)
            {
                memcpy(batch->stagedDeltas, batch->existing->deltas,
                    (size_t)existingCount * sizeof(DeltaEntry));
            }
        }

        uint32_t localIndex = PackLocalIndex(
            LocalFromBlock(mutation->block[0], coordinate.x),
            LocalFromBlock(mutation->block[1], coordinate.y),
            LocalFromBlock(mutation->block[2], coordinate.z));
        Chunk stagedView = {
            .deltaCount = batch->stagedCount,
            .deltaCapacity = batch->stagedCapacity,
            .deltas = batch->stagedDeltas,
        };
        BlockType current = GeneratedBlock(world,
            mutation->block[0], mutation->block[1], mutation->block[2]);
        (void)ChunkGetDelta(&stagedView, localIndex, &current);
        if (current != mutation->expected)
        {
            succeeded = false;
            break;
        }
        if (current == mutation->replacement) continue;
        succeeded = WorldBatchApplyToChunk(world, batch, mutation);
        if (succeeded) ++totalChanged;
    }

    uint32_t newChunkCount = 0;
    for (uint32_t index = 0; index < chunkCount; ++index)
    {
        if (chunks[index].existing == NULL &&
            chunks[index].stagedCount != 0U)
        {
            ++newChunkCount;
        }
    }
    while (succeeded &&
        (world->count + newChunkCount) * 2U >= world->capacity)
    {
        succeeded = WorldGrow(world);
    }

    for (uint32_t index = 0; index < chunkCount && succeeded; ++index)
    {
        WorldBatchChunk* batch = &chunks[index];
        if (batch->existing != NULL || batch->stagedCount == 0U)
            continue;
        batch->newChunk = PlatformAllocate(sizeof(Chunk), true);
        succeeded = batch->newChunk != NULL &&
            GlobalChunkCoordinateTryCreate(
                &batch->newKey, world, batch->coordinate);
        batch->newKeyReady = succeeded;
    }

    if (succeeded)
    {
        for (uint32_t index = 0; index < chunkCount; ++index)
        {
            WorldBatchChunk* batch = &chunks[index];
            if (batch->changedCount == 0U) continue;
            if (batch->existing != NULL)
            {
                PlatformFree(batch->existing->deltas);
                batch->existing->deltas = batch->stagedDeltas;
                batch->existing->deltaCount = batch->stagedCount;
                batch->existing->deltaCapacity = batch->stagedCapacity;
                batch->existing->revision = SaturatingAddRevision(
                    batch->existing->revision, batch->changedCount);
                batch->stagedDeltas = NULL;
                continue;
            }
            if (batch->stagedCount == 0U) continue;
            batch->newChunk->deltas = batch->stagedDeltas;
            batch->newChunk->deltaCount = batch->stagedCount;
            batch->newChunk->deltaCapacity = batch->stagedCapacity;
            batch->newChunk->revision = batch->changedCount;
            uint32_t mask = world->capacity - 1U;
            uint32_t slot = (uint32_t)(batch->newKey.hash ^
                (batch->newKey.hash >> 32U)) & mask;
            while (world->occupied[slot]) slot = (slot + 1U) & mask;
            world->keys[slot] = batch->newKey;
            world->chunks[slot] = batch->newChunk;
            world->occupied[slot] = true;
            ++world->count;
            batch->stagedDeltas = NULL;
            batch->newChunk = NULL;
            batch->newKeyReady = false;
        }
        world->revision = SaturatingAddRevision(
            world->revision, totalChanged);
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);
    WorldBatchCleanup(chunks, chunkCount);
    return succeeded;
}

uint64_t WorldGetRevision(World* world)
{
    if (world == NULL) return 0;
    PlatformRwLockAcquireShared(&world->tableLock);
    uint64_t revision = world->revision;
    PlatformRwLockReleaseShared(&world->tableLock);
    return revision;
}

uint64_t WorldGetChunkRevision(World* world, const int64_t chunk[3])
{
    if (world == NULL || chunk == NULL) return 0;
    LocalChunkCoordinate coordinate = { chunk[0], chunk[1], chunk[2] };
    PlatformRwLockAcquireShared(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    uint64_t revision = entry == NULL ? 0 : (*entry)->revision;
    PlatformRwLockReleaseShared(&world->tableLock);
    return revision;
}

static bool GlobalChunkCoordinateToLocal(const GlobalChunkCoordinate* global,
    const World* world, int64_t output[3])
{
    const int64_t offsets[3] = {
        global->local.x, global->local.y, global->local.z,
    };
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoord absolute;
        InfiniteCoordInit(&absolute);
        bool succeeded = InfiniteCoordTryCopyAddInt64(&absolute,
            &global->frame->chunkOrigin[axis], offsets[axis])
            && InfiniteCoordTrySubtractToInt64(
                &absolute, &world->chunkOrigin[axis], &output[axis]);
        InfiniteCoordDestroy(&absolute);
        if (!succeeded) return false;
    }
    return true;
}

static bool ChunkSummaryLess(
    const WorldChunkSummary* left, const WorldChunkSummary* right)
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (left->chunk[axis] != right->chunk[axis])
            return left->chunk[axis] < right->chunk[axis];
    }
    return false;
}

uint32_t WorldCopyEditedChunkSummaries(
    World* world, const int64_t minimumChunk[3], const int64_t maximumChunk[3],
    WorldChunkSummary* output, uint32_t capacity, bool* outTruncated,
    uint64_t* outWorldRevision)
{
    if (outTruncated != NULL) *outTruncated = false;
    if (outWorldRevision != NULL) *outWorldRevision = 0;
    if (world == NULL || minimumChunk == NULL || maximumChunk == NULL
        || (capacity > 0 && output == NULL))
        return 0;
    for (uint32_t axis = 0; axis < 3U; ++axis)
        if (minimumChunk[axis] > maximumChunk[axis]) return 0;

    uint32_t count = 0;
    bool truncated = false;
    PlatformRwLockAcquireShared(&world->tableLock);
    if (outWorldRevision != NULL) *outWorldRevision = world->revision;
    for (uint32_t slot = 0; slot < world->capacity; ++slot)
    {
        if (!world->occupied[slot] || world->chunks[slot] == NULL
            || (world->chunks[slot]->deltaCount == 0
                && world->chunks[slot]->revision == 0))
            continue;
        int64_t local[3];
        if (!GlobalChunkCoordinateToLocal(&world->keys[slot], world, local))
            continue;
        bool inside = true;
        for (uint32_t axis = 0; axis < 3U; ++axis)
            inside = inside && local[axis] >= minimumChunk[axis]
                && local[axis] <= maximumChunk[axis];
        if (!inside) continue;
        if (count >= capacity)
        {
            truncated = true;
            continue;
        }
        for (uint32_t axis = 0; axis < 3U; ++axis)
            output[count].chunk[axis] = local[axis];
        output[count].revision = world->chunks[slot]->revision;
        output[count].deltaCount = world->chunks[slot]->deltaCount;
        ++count;
    }
    PlatformRwLockReleaseShared(&world->tableLock);

    for (uint32_t i = 1; i < count; ++i)
    {
        WorldChunkSummary value = output[i];
        uint32_t position = i;
        while (position > 0
            && ChunkSummaryLess(&value, &output[position - 1U]))
        {
            output[position] = output[position - 1U];
            --position;
        }
        output[position] = value;
    }
    if (outTruncated != NULL) *outTruncated = truncated;
    return count;
}

bool WorldCopyChunkDeltas(
    World* world, const int64_t chunk[3], WorldChunkDelta* output,
    uint32_t capacity, uint32_t* outCount, uint64_t* outChunkRevision)
{
    if (outCount != NULL) *outCount = 0;
    if (outChunkRevision != NULL) *outChunkRevision = 0;
    if (world == NULL || chunk == NULL || outCount == NULL
        || (capacity > 0 && output == NULL))
        return false;
    LocalChunkCoordinate coordinate = { chunk[0], chunk[1], chunk[2] };
    PlatformRwLockAcquireShared(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    uint32_t count = entry == NULL ? 0 : (*entry)->deltaCount;
    if (count > capacity)
    {
        *outCount = count;
        PlatformRwLockReleaseShared(&world->tableLock);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        output[i].localIndex = DeltaLocalIndex((*entry)->deltas[i]);
        output[i].block = DeltaBlock((*entry)->deltas[i]);
    }
    if (entry != NULL && outChunkRevision != NULL)
        *outChunkRevision = (*entry)->revision;
    *outCount = count;
    PlatformRwLockReleaseShared(&world->tableLock);
    return true;
}

bool WorldReplaceChunkDeltas(
    World* world, const int64_t chunk[3], const WorldChunkDelta* deltas,
    uint32_t count, uint64_t chunkRevision, uint64_t worldRevision)
{
    if (world == NULL || chunk == NULL || (count > 0 && deltas == NULL))
        return false;
    DeltaEntry* packed = count == 0 ? NULL
        : PlatformAllocate((size_t)count * sizeof(DeltaEntry), false);
    if (count > 0 && packed == NULL) return false;
    uint32_t previousIndex = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (deltas[i].localIndex > DELTA_INDEX_MASK
            || (i > 0 && deltas[i].localIndex <= previousIndex))
        {
            PlatformFree(packed);
            return false;
        }
        previousIndex = deltas[i].localIndex;
        packed[i] = PackDelta(deltas[i].localIndex, deltas[i].block);
    }

    LocalChunkCoordinate coordinate = { chunk[0], chunk[1], chunk[2] };
    PlatformRwLockAcquireExclusive(&world->tableLock);
    Chunk* target = WorldGetOrCreateChunk(world, coordinate);
    if (target == NULL)
    {
        PlatformRwLockReleaseExclusive(&world->tableLock);
        PlatformFree(packed);
        return false;
    }
    // Snapshot и control traffic идут по независимым QUIC streams. Поэтому
    // более новая live-delta может быть применена до старого snapshot chunk.
    // Revision чанка монотонна: старый снимок подтверждаем, но не позволяем
    // ему откатить уже видимое состояние.
    if (target->revision > chunkRevision)
    {
        if (world->revision < worldRevision)
            world->revision = worldRevision;
        PlatformRwLockReleaseExclusive(&world->tableLock);
        PlatformFree(packed);
        return true;
    }
    PlatformFree(target->deltas);
    target->deltas = packed;
    target->deltaCount = count;
    target->deltaCapacity = count;
    target->revision = chunkRevision;
    packed = NULL;
    if (world->revision < worldRevision) world->revision = worldRevision;
    PlatformRwLockReleaseExclusive(&world->tableLock);
    PlatformFree(packed);
    return true;
}

WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks,
    float* heightScratch, size_t heightScratchCount)
{
    if (world == NULL || outBlocks == NULL ||
        sizeX <= 0 || sizeY <= 0 || sizeZ <= 0 ||
        minBlockX > INT64_MAX - ((int64_t)sizeX - 1) ||
        minBlockY > INT64_MAX - ((int64_t)sizeY - 1) ||
        minBlockZ > INT64_MAX - ((int64_t)sizeZ - 1))
    {
        return WORLD_REGION_ALL_AIR;
    }
    bool regionHasDeltas = false;
    int64_t minChunkX = ChunkFromBlock(minBlockX);
    int64_t minChunkY = ChunkFromBlock(minBlockY);
    int64_t minChunkZ = ChunkFromBlock(minBlockZ);
    int64_t maxChunkX = ChunkFromBlock(minBlockX + sizeX - 1);
    int64_t maxChunkY = ChunkFromBlock(minBlockY + sizeY - 1);
    int64_t maxChunkZ = ChunkFromBlock(minBlockZ + sizeZ - 1);

    PlatformRwLockAcquireShared(&world->tableLock);
    for (int64_t chunkZ = minChunkZ; chunkZ <= maxChunkZ && !regionHasDeltas; ++chunkZ)
    {
        for (int64_t chunkY = minChunkY; chunkY <= maxChunkY && !regionHasDeltas; ++chunkY)
        {
            for (int64_t chunkX = minChunkX; chunkX <= maxChunkX && !regionHasDeltas; ++chunkX)
            {
                LocalChunkCoordinate coordinate = { chunkX, chunkY, chunkZ };
                Chunk** entry = WorldFindEntry(world, coordinate);
                if (entry != NULL && (*entry)->deltaCount > 0) regionHasDeltas = true;
            }
        }
    }
    PlatformRwLockReleaseShared(&world->tableLock);

    size_t heightCount = (size_t)sizeX * (size_t)sizeY;
    bool heightsOnHeap = heightScratch == NULL
        || heightScratchCount < heightCount;
    float* heights = heightsOnHeap
        ? PlatformAllocate(heightCount * sizeof(float), false)
        : heightScratch;
    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    bool boundsKnown = false;

    if (heights != NULL)
    {
        if (sizeX == HEIGHT_GRID_SIZE && sizeY == HEIGHT_GRID_SIZE
            && (minBlockX + 1) % CHUNK_SIZE == 0
            && (minBlockY + 1) % CHUNK_SIZE == 0)
        {
            WorldObtainHeightGrid(world,
                (minBlockX + 1) / CHUNK_SIZE, (minBlockY + 1) / CHUNK_SIZE,
                heights, &minimumHeight, &maximumHeight);
            boundsKnown = true;
        }
        else
        {
            bool first = true;
            for (int32_t y = 0; y < sizeY; ++y)
            {
                for (int32_t x = 0; x < sizeX; ++x)
                {
                    float height = TerrainHeight(world, minBlockX + x, minBlockY + y);
                    heights[y * sizeX + x] = height;
                    if (first || height < minimumHeight) minimumHeight = height;
                    if (first || height > maximumHeight) maximumHeight = height;
                    first = false;
                }
            }
            boundsKnown = true;
        }
    }

    if (!regionHasDeltas && boundsKnown)
    {
        if (!IsAbsoluteZBelow(world, minBlockZ, ColumnCeiling(maximumHeight)))
        {
            if (heightsOnHeap) PlatformFree(heights);
            return WORLD_REGION_ALL_AIR;
        }
        if (IsAbsoluteZBelow(world, minBlockZ + sizeZ - 1, ColumnCeiling(minimumHeight)))
        {
            if (heightsOnHeap) PlatformFree(heights);
            return WORLD_REGION_ALL_SOLID;
        }
    }

    // И solidCount, и признак травы зависят только от boundary: minBlockZ,
    // sizeZ и мир в пределах вызова неизменны. Рельеф гладкий, у соседних
    // колонн граница обычно та же, поэтому хватает памяти на один прошлый
    // результат. Без неё каждая колонна гоняла бинарный поиск по сравнениям
    // произвольной точности — на регион 66x66 это ~39 тысяч сравнений.
    int32_t cachedBoundary = 0;
    int32_t cachedSolidCount = 0;
    bool cachedGrass = false;
    bool cacheValid = false;

    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            float height = heights != NULL
                ? heights[y * sizeX + x]
                : TerrainHeight(world, minBlockX + x, minBlockY + y);
            int32_t boundary = ColumnCeiling(height);

            if (!cacheValid || boundary != cachedBoundary)
            {
                cachedSolidCount = SolidCountInColumn(
                    world, minBlockZ, sizeZ, boundary);
                // Верхний сгенерированный solid-блок может лежать на
                // последней позиции региона, поэтому проверяем также блок
                // сразу за регионом.
                cachedGrass = cachedSolidCount > 0
                    && !IsAbsoluteZBelow(
                        world, minBlockZ + cachedSolidCount, boundary);
                cachedBoundary = boundary;
                cacheValid = true;
            }

            uint32_t solidCount = cachedSolidCount > 0
                ? (uint32_t)cachedSolidCount : 0U;
            uint32_t columnDepth = (uint32_t)sizeZ;
            if (solidCount > columnDepth)
            {
                solidCount = columnDepth;
            }
            BlockType* column = &outBlocks[(((size_t)y * sizeX) + (size_t)x) * sizeZ];
            memset(column, BLOCK_EARTH, (size_t)solidCount);
            memset(column + solidCount, BLOCK_AIR,
                (size_t)(columnDepth - solidCount));

            if (cachedGrass)
            {
                column[solidCount - 1] = BLOCK_GRASS;
            }
        }
    }

    if (heightsOnHeap && heights != NULL) PlatformFree(heights);

    if (regionHasDeltas)
    {
        PlatformRwLockAcquireShared(&world->tableLock);
        for (int64_t chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ)
        {
            for (int64_t chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
            {
                for (int64_t chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
                {
                    LocalChunkCoordinate coordinate = { chunkX, chunkY, chunkZ };
                    Chunk** entry = WorldFindEntry(world, coordinate);
                    if (entry == NULL) continue;

                    const Chunk* chunk = *entry;
                    int64_t chunkBaseX = chunkX * CHUNK_SIZE;
                    int64_t chunkBaseY = chunkY * CHUNK_SIZE;
                    int64_t chunkBaseZ = chunkZ * CHUNK_SIZE;

                    for (uint32_t i = 0; i < chunk->deltaCount; ++i)
                    {
                        uint32_t localIndex = DeltaLocalIndex(chunk->deltas[i]);
                        int64_t blockX = chunkBaseX + localIndex / (CHUNK_SIZE * CHUNK_SIZE);
                        int64_t blockY = chunkBaseY + (localIndex / CHUNK_SIZE) % CHUNK_SIZE;
                        int64_t blockZ = chunkBaseZ + localIndex % CHUNK_SIZE;
                        int64_t relativeX = blockX - minBlockX;
                        int64_t relativeY = blockY - minBlockY;
                        int64_t relativeZ = blockZ - minBlockZ;
                        if (relativeX < 0 || relativeX >= sizeX
                            || relativeY < 0 || relativeY >= sizeY
                            || relativeZ < 0 || relativeZ >= sizeZ)
                        {
                            continue;
                        }
                        outBlocks[((size_t)relativeY * sizeX + (size_t)relativeX) * sizeZ
                            + (size_t)relativeZ] = DeltaBlock(chunk->deltas[i]);
                    }
                }
            }
        }
        PlatformRwLockReleaseShared(&world->tableLock);
    }

    return WORLD_REGION_MIXED;
}

int64_t WorldGetTerrainHeight(World* world, int64_t x, int64_t y)
{
    int64_t globalTop = (int64_t)ColumnCeiling(TerrainHeight(world, x, y)) - 1;
    return InfiniteCoordSubtractFromInt64Clamped(globalTop, &world->blockOrigin[2]);
}

void WorldFormatAbsoluteBlockCoordinate(World* world,
    int32_t axis, int64_t localBlock, wchar_t* outText, uint32_t capacity)
{
    if (axis < 0 || axis >= 3)
    {
        if (capacity > 0)
        {
            outText[0] = L'\0';
        }
        return;
    }

    InfiniteCoordFormatShortOffsetW(
        &world->blockOrigin[axis], localBlock, outText, capacity);
}

// === Сохранение правок (Laiue World Format v1, docs/world_format.md) ===

#define WORLD_SAVE_MAGIC        0x3143574Cu  // байты 'L' 'W' 'C' '1'
#define WORLD_SAVE_VERSION      1u
#define WORLD_SAVE_MAX_LIMBS    1024u
#define WORLD_SAVE_INITIAL_BYTES 65536u
#define WORLD_SAVE_MAX_BYTES (64u * 1024u * 1024u)

typedef struct SaveWriter
{
    uint8_t* bytes;
    uint32_t size;
    uint32_t capacity;
    bool failed;
} SaveWriter;

static void SaveWriterBytes(SaveWriter* writer,
    const void* bytes, uint32_t count)
{
    if (writer->failed) return;
    if (count > WORLD_SAVE_MAX_BYTES - writer->size)
    {
        writer->failed = true;
        return;
    }
    uint32_t required = writer->size + count;
    if (required > writer->capacity)
    {
        uint32_t capacity = writer->capacity;
        while (capacity < required && capacity <= WORLD_SAVE_MAX_BYTES / 2U)
            capacity *= 2U;
        if (capacity < required) capacity = required;
        uint8_t* expanded = PlatformReallocate(
            writer->bytes, capacity, false);
        if (expanded == NULL)
        {
            writer->failed = true;
            return;
        }
        writer->bytes = expanded;
        writer->capacity = capacity;
    }
    memcpy(writer->bytes + writer->size, bytes, count);
    writer->size += count;
}

static void SaveWriterU16(SaveWriter* writer, uint16_t value)
{
    SaveWriterBytes(writer, &value, sizeof(value));
}

static void SaveWriterU32(SaveWriter* writer, uint32_t value)
{
    SaveWriterBytes(writer, &value, sizeof(value));
}

static void SaveWriterI64(SaveWriter* writer, int64_t value)
{
    SaveWriterBytes(writer, &value, sizeof(value));
}

static void SaveWriterCoord(SaveWriter* writer, const InfiniteCoord* value)
{
    SaveWriterBytes(writer, &value->sign, sizeof(value->sign));
    SaveWriterU32(writer, value->limbCount);
    if (value->limbCount > 0)
    {
        SaveWriterBytes(writer, value->limbs,
            value->limbCount * (uint32_t)sizeof(uint64_t));
    }
}

// Абсолютная координата чанка: origin кадра + локальное смещение.
static bool SaveWriterFrameCoord(SaveWriter* writer,
    const InfiniteCoord* base, int64_t offset)
{
    InfiniteCoord absolute;
    InfiniteCoordInit(&absolute);
    if (!InfiniteCoordTryCopyAddInt64(&absolute, base, offset))
    {
        writer->failed = true;
        return false;
    }
    SaveWriterCoord(writer, &absolute);
    InfiniteCoordDestroy(&absolute);
    return true;
}

bool WorldSaveDeltas(World* world, const wchar_t* path)
{
    if (world == NULL || path == NULL) return false;
    SaveWriter writer = {
        .bytes = PlatformAllocate(WORLD_SAVE_INITIAL_BYTES, false),
        .capacity = WORLD_SAVE_INITIAL_BYTES,
    };
    if (writer.bytes == NULL) return false;

    SaveWriterU32(&writer, WORLD_SAVE_MAGIC);
    SaveWriterU16(&writer, WORLD_SAVE_VERSION);
    SaveWriterU16(&writer, 0);
    SaveWriterI64(&writer, world->seed);

    // Таблица правок читается под общим замком: рабочие потоки мешинга
    // продолжают читать мир, а правки делает только главный поток.
    PlatformRwLockAcquireShared(&world->tableLock);

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        SaveWriterCoord(&writer, &world->blockOrigin[axis]);
    }

    uint32_t chunkCount = 0;
    for (uint32_t slot = 0; slot < world->capacity; ++slot)
    {
        if (world->occupied[slot] && world->chunks[slot] != NULL
            && world->chunks[slot]->deltaCount > 0)
        {
            ++chunkCount;
        }
    }
    SaveWriterU32(&writer, chunkCount);

    for (uint32_t slot = 0; slot < world->capacity; ++slot)
    {
        if (!world->occupied[slot] || world->chunks[slot] == NULL
            || world->chunks[slot]->deltaCount == 0)
        {
            continue;
        }

        const GlobalChunkCoordinate* key = &world->keys[slot];
        const Chunk* chunk = world->chunks[slot];
        const int64_t local[3] = { key->local.x, key->local.y, key->local.z };
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (!SaveWriterFrameCoord(&writer,
                    &key->frame->chunkOrigin[axis], local[axis]))
            {
                break;
            }
        }
        SaveWriterU32(&writer, chunk->deltaCount);
        SaveWriterBytes(&writer, chunk->deltas,
            chunk->deltaCount * (uint32_t)sizeof(DeltaEntry));
    }

    PlatformRwLockReleaseShared(&world->tableLock);

    bool succeeded = !writer.failed
        && PlatformWriteFileAtomic(path, writer.bytes, writer.size);
    PlatformFree(writer.bytes);
    return succeeded;
}

typedef struct SaveReader
{
    const uint8_t* bytes;
    uint32_t length;
    uint32_t offset;
    bool failed;
} SaveReader;

static void SaveReaderBytes(SaveReader* reader, void* destination,
    uint32_t count)
{
    if (reader->failed || reader->offset + count > reader->length)
    {
        reader->failed = true;
        return;
    }
    memcpy(destination, reader->bytes + reader->offset, count);
    reader->offset += count;
}

static uint16_t SaveReaderU16(SaveReader* reader)
{
    uint16_t value = 0;
    SaveReaderBytes(reader, &value, sizeof(value));
    return value;
}

static uint32_t SaveReaderU32(SaveReader* reader)
{
    uint32_t value = 0;
    SaveReaderBytes(reader, &value, sizeof(value));
    return value;
}

static int64_t SaveReaderI64(SaveReader* reader)
{
    int64_t value = 0;
    SaveReaderBytes(reader, &value, sizeof(value));
    return value;
}

// Читает InfiniteCoord и сворачивает в int64. representable сбрасывается,
// если модуль шире 63 бит — v1 такие координаты пропускает.
static int64_t SaveReaderCoordInt64(SaveReader* reader, bool* representable)
{
    int32_t sign = 0;
    SaveReaderBytes(reader, &sign, sizeof(sign));
    uint32_t limbCount = SaveReaderU32(reader);
    if (limbCount > WORLD_SAVE_MAX_LIMBS)
    {
        reader->failed = true;
        return 0;
    }

    uint64_t low = 0;
    bool fits = true;
    for (uint32_t i = 0; i < limbCount; ++i)
    {
        uint64_t limb = 0;
        SaveReaderBytes(reader, &limb, sizeof(limb));
        if (i == 0)
        {
            low = limb;
        }
        else if (limb != 0)
        {
            fits = false;
        }
    }

    if (sign == 0 || limbCount == 0)
    {
        return 0;
    }
    if (low > (uint64_t)INT64_MAX)
    {
        fits = false;
    }
    if (!fits)
    {
        *representable = false;
        return 0;
    }
    return sign < 0 ? -(int64_t)low : (int64_t)low;
}

static bool SafeSubtractInt64(int64_t value, int64_t difference,
    int64_t* outValue)
{
    if (difference > 0 && value < INT64_MIN + difference) return false;
    if (difference < 0 && value > INT64_MAX + difference) return false;
    *outValue = value - difference;
    return true;
}

bool WorldLoadDeltas(World* world, const wchar_t* path)
{
    uint8_t* bytes = NULL;
    uint64_t size = 0;
    if (world == NULL || path == NULL
        || !PlatformReadEntireFile(
            path, WORLD_SAVE_MAX_BYTES, &bytes, &size)
        || size == 0 || size > UINT32_MAX)
    {
        PlatformFree(bytes);
        return false;
    }
    uint32_t length = (uint32_t)size;

    SaveReader reader = { .bytes = bytes, .length = length };
    bool succeeded = false;

    if (SaveReaderU32(&reader) == WORLD_SAVE_MAGIC
        && SaveReaderU16(&reader) == WORLD_SAVE_VERSION)
    {
        SaveReaderU16(&reader);  // reserved
        int64_t savedSeed = SaveReaderI64(&reader);

        bool originRepresentable = true;
        int64_t blockShift[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            blockShift[axis] =
                SaveReaderCoordInt64(&reader, &originRepresentable);
        }

        // Мир обязан быть свежесозданным с тем же зерном; v1 требует
        // представимости начала координат в int64.
        if (!reader.failed && savedSeed == world->seed
            && originRepresentable
            && (blockShift[0] == 0 && blockShift[1] == 0
                && blockShift[2] == 0
                ? true
                : WorldRebase(world,
                      blockShift[0], blockShift[1], blockShift[2])))
        {
            int64_t chunkShift[3] = {
                blockShift[0] / CHUNK_SIZE,
                blockShift[1] / CHUNK_SIZE,
                blockShift[2] / CHUNK_SIZE,
            };

            uint32_t chunkCount = SaveReaderU32(&reader);
            succeeded = !reader.failed;

            for (uint32_t i = 0; i < chunkCount && !reader.failed; ++i)
            {
                bool representable = true;
                int64_t absolute[3];
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    absolute[axis] =
                        SaveReaderCoordInt64(&reader, &representable);
                }

                int64_t localChunk[3];
                for (int32_t axis = 0; axis < 3 && representable; ++axis)
                {
                    representable = SafeSubtractInt64(absolute[axis],
                        chunkShift[axis], &localChunk[axis]);
                }

                uint32_t deltaCount = SaveReaderU32(&reader);
                for (uint32_t d = 0; d < deltaCount && !reader.failed; ++d)
                {
                    uint32_t entry = SaveReaderU32(&reader);
                    if (!representable)
                    {
                        continue;  // недостижимо далёкий чанк — пропуск v1
                    }
                    uint32_t index = DeltaLocalIndex(entry);
                    int64_t x = localChunk[0] * CHUNK_SIZE
                        + (int64_t)(index / (CHUNK_SIZE * CHUNK_SIZE));
                    int64_t y = localChunk[1] * CHUNK_SIZE
                        + (int64_t)((index / CHUNK_SIZE) % CHUNK_SIZE);
                    int64_t z = localChunk[2] * CHUNK_SIZE
                        + (int64_t)(index % CHUNK_SIZE);
                    WorldSetBlock(world, x, y, z, DeltaBlock(entry));
                }
            }

            succeeded = succeeded && !reader.failed;
        }
    }

    PlatformFree(bytes);
    return succeeded;
}
