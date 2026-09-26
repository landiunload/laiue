#include "world/world.h"
#include "numeric/infinite_coord.h"
#include "world/numeric_provider.h"
#include "platform/system.h"

#include <limits.h>
#include <string.h>

#define InfiniteCoordInit WorldNumericInit
#define InfiniteCoordDestroy WorldNumericDestroy
#define InfiniteCoordTryCopyAddInt64 WorldNumericTryCopyAddInt64
#define InfiniteCoordEqualsOffsets WorldNumericEqualsOffsets
#define InfiniteCoordSwap WorldNumericSwap
#define InfiniteCoordHashOffset WorldNumericHashOffset
#define InfiniteCoordFormatShortOffsetW WorldNumericFormatShortOffsetW

/* Разбор региона читает буфер сравнением с нулём. SSE2 входит в базовый набор
 * x64, AVX2 включается профилем сборки. Там, где векторного сравнения нет
 * (например, ARM64), остаётся переносимый 8-байтный цикл. */
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
#include <emmintrin.h>
#endif

typedef struct LocalChunkCoordinate
{
    int64_t x;
    int64_t y;
    int64_t z;
} LocalChunkCoordinate;

typedef struct CoordinateFrame
{
    const LaiueNumericServiceV1* numeric;
    WorldAllocator allocator;
    InfiniteCoord chunkOrigin[3];
    uint32_t referenceCount;
} CoordinateFrame;

typedef struct GlobalChunkCoordinate
{
    uint64_t hash;
    CoordinateFrame* frame;
    LocalChunkCoordinate local;
} GlobalChunkCoordinate;

// 18 bits of block index followed by the application-owned material byte.
typedef uint32_t DeltaEntry;
#define DELTA_INDEX_BITS (CHUNK_SIZE_LOG2 * 3)
#define DELTA_INDEX_MASK ((1U << DELTA_INDEX_BITS) - 1U)

typedef struct Chunk
{
    uint32_t deltaCount;
    uint32_t deltaCapacity;
    DeltaEntry* deltas;
    WorldAllocator allocator;
} Chunk;

#define WORLD_INITIAL_CAPACITY 32U

struct World
{
    const LaiueNumericServiceV1* numeric;
    WorldAllocator allocator;
    PlatformRwLock tableLock;
    GlobalChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    /* Ненулевое значение означает, что в таблице есть хоть один чанк, а
     * значит возможна правка. Читается без блокировки в WorldGetBlock:
     * пока ноль, ни один чанк ещё не опубликован, и запрос можно сразу
     * отдать провайдеру. Поле только растёт, поэтому гонка с
     * WorldTrySetBlock безопасна: запись идёт под исключительным захватом
     * с release, а чтение — с acquire, и увиденный ноль гарантирует, что
     * правок ещё нет. */
    volatile uint32_t editedChunkCount;
    uint64_t revision;

    InfiniteCoord blockOrigin[3];
    InfiniteCoord chunkOrigin[3];
    CoordinateFrame* editFrame;
    WorldBaseProvider provider;
};

static WorldAllocator NormalizeAllocator(const WorldAllocator *allocator)
{
    if (allocator != NULL && allocator->allocate != NULL &&
        allocator->reallocate != NULL && allocator->free != NULL)
        return *allocator;
    return (WorldAllocator){0};
}

static bool WorldAllocatorIsComplete(const WorldAllocator *allocator)
{
    return allocator == NULL ||
           (allocator->allocate != NULL && allocator->reallocate != NULL &&
            allocator->free != NULL);
}

static void *WorldAllocateMemory(const WorldAllocator *allocator, size_t size, bool zero)
{
    void *memory = allocator != NULL && allocator->allocate != NULL
                       ? allocator->allocate(allocator->context, (uint64_t)size)
                       : PlatformAllocate(size, false);
    if (memory != NULL && zero)
        memset(memory, 0, size);
    return memory;
}

static void *WorldReallocateMemory(const WorldAllocator *allocator, void *memory,
                                   size_t size, bool zeroNewMemory)
{
    void *result = allocator != NULL && allocator->reallocate != NULL
                       ? allocator->reallocate(allocator->context, memory, (uint64_t)size)
                       : PlatformReallocate(memory, size, zeroNewMemory);
    if (result != NULL && zeroNewMemory && memory == NULL)
        memset(result, 0, size);
    return result;
}

static void WorldFreeMemory(const WorldAllocator *allocator, void *memory)
{
    if (memory == NULL)
        return;
    if (allocator != NULL && allocator->free != NULL)
        allocator->free(allocator->context, memory);
    else
        PlatformFree(memory);
}

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
    return (uint32_t)x * CHUNK_SIZE * CHUNK_SIZE
        + (uint32_t)y * CHUNK_SIZE + (uint32_t)z;
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
    return (value << amount) | (value >> (64U - amount));
}

static uint64_t HashLocalChunkCoordinate(
    const World* world, LocalChunkCoordinate coordinate)
{
    uint64_t x = WorldNumericHashOffsetWithService(
        world->numeric, &world->chunkOrigin[0], coordinate.x);
    uint64_t y = WorldNumericHashOffsetWithService(
        world->numeric, &world->chunkOrigin[1], coordinate.y);
    uint64_t z = WorldNumericHashOffsetWithService(
        world->numeric, &world->chunkOrigin[2], coordinate.z);
    return x ^ HashRotateLeft64(y, 21U) ^ HashRotateLeft64(z, 42U);
}

static void CoordinateFrameRelease(CoordinateFrame* frame)
{
    if (frame == NULL || --frame->referenceCount != 0U)
    {
        return;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericDestroyWithService(frame->numeric,
            &frame->chunkOrigin[axis]);
    }
    WorldFreeMemory(&frame->allocator, frame);
}

static CoordinateFrame* WorldGetEditFrame(World* world)
{
    if (world->editFrame != NULL)
    {
        return world->editFrame;
    }

    CoordinateFrame* frame =
        (CoordinateFrame *)WorldAllocateMemory(&world->allocator, sizeof(*frame), true);
    if (frame == NULL)
    {
        return NULL;
    }
    frame->referenceCount = 1U;
    frame->numeric = world->numeric;
    frame->allocator = world->allocator;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericInitWithService(frame->numeric, &frame->chunkOrigin[axis]);
        if (!WorldNumericTryCopyAddInt64WithService(
                frame->numeric,
                &frame->chunkOrigin[axis], &world->chunkOrigin[axis], 0))
        {
            CoordinateFrameRelease(frame);
            return NULL;
        }
    }
    world->editFrame = frame;
    return frame;
}

static bool GlobalChunkCoordinateMatchesLocal(
    const GlobalChunkCoordinate* global, const World* world,
    LocalChunkCoordinate local)
{
    if (global->frame == world->editFrame)
    {
        return global->local.x == local.x
            && global->local.y == local.y
            && global->local.z == local.z;
    }
    return WorldNumericEqualsOffsetsWithService(world->numeric,
            &global->frame->chunkOrigin[0], global->local.x,
            &world->chunkOrigin[0], local.x)
        && WorldNumericEqualsOffsetsWithService(world->numeric,
            &global->frame->chunkOrigin[1], global->local.y,
            &world->chunkOrigin[1], local.y)
        && WorldNumericEqualsOffsetsWithService(world->numeric,
            &global->frame->chunkOrigin[2], global->local.z,
            &world->chunkOrigin[2], local.z);
}

static void GlobalChunkCoordinateDestroy(GlobalChunkCoordinate* coordinate)
{
    CoordinateFrameRelease(coordinate->frame);
    coordinate->frame = NULL;
    coordinate->hash = 0U;
}

