#include "world/world.h"
#include "world/infinite_coord.h"
#include "world/terrain_noise.h"

#include <windows.h>
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
    SRWLOCK tableLock;
    GlobalChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;

    int64_t seed;
    InfiniteCoord blockOrigin[3];
    InfiniteCoord chunkOrigin[3];
    CoordinateFrame* editFrame;
    TerrainOrigin terrainOrigin;

    SRWLOCK heightCacheLock;
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
    HeapFree(GetProcessHeap(), 0, frame);
}

static CoordinateFrame* WorldGetEditFrame(World* world)
{
    if (world->editFrame != NULL) return world->editFrame;

    CoordinateFrame* frame = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*frame));
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

    AcquireSRWLockShared(&world->heightCacheLock);
    if (slot->valid && slot->chunkX == chunkX && slot->chunkY == chunkY)
    {
        memcpy(outHeights, slot->heights, HEIGHT_GRID_CELLS * sizeof(float));
        *outMinimum = slot->minimumHeight;
        *outMaximum = slot->maximumHeight;
        ReleaseSRWLockShared(&world->heightCacheLock);
        return;
    }
    ReleaseSRWLockShared(&world->heightCacheLock);

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

    AcquireSRWLockExclusive(&world->heightCacheLock);
    memcpy(slot->heights, outHeights, HEIGHT_GRID_CELLS * sizeof(float));
    slot->chunkX = chunkX;
    slot->chunkY = chunkY;
    slot->minimumHeight = minimumHeight;
    slot->maximumHeight = maximumHeight;
    slot->valid = true;
    ReleaseSRWLockExclusive(&world->heightCacheLock);
}

static void ChunkDestroy(Chunk* chunk)
{
    if (chunk->deltas != NULL)
    {
        HeapFree(GetProcessHeap(), 0, chunk->deltas);
    }
    HeapFree(GetProcessHeap(), 0, chunk);
}

static bool WorldGrow(World* world)
{
    uint32_t oldCapacity = world->capacity;
    uint32_t newCapacity = oldCapacity * 2;

    GlobalChunkCoordinate* newKeys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)newCapacity * sizeof(GlobalChunkCoordinate));
    Chunk** newChunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)newCapacity * sizeof(Chunk*));
    bool* newOccupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)newCapacity * sizeof(bool));

    if (newKeys == NULL || newChunks == NULL || newOccupied == NULL)
    {
        if (newKeys != NULL) HeapFree(GetProcessHeap(), 0, newKeys);
        if (newChunks != NULL) HeapFree(GetProcessHeap(), 0, newChunks);
        if (newOccupied != NULL) HeapFree(GetProcessHeap(), 0, newOccupied);
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

    HeapFree(GetProcessHeap(), 0, world->keys);
    HeapFree(GetProcessHeap(), 0, world->chunks);
    HeapFree(GetProcessHeap(), 0, world->occupied);

    world->keys = newKeys;
    world->chunks = newChunks;
    world->occupied = newOccupied;
    world->capacity = newCapacity;
    return true;
}

World* WorldCreate(int64_t seed)
{
    World* world = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*world));
    if (world == NULL) return NULL;

    InitializeSRWLock(&world->tableLock);
    InitializeSRWLock(&world->heightCacheLock);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&world->blockOrigin[axis]);
        InfiniteCoordInit(&world->chunkOrigin[axis]);
    }

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)world->capacity * sizeof(GlobalChunkCoordinate));
    world->chunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)world->capacity * sizeof(Chunk*));
    world->occupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)world->capacity * sizeof(bool));

    bool heightCacheAllocated = true;
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].heights = HeapAlloc(GetProcessHeap(), 0,
            HEIGHT_GRID_CELLS * sizeof(float));
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
            HeapFree(GetProcessHeap(), 0, world->heightCache[slot].heights);
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&world->blockOrigin[axis]);
        InfiniteCoordDestroy(&world->chunkOrigin[axis]);
    }

    if (world->keys != NULL) HeapFree(GetProcessHeap(), 0, world->keys);
    if (world->chunks != NULL) HeapFree(GetProcessHeap(), 0, world->chunks);
    if (world->occupied != NULL) HeapFree(GetProcessHeap(), 0, world->occupied);
    HeapFree(GetProcessHeap(), 0, world);
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

    AcquireSRWLockExclusive(&world->heightCacheLock);
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].valid = false;
    }
    ReleaseSRWLockExclusive(&world->heightCacheLock);
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

    AcquireSRWLockExclusive(&world->heightCacheLock);
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].valid = false;
    }
    ReleaseSRWLockExclusive(&world->heightCacheLock);
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

    Chunk* chunk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Chunk));
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

