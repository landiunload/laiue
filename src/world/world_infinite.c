#include "world/world.h"
#include "world/infinite_coord.h"
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

// 18 bits of block index followed by the application-owned material byte.
typedef uint32_t DeltaEntry;
#define DELTA_INDEX_BITS (CHUNK_SIZE_LOG2 * 3)
#define DELTA_INDEX_MASK ((1U << DELTA_INDEX_BITS) - 1U)

typedef struct Chunk
{
    uint32_t deltaCount;
    uint32_t deltaCapacity;
    DeltaEntry* deltas;
} Chunk;

#define WORLD_INITIAL_CAPACITY 32U

struct World
{
    PlatformRwLock tableLock;
    GlobalChunkCoordinate* keys;
    Chunk** chunks;
    bool* occupied;
    uint32_t count;
    uint32_t capacity;
    uint64_t revision;

    InfiniteCoord blockOrigin[3];
    InfiniteCoord chunkOrigin[3];
    CoordinateFrame* editFrame;
    WorldBaseProvider provider;
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
    uint64_t x = InfiniteCoordHashOffset(
        &world->chunkOrigin[0], coordinate.x);
    uint64_t y = InfiniteCoordHashOffset(
        &world->chunkOrigin[1], coordinate.y);
    uint64_t z = InfiniteCoordHashOffset(
        &world->chunkOrigin[2], coordinate.z);
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
        InfiniteCoordDestroy(&frame->chunkOrigin[axis]);
    }
    PlatformFree(frame);
}

static CoordinateFrame* WorldGetEditFrame(World* world)
{
    if (world->editFrame != NULL)
    {
        return world->editFrame;
    }

    CoordinateFrame* frame = PlatformAllocate(sizeof(*frame), true);
    if (frame == NULL)
    {
        return NULL;
    }
    frame->referenceCount = 1U;
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
    PlatformFree(chunk->deltas);
    PlatformFree(chunk);
}

static bool WorldGrow(World* world)
{
    if (world->capacity > UINT32_MAX / 2U)
    {
        return false;
    }
    uint32_t newCapacity = world->capacity * 2U;
    GlobalChunkCoordinate* newKeys = PlatformAllocate(
        (size_t)newCapacity * sizeof(*newKeys), true);
    Chunk** newChunks = PlatformAllocate(
        (size_t)newCapacity * sizeof(*newChunks), true);
    bool* newOccupied = PlatformAllocate(
        (size_t)newCapacity * sizeof(*newOccupied), true);
    if (newKeys == NULL || newChunks == NULL || newOccupied == NULL)
    {
        PlatformFree(newKeys);
        PlatformFree(newChunks);
        PlatformFree(newOccupied);
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

    PlatformFree(world->keys);
    PlatformFree(world->chunks);
    PlatformFree(world->occupied);
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
    Chunk* chunk = PlatformAllocate(sizeof(*chunk), true);
    if (chunk == NULL)
    {
        GlobalChunkCoordinateDestroy(&key);
        return NULL;
    }

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
    return chunk;
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
            ? PlatformAllocate((size_t)newCapacity * sizeof(*expanded), false)
            : PlatformReallocate(chunk->deltas,
                (size_t)newCapacity * sizeof(*expanded), false);
        if (expanded == NULL)
        {
            return false;
        }
        chunk->deltas = expanded;
        chunk->deltaCapacity = newCapacity;
    }
    for (uint32_t index = chunk->deltaCount; index > position; --index)
    {
        chunk->deltas[index] = chunk->deltas[index - 1U];
    }
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
    for (uint32_t index = position + 1U;
         index < chunk->deltaCount; ++index)
    {
        chunk->deltas[index - 1U] = chunk->deltas[index];
    }
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
    if (provider != NULL && provider->getBlock == NULL)
    {
        return NULL;
    }
    World* world = PlatformAllocate(sizeof(*world), true);
    if (world == NULL)
    {
        return NULL;
    }
    if (!PlatformRwLockInitialize(&world->tableLock))
    {
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
        (size_t)world->capacity * sizeof(*world->keys), true);
    world->chunks = PlatformAllocate(
        (size_t)world->capacity * sizeof(*world->chunks), true);
    world->occupied = PlatformAllocate(
        (size_t)world->capacity * sizeof(*world->occupied), true);
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
        InfiniteCoordDestroy(&world->blockOrigin[axis]);
        InfiniteCoordDestroy(&world->chunkOrigin[axis]);
    }
    PlatformFree(world->keys);
    PlatformFree(world->chunks);
    PlatformFree(world->occupied);
    PlatformRwLockDestroy(&world->tableLock);
    PlatformFree(world);
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
        InfiniteCoordInit(&newBlockOrigin[axis]);
        InfiniteCoordInit(&newChunkOrigin[axis]);
    }

    bool prepared = true;
    for (int32_t axis = 0; axis < 3 && prepared; ++axis)
    {
        prepared = shifts[axis] % CHUNK_SIZE == 0
            && InfiniteCoordTryCopyAddInt64(
                &newBlockOrigin[axis], &world->blockOrigin[axis],
                shifts[axis])
            && InfiniteCoordTryCopyAddInt64(
                &newChunkOrigin[axis], &world->chunkOrigin[axis],
                shifts[axis] / CHUNK_SIZE);
    }
    if (!prepared)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            InfiniteCoordDestroy(&newBlockOrigin[axis]);
            InfiniteCoordDestroy(&newChunkOrigin[axis]);
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
            InfiniteCoordSwap(
                &world->blockOrigin[axis], &newBlockOrigin[axis]);
            InfiniteCoordSwap(
                &world->chunkOrigin[axis], &newChunkOrigin[axis]);
        }
        CoordinateFrameRelease(world->editFrame);
        world->editFrame = NULL;
    }
    PlatformRwLockReleaseExclusive(&world->tableLock);

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&newBlockOrigin[axis]);
        InfiniteCoordDestroy(&newChunkOrigin[axis]);
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
    InfiniteCoordFormatShortOffsetW(
        &world->blockOrigin[axis], localBlock, outText, capacity);
    PlatformRwLockReleaseShared(&world->tableLock);
}

BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z)
{
    if (world == NULL)
    {
        return BLOCK_AIR;
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
    return overridden ? block : WorldBaseBlock(world, x, y, z);
}

bool WorldTrySetBlock(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block)
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
    if (block == base)
    {
        succeeded = entry != NULL && ChunkRemoveDelta(*entry, localIndex);
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

void WorldSetBlock(World* world,
    int64_t x, int64_t y, int64_t z, BlockType block)
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
    if (chunks == NULL)
    {
        return;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (chunks[index].newChunk != NULL)
        {
            PlatformFree(chunks[index].newChunk);
        }
        if (chunks[index].newKeyReady)
        {
            GlobalChunkCoordinateDestroy(&chunks[index].newKey);
        }
        PlatformFree(chunks[index].stagedDeltas);
    }
    PlatformFree(chunks);
}

static bool WorldBatchSetValue(WorldBatchChunk* batch,
    uint32_t localIndex, BlockType base, BlockType replacement)
{
    Chunk staged = {
        .deltaCount = batch->stagedCount,
        .deltaCapacity = batch->stagedCapacity,
        .deltas = batch->stagedDeltas,
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
    for (uint32_t index = 0; index < count; ++index)
    {
        for (uint32_t previous = 0; previous < index; ++previous)
        {
            if (mutations[index].block[0] == mutations[previous].block[0]
                && mutations[index].block[1] == mutations[previous].block[1]
                && mutations[index].block[2] == mutations[previous].block[2])
            {
                return false;
            }
        }
    }

    WorldBatchChunk* chunks = PlatformAllocate(
        (size_t)count * sizeof(*chunks), true);
    if (chunks == NULL)
    {
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
        WorldBatchChunk* batch = NULL;
        for (uint32_t candidate = 0; candidate < chunkCount; ++candidate)
        {
            if (LocalChunkCoordinateEqual(
                    chunks[candidate].coordinate, coordinate))
            {
                batch = &chunks[candidate];
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
            batch->stagedDeltas = PlatformAllocate(
                (size_t)batch->stagedCapacity * sizeof(DeltaEntry), false);
            if (batch->stagedDeltas == NULL)
            {
                succeeded = false;
                break;
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
        batch->newChunk = PlatformAllocate(sizeof(*batch->newChunk), true);
        if (batch->newChunk == NULL
            || !GlobalChunkCoordinateTryCreate(
                &batch->newKey, world, batch->coordinate))
        {
            succeeded = false;
            break;
        }
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
                PlatformFree(batch->existing->deltas);
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

    int64_t maxBlockX = minBlockX + sizeX - 1;
    int64_t maxBlockY = minBlockY + sizeY - 1;
    int64_t maxBlockZ = minBlockZ + sizeZ - 1;
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
                    for (uint32_t delta = 0;
                         delta < chunk->deltaCount; ++delta)
                    {
                        uint32_t localIndex =
                            DeltaLocalIndex(chunk->deltas[delta]);
                        int64_t localX = (int64_t)(localIndex
                            / (CHUNK_SIZE * CHUNK_SIZE));
                        uint32_t remainder = localIndex
                            % (CHUNK_SIZE * CHUNK_SIZE);
                        int64_t localY = (int64_t)(remainder / CHUNK_SIZE);
                        int64_t localZ = (int64_t)(remainder % CHUNK_SIZE);
                        int64_t blockX = chunkX * CHUNK_SIZE + localX;
                        int64_t blockY = chunkY * CHUNK_SIZE + localY;
                        int64_t blockZ = chunkZ * CHUNK_SIZE + localZ;
                        if (blockX >= minBlockX && blockX <= maxBlockX
                            && blockY >= minBlockY && blockY <= maxBlockY
                            && blockZ >= minBlockZ && blockZ <= maxBlockZ)
                        {
                            outBlocks[RegionIndex(blockX, blockY, blockZ,
                                minBlockX, minBlockY, minBlockZ,
                                sizeX, sizeZ)] =
                                DeltaBlock(chunk->deltas[delta]);
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

    bool anyAir = false;
    bool anySolid = false;
    for (size_t index = 0; index < cellCount; ++index)
    {
        anyAir |= outBlocks[index] == BLOCK_AIR;
        anySolid |= outBlocks[index] != BLOCK_AIR;
        if (anyAir && anySolid)
        {
            return WORLD_REGION_MIXED;
        }
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}