static bool GlobalChunkCoordinateTryCreate(
    GlobalChunkCoordinate* out, World* world, LocalChunkCoordinate local)
{
    CoordinateFrame* frame = WorldGetEditFrame(world);
    if (frame == NULL || frame->referenceCount == UINT32_MAX)
    {
        return false;
    }
    ++frame->referenceCount;
    out->hash = HashLocalChunkCoordinate(world, local);
    out->frame = frame;
    out->local = local;
    return true;
}

static void ChunkDestroy(Chunk* chunk)
{
    if (chunk == NULL)
    {
        return;
    }
    WorldFreeMemory(&chunk->allocator, chunk->deltas);
    WorldFreeMemory(&chunk->allocator, chunk);
}

static bool WorldGrow(World* world)
{
    if (world->capacity > UINT32_MAX / 2U)
    {
        return false;
    }
    uint32_t newCapacity = world->capacity * 2U;
    GlobalChunkCoordinate* newKeys = (GlobalChunkCoordinate *)WorldAllocateMemory(
        &world->allocator, (size_t)newCapacity * sizeof(*newKeys), true);
    Chunk** newChunks = (Chunk **)WorldAllocateMemory(
        &world->allocator, (size_t)newCapacity * sizeof(*newChunks), true);
    bool* newOccupied = (bool *)WorldAllocateMemory(
        &world->allocator, (size_t)newCapacity * sizeof(*newOccupied), true);
    if (newKeys == NULL || newChunks == NULL || newOccupied == NULL)
    {
        WorldFreeMemory(&world->allocator, newKeys);
        WorldFreeMemory(&world->allocator, newChunks);
        WorldFreeMemory(&world->allocator, newOccupied);
        return false;
    }

    for (uint32_t index = 0; index < world->capacity; ++index)
    {
        if (!world->occupied[index])
        {
            continue;
        }
        uint32_t slot = (uint32_t)(world->keys[index].hash
            ^ (world->keys[index].hash >> 32U)) & (newCapacity - 1U);
        while (newOccupied[slot])
        {
            slot = (slot + 1U) & (newCapacity - 1U);
        }
        newKeys[slot] = world->keys[index];
        newChunks[slot] = world->chunks[index];
        newOccupied[slot] = true;
    }

    WorldFreeMemory(&world->allocator, world->keys);
    WorldFreeMemory(&world->allocator, world->chunks);
    WorldFreeMemory(&world->allocator, world->occupied);
    world->keys = newKeys;
    world->chunks = newChunks;
    world->occupied = newOccupied;
    world->capacity = newCapacity;
    return true;
}

static Chunk** WorldFindEntry(World* world, LocalChunkCoordinate key)
{
    if (world->count == 0U)
    {
        return NULL;
    }
    uint64_t hash = HashLocalChunkCoordinate(world, key);
    uint32_t mask = world->capacity - 1U;
    uint32_t index = (uint32_t)(hash ^ (hash >> 32U)) & mask;
    for (uint32_t probe = 0; probe < world->capacity; ++probe)
    {
        if (!world->occupied[index])
        {
            return NULL;
        }
        if (world->keys[index].hash == hash
            && GlobalChunkCoordinateMatchesLocal(
                &world->keys[index], world, key))
        {
            return &world->chunks[index];
        }
        index = (index + 1U) & mask;
    }
    return NULL;
}

/* Публикует факт появления чанка с правкой. Вызывается под исключительным
 * захватом; release-запись гарантирует, что читатель, увидевший ненулевой
 * счётчик, пойдёт медленным путём и увидит сам чанк. */
static void WorldPublishEditedChunkCount(World* world)
{
    PlatformAtomicStoreU32Release(&world->editedChunkCount, world->count);
}

static Chunk* WorldGetOrCreateChunk(
    World* world, LocalChunkCoordinate coordinate)
{
    Chunk** entry = WorldFindEntry(world, coordinate);
    if (entry != NULL)
    {
        return *entry;
    }
    if (world->count * 2U >= world->capacity && !WorldGrow(world))
    {
        return NULL;
    }

    GlobalChunkCoordinate key;
    if (!GlobalChunkCoordinateTryCreate(&key, world, coordinate))
    {
        return NULL;
    }
    Chunk* chunk =
        (Chunk *)WorldAllocateMemory(&world->allocator, sizeof(*chunk), true);
    if (chunk == NULL)
    {
        GlobalChunkCoordinateDestroy(&key);
        return NULL;
    }
    chunk->allocator = world->allocator;

    uint32_t mask = world->capacity - 1U;
    uint32_t slot = (uint32_t)(key.hash ^ (key.hash >> 32U)) & mask;
    while (world->occupied[slot])
    {
        slot = (slot + 1U) & mask;
    }
    world->keys[slot] = key;
    world->chunks[slot] = chunk;
    world->occupied[slot] = true;
    ++world->count;
    WorldPublishEditedChunkCount(world);
    return chunk;
}

/* Убирает запись таблицы целиком, когда у чанка не осталось ни одной правки.
 * Без этого откат блока до базового значения оставлял бы в таблице пустой
 * чанк с буфером дельт на всю прежнюю пиковую ёмкость — навсегда, хотя
 * помнить там уже нечего. Удаление идёт сдвигом кластера (как EraseEntry в
 * стриминге), чтобы не заводить надгробий и не ломать чужие цепочки
 * пробирования. Вызывается под исключительным захватом. */
static void WorldEraseChunkAt(World* world, uint32_t index)
{
    ChunkDestroy(world->chunks[index]);
    GlobalChunkCoordinateDestroy(&world->keys[index]);

    const uint32_t mask = world->capacity - 1U;
    uint32_t hole = index;
    for (uint32_t scan = hole;;)
    {
        scan = (scan + 1U) & mask;
        if (!world->occupied[scan])
        {
            break;
        }

        const uint32_t home = (uint32_t)(world->keys[scan].hash
            ^ (world->keys[scan].hash >> 32U)) & mask;
        const uint32_t homeToHole = (hole - home) & mask;
        const uint32_t homeToScan = (scan - home) & mask;
        if (homeToHole <= homeToScan)
        {
            world->keys[hole] = world->keys[scan];
            world->chunks[hole] = world->chunks[scan];
            world->occupied[hole] = true;
            hole = scan;
        }
    }

    world->occupied[hole] = false;

    /* Копии ключей в освобождённых слотах — не отдельные ссылки на frame
     * (referenceCount не увеличивался при сдвиге), поэтому их не трогаем. */
    --world->count;
    WorldPublishEditedChunkCount(world);
}

