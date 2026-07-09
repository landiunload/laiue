#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;

typedef uint8_t BlockType;

#define BLOCK_AIR   0
#define BLOCK_EARTH 1

#define CHUNK_SIZE      64
#define CHUNK_SIZE_LOG2 6

LAIUE_WORLD_API World*    WorldCreate(int64_t seed);
LAIUE_WORLD_API void      WorldDestroy(World* world);

LAIUE_WORLD_API BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z);
LAIUE_WORLD_API void      WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block);

LAIUE_WORLD_API void WorldLoadArea(World* world, int64_t cx, int64_t cy, int64_t cz, int32_t radius);

// Get the procedural terrain height at block-space (x, z).
// Returns the Y coordinate of the top solid block at this column.
LAIUE_WORLD_API int32_t WorldGetTerrainHeight(World* world, int64_t x, int64_t z);
