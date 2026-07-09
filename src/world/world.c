#include "world/world.h"

#include <windows.h>

#define CHUNK_VOLUME (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

typedef struct ChunkCoord
{
    int64_t x;
    int64_t y;
    int64_t z;
} ChunkCoord;

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

struct World
{
    ChunkCoord* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    int64_t seed;
};

static inline int64_t ChunkFromBlock(int64_t block)
{
    return block >> CHUNK_SIZE_LOG2;
}

static inline uint16_t LocalFromBlock(int64_t block, int64_t chunkCoord)
{
    return (uint16_t)(block - (chunkCoord << CHUNK_SIZE_LOG2));
}

static inline uint32_t PackLocalIndex(uint16_t x, uint16_t y, uint16_t z)
{
    return (uint32_t)x * CHUNK_SIZE * CHUNK_SIZE + (uint32_t)y * CHUNK_SIZE + (uint32_t)z;
}

static ChunkCoord MakeChunkCoord(int64_t x, int64_t y, int64_t z)
{
    ChunkCoord c;
    c.x = x;
    c.y = y;
    c.z = z;
    return c;
}

static bool ChunkCoordEquals(ChunkCoord a, ChunkCoord b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static uint32_t HashChunkCoord(ChunkCoord c)
{
    uint64_t h = (uint64_t)c.x * 73856093ULL
               ^ (uint64_t)c.y * 19349663ULL
               ^ (uint64_t)c.z * 83492791ULL;
    return (uint32_t)(h ^ (h >> 33));
}

static float TerrainHeight(int64_t seed, int64_t bx, int64_t bz);

static Chunk* ChunkCreate(void)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Chunk));
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
    ChunkCoord* oldKeys = world->keys;
    Chunk** oldChunks = world->chunks;
    bool* oldOccupied = world->occupied;

    uint32_t newCapacity = oldCapacity * 2;
    ChunkCoord* newKeys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newCapacity * sizeof(ChunkCoord));
    Chunk** newChunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newCapacity * sizeof(Chunk*));
    bool* newOccupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newCapacity * sizeof(bool));

    if (newKeys == NULL || newChunks == NULL || newOccupied == NULL)
    {
        if (newKeys != NULL) HeapFree(GetProcessHeap(), 0, newKeys);
        if (newChunks != NULL) HeapFree(GetProcessHeap(), 0, newChunks);
        if (newOccupied != NULL) HeapFree(GetProcessHeap(), 0, newOccupied);
        return false;
    }

    for (uint32_t i = 0; i < oldCapacity; ++i)
    {
        if (!oldOccupied[i]) continue;

        uint32_t mask = newCapacity - 1;
        uint32_t idx = HashChunkCoord(oldKeys[i]) & mask;
        while (newOccupied[idx])
        {
            idx = (idx + 1) & mask;
        }
        newKeys[idx] = oldKeys[i];
        newChunks[idx] = oldChunks[i];
        newOccupied[idx] = true;
    }

    if (oldKeys != NULL) HeapFree(GetProcessHeap(), 0, oldKeys);
    if (oldChunks != NULL) HeapFree(GetProcessHeap(), 0, oldChunks);
    if (oldOccupied != NULL) HeapFree(GetProcessHeap(), 0, oldOccupied);

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

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(ChunkCoord));
    world->chunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(Chunk*));
    world->occupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(bool));

    if (world->keys == NULL || world->chunks == NULL || world->occupied == NULL)
    {
        if (world->keys != NULL) HeapFree(GetProcessHeap(), 0, world->keys);
        if (world->chunks != NULL) HeapFree(GetProcessHeap(), 0, world->chunks);
        if (world->occupied != NULL) HeapFree(GetProcessHeap(), 0, world->occupied);
        HeapFree(GetProcessHeap(), 0, world);
        return NULL;
    }

    world->seed = seed;
    return world;
}

void WorldDestroy(World* world)
{
    if (world == NULL) return;

    for (uint32_t i = 0; i < world->capacity; ++i)
    {
        if (world->occupied[i] && world->chunks[i] != NULL)
        {
            ChunkDestroy(world->chunks[i]);
        }
    }

    HeapFree(GetProcessHeap(), 0, world->keys);
    HeapFree(GetProcessHeap(), 0, world->chunks);
    HeapFree(GetProcessHeap(), 0, world->occupied);
    HeapFree(GetProcessHeap(), 0, world);
}

static Chunk** WorldFindEntry(World* world, ChunkCoord key)
{
    if (world->count == 0) return NULL;

    uint32_t mask = world->capacity - 1;
    uint32_t idx = HashChunkCoord(key) & mask;

    for (uint32_t i = 0; i < world->capacity; ++i)
    {
        if (!world->occupied[idx]) return NULL;
        if (ChunkCoordEquals(world->keys[idx], key))
        {
            return &world->chunks[idx];
        }
        idx = (idx + 1) & mask;
    }

    return NULL;
}