static uint32_t ChunkDeltaLowerBound(
    const Chunk* chunk, uint32_t localIndex)
{
    uint32_t low = 0U;
    uint32_t high = chunk->deltaCount;
    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2U;
        if (DeltaLocalIndex(chunk->deltas[middle]) < localIndex)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

static bool ChunkGetDelta(
    const Chunk* chunk, uint32_t localIndex, BlockType* outBlock)
{
    uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
    if (position >= chunk->deltaCount
        || DeltaLocalIndex(chunk->deltas[position]) != localIndex)
    {
        return false;
    }
    *outBlock = DeltaBlock(chunk->deltas[position]);
    return true;
}

/* Сдвиг хвоста дельт на одну позицию вправо: [position, count) переезжает в
 * [position+1, count+1). Это горячая точка одиночной правки: на заселённом
 * чанке хвост — тысячи записей, и поэлементный цикл копирует одну запись за
 * итерацию. Копия идёт блоками от конца к началу: следующее чтение лежит
 * строго ниже уже записанного, поэтому перекрытие в одну запись безопасно.
 * Векторные ветки — тот же приём, что и в ClassifyRegion; там, где векторов
 * нет (например, ARM64), остаётся прежний переносимый цикл. */
static void ChunkShiftRight(
    DeltaEntry* deltas, uint32_t position, uint32_t count)
{
    uint32_t index = count;
#if defined(__AVX2__)
    while (index >= position + 8U)
    {
        __m256i tail = _mm256_loadu_si256(
            (const __m256i*)(const void*)(deltas + index - 8U));
        _mm256_storeu_si256((__m256i*)(void*)(deltas + index - 7U), tail);
        index -= 8U;
    }
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    while (index >= position + 4U)
    {
        __m128i tail = _mm_loadu_si128(
            (const __m128i*)(const void*)(deltas + index - 4U));
        _mm_storeu_si128((__m128i*)(void*)(deltas + index - 3U), tail);
        index -= 4U;
    }
#endif
    while (index > position)
    {
        deltas[index] = deltas[index - 1U];
        --index;
    }
}

/* Сдвиг хвоста на одну позицию влево: [position+1, count) переезжает в
 * [position, count-1). Идём от начала к концу: блок читается целиком до
 * записи, а следующий блок лежит выше уже записанного. */
static void ChunkShiftLeft(
    DeltaEntry* deltas, uint32_t position, uint32_t count)
{
    uint32_t index = position + 1U;
#if defined(__AVX2__)
    while (index + 8U <= count)
    {
        __m256i head = _mm256_loadu_si256(
            (const __m256i*)(const void*)(deltas + index));
        _mm256_storeu_si256((__m256i*)(void*)(deltas + index - 1U), head);
        index += 8U;
    }
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    while (index + 4U <= count)
    {
        __m128i head = _mm_loadu_si128(
            (const __m128i*)(const void*)(deltas + index));
        _mm_storeu_si128((__m128i*)(void*)(deltas + index - 1U), head);
        index += 4U;
    }
#endif
    while (index < count)
    {
        deltas[index - 1U] = deltas[index];
        ++index;
    }
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
    if (chunk->deltaCount == chunk->deltaCapacity)
    {
        uint32_t newCapacity = chunk->deltaCapacity < 4U
            ? 4U : chunk->deltaCapacity * 2U;
        if (newCapacity < chunk->deltaCapacity)
        {
            return false;
        }
        DeltaEntry* expanded = chunk->deltas == NULL
            ? (DeltaEntry *)WorldAllocateMemory(&chunk->allocator,
                (size_t)newCapacity * sizeof(*expanded), false)
            : (DeltaEntry *)WorldReallocateMemory(&chunk->allocator, chunk->deltas,
                (size_t)newCapacity * sizeof(*expanded), false);
        if (expanded == NULL)
        {
            return false;
        }
        chunk->deltas = expanded;
        chunk->deltaCapacity = newCapacity;
    }
    /* Сдвиг хвоста — единственная стоимость вставки на заселённом чанке,
     * поэтому копируется блоками, а не по одной записи. Порядок и
     * содержимое массива те же. */
    ChunkShiftRight(chunk->deltas, position, chunk->deltaCount);
    chunk->deltas[position] = PackDelta(localIndex, block);
    ++chunk->deltaCount;
    return true;
}

static bool ChunkRemoveDelta(Chunk* chunk, uint32_t localIndex)
{
    uint32_t position = ChunkDeltaLowerBound(chunk, localIndex);
    if (position >= chunk->deltaCount
        || DeltaLocalIndex(chunk->deltas[position]) != localIndex)
    {
        return false;
    }
    /* Сдвиг хвоста влево — такое же копирование, как при вставке, и та же
     * горячая точка одиночных правок. */
    ChunkShiftLeft(chunk->deltas, position, chunk->deltaCount);
    --chunk->deltaCount;
    return true;
}

static BlockType WorldBaseBlock(
    const World* world, int64_t x, int64_t y, int64_t z)
{
    return world->provider.getBlock == NULL
        ? BLOCK_AIR
        : world->provider.getBlock(world->provider.context, x, y, z);
}

static uint64_t SaturatingAddRevision(uint64_t value, uint32_t amount)
{
    uint64_t remaining = UINT64_MAX - value;
    return remaining < (uint64_t)amount
        ? UINT64_MAX : value + (uint64_t)amount;
}

World* WorldCreate(const WorldBaseProvider* provider)
{
    return WorldCreateWithNumericService(provider, WorldGetNumericService());
}

World* WorldCreateWithNumericService(const WorldBaseProvider* provider,
    const LaiueNumericServiceV1* numeric)
{
    return WorldCreateWithNumericServiceAndAllocator(provider, numeric, NULL);
}

World* WorldCreateWithNumericServiceAndAllocator(const WorldBaseProvider* provider,
    const LaiueNumericServiceV1* numeric, const WorldAllocator* allocator)
{
    /* Do not mix a user heap with the platform heap on later growth paths. */
    if (!WorldAllocatorIsComplete(allocator))
    {
        return NULL;
    }
    if (provider != NULL && provider->getBlock == NULL)
    {
        return NULL;
    }
    WorldAllocator resolvedAllocator = NormalizeAllocator(allocator);
    World* world = (World *)WorldAllocateMemory(&resolvedAllocator, sizeof(*world), true);
    if (world == NULL)
    {
        return NULL;
    }
    if (!PlatformRwLockInitialize(&world->tableLock))
    {
        WorldFreeMemory(&resolvedAllocator, world);
        return NULL;
    }
    world->numeric = numeric;
    world->allocator = resolvedAllocator;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericInitWithService(world->numeric, &world->blockOrigin[axis]);
        WorldNumericInitWithService(world->numeric, &world->chunkOrigin[axis]);
    }

    world->capacity = WORLD_INITIAL_CAPACITY;
    world->keys = (GlobalChunkCoordinate *)WorldAllocateMemory(
        &world->allocator, (size_t)world->capacity * sizeof(*world->keys), true);
    world->chunks = (Chunk **)WorldAllocateMemory(
        &world->allocator, (size_t)world->capacity * sizeof(*world->chunks), true);
    world->occupied = (bool *)WorldAllocateMemory(
        &world->allocator, (size_t)world->capacity * sizeof(*world->occupied), true);
    if (world->keys == NULL || world->chunks == NULL
        || world->occupied == NULL)
    {
        WorldDestroy(world);
        return NULL;
    }
    if (provider != NULL)
    {
        world->provider = *provider;
    }
    return world;
}

void WorldDestroy(World* world)
{
    if (world == NULL)
    {
        return;
    }
    if (world->occupied != NULL)
    {
        for (uint32_t index = 0; index < world->capacity; ++index)
        {
            if (!world->occupied[index])
            {
                continue;
            }
            GlobalChunkCoordinateDestroy(&world->keys[index]);
            if (world->chunks != NULL)
            {
                ChunkDestroy(world->chunks[index]);
            }
        }
    }
    CoordinateFrameRelease(world->editFrame);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericDestroyWithService(world->numeric,
            &world->blockOrigin[axis]);
        WorldNumericDestroyWithService(world->numeric,
            &world->chunkOrigin[axis]);
    }
    WorldFreeMemory(&world->allocator, world->keys);
    WorldFreeMemory(&world->allocator, world->chunks);
    WorldFreeMemory(&world->allocator, world->occupied);
    PlatformRwLockDestroy(&world->tableLock);
    WorldFreeMemory(&world->allocator, world);
}