static void ChunkSetDelta(Chunk* chunk, uint32_t localIndex, BlockType block)
{
    uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
    if (position < chunk->deltaCount
        && DeltaLocalIndex(chunk->deltas[position]) == localIndex)
    {
        chunk->deltas[position] = PackDelta(localIndex, block);
        return;
    }

    if (chunk->deltaCount >= chunk->deltaCapacity)
    {
        uint32_t newCapacity = chunk->deltaCapacity < 4 ? 4 : chunk->deltaCapacity * 2;
        DeltaEntry* newDeltas = chunk->deltas == NULL
            ? HeapAlloc(GetProcessHeap(), 0, (size_t)newCapacity * sizeof(DeltaEntry))
            : HeapReAlloc(GetProcessHeap(), 0, chunk->deltas,
                (size_t)newCapacity * sizeof(DeltaEntry));
        if (newDeltas == NULL) return;
        chunk->deltas = newDeltas;
        chunk->deltaCapacity = newCapacity;
    }

    for (uint32_t index = chunk->deltaCount; index > position; --index)
    {
        chunk->deltas[index] = chunk->deltas[index - 1u];
    }
    chunk->deltas[position] = PackDelta(localIndex, block);
    chunk->deltaCount++;
}

static void WorldInsertExistingEntry(
    World* world, GlobalChunkCoordinate key, Chunk* chunk)
{
    uint32_t mask = world->capacity - 1u;
    uint32_t index = (uint32_t)(key.hash ^ (key.hash >> 32)) & mask;
    while (world->occupied[index])
    {
        index = (index + 1u) & mask;
    }

    world->keys[index] = key;
    world->chunks[index] = chunk;
    world->occupied[index] = true;
    world->count++;
}

static void WorldRemoveEntry(World* world, LocalChunkCoordinate key)
{
    if (world->count == 0) return;

    uint64_t hash = HashLocalChunkCoordinate(world, key);
    uint32_t mask = world->capacity - 1u;
    uint32_t index = (uint32_t)(hash ^ (hash >> 32)) & mask;

    while (world->occupied[index])
    {
        if (world->keys[index].hash == hash
            && GlobalChunkCoordinateMatchesLocal(&world->keys[index], world, key))
        {
            break;
        }
        index = (index + 1u) & mask;
    }
    if (!world->occupied[index]) return;

    GlobalChunkCoordinateDestroy(&world->keys[index]);
    ChunkDestroy(world->chunks[index]);
    world->chunks[index] = NULL;
    world->occupied[index] = false;
    world->count--;

    // Открытая адресация без tombstone: хвост кластера надо вставить заново,
    // иначе поиск остановится на образовавшейся дыре.
    uint32_t scan = (index + 1u) & mask;
    while (world->occupied[scan])
    {
        GlobalChunkCoordinate movedKey = world->keys[scan];
        Chunk* movedChunk = world->chunks[scan];
        world->chunks[scan] = NULL;
        world->occupied[scan] = false;
        world->count--;
        WorldInsertExistingEntry(world, movedKey, movedChunk);
        scan = (scan + 1u) & mask;
    }
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };

    AcquireSRWLockShared(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    if (entry != NULL)
    {
        uint32_t localIndex = PackLocalIndex(
            LocalFromBlock(x, coordinate.x),
            LocalFromBlock(y, coordinate.y),
            LocalFromBlock(z, coordinate.z));
        BlockType block;
        if (ChunkGetDelta(*entry, localIndex, &block))
        {
            ReleaseSRWLockShared(&world->tableLock);
            return block;
        }
    }
    ReleaseSRWLockShared(&world->tableLock);
    return GeneratedBlock(world, x, y, z);
}

void WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block)
{
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };
    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z));
    BlockType generated = GeneratedBlock(world, x, y, z);

    AcquireSRWLockExclusive(&world->tableLock);
    if (block == generated)
    {
        Chunk** entry = WorldFindEntry(world, coordinate);
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
                if (chunk->deltaCount == 0)
                {
                    WorldRemoveEntry(world, coordinate);
                }
            }
        }
    }
    else
    {
        Chunk* chunk = WorldGetOrCreateChunk(world, coordinate);
        if (chunk != NULL) ChunkSetDelta(chunk, localIndex, block);
    }
    ReleaseSRWLockExclusive(&world->tableLock);
}

WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks,
    float* heightScratch, size_t heightScratchCount)
{
    bool regionHasDeltas = false;
    int64_t minChunkX = ChunkFromBlock(minBlockX);
    int64_t minChunkY = ChunkFromBlock(minBlockY);
    int64_t minChunkZ = ChunkFromBlock(minBlockZ);
    int64_t maxChunkX = ChunkFromBlock(minBlockX + sizeX - 1);
    int64_t maxChunkY = ChunkFromBlock(minBlockY + sizeY - 1);
    int64_t maxChunkZ = ChunkFromBlock(minBlockZ + sizeZ - 1);

    AcquireSRWLockShared(&world->tableLock);
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
    ReleaseSRWLockShared(&world->tableLock);

    size_t heightCount = (size_t)sizeX * (size_t)sizeY;
    bool heightsOnHeap = heightScratch == NULL
        || heightScratchCount < heightCount;
    float* heights = heightsOnHeap
        ? HeapAlloc(GetProcessHeap(), 0, heightCount * sizeof(float))
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
            if (heightsOnHeap) HeapFree(GetProcessHeap(), 0, heights);
            return WORLD_REGION_ALL_AIR;
        }
        if (IsAbsoluteZBelow(world, minBlockZ + sizeZ - 1, ColumnCeiling(minimumHeight)))
        {
            if (heightsOnHeap) HeapFree(GetProcessHeap(), 0, heights);
            return WORLD_REGION_ALL_SOLID;
        }
    }

    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            float height = heights != NULL
                ? heights[y * sizeX + x]
                : TerrainHeight(world, minBlockX + x, minBlockY + y);
            int32_t boundary = ColumnCeiling(height);
            int32_t solidCount = SolidCountInColumn(
                world, minBlockZ, sizeZ, boundary);

            BlockType* column = &outBlocks[(((size_t)y * sizeX) + (size_t)x) * sizeZ];
            memset(column, BLOCK_EARTH, (size_t)solidCount);
            memset(column + solidCount, BLOCK_AIR, (size_t)(sizeZ - solidCount));

            // Верхний сгенерированный solid-блок может лежать на последней
            // позиции региона, поэтому проверяем также блок сразу за регионом.
            if (solidCount > 0
                && !IsAbsoluteZBelow(
                    world, minBlockZ + solidCount, boundary))
            {
                column[solidCount - 1] = BLOCK_GRASS;
            }
        }
    }

    if (heightsOnHeap && heights != NULL) HeapFree(GetProcessHeap(), 0, heights);

    if (regionHasDeltas)
    {
        AcquireSRWLockShared(&world->tableLock);
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
        ReleaseSRWLockShared(&world->tableLock);
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
#define WORLD_SAVE_BUFFER_BYTES 65536u

typedef struct SaveWriter
{
    HANDLE file;
    uint8_t* buffer;
    uint32_t used;
    bool failed;
} SaveWriter;

static void SaveWriterFlush(SaveWriter* writer)
{
    if (writer->failed || writer->used == 0)
    {
        return;
    }
    DWORD written = 0;
    if (!WriteFile(writer->file, writer->buffer, writer->used, &written, NULL)
        || written != writer->used)
    {
        writer->failed = true;
    }
    writer->used = 0;
}

static void SaveWriterBytes(SaveWriter* writer,
    const void* bytes, uint32_t count)
{
    if (writer->failed)
    {
        return;
    }
    const uint8_t* source = bytes;
    while (count > 0)
    {
        uint32_t space = WORLD_SAVE_BUFFER_BYTES - writer->used;
        uint32_t portion = count < space ? count : space;
        memcpy(writer->buffer + writer->used, source, portion);
        writer->used += portion;
        source += portion;
        count -= portion;
        if (writer->used == WORLD_SAVE_BUFFER_BYTES)
        {
            SaveWriterFlush(writer);
        }
    }
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
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    SaveWriter writer = { .file = file };
    writer.buffer = HeapAlloc(GetProcessHeap(), 0, WORLD_SAVE_BUFFER_BYTES);
    if (writer.buffer == NULL)
    {
        CloseHandle(file);
        return false;
    }

    SaveWriterU32(&writer, WORLD_SAVE_MAGIC);
    SaveWriterU16(&writer, WORLD_SAVE_VERSION);
    SaveWriterU16(&writer, 0);
    SaveWriterI64(&writer, world->seed);

    // Таблица правок читается под общим замком: рабочие потоки мешинга
    // продолжают читать мир, а правки делает только главный поток.
    AcquireSRWLockShared(&world->tableLock);

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

    ReleaseSRWLockShared(&world->tableLock);

    SaveWriterFlush(&writer);
    bool succeeded = !writer.failed;
    HeapFree(GetProcessHeap(), 0, writer.buffer);
    CloseHandle(file);
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
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > 64ll * 1024 * 1024)
    {
        CloseHandle(file);
        return false;
    }

    uint32_t length = (uint32_t)size.QuadPart;
    uint8_t* bytes = HeapAlloc(GetProcessHeap(), 0, length);
    if (bytes == NULL)
    {
        CloseHandle(file);
        return false;
    }
    uint32_t completed = 0;
    while (completed < length)
    {
        DWORD read = 0;
        if (!ReadFile(file, bytes + completed, length - completed, &read, NULL)
            || read == 0)
        {
            HeapFree(GetProcessHeap(), 0, bytes);
            CloseHandle(file);
            return false;
        }
        completed += read;
    }
    CloseHandle(file);

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

    HeapFree(GetProcessHeap(), 0, bytes);
    return succeeded;
}
