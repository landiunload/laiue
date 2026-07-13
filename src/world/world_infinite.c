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

typedef struct GlobalChunkCoordinate
{
    InfiniteCoord axis[3];
    uint64_t hash;
} GlobalChunkCoordinate;

typedef struct DeltaEntry
{
    uint32_t localIndex;
    BlockType block;
} DeltaEntry;

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

static uint64_t RotateLeft64(uint64_t value, uint32_t amount)
{
    return (value << amount) | (value >> (64u - amount));
}

static uint64_t HashLocalChunkCoordinate(const World* world, LocalChunkCoordinate coordinate)
{
    uint64_t x = InfiniteCoordHashOffset(&world->chunkOrigin[0], coordinate.x);
    uint64_t y = InfiniteCoordHashOffset(&world->chunkOrigin[1], coordinate.y);
    uint64_t z = InfiniteCoordHashOffset(&world->chunkOrigin[2], coordinate.z);
    return x ^ RotateLeft64(y, 21) ^ RotateLeft64(z, 42);
}

static bool GlobalChunkCoordinateMatchesLocal(const GlobalChunkCoordinate* global,
    const World* world, LocalChunkCoordinate local)
{
    return InfiniteCoordEqualsOffset(&global->axis[0], &world->chunkOrigin[0], local.x)
        && InfiniteCoordEqualsOffset(&global->axis[1], &world->chunkOrigin[1], local.y)
        && InfiniteCoordEqualsOffset(&global->axis[2], &world->chunkOrigin[2], local.z);
}

static void GlobalChunkCoordinateDestroy(GlobalChunkCoordinate* coordinate)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&coordinate->axis[axis]);
    }
    coordinate->hash = 0;
}

static bool GlobalChunkCoordinateTryCreate(GlobalChunkCoordinate* out,
    const World* world, LocalChunkCoordinate local)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&out->axis[axis]);
    }
    out->hash = HashLocalChunkCoordinate(world, local);

    int64_t offset[3] = { local.x, local.y, local.z };
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!InfiniteCoordTryCopyAddInt64(&out->axis[axis], &world->chunkOrigin[axis], offset[axis]))
        {
            GlobalChunkCoordinateDestroy(out);
            return false;
        }
    }
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
    return IsAbsoluteZBelow(world, z, boundary) ? BLOCK_EARTH : BLOCK_AIR;
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

static bool ChunkGetDelta(const Chunk* chunk, uint32_t localIndex, BlockType* outBlock)
{
    for (uint32_t i = 0; i < chunk->deltaCount; ++i)
    {
        if (chunk->deltas[i].localIndex == localIndex)
        {
            *outBlock = chunk->deltas[i].block;
            return true;
        }
    }
    return false;
}

static void ChunkSetDelta(Chunk* chunk, uint32_t localIndex, BlockType block)
{
    for (uint32_t i = 0; i < chunk->deltaCount; ++i)
    {
        if (chunk->deltas[i].localIndex == localIndex)
        {
            chunk->deltas[i].block = block;
            return;
        }
    }

    if (chunk->deltaCount >= chunk->deltaCapacity)
    {
        uint32_t newCapacity = chunk->deltaCapacity < 8 ? 8 : chunk->deltaCapacity * 2;
        DeltaEntry* newDeltas = chunk->deltas == NULL
            ? HeapAlloc(GetProcessHeap(), 0, (size_t)newCapacity * sizeof(DeltaEntry))
            : HeapReAlloc(GetProcessHeap(), 0, chunk->deltas,
                (size_t)newCapacity * sizeof(DeltaEntry));
        if (newDeltas == NULL) return;
        chunk->deltas = newDeltas;
        chunk->deltaCapacity = newCapacity;
    }

    chunk->deltas[chunk->deltaCount].localIndex = localIndex;
    chunk->deltas[chunk->deltaCount].block = block;
    chunk->deltaCount++;
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
            for (uint32_t i = 0; i < chunk->deltaCount; ++i)
            {
                if (chunk->deltas[i].localIndex == localIndex)
                {
                    chunk->deltas[i] = chunk->deltas[--chunk->deltaCount];
                    break;
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
    BlockType* outBlocks)
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

    float* heights = HeapAlloc(GetProcessHeap(), 0, (size_t)sizeX * sizeY * sizeof(float));
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
            HeapFree(GetProcessHeap(), 0, heights);
            return WORLD_REGION_ALL_AIR;
        }
        if (IsAbsoluteZBelow(world, minBlockZ + sizeZ - 1, ColumnCeiling(minimumHeight)))
        {
            HeapFree(GetProcessHeap(), 0, heights);
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
            int32_t solidCount = SolidCountInColumn(
                world, minBlockZ, sizeZ, ColumnCeiling(height));

            BlockType* column = &outBlocks[(((size_t)y * sizeX) + (size_t)x) * sizeZ];
            memset(column, BLOCK_EARTH, (size_t)solidCount);
            memset(column + solidCount, BLOCK_AIR, (size_t)(sizeZ - solidCount));
        }
    }

    if (heights != NULL) HeapFree(GetProcessHeap(), 0, heights);

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
                        uint32_t localIndex = chunk->deltas[i].localIndex;
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
                            + (size_t)relativeZ] = chunk->deltas[i].block;
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

void WorldGetAbsoluteBlockLow32(World* world,
    int64_t x, int64_t y, int64_t z, uint32_t outLow[3])
{
    outLow[0] = InfiniteCoordLow32Offset(&world->blockOrigin[0], x);
    outLow[1] = InfiniteCoordLow32Offset(&world->blockOrigin[1], y);
    outLow[2] = InfiniteCoordLow32Offset(&world->blockOrigin[2], z);
}