bool WorldRebase(World* world,
    int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ)
{
    if (world == NULL)
    {
        return false;
    }
    int64_t shifts[3] = { blockShiftX, blockShiftY, blockShiftZ };
    InfiniteCoord newBlockOrigin[3];
    InfiniteCoord newChunkOrigin[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericInitWithService(world->numeric, &newBlockOrigin[axis]);
        WorldNumericInitWithService(world->numeric, &newChunkOrigin[axis]);
    }

    bool prepared = true;
    for (int32_t axis = 0; axis < 3 && prepared; ++axis)
    {
        prepared = shifts[axis] % CHUNK_SIZE == 0
            && WorldNumericTryCopyAddInt64WithService(
                world->numeric,
                &newBlockOrigin[axis], &world->blockOrigin[axis],
                shifts[axis])
            && WorldNumericTryCopyAddInt64WithService(
                world->numeric,
                &newChunkOrigin[axis], &world->chunkOrigin[axis],
                shifts[axis] / CHUNK_SIZE);
    }
    if (!prepared)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            WorldNumericDestroyWithService(world->numeric,
                &newBlockOrigin[axis]);
            WorldNumericDestroyWithService(world->numeric,
                &newChunkOrigin[axis]);
        }
        return false;
    }

    PlatformRwLockAcquireExclusive(&world->tableLock);
    bool providerAccepted = world->provider.rebase == NULL
        || world->provider.rebase(world->provider.context,
            blockShiftX, blockShiftY, blockShiftZ);
    if (providerAccepted)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            WorldNumericSwapWithService(world->numeric,
                &world->blockOrigin[axis], &newBlockOrigin[axis]);
            WorldNumericSwapWithService(world->numeric,
                &world->chunkOrigin[axis], &newChunkOrigin[axis]);
        }
        CoordinateFrameRelease(world->editFrame);
        world->editFrame = NULL;
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldNumericDestroyWithService(world->numeric,
            &newBlockOrigin[axis]);
        WorldNumericDestroyWithService(world->numeric,
            &newChunkOrigin[axis]);
    }
    return providerAccepted;
}

void WorldFormatAbsoluteBlockCoordinate(World* world,
    int32_t axis, int64_t localBlock, wchar_t* outText, uint32_t capacity)
{
    if (capacity == 0U)
    {
        return;
    }
    if (world == NULL || outText == NULL || axis < 0 || axis >= 3)
    {
        if (outText != NULL)
        {
            outText[0] = L'\0';
        }
        return;
    }
    PlatformRwLockAcquireShared(&world->tableLock);
    WorldNumericFormatShortOffsetWWithService(world->numeric,
        &world->blockOrigin[axis], localBlock, outText, capacity);
    PlatformRwLockReleaseShared(&world->tableLock);
}

bool WorldGetBlockState(World* world, int64_t x, int64_t y, int64_t z,
    BlockType* outBlock, bool* outExplicit)
{
    if (world == NULL || outBlock == NULL || outExplicit == NULL)
        return false;
    *outBlock = BLOCK_AIR;
    *outExplicit = false;
    /* Пока в таблице нет ни одного чанка, правок нет по определению, и
     * ответ даёт только провайдер. Тогда не нужны ни блокировка, ни
     * координаты, ни поиск в таблице: именно этот случай и есть основной
     * для физики в мире без правок. */
    if (PlatformAtomicLoadU32Acquire(&world->editedChunkCount) == 0U)
    {
        *outBlock = WorldBaseBlock(world, x, y, z);
        return true;
    }
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };
    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z));

    PlatformRwLockAcquireShared(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    BlockType block = BLOCK_AIR;
    bool overridden = entry != NULL
        && ChunkGetDelta(*entry, localIndex, &block);
    PlatformRwLockReleaseShared(&world->tableLock);
    *outExplicit = overridden;
    *outBlock = overridden ? block : WorldBaseBlock(world, x, y, z);
    return true;
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    BlockType block = BLOCK_AIR;
    bool explicitEdit = false;
    (void)WorldGetBlockState(world, x, y, z, &block, &explicitEdit);
    return block;
}

static bool WorldTrySetBlockInternal(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block, bool preserveBase)
{
    if (world == NULL)
    {
        return false;
    }
    LocalChunkCoordinate coordinate = {
        ChunkFromBlock(x), ChunkFromBlock(y), ChunkFromBlock(z)
    };
    uint32_t localIndex = PackLocalIndex(
        LocalFromBlock(x, coordinate.x),
        LocalFromBlock(y, coordinate.y),
        LocalFromBlock(z, coordinate.z));
    BlockType base = WorldBaseBlock(world, x, y, z);

    PlatformRwLockAcquireExclusive(&world->tableLock);
    Chunk** entry = WorldFindEntry(world, coordinate);
    BlockType current = base;
    if (entry != NULL)
    {
        (void)ChunkGetDelta(*entry, localIndex, &current);
    }
    if (current == block)
    {
        PlatformRwLockReleaseExclusive(&world->tableLock);
        return true;
    }

    bool succeeded;
    if (block == base && !preserveBase)
    {
        succeeded = entry != NULL && ChunkRemoveDelta(*entry, localIndex);
        if (succeeded && (*entry)->deltaCount == 0U)
        {
            WorldEraseChunkAt(world, (uint32_t)(entry - world->chunks));
        }
    }
    else
    {
        Chunk* chunk = WorldGetOrCreateChunk(world, coordinate);
        succeeded = chunk != NULL
            && ChunkSetDelta(chunk, localIndex, block);
    }
    if (succeeded)
    {
        world->revision = SaturatingAddRevision(world->revision, 1U);
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);
    return succeeded;
}

bool WorldTrySetBlock(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block)
{
    return WorldTrySetBlockInternal(world, x, y, z, block, false);
}

bool WorldTrySetBlockExplicit(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block)
{
    return WorldTrySetBlockInternal(world, x, y, z, block, true);
}

static bool WorldChunkLocalToBlock(int64_t chunk, uint32_t local, int64_t *outBlock)
{
    if (outBlock == NULL || local >= CHUNK_SIZE ||
        chunk > INT64_MAX / CHUNK_SIZE || chunk < INT64_MIN / CHUNK_SIZE)
        return false;
    int64_t block = chunk * CHUNK_SIZE;
    if (local != 0u && block > INT64_MAX - (int64_t)local)
        return false;
    if (local != 0u && block < INT64_MIN + (int64_t)local)
        return false;
    *outBlock = block + (int64_t)local;
    return true;
}

bool WorldEnumerateOverrides(World* world,
    int64_t minimumX, int64_t minimumY, int64_t minimumZ,
    int64_t maximumX, int64_t maximumY, int64_t maximumZ,
    WorldOverrideVisitor visitor, void* context)
{
    if (world == NULL || visitor == NULL || minimumX > maximumX ||
        minimumY > maximumY || minimumZ > maximumZ)
        return false;
    PlatformRwLockAcquireShared(&world->tableLock);
    for (uint32_t chunkIndex = 0u; chunkIndex < world->capacity; ++chunkIndex)
    {
        if (!world->occupied[chunkIndex])
            continue;
        const GlobalChunkCoordinate *key = &world->keys[chunkIndex];
        const Chunk *chunk = world->chunks[chunkIndex];
        for (uint32_t deltaIndex = 0u; deltaIndex < chunk->deltaCount; ++deltaIndex)
        {
            const uint32_t localIndex = DeltaLocalIndex(chunk->deltas[deltaIndex]);
            int64_t x = 0;
            int64_t y = 0;
            int64_t z = 0;
            if (!WorldChunkLocalToBlock(key->local.x,
                                        localIndex / (CHUNK_SIZE * CHUNK_SIZE), &x) ||
                !WorldChunkLocalToBlock(key->local.y,
                                        (localIndex / CHUNK_SIZE) % CHUNK_SIZE, &y) ||
                !WorldChunkLocalToBlock(key->local.z,
                                        localIndex % CHUNK_SIZE, &z))
                continue;
            if (x < minimumX || x > maximumX || y < minimumY || y > maximumY ||
                z < minimumZ || z > maximumZ)
                continue;
            if (!visitor(context, x, y, z, DeltaBlock(chunk->deltas[deltaIndex])))
            {
                PlatformRwLockReleaseShared(&world->tableLock);
                return false;
            }
        }
    }
    PlatformRwLockReleaseShared(&world->tableLock);
    return true;
}

