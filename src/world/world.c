#include "world/world.h"
#include "world/noise.h"

#include <windows.h>
#include <string.h>

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
#define HEIGHT_GRID_CELLS (HEIGHT_GRID_SIZE * HEIGHT_GRID_SIZE)
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
// tableLock: читатели (GetBlock, FillRegion) — shared, писатель (SetBlock) —
// exclusive. heightCacheLock защищает кеш высот; сетка при промахе
// считается вне замка, чтобы читатели не сериализовали вычисление шума.
struct World
{
    SRWLOCK tableLock;
    ChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    int64_t seed;
    TerrainOrigin terrainOrigin;   // абсолютный origin мира (остатки для шума)
    // Границы мира в ЛОКАЛЬНЫХ координатах (относительно origin): за ними
    // рельефа нет — это край мира (пустота).
    int64_t worldLocalMinX, worldLocalMaxX;
    int64_t worldLocalMinZ, worldLocalMaxZ;

    SRWLOCK heightCacheLock;
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

// Высота ландшафта в блочных координатах (x, z); блок твёрдый, если y < высоты.
static float TerrainHeight(const World* world, int64_t localX, int64_t localZ)
{
    // За краем мира рельефа нет: возвращаем очень низкую «высоту», и
    // существующая логика заполнения делает всю колонну воздухом.
    if (localX < world->worldLocalMinX || localX >= world->worldLocalMaxX ||
        localZ < world->worldLocalMinZ || localZ >= world->worldLocalMaxZ)
    {
        return -1.0e9f;
    }

    // Шум по АБСОЛЮТНОЙ координате (origin мира + локальная) через
    // предвычисленные остатки origin — bignum в горячем пути нет.
    float noise = GenerateTerrainNoise(world->seed, &world->terrainOrigin, localX, localZ);
    return (noise - 0.5f) * 32.0f;
}

// Первый воздушный уровень колонны: ceil(height).
static int32_t ColumnCeiling(float height)
{
    int32_t ceiling = (int32_t)height;
    if ((float)ceiling < height)
    {
        ceiling++;
    }
    return ceiling;
}

// Заполняет сетку высот столба чанков; при попадании в кеш —
// без единого вычисления шума (копия из слота).
static void WorldObtainHeightGrid(World* world, int64_t chunkX, int64_t chunkZ,
    float* outHeights, float* outMinimum, float* outMaximum)
{
    uint32_t slotIndex = WorldHashChunkCoordinate(chunkX, 0, chunkZ) & (HEIGHT_CACHE_SLOTS - 1);
    HeightGridSlot* slot = &world->heightCache[slotIndex];

    AcquireSRWLockShared(&world->heightCacheLock);
    if (slot->valid && slot->chunkX == chunkX && slot->chunkZ == chunkZ)
    {
        memcpy(outHeights, slot->heights, HEIGHT_GRID_CELLS * sizeof(float));
        *outMinimum = slot->minimumHeight;
        *outMaximum = slot->maximumHeight;
        ReleaseSRWLockShared(&world->heightCacheLock);
        return;
    }
    ReleaseSRWLockShared(&world->heightCacheLock);

    // Промах: тяжёлое вычисление шума — вне замка.
    int64_t minBlockX = (chunkX << CHUNK_SIZE_LOG2) - 1;
    int64_t minBlockZ = (chunkZ << CHUNK_SIZE_LOG2) - 1;

    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    bool first = true;

    for (int32_t z = 0; z < HEIGHT_GRID_SIZE; ++z)
    {
        for (int32_t x = 0; x < HEIGHT_GRID_SIZE; ++x)
        {
            float height = TerrainHeight(world, minBlockX + x, minBlockZ + z);
            outHeights[z * HEIGHT_GRID_SIZE + x] = height;
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
    slot->chunkZ = chunkZ;
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

// Вызывается только под эксклюзивным tableLock.
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
        uint32_t index = WorldHashChunkCoordinate(world->keys[i].x, world->keys[i].y, world->keys[i].z) & mask;
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

// Пересчитывает границу мира [0, worldSize) по одной оси в локальные
// координаты (относительно origin) с насыщением до диапазона int64.
static void ComputeLocalBounds(BigCoord worldSize, BigCoord origin, int64_t* outMin, int64_t* outMax)
{
    uint64_t low;
    BigCoord upper = BigCoordSub(worldSize, origin);   // worldSize >= origin
    *outMax = BigCoordFitsInt64(upper, &low) ? (int64_t)low : INT64_MAX;
    *outMin = BigCoordFitsInt64(origin, &low) ? -(int64_t)low : INT64_MIN;
}

World* WorldCreate(int64_t seed, BigCoord originX, BigCoord originZ, BigCoord worldSize)
{
    World* world = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*world));
    if (world == NULL)
    {
        return NULL;
    }

    InitializeSRWLock(&world->tableLock);
    InitializeSRWLock(&world->heightCacheLock);

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(ChunkCoordinate));
    world->chunks = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(Chunk*));
    world->occupied = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, world->capacity * sizeof(bool));

    bool heightCacheAllocated = true;
    for (uint32_t slot = 0; slot < HEIGHT_CACHE_SLOTS; ++slot)
    {
        world->heightCache[slot].heights = HeapAlloc(GetProcessHeap(), 0, HEIGHT_GRID_CELLS * sizeof(float));
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
    TerrainOriginInit(&world->terrainOrigin, originX, originZ);
    ComputeLocalBounds(worldSize, originX, &world->worldLocalMinX, &world->worldLocalMaxX);
    ComputeLocalBounds(worldSize, originZ, &world->worldLocalMinZ, &world->worldLocalMaxZ);
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

// Вызывается под tableLock (любым: shared для чтения, exclusive для записи).
static Chunk** WorldFindEntry(World* world, ChunkCoordinate key)
{
    if (world->count == 0)
    {
        return NULL;
    }

    uint32_t mask = world->capacity - 1;
    uint32_t index = WorldHashChunkCoordinate(key.x, key.y, key.z) & mask;

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
// Вызывается под эксклюзивным tableLock.
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
    uint32_t index = WorldHashChunkCoordinate(key.x, key.y, key.z) & mask;
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

// Вызывается под эксклюзивным tableLock.
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

    AcquireSRWLockShared(&world->tableLock);
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
            ReleaseSRWLockShared(&world->tableLock);
            return block;
        }
    }
    ReleaseSRWLockShared(&world->tableLock);

    return (float)y < TerrainHeight(world, x, z) ? BLOCK_EARTH : BLOCK_AIR;
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
    BlockType generated = (float)y < TerrainHeight(world, x, z) ? BLOCK_EARTH : BLOCK_AIR;

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
        if (chunk != NULL)
        {
            ChunkSetDelta(chunk, localIndex, block);
        }
    }
    ReleaseSRWLockExclusive(&world->tableLock);
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

    AcquireSRWLockShared(&world->tableLock);
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
    ReleaseSRWLockShared(&world->tableLock);

    // 2. Высоты колонн. Запрос мешера (регион 66x66 вокруг чанка)
    // идёт через кеш сеток высот; произвольные регионы считают шум сами.
    float* heights = HeapAlloc(GetProcessHeap(), 0, (size_t)sizeX * sizeZ * sizeof(float));
    float minimumHeight = 0.0f;
    float maximumHeight = 0.0f;
    bool boundsKnown = false;

    if (heights != NULL)
    {
        if (sizeX == HEIGHT_GRID_SIZE && sizeZ == HEIGHT_GRID_SIZE &&
            ((minBlockX + 1) & (CHUNK_SIZE - 1)) == 0 &&
            ((minBlockZ + 1) & (CHUNK_SIZE - 1)) == 0)
        {
            WorldObtainHeightGrid(world,
                (minBlockX + 1) >> CHUNK_SIZE_LOG2, (minBlockZ + 1) >> CHUNK_SIZE_LOG2,
                heights, &minimumHeight, &maximumHeight);
            boundsKnown = true;
        }
        else
        {
            bool first = true;
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                for (int32_t x = 0; x < sizeX; ++x)
                {
                    float height = TerrainHeight(world, minBlockX + x, minBlockZ + z);
                    heights[z * sizeX + x] = height;
                    if (first || height < minimumHeight) minimumHeight = height;
                    if (first || height > maximumHeight) maximumHeight = height;
                    first = false;
                }
            }
            boundsKnown = true;
        }
    }

    // 3. Однородный регион без дельт — данные не нужны вовсе.
    if (!regionHasDeltas && boundsKnown)
    {
        if ((float)minBlockY >= maximumHeight)
        {
            HeapFree(GetProcessHeap(), 0, heights);
            return WORLD_REGION_ALL_AIR;
        }
        if ((float)(minBlockY + sizeY - 1) < minimumHeight)
        {
            HeapFree(GetProcessHeap(), 0, heights);
            return WORLD_REGION_ALL_SOLID;
        }
    }

    // 4. Заполнение сгенерированным ландшафтом: граница «земля/воздух»
    // колонны — одно целое число, дальше два memset вместо
    // поблочных float-сравнений.
    for (int32_t z = 0; z < sizeZ; ++z)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            float height = heights != NULL
                ? heights[z * sizeX + x]
                : TerrainHeight(world, minBlockX + x, minBlockZ + z);

            int64_t solidCount = (int64_t)ColumnCeiling(height) - minBlockY;
            if (solidCount < 0) solidCount = 0;
            if (solidCount > sizeY) solidCount = sizeY;

            BlockType* column = &outBlocks[((size_t)z * sizeX + x) * sizeY];
            memset(column, BLOCK_EARTH, (size_t)solidCount);
            memset(column + solidCount, BLOCK_AIR, (size_t)(sizeY - solidCount));
        }
    }

    if (heights != NULL)
    {
        HeapFree(GetProcessHeap(), 0, heights);
    }

    // 5. Наложение дельт пересекающих чанков.
    if (regionHasDeltas)
    {
        AcquireSRWLockShared(&world->tableLock);
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
        ReleaseSRWLockShared(&world->tableLock);
    }

    return WORLD_REGION_MIXED;
}

int32_t WorldGetTerrainHeight(World* world, int64_t x, int64_t z)
{
    // Блок твёрдый при y < height, значит верхний твёрдый = ceil(height) - 1.
    return ColumnCeiling(TerrainHeight(world, x, z)) - 1;
}
