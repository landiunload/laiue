#include "world/world.h"
#include "world/noise.h"

#include <windows.h>

typedef struct ChunkCoordinate
{
    int64_t x;
    int64_t y;
    int64_t z;
} ChunkCoordinate;

// Чанк хранит только отличия от процедурной генерации (дельты):
// нетронутый мир не занимает памяти вовсе.
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

// Кеш сеток высот: столб чанков (chunkX, chunkZ) использует одну и ту же
// сетку 66x66 — шум считается один раз на столб, а не на каждый чанк.
#define HEIGHT_GRID_SIZE (CHUNK_SIZE + 2)
#define HEIGHT_CACHE_SLOTS 16

typedef struct HeightGridSlot
{
    bool valid;
    int64_t chunkX;
    int64_t chunkZ;
    float minimumHeight;
    float maximumHeight;
    float* heights;
} HeightGridSlot;

// Хеш-таблица чанков с открытой адресацией (линейное пробирование).
// Внимание: кеш высот делает WorldFillRegion небезопасным для
// параллельных вызовов — читатель должен быть один (поток мешинга).
struct World
{
    ChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    int64_t seed;
    HeightGridSlot heightCache[HEIGHT_CACHE_SLOTS];
};

static inline int64_t ChunkFromBlock(int64_t block)
{
    return block >> CHUNK_SIZE_LOG2;
}

static inline uint16_t LocalFromBlock(int64_t block, int64_t chunkCoordinate)
{
    return (uint16_t)(block - (chunkCoordinate << CHUNK_SIZE_LOG2));
}

static inline uint32_t PackLocalIndex(uint16_t x, uint16_t y, uint16_t z)
{
    return (uint32_t)x * CHUNK_SIZE * CHUNK_SIZE + (uint32_t)y * CHUNK_SIZE + (uint32_t)z;
}

static bool ChunkCoordinateEquals(ChunkCoordinate a, ChunkCoordinate b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static uint32_t HashChunkCoordinate(ChunkCoordinate coordinate)
{
    uint64_t hash = (uint64_t)coordinate.x * 73856093ULL
                  ^ (uint64_t)coordinate.y * 19349663ULL
                  ^ (uint64_t)coordinate.z * 83492791ULL;
    return (uint32_t)(hash ^ (hash >> 33));
}

// Высота ландшафта в блочных координатах (x, z); блок твёрдый, если y < высоты.
static float TerrainHeight(int64_t seed, int64_t blockX, int64_t blockZ)
{
    float noiseX = (float)blockX * 0.025f;
    float noiseZ = (float)blockZ * 0.025f;
    float noise = GenerateNoise(seed, noiseX, 0.0f, noiseZ);
    return (noise - 0.5f) * 32.0f;
}

// Возвращает сетку высот столба чанков (66x66 колонн вокруг чанка),
// при попадании в кеш — без единого вычисления шума.
static const HeightGridSlot* WorldGetHeightGrid(World* world, int64_t chunkX, int64_t chunkZ)
{
    uint32_t slotIndex = (uint32_t)((uint64_t)chunkX * 73856093ULL
                                  ^ (uint64_t)chunkZ * 83492791ULL) & (HEIGHT_CACHE_SLOTS - 1);
    HeightGridSlot* slot = &world->heightCache[slotIndex];

    if (slot->valid && slot->chunkX == chunkX && slot->chunkZ == chunkZ)
    {
        return slot;
    }

    int64_t minBlockX = (chunkX << CHUNK_SIZE_LOG2) - 1;
    int64_t minBlockZ = (chunkZ << CHUNK_SIZE_LOG2) - 1;

    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    bool first = true;

    for (int32_t z = 0; z < HEIGHT_GRID_SIZE; ++z)
    {
        for (int32_t x = 0; x < HEIGHT_GRID_SIZE; ++x)
        {
            float height = TerrainHeight(world->seed, minBlockX + x, minBlockZ + z);
            slot->heights[z * HEIGHT_GRID_SIZE + x] = height;
            if (first || height < minimumHeight) minimumHeight = height;
            if (first || height > maximumHeight) maximumHeight = height;
            first = false;
        }
    }

    slot->valid = true;
    slot->chunkX = chunkX;
    slot->chunkZ = chunkZ;
    slot->minimumHeight = minimumHeight;
    slot->maximumHeight = maximumHeight;
    return slot;
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

    ChunkCoordinate* newKeys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newCapacity * sizeof(ChunkCoordinate));
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
        if (!world->occupied[i])
        {
            continue;
        }

        uint32_t mask = newCapacity - 1;
        uint32_t index = HashChunkCoordinate(world->keys[i]) & mask;
        while (newOccupied[index])
        {
            index = (index + 1) & mask;
        }
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
    if (world == NULL)
    {
        return NULL;
    }

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(ChunkCoordinate));
    world->chunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(Chunk*));
    world->occupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(bool));

    bool heightCacheAllocated = true;
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].heights = HeapAlloc(GetProcessHeap(), 0,
            (size_t)HEIGHT_GRID_SIZE * HEIGHT_GRID_SIZE * sizeof(float));
        if (world->heightCache[slot].heights == NULL)
        {
            heightCacheAllocated = false;
        }
    }

    if (world->keys == NULL || world->chunks == NULL || world->occupied == NULL || !heightCacheAllocated)
    {
        WorldDestroy(world);
        return NULL;
    }

    world->seed = seed;
    return world;
}