void WorldSetBlock(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block)
{
    (void)WorldTrySetBlock(world, x, y, z, block);
}

typedef struct WorldBatchChunk
{
    LocalChunkCoordinate coordinate;
    Chunk* existing;
    WorldAllocator allocator;
    DeltaEntry* stagedDeltas;
    uint32_t stagedCount;
    uint32_t stagedCapacity;
    uint32_t changedCount;
    GlobalChunkCoordinate newKey;
    Chunk* newChunk;
    bool newKeyReady;
} WorldBatchChunk;

/* Слот открытой таблицы пакета. value == 0 — слот пуст, иначе хранит
 * значение 1..(число записей). Ключ — тройка int64: для таблицы дублей это
 * координата блока, для таблицы чанков — координата чанка. */
typedef struct WorldBatchProbe
{
    int64_t x;
    int64_t y;
    int64_t z;
    uint32_t value;
} WorldBatchProbe;

static uint64_t WorldBatchHash3(int64_t x, int64_t y, int64_t z)
{
    uint64_t hash = (uint64_t)x * UINT64_C(0x9E3779B97F4A7C15);
    hash ^= (uint64_t)y * UINT64_C(0xC2B2AE3D27D4EB4F);
    hash ^= (uint64_t)z * UINT64_C(0x165667B19E3779F9);
    hash ^= hash >> 29U;
    hash *= UINT64_C(0xBF58476D1CE4E5B9);
    hash ^= hash >> 32U;
    return hash;
}

/* Ищет точный ключ; при первом вхождении вставляет его со значением value и
 * возвращает false. Нагрузка не выше половины, поэтому пустой слот всегда
 * найдётся и цикл завершается. */
static bool WorldBatchProbeFindOrInsert(
    WorldBatchProbe* table, uint32_t mask, uint64_t hash,
    int64_t x, int64_t y, int64_t z, uint32_t value, uint32_t* outValue)
{
    uint32_t slot = (uint32_t)hash & mask;
    for (;;)
    {
        WorldBatchProbe* entry = &table[slot];
        if (entry->value == 0U)
        {
            entry->x = x;
            entry->y = y;
            entry->z = z;
            entry->value = value;
            *outValue = 0U;
            return false;
        }
        if (entry->x == x && entry->y == y && entry->z == z)
        {
            *outValue = entry->value;
            return true;
        }
        slot = (slot + 1U) & mask;
    }
}

/* Порог, с которого пакет обслуживается хеш-таблицами. Ниже него прежний
 * линейный поиск дешевле: пара таблиц на несколько мутаций стоит дороже
 * самого пакета. */
#define WORLD_BATCH_HASH_MIN 64U