static Chunk* WorldGetOrCreateChunk(World* world, ChunkCoord key)
{
    Chunk** entry = WorldFindEntry(world, key);
    if (entry != NULL) return *entry;

    if (world->count * 2 >= world->capacity)
    {
        if (!WorldGrow(world)) return NULL;
    }

    uint32_t mask = world->capacity - 1;
    uint32_t idx = HashChunkCoord(key) & mask;
    while (world->occupied[idx])
    {
        idx = (idx + 1) & mask;
    }

    Chunk* chunk = ChunkCreate();
    if (chunk == NULL) return NULL;

    world->keys[idx] = key;
    world->chunks[idx] = chunk;
    world->occupied[idx] = true;
    world->count++;

    return chunk;
}

static bool ChunkGetDelta(const Chunk* chunk, uint32_t localIndex, BlockType* out)
{
    for (uint32_t i = 0; i < chunk->deltaCount; ++i)
    {
        if (chunk->deltas[i].localIndex == localIndex)
        {
            *out = chunk->deltas[i].block;
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
        DeltaEntry* newDeltas = HeapAlloc(GetProcessHeap(), 0, newCapacity * sizeof(DeltaEntry));
        if (newDeltas == NULL) return;

        for (uint32_t i = 0; i < chunk->deltaCount; ++i)
        {
            newDeltas[i] = chunk->deltas[i];
        }

        if (chunk->deltas != NULL) HeapFree(GetProcessHeap(), 0, chunk->deltas);
        chunk->deltas = newDeltas;
        chunk->deltaCapacity = newCapacity;
    }

    chunk->deltas[chunk->deltaCount].localIndex = localIndex;
    chunk->deltas[chunk->deltaCount].block = block;
    chunk->deltaCount++;
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    ChunkCoord cc;
    cc.x = ChunkFromBlock(x);
    cc.y = ChunkFromBlock(y);
    cc.z = ChunkFromBlock(z);

    Chunk** entry = WorldFindEntry(world, cc);
    if (entry != NULL)
    {
        uint32_t localIndex = PackLocalIndex(
            LocalFromBlock(x, cc.x),
            LocalFromBlock(y, cc.y),
            LocalFromBlock(z, cc.z)
        );
        BlockType block;
        if (ChunkGetDelta(*entry, localIndex, &block))
        {
            return block;
        }
    }

    return (float)y < TerrainHeight(world->seed, x, z) ? BLOCK_EARTH : BLOCK_AIR;
}

void WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block)
{
    ChunkCoord cc;
    cc.x = ChunkFromBlock(x);
    cc.y = ChunkFromBlock(y);
    cc.z = ChunkFromBlock(z);

    Chunk* chunk = WorldGetOrCreateChunk(world, cc);
    if (chunk == NULL) return;

    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, cc.x),
        LocalFromBlock(y, cc.y),
        LocalFromBlock(z, cc.z)
    );

    BlockType generated = (float)y < TerrainHeight(world->seed, x, z) ? BLOCK_EARTH : BLOCK_AIR;
    if (block == generated)
    {
        for (uint32_t i = 0; i < chunk->deltaCount; ++i)
        {
            if (chunk->deltas[i].localIndex == localIndex)
            {
                chunk->deltas[i] = chunk->deltas[--chunk->deltaCount];
                return;
            }
        }
        return;
    }

    ChunkSetDelta(chunk, localIndex, block);
}

void WorldLoadArea(World* world, int64_t cx, int64_t cy, int64_t cz, int32_t radius)
{
    int64_t startX = cx - radius;
    int64_t startY = cy - radius;
    int64_t startZ = cz - radius;
    int64_t endX = cx + radius;
    int64_t endY = cy + radius;
    int64_t endZ = cz + radius;

    for (int64_t x = startX; x <= endX; ++x)
    {
        for (int64_t y = startY; y <= endY; ++y)
        {
            for (int64_t z = startZ; z <= endZ; ++z)
            {
                ChunkCoord cc = MakeChunkCoord(x, y, z);
                WorldGetOrCreateChunk(world, cc);
            }
        }
    }
}

// Генерация высоты ландшафта через 2D-шум.
// Обёртка: функция определена в noise.c.
float GenerateNoise(int64_t seed, float x, float y, float z);

static float TerrainHeight(int64_t seed, int64_t bx, int64_t bz)
{
    float fx = (float)bx * 0.025f;
    float fz = (float)bz * 0.025f;
    float h = GenerateNoise(seed, fx, 0.0f, fz);
    return (h - 0.5f) * 32.0f;
}

int32_t WorldGetTerrainHeight(World* world, int64_t x, int64_t z)
{
    float h = TerrainHeight(world->seed, x, z);
    return h >= 0.0f ? (int32_t)(h + 1.0f) : (int32_t)h;
}