void WorldDestroy(World* world)
{
    if (world == NULL)
    {
        return;
    }

    if (world->occupied != NULL && world->chunks != NULL)
    {
        for (uint32_t i = 0; i < world->capacity; ++i)
        {
            if (world->occupied[i] && world->chunks[i] != NULL)
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

    if (world->keys != NULL) HeapFree(GetProcessHeap(), 0, world->keys);
    if (world->chunks != NULL) HeapFree(GetProcessHeap(), 0, world->chunks);
    if (world->occupied != NULL) HeapFree(GetProcessHeap(), 0, world->occupied);
    HeapFree(GetProcessHeap(), 0, world);
}

static Chunk** WorldFindEntry(World* world, ChunkCoordinate key)
{
    if (world->count == 0)
    {
        return NULL;
    }

    uint32_t mask = world->capacity - 1;
    uint32_t index = HashChunkCoordinate(key) & mask;

    for (uint32_t probe = 0; probe < world->capacity; ++probe)
    {
        if (!world->occupied[index])
        {
            return NULL;
        }
        if (ChunkCoordinateEquals(world->keys[index], key))
        {
            return &world->chunks[index];
        }
        index = (index + 1) & mask;
    }

    return NULL;
}

// Чанк создаётся лениво — только когда появляется первая дельта.
static Chunk* WorldGetOrCreateChunk(World* world, ChunkCoordinate key)
{
    Chunk** entry = WorldFindEntry(world, key);
    if (entry != NULL)
    {
        return *entry;
    }

    if (world->count * 2 >= world->capacity)
    {
        if (!WorldGrow(world))
        {
            return NULL;
        }
    }

    uint32_t mask = world->capacity - 1;
    uint32_t index = HashChunkCoordinate(key) & mask;
    while (world->occupied[index])
    {
        index = (index + 1) & mask;
    }

    Chunk* chunk = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Chunk));
    if (chunk == NULL)
    {
        return NULL;
    }

    world->keys[index] = key;
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
            ? HeapAlloc(GetProcessHeap(), 0, newCapacity * sizeof(DeltaEntry))
            : HeapReAlloc(GetProcessHeap(), 0, chunk->deltas, newCapacity * sizeof(DeltaEntry));
        if (newDeltas == NULL)
        {
            return;
        }

        chunk->deltas = newDeltas;
        chunk->deltaCapacity = newCapacity;
    }

    chunk->deltas[chunk->deltaCount].localIndex = localIndex;
    chunk->deltas[chunk->deltaCount].block = block;
    chunk->deltaCount++;
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    ChunkCoordinate coordinate = {
        .x = ChunkFromBlock(x),
        .y = ChunkFromBlock(y),
        .z = ChunkFromBlock(z),
    };

    Chunk** entry = WorldFindEntry(world, coordinate);
    if (entry != NULL)
    {
        uint32_t localIndex = PackLocalIndex(
            LocalFromBlock(x, coordinate.x),
            LocalFromBlock(y, coordinate.y),
            LocalFromBlock(z, coordinate.z)
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
    ChunkCoordinate coordinate = {
        .x = ChunkFromBlock(x),
        .y = ChunkFromBlock(y),
        .z = ChunkFromBlock(z),
    };

    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z)
    );

    // Установка блока в сгенерированное значение — удаление дельты,
    // а не создание новой: чанк не накапливает бесполезные записи.
    BlockType generated = (float)y < TerrainHeight(world->seed, x, z) ? BLOCK_EARTH : BLOCK_AIR;
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
                    return;
                }
            }
        }
        return;
    }

    Chunk* chunk = WorldGetOrCreateChunk(world, coordinate);
    if (chunk == NULL)
    {
        return;
    }

    ChunkSetDelta(chunk, localIndex, block);
}

WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks)
{
    // 1. Есть ли дельты в чанках, пересекающих регион.
    bool regionHasDeltas = false;
    int64_t minChunkX = ChunkFromBlock(minBlockX);
    int64_t minChunkY = ChunkFromBlock(minBlockY);
    int64_t minChunkZ = ChunkFromBlock(minBlockZ);
    int64_t maxChunkX = ChunkFromBlock(minBlockX + sizeX - 1);
    int64_t maxChunkY = ChunkFromBlock(minBlockY + sizeY - 1);
    int64_t maxChunkZ = ChunkFromBlock(minBlockZ + sizeZ - 1);

    for (int64_t chunkZ = minChunkZ; chunkZ <= maxChunkZ && !regionHasDeltas; ++chunkZ)
    {
        for (int64_t chunkY = minChunkY; chunkY <= maxChunkY && !regionHasDeltas; ++chunkY)
        {
            for (int64_t chunkX = minChunkX; chunkX <= maxChunkX && !regionHasDeltas; ++chunkX)
            {
                ChunkCoordinate coordinate = { chunkX, chunkY, chunkZ };
                Chunk** entry = WorldFindEntry(world, coordinate);
                if (entry != NULL && (*entry)->deltaCount > 0)
                {
                    regionHasDeltas = true;
                }
            }
        }
    }

    // 2. Высоты колонн. Запрос мешера (регион 66x66 вокруг чанка)
    // попадает в кеш сеток высот; произвольные регионы считают шум сами.
    const HeightGridSlot* cachedGrid = NULL;
    if (sizeX == HEIGHT_GRID_SIZE && sizeZ == HEIGHT_GRID_SIZE &&
        ((minBlockX + 1) & (CHUNK_SIZE - 1)) == 0 &&
        ((minBlockZ + 1) & (CHUNK_SIZE - 1)) == 0)
    {
        cachedGrid = WorldGetHeightGrid(world, (minBlockX + 1) >> CHUNK_SIZE_LOG2, (minBlockZ + 1) >> CHUNK_SIZE_LOG2);
    }

    float* localHeights = NULL;
    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    const float* heights = NULL;
    bool boundsKnown = false;

    if (cachedGrid != NULL)
    {
        heights = cachedGrid->heights;
        minimumHeight = cachedGrid->minimumHeight;
        maximumHeight = cachedGrid->maximumHeight;
        boundsKnown = true;
    }
    else
    {
        localHeights = HeapAlloc(GetProcessHeap(), 0, (size_t)sizeX * sizeZ * sizeof(float));
        if (localHeights != NULL)
        {
            bool first = true;
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                for (int32_t x = 0; x < sizeX; ++x)
                {
                    float height = TerrainHeight(world->seed, minBlockX + x, minBlockZ + z);
                    localHeights[z * sizeX + x] = height;
                    if (first || height < minimumHeight) minimumHeight = height;
                    if (first || height > maximumHeight) maximumHeight = height;
                    first = false;
                }
            }
            heights = localHeights;
            boundsKnown = true;
        }
    }

    // 3. Однородный регион без дельт — данные не нужны вовсе.
    if (!regionHasDeltas && boundsKnown)
    {
        if ((float)minBlockY >= maximumHeight)
        {
            if (localHeights != NULL) HeapFree(GetProcessHeap(), 0, localHeights);
            return WORLD_REGION_ALL_AIR;
        }
        if ((float)(minBlockY + sizeY - 1) < minimumHeight)
        {
            if (localHeights != NULL) HeapFree(GetProcessHeap(), 0, localHeights);
            return WORLD_REGION_ALL_SOLID;
        }
    }

    // 4. Заполнение сгенерированным ландшафтом.
    for (int32_t z = 0; z < sizeZ; ++z)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            float height = heights != NULL
                ? heights[z * sizeX + x]
                : TerrainHeight(world->seed, minBlockX + x, minBlockZ + z);

            BlockType* column = &outBlocks[((size_t)z * sizeX + x) * sizeY];
            for (int32_t y = 0; y < sizeY; ++y)
            {
                column[y] = (float)(minBlockY + y) < height ? BLOCK_EARTH : BLOCK_AIR;
            }
        }
    }

    if (localHeights != NULL)
    {
        HeapFree(GetProcessHeap(), 0, localHeights);
    }

    // 5. Наложение дельт пересекающих чанков.
    if (regionHasDeltas)
    {
        for (int64_t chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ)
        {
            for (int64_t chunkY = minChunkY; chunkY <= maxChunkY; ++chunkY)
            {
                for (int64_t chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX)
                {
                    ChunkCoordinate coordinate = { chunkX, chunkY, chunkZ };
                    Chunk** entry = WorldFindEntry(world, coordinate);
                    if (entry == NULL)
                    {
                        continue;
                    }

                    const Chunk* chunk = *entry;
                    int64_t chunkBaseX = chunkX << CHUNK_SIZE_LOG2;
                    int64_t chunkBaseY = chunkY << CHUNK_SIZE_LOG2;
                    int64_t chunkBaseZ = chunkZ << CHUNK_SIZE_LOG2;

                    for (uint32_t i = 0; i < chunk->deltaCount; ++i)
                    {
                        uint32_t localIndex = chunk->deltas[i].localIndex;
                        int64_t blockX = chunkBaseX + (localIndex / (CHUNK_SIZE * CHUNK_SIZE));
                        int64_t blockY = chunkBaseY + ((localIndex / CHUNK_SIZE) % CHUNK_SIZE);
                        int64_t blockZ = chunkBaseZ + (localIndex % CHUNK_SIZE);

                        int64_t relativeX = blockX - minBlockX;
                        int64_t relativeY = blockY - minBlockY;
                        int64_t relativeZ = blockZ - minBlockZ;
                        if (relativeX < 0 || relativeX >= sizeX ||
                            relativeY < 0 || relativeY >= sizeY ||
                            relativeZ < 0 || relativeZ >= sizeZ)
                        {
                            continue;
                        }

                        outBlocks[(((size_t)relativeZ * sizeX) + (size_t)relativeX) * sizeY + (size_t)relativeY] =
                            chunk->deltas[i].block;
                    }
                }
            }
        }
    }

    return WORLD_REGION_MIXED;
}

int32_t WorldGetTerrainHeight(World* world, int64_t x, int64_t z)
{
    float height = TerrainHeight(world->seed, x, z);
    return height >= 0.0f ? (int32_t)(height + 1.0f) : (int32_t)height;
}