static bool LocalChunkCoordinateEqual(
    LocalChunkCoordinate left, LocalChunkCoordinate right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

static void WorldBatchCleanup(World* world, WorldBatchChunk* chunks, uint32_t count)
{
    if (world == NULL || chunks == NULL)
    {
        return;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (chunks[index].newChunk != NULL)
        {
            WorldFreeMemory(&world->allocator, chunks[index].newChunk);
        }
        if (chunks[index].newKeyReady)
        {
            GlobalChunkCoordinateDestroy(&chunks[index].newKey);
        }
        WorldFreeMemory(&world->allocator, chunks[index].stagedDeltas);
    }
    WorldFreeMemory(&world->allocator, chunks);
}

static bool WorldBatchSetValue(WorldBatchChunk* batch,
    uint32_t localIndex, BlockType base, BlockType replacement)
{
    Chunk staged = {
        .deltaCount = batch->stagedCount,
        .deltaCapacity = batch->stagedCapacity,
        .deltas = batch->stagedDeltas,
        .allocator = batch->allocator,
    };
    bool succeeded = replacement == base
        ? (ChunkRemoveDelta(&staged, localIndex), true)
        : ChunkSetDelta(&staged, localIndex, replacement);
    if (!succeeded)
    {
        return false;
    }
    batch->stagedDeltas = staged.deltas;
    batch->stagedCount = staged.deltaCount;
    batch->stagedCapacity = staged.deltaCapacity;
    ++batch->changedCount;
    return true;
}

bool WorldApplyBlockBatch(World* world,
    const WorldBlockMutation* mutations, uint32_t count)
{
    if (world == NULL || (count != 0U && mutations == NULL)
        || count > WORLD_MAX_ATOMIC_BLOCK_MUTATIONS)
    {
        return false;
    }
    if (count == 0U)
    {
        return true;
    }

    /* Мелкий пакет выгоднее обслужить прежним линейным поиском: пара
     * хеш-таблиц на восемь мутаций стоит дороже самого пакета. Хеширование
     * включается там, где квадрат уже заметен. */
    const bool hashed = count >= WORLD_BATCH_HASH_MIN;
    uint32_t tableCapacity = 0U;
    uint32_t tableMask = 0U;
    WorldBatchProbe* duplicates = NULL;
    WorldBatchProbe* groups = NULL;
    if (hashed)
    {
        tableCapacity = 1U;
        while (tableCapacity < count * 2U)
        {
            tableCapacity <<= 1U;
        }
        tableMask = tableCapacity - 1U;
        duplicates = PlatformAllocate(
            (size_t)tableCapacity * sizeof(*duplicates), true);
        groups = PlatformAllocate(
            (size_t)tableCapacity * sizeof(*groups), true);
        if (duplicates == NULL || groups == NULL)
        {
            PlatformFree(duplicates);
            PlatformFree(groups);
            return false;
        }
    }

    WorldBatchChunk* chunks = (WorldBatchChunk *)WorldAllocateMemory(
        &world->allocator, (size_t)count * sizeof(*chunks), true);
    if (chunks == NULL)
    {
        PlatformFree(duplicates);
        PlatformFree(groups);
        return false;
    }

    /* Две мутации с одной координатой отвергают весь набор. В крупном
     * пакете — хеш-таблицей, а не попарным сравнением: у 4096 мутаций
     * прежний квадрат — восемь миллионов сравнений ещё до первой правки. */
    bool duplicateFound = false;
    if (hashed)
    {
        for (uint32_t index = 0; index < count; ++index)
        {
            const WorldBlockMutation* mutation = &mutations[index];
            uint32_t duplicate = 0U;
            if (WorldBatchProbeFindOrInsert(duplicates, tableMask,
                    WorldBatchHash3(mutation->block[0], mutation->block[1],
                        mutation->block[2]),
                    mutation->block[0], mutation->block[1],
                    mutation->block[2], 1U, &duplicate))
            {
                duplicateFound = true;
                break;
            }
        }
    }
    else
    {
        for (uint32_t index = 0; index < count && !duplicateFound; ++index)
        {
            for (uint32_t previous = 0; previous < index; ++previous)
            {
                if (mutations[index].block[0] == mutations[previous].block[0]
                    && mutations[index].block[1] == mutations[previous].block[1]
                    && mutations[index].block[2] == mutations[previous].block[2])
                {
                    duplicateFound = true;
                    break;
                }
            }
        }
    }
    if (duplicateFound)
    {
        PlatformFree(duplicates);
        PlatformFree(groups);
        WorldBatchCleanup(world, chunks, 0U);
        return false;
    }

    uint32_t chunkCount = 0U;
    uint32_t totalChanged = 0U;
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
        uint32_t groupValue = 0U;
        WorldBatchChunk* batch = NULL;
        if (hashed)
        {
            if (WorldBatchProbeFindOrInsert(groups, tableMask,
                    WorldBatchHash3(coordinate.x, coordinate.y, coordinate.z),
                    coordinate.x, coordinate.y, coordinate.z,
                    chunkCount + 1U, &groupValue))
            {
                batch = &chunks[groupValue - 1U];
            }
        }
        else
        {
            for (uint32_t candidate = 0U; candidate < chunkCount; ++candidate)
            {
                if (LocalChunkCoordinateEqual(
                        chunks[candidate].coordinate, coordinate))
                {
                    batch = &chunks[candidate];
                    break;
                }
            }
        }
        if (batch == NULL)
        {
            batch = &chunks[chunkCount++];
            batch->coordinate = coordinate;
            batch->allocator = world->allocator;
            Chunk** entry = WorldFindEntry(world, coordinate);
            batch->existing = entry == NULL ? NULL : *entry;
            uint32_t existingCount = batch->existing == NULL
                ? 0U : batch->existing->deltaCount;
            batch->stagedCount = existingCount;
            batch->stagedCapacity = existingCount;
            if (existingCount != 0U)
            {
                /* Резерв точный: только под существующие дельты. Новые
                 * добавки буфер добирает сам удвоением ёмкости, поэтому
                 * пакету из count мутаций на count разных чанков больше не
                 * нужен count-элементный запас в каждом чанке. */
                batch->stagedDeltas = (DeltaEntry *)WorldAllocateMemory(
                    &world->allocator, (size_t)existingCount * sizeof(DeltaEntry), false);
                if (batch->stagedDeltas == NULL)
                {
                    succeeded = false;
                    break;
                }
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
            .allocator = batch->allocator,
        };
        BlockType base = WorldBaseBlock(world,
            mutation->block[0], mutation->block[1], mutation->block[2]);
        BlockType current = base;
        (void)ChunkGetDelta(&stagedView, localIndex, &current);
        if (current != mutation->expected)
        {
            succeeded = false;
            break;
        }
        if (current == mutation->replacement)
        {
            continue;
        }
        succeeded = WorldBatchSetValue(
            batch, localIndex, base, mutation->replacement);
        if (succeeded)
        {
            ++totalChanged;
        }
    }

    uint32_t newChunkCount = 0U;
    for (uint32_t index = 0; index < chunkCount; ++index)
    {
        if (chunks[index].existing == NULL
            && chunks[index].stagedCount != 0U)
        {
            ++newChunkCount;
        }
    }
    while (succeeded
        && (world->count + newChunkCount) * 2U >= world->capacity)
    {
        succeeded = WorldGrow(world);
    }
    for (uint32_t index = 0; index < chunkCount && succeeded; ++index)
    {
        WorldBatchChunk* batch = &chunks[index];
        if (batch->existing != NULL || batch->stagedCount == 0U)
        {
            continue;
        }
        batch->newChunk = (Chunk *)WorldAllocateMemory(
            &world->allocator, sizeof(*batch->newChunk), true);
        if (batch->newChunk == NULL
            || !GlobalChunkCoordinateTryCreate(
                &batch->newKey, world, batch->coordinate))
        {
            succeeded = false;
            break;
        }
        batch->newChunk->allocator = world->allocator;
        batch->newKeyReady = true;
    }

    if (succeeded)
    {
        for (uint32_t index = 0; index < chunkCount; ++index)
        {
            WorldBatchChunk* batch = &chunks[index];
            if (batch->changedCount == 0U)
            {
                continue;
            }
            if (batch->existing != NULL)
            {
                if (batch->stagedCount == 0U)
                {
                    // Все правки чанка откачены: пустую запись убираем
                    // целиком, чтобы не держать ни буфер дельт, ни слот.
                    Chunk** slot = WorldFindEntry(world, batch->coordinate);
                    if (slot != NULL)
                    {
                        WorldEraseChunkAt(world,
                            (uint32_t)(slot - world->chunks));
                    }
                    continue;
                }
                WorldFreeMemory(&world->allocator, batch->existing->deltas);
                batch->existing->deltas = batch->stagedDeltas;
                batch->existing->deltaCount = batch->stagedCount;
                batch->existing->deltaCapacity = batch->stagedCapacity;
                batch->stagedDeltas = NULL;
                continue;
            }
            if (batch->stagedCount == 0U)
            {
                continue;
            }
            batch->newChunk->deltas = batch->stagedDeltas;
            batch->newChunk->deltaCount = batch->stagedCount;
            batch->newChunk->deltaCapacity = batch->stagedCapacity;
            uint32_t mask = world->capacity - 1U;
            uint32_t slot = (uint32_t)(batch->newKey.hash
                ^ (batch->newKey.hash >> 32U)) & mask;
            while (world->occupied[slot])
            {
                slot = (slot + 1U) & mask;
            }
            world->keys[slot] = batch->newKey;
            world->chunks[slot] = batch->newChunk;
            world->occupied[slot] = true;
            ++world->count;
            WorldPublishEditedChunkCount(world);
            batch->stagedDeltas = NULL;
            batch->newChunk = NULL;
            batch->newKeyReady = false;
        }
        world->revision = SaturatingAddRevision(
            world->revision, totalChanged);
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);
    PlatformFree(duplicates);
    PlatformFree(groups);
    WorldBatchCleanup(world, chunks, chunkCount);
    return succeeded;
}

uint64_t WorldGetRevision(World* world)
{
    if (world == NULL)
    {
        return 0U;
    }
    PlatformRwLockAcquireShared(&world->tableLock);
    uint64_t revision = world->revision;
    PlatformRwLockReleaseShared(&world->tableLock);
    return revision;
}

static bool RegionValid(int64_t minBlockX, int64_t minBlockY,
    int64_t minBlockZ, int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    size_t* outCellCount)
{
    if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0
        || minBlockX > INT64_MAX - ((int64_t)sizeX - 1)
        || minBlockY > INT64_MAX - ((int64_t)sizeY - 1)
        || minBlockZ > INT64_MAX - ((int64_t)sizeZ - 1))
    {
        return false;
    }
    size_t cells = (size_t)sizeX;
    if (cells > SIZE_MAX / (size_t)sizeY)
    {
        return false;
    }
    cells *= (size_t)sizeY;
    if (cells > SIZE_MAX / (size_t)sizeZ)
    {
        return false;
    }
    *outCellCount = cells * (size_t)sizeZ;
    return true;
}

static size_t RegionIndex(int64_t x, int64_t y, int64_t z,
    int64_t minX, int64_t minY, int64_t minZ,
    int32_t sizeX, int32_t sizeZ)
{
    return (((size_t)(y - minY) * (size_t)sizeX)
        + (size_t)(x - minX)) * (size_t)sizeZ
        + (size_t)(z - minZ);
}

/* Разбор региона ищет в буфере пустые и непустые ячейки. Смешанный регион
 * находит оба признака почти сразу и выходит, а вот однородный обязан
 * прочитать всё: именно он и стоит дорого, потому что чанк без единого блока
 * над поверхностью — самый частый в мире. Поэтому сравнение с нулём идёт
 * векторами по 32 (AVX2) или 16 (SSE2) байт, и только хвост — словом.
 * BLOCK_AIR равен нулю, поэтому «есть непустая ячейка» — это ненулевой
 * вектор, а «есть пустая» — ненулевая маска совпадений с нулём. Там, где
 * векторов нет (например, ARM64), остаётся прежний приём поиска нулевого
 * байта в 8-байтном слове. */
#if !(defined(__AVX2__) || defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))))
static inline bool WordHasZeroByte(uint64_t word)
{
    return ((word - UINT64_C(0x0101010101010101))
        & ~word & UINT64_C(0x8080808080808080)) != 0;
}
#endif

