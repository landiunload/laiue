#pragma once

#include "api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Ordinary reads and mutations are thread-safe. WorldRebase must be called
// only while callers which use local coordinates (streaming/meshing/providers)
// are paused.
typedef struct World World;
typedef uint8_t BlockType;

// Zero is the only material reserved by the engine. Values 1..255 are owned
// by the embedding application and its content catalog.
#define BLOCK_AIR 0U

#define CHUNK_SIZE      64
#define CHUNK_SIZE_LOG2 6

static inline uint32_t WorldHashChunkCoordinate(int64_t x, int64_t y, int64_t z)
{
    uint64_t hash = (uint64_t)x * 73856093ULL
                  ^ (uint64_t)y * 19349663ULL
                  ^ (uint64_t)z * 83492791ULL;
    return (uint32_t)(hash ^ (hash >> 33));
}

typedef enum WorldRegionContents
{
    WORLD_REGION_ALL_AIR,
    WORLD_REGION_ALL_SOLID,
    WORLD_REGION_MIXED
} WorldRegionContents;

// A base provider is application-owned and is never freed by the engine.
// Coordinates start at zero and are local to the current world origin.
// getBlock is required when a provider is supplied. fillRegion is optional;
// when present it must fill every output cell using the same layout as
// WorldFillRegion. rebase is optional and is called before the engine commits
// an origin shift; false aborts the entire rebase and therefore must leave the
// provider context unchanged. Provider callbacks may run concurrently with
// reads, must be thread-safe, and must not re-enter the same World.
typedef BlockType (*WorldBaseGetBlock)(void* context,
    int64_t x, int64_t y, int64_t z);
typedef WorldRegionContents (*WorldBaseFillRegion)(void* context,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks);
typedef bool (*WorldBaseRebase)(void* context,
    int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ);

typedef struct WorldBaseProvider
{
    void* context;
    WorldBaseGetBlock getBlock;
    WorldBaseFillRegion fillRegion;
    WorldBaseRebase rebase;
} WorldBaseProvider;

#define WORLD_MAX_ATOMIC_BLOCK_MUTATIONS 4096U

typedef struct WorldBlockMutation
{
    int64_t block[3];
    BlockType expected;
    BlockType replacement;
} WorldBlockMutation;

// NULL provider creates an empty world. The provider structure is copied, but
// its context remains application-owned and must outlive the World.
LAIUE_WORLD_API World* WorldCreate(const WorldBaseProvider* provider);
LAIUE_WORLD_API void WorldDestroy(World* world);

// Shifts the local origin by whole chunks. Infinite absolute coordinates and
// sparse overrides remain unchanged.
LAIUE_WORLD_API bool WorldRebase(World* world,
    int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ);

LAIUE_WORLD_API void WorldFormatAbsoluteBlockCoordinate(World* world,
    int32_t axis, int64_t localBlock, wchar_t* outText, uint32_t capacity);

LAIUE_WORLD_API BlockType WorldGetBlock(
    World* world, int64_t x, int64_t y, int64_t z);
LAIUE_WORLD_API bool WorldTrySetBlock(
    World* world, int64_t x, int64_t y, int64_t z, BlockType block);
LAIUE_WORLD_API void WorldSetBlock(
    World* world, int64_t x, int64_t y, int64_t z, BlockType block);

// Validates all expected values and prepares all allocations before publishing
// any change. A false result leaves blocks and revision unchanged.
LAIUE_WORLD_API bool WorldApplyBlockBatch(
    World* world, const WorldBlockMutation* mutations, uint32_t count);
LAIUE_WORLD_API uint64_t WorldGetRevision(World* world);

// Output layout is ((y * sizeX) + x) * sizeZ + z. Invalid dimensions return
// WORLD_REGION_ALL_AIR without touching output.
LAIUE_WORLD_API WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks);