static WorldRegionContents ClassifyRegion(const BlockType* blocks, size_t cellCount)
{
    bool anyAir = false;
    bool anySolid = false;
    size_t index = 0;

#if defined(__AVX2__)
    const __m256i airVector = _mm256_setzero_si256();
    /* Маски совпадений с нулём копятся по четыре вектора сразу. «Есть пустая
     * ячейка» — это ненулевая дизъюнкция масок, «есть непустая» — не все
     * единицы в их конъюнкции. Ветка выхода на смешанном регионе остаётся
     * одна на 128 байт вместо одной на 32, а разбор упирается в число
     * инструкций на байт: SSE2 и AVX2 удешевили его вдвое каждый, и этот шаг
     * продолжает тот же ряд. Ответ тот же: смешанный регион по-прежнему
     * выходит сразу, однородный обязан прочитать всё. */
    uint32_t airAny = 0U;
    uint32_t airAll = 0xFFFFFFFFU;
    for (; index + 128U <= cellCount; index += 128U)
    {
        __m256i voxels0 = _mm256_loadu_si256(
            (const __m256i*)(const void*)(blocks + index));
        __m256i voxels1 = _mm256_loadu_si256(
            (const __m256i*)(const void*)(blocks + index + 32U));
        __m256i voxels2 = _mm256_loadu_si256(
            (const __m256i*)(const void*)(blocks + index + 64U));
        __m256i voxels3 = _mm256_loadu_si256(
            (const __m256i*)(const void*)(blocks + index + 96U));
        uint32_t bits0 = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(voxels0, airVector));
        uint32_t bits1 = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(voxels1, airVector));
        uint32_t bits2 = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(voxels2, airVector));
        uint32_t bits3 = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(voxels3, airVector));
        airAny |= bits0 | bits1 | bits2 | bits3;
        airAll &= bits0 & bits1 & bits2 & bits3;
        if (airAny != 0U && airAll != 0xFFFFFFFFU)
        {
            return WORLD_REGION_MIXED;
        }
    }
    for (; index + 32U <= cellCount; index += 32U)
    {
        __m256i voxels = _mm256_loadu_si256(
            (const __m256i*)(const void*)(blocks + index));
        uint32_t airBits = (uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(voxels, airVector));
        airAny |= airBits;
        airAll &= airBits;
        if (airAny != 0U && airAll != 0xFFFFFFFFU)
        {
            return WORLD_REGION_MIXED;
        }
    }
    anyAir = airAny != 0U;
    anySolid = airAll != 0xFFFFFFFFU;
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    const __m128i airVector = _mm_setzero_si128();
    for (; index + 16U <= cellCount; index += 16U)
    {
        __m128i voxels = _mm_loadu_si128(
            (const __m128i*)(const void*)(blocks + index));
        uint32_t airBits = (uint32_t)_mm_movemask_epi8(
            _mm_cmpeq_epi8(voxels, airVector));
        anyAir |= airBits != 0U;
        anySolid |= airBits != 0xFFFFU;
        if (anyAir && anySolid)
        {
            return WORLD_REGION_MIXED;
        }
    }
#else
    /* Копирование в слово, а не приведение указателя: выравнивание массива
     * задаёт вызывающая сторона, а memcpy этой длины компилятор превращает
     * в обычную загрузку. */
    for (; index + sizeof(uint64_t) <= cellCount; index += sizeof(uint64_t))
    {
        uint64_t word;
        memcpy(&word, blocks + index, sizeof(word));
        anySolid |= word != 0;
        anyAir |= WordHasZeroByte(word);
        if (anyAir && anySolid)
        {
            return WORLD_REGION_MIXED;
        }
    }
#endif
    for (; index < cellCount; ++index)
    {
        anyAir |= blocks[index] == BLOCK_AIR;
        anySolid |= blocks[index] != BLOCK_AIR;
        if (anyAir && anySolid)
        {
            return WORLD_REGION_MIXED;
        }
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}


/* Кусок чанка, попавший в регион, в его собственных координатах. Оси идут в
 * том же порядке, в каком они уложены в локальный индекс: x старшая, z
 * младшая. Поэтому набор нужных индексов — это отрезок по x, внутри него
 * отрезок по y и внутри того отрезок по z. */
typedef struct ChunkClip
{
    uint32_t low[3];
    uint32_t high[3];
    bool whole;
} ChunkClip;

static uint32_t ClipLocalIndex(const uint32_t coordinate[3])
{
    return coordinate[0] * CHUNK_SIZE * CHUNK_SIZE + coordinate[1] * CHUNK_SIZE + coordinate[2];
}

/* Наименьший локальный индекс не меньше данного, который лежит внутри куска.
 * Если такого нет, кусок кончился. Обычный перенос разряда: ось, вышедшая за
 * верхнюю границу, обнуляется до нижней, а старшая соседняя увеличивается. */
static bool ClipNextIndex(uint32_t index, const ChunkClip* clip, uint32_t* outIndex)
{
    uint32_t coordinate[3] = {
        index / (CHUNK_SIZE * CHUNK_SIZE),
        (index / CHUNK_SIZE) % CHUNK_SIZE,
        index % CHUNK_SIZE
    };
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (coordinate[axis] < clip->low[axis])
        {
            coordinate[axis] = clip->low[axis];
            for (uint32_t lower = axis + 1U; lower < 3U; ++lower)
            {
                coordinate[lower] = clip->low[lower];
            }
            *outIndex = ClipLocalIndex(coordinate);
            return true;
        }
        if (coordinate[axis] > clip->high[axis])
        {
            uint32_t carry = axis;
            while (carry != 0U)
            {
                --carry;
                if (coordinate[carry] < clip->high[carry])
                {
                    ++coordinate[carry];
                    for (uint32_t lower = carry + 1U; lower < 3U; ++lower)
                    {
                        coordinate[lower] = clip->low[lower];
                    }
                    *outIndex = ClipLocalIndex(coordinate);
                    return true;
                }
            }
            return false;
        }
    }
    *outIndex = index;
    return true;
}

/* Первая дельта не раньше cursor, чей индекс не меньше target. Скачки идут
 * удвоением, а не сразу двоичным поиском по всему массиву: соседняя нужная
 * дельта обычно рядом, и тогда хватает пары шагов. */
static uint32_t ChunkDeltaAdvance(const Chunk* chunk, uint32_t cursor, uint32_t target)
{
    uint32_t count = chunk->deltaCount;
    if (cursor >= count || DeltaLocalIndex(chunk->deltas[cursor]) >= target)
    {
        return cursor;
    }
    uint32_t low = cursor;
    uint32_t step = 1U;
    uint32_t high = cursor + 1U;
    while (high < count && DeltaLocalIndex(chunk->deltas[high]) < target)
    {
        low = high;
        step <<= 1;
        high = cursor + step;
        if (high > count || high < cursor)
        {
            high = count;
        }
    }
    if (high > count)
    {
        high = count;
    }
    uint32_t left = low + 1U;
    uint32_t right = high;
    while (left < right)
    {
        uint32_t middle = left + (right - left) / 2U;
        if (DeltaLocalIndex(chunk->deltas[middle]) < target)
        {
            left = middle + 1U;
        }
        else
        {
            right = middle;
        }
    }
    return left;
}

WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks)
{
    size_t cellCount = 0U;
    if (world == NULL || outBlocks == NULL
        || !RegionValid(minBlockX, minBlockY, minBlockZ,
            sizeX, sizeY, sizeZ, &cellCount))
    {
        return WORLD_REGION_ALL_AIR;
    }

    if (world->provider.fillRegion != NULL)
    {
        (void)world->provider.fillRegion(world->provider.context,
            minBlockX, minBlockY, minBlockZ,
            sizeX, sizeY, sizeZ, outBlocks);
    }
    else if (world->provider.getBlock != NULL)
    {
        for (int32_t y = 0; y < sizeY; ++y)
        {
            for (int32_t x = 0; x < sizeX; ++x)
            {
                for (int32_t z = 0; z < sizeZ; ++z)
                {
                    size_t outputIndex = (((size_t)y * (size_t)sizeX)
                        + (size_t)x) * (size_t)sizeZ + (size_t)z;
                    outBlocks[outputIndex] = WorldBaseBlock(world,
                        minBlockX + x, minBlockY + y, minBlockZ + z);
                }
            }
        }
    }
    else
    {
        memset(outBlocks, BLOCK_AIR, cellCount * sizeof(*outBlocks));
    }

    /* Пока в таблице нет ни одного чанка, правок нет по определению, и обход
     * чанков региона не нашёл бы ни одной дельты. Тогда не нужны ни
     * разделяемый захват, ни поиск в таблице. Это ровно тот же случай, что и
     * быстрый путь WorldGetBlock, и он так же част: мир без правок — основной
     * для чтения, а provider для него единственный источник ответа.
     *
     * Если при этом базовый слой пуст (провайдера нет), то весь регион —
     * воздух, и разбор содержимого тоже не нужен: буфер только что заполнен
     * нулями, а дельт, которые могли бы его изменить, в таблице нет. */
    if (PlatformAtomicLoadU32Acquire(&world->editedChunkCount) == 0U)
    {
        if (world->provider.getBlock == NULL)
        {
            return WORLD_REGION_ALL_AIR;
        }
        return ClassifyRegion(outBlocks, cellCount);
    }

    // RegionValid bounds the inclusive end, not min + size (which can overflow).
    int64_t maxBlockX = minBlockX + ((int64_t)sizeX - 1);
    int64_t maxBlockY = minBlockY + ((int64_t)sizeY - 1);
    int64_t maxBlockZ = minBlockZ + ((int64_t)sizeZ - 1);
    int64_t minChunkX = ChunkFromBlock(minBlockX);
    int64_t minChunkY = ChunkFromBlock(minBlockY);
    int64_t minChunkZ = ChunkFromBlock(minBlockZ);
    int64_t maxChunkX = ChunkFromBlock(maxBlockX);
    int64_t maxChunkY = ChunkFromBlock(maxBlockY);
    int64_t maxChunkZ = ChunkFromBlock(maxBlockZ);

    PlatformRwLockAcquireShared(&world->tableLock);
    for (int64_t chunkY = minChunkY;; ++chunkY)
    {
        for (int64_t chunkX = minChunkX;; ++chunkX)
        {
            for (int64_t chunkZ = minChunkZ;; ++chunkZ)
            {
                LocalChunkCoordinate coordinate = {
                    chunkX, chunkY, chunkZ
                };
                Chunk** entry = WorldFindEntry(world, coordinate);
                if (entry != NULL)
                {
                    const Chunk* chunk = *entry;
                    /* Регион почти всегда захватывает от соседнего чанка один
                     * слой блоков, а дельты в чанке лежат по возрастанию
                     * локального индекса. Раньше слой искали перебором всего
                     * массива: у заселённого соседа это сотня тысяч записей
                     * ради нескольких тысяч нужных. Теперь по массиву идут
                     * скачками, пропуская всё, что заведомо вне куска. */
                    const int64_t chunkBase[3] = {
                        chunkX * CHUNK_SIZE, chunkY * CHUNK_SIZE, chunkZ * CHUNK_SIZE
                    };
                    const int64_t regionMinimum[3] = { minBlockX, minBlockY, minBlockZ };
                    const int64_t regionMaximum[3] = { maxBlockX, maxBlockY, maxBlockZ };
                    ChunkClip clip;
                    clip.whole = true;
                    for (uint32_t axis = 0; axis < 3U; ++axis)
                    {
                        int64_t low = regionMinimum[axis] - chunkBase[axis];
                        int64_t high = regionMaximum[axis] - chunkBase[axis];
                        if (low < 0)
                        {
                            low = 0;
                        }
                        if (high > CHUNK_SIZE - 1)
                        {
                            high = CHUNK_SIZE - 1;
                        }
                        clip.low[axis] = (uint32_t)low;
                        clip.high[axis] = (uint32_t)high;
                        clip.whole = clip.whole && low == 0 && high == CHUNK_SIZE - 1;
                    }

                    /* Чанк целиком внутри региона — обычный случай для того
                     * чанка, ради которого регион и берут. Тогда проверять
                     * нечего, и цикл идёт без единой ветки.
                     *
                     * Дельты отсортированы по локальному индексу, а младшие
                     * шесть его бит — это z. Значит подряд идущие индексы
                     * внутри одной колонны ложатся в регион тоже подряд:
                     * место считается один раз на отрезок, а внутри него
                     * хватает наращивания. У заселённого чанка почти вся
                     * работа приходится на такие отрезки. */
                    const int64_t strideY = (int64_t)sizeX * (int64_t)sizeZ;
                    const int64_t strideX = (int64_t)sizeZ;
                    const int64_t regionBase =
                        (chunkBase[1] - minBlockY) * strideY +
                        (chunkBase[0] - minBlockX) * strideX + (chunkBase[2] - minBlockZ);
                    if (clip.whole)
                    {
                        uint32_t delta = 0;
                        while (delta < chunk->deltaCount)
                        {
                            uint32_t localIndex = DeltaLocalIndex(chunk->deltas[delta]);
                            uint32_t columnEnd = localIndex | (CHUNK_SIZE - 1U);
                            int64_t place = regionBase +
                                (int64_t)(localIndex / (CHUNK_SIZE * CHUNK_SIZE)) * strideX +
                                (int64_t)((localIndex / CHUNK_SIZE) % CHUNK_SIZE) * strideY +
                                (int64_t)(localIndex % CHUNK_SIZE);
                            outBlocks[place] = DeltaBlock(chunk->deltas[delta]);
                            ++delta;
                            ++place;
                            ++localIndex;
                            while (delta < chunk->deltaCount && localIndex <= columnEnd &&
                                   DeltaLocalIndex(chunk->deltas[delta]) == localIndex)
                            {
                                outBlocks[place] = DeltaBlock(chunk->deltas[delta]);
                                ++delta;
                                ++place;
                                ++localIndex;
                            }
                        }
                    }
                    else
                    {
                        uint32_t last = ClipLocalIndex(clip.high);
                        uint32_t cursor =
                            ChunkDeltaLowerBound(chunk, ClipLocalIndex(clip.low));
                        while (cursor < chunk->deltaCount)
                        {
                            uint32_t localIndex = DeltaLocalIndex(chunk->deltas[cursor]);
                            if (localIndex > last)
                            {
                                break;
                            }
                            uint32_t target;
                            if (!ClipNextIndex(localIndex, &clip, &target))
                            {
                                break;
                            }
                            if (target != localIndex)
                            {
                                cursor = ChunkDeltaAdvance(chunk, cursor + 1U, target);
                                continue;
                            }
                            int64_t localX = (int64_t)(localIndex / (CHUNK_SIZE * CHUNK_SIZE));
                            uint32_t remainder = localIndex % (CHUNK_SIZE * CHUNK_SIZE);
                            int64_t localY = (int64_t)(remainder / CHUNK_SIZE);
                            int64_t localZ = (int64_t)(remainder % CHUNK_SIZE);
                            outBlocks[RegionIndex(chunkBase[0] + localX, chunkBase[1] + localY,
                                chunkBase[2] + localZ, minBlockX, minBlockY, minBlockZ,
                                sizeX, sizeZ)] = DeltaBlock(chunk->deltas[cursor]);
                            ++cursor;
                        }
                    }
                }
                if (chunkZ == maxChunkZ) break;
            }
            if (chunkX == maxChunkX) break;
        }
        if (chunkY == maxChunkY) break;
    }
    PlatformRwLockReleaseShared(&world->tableLock);

    return ClassifyRegion(outBlocks, cellCount);
}
