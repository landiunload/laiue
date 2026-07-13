#pragma once

#include "api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Потокобезопасность: обычные запросы мира можно выполнять параллельно.
// WorldRebase вызывается только после остановки рабочих потоков мешинга.
typedef struct World World;

typedef uint8_t BlockType;

#define BLOCK_AIR   0
#define BLOCK_EARTH 1

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

LAIUE_WORLD_API World* WorldCreate(int64_t seed);
LAIUE_WORLD_API void   WorldDestroy(World* world);

// Переносит локальное начало координат на целое число блоков.
// Сдвиги обязаны быть кратны CHUNK_SIZE. Абсолютные координаты произвольной
// точности растут динамически, а локальные координаты остаются маленькими.
LAIUE_WORLD_API bool WorldRebase(World* world,
    int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ);

// Удваивает абсолютную координату X текущей точки. blockShiftX выбирает
// новое локальное начало и обязан быть кратен CHUNK_SIZE.
LAIUE_WORLD_API bool WorldDoubleAbsoluteX(World* world, int64_t blockShiftX);

LAIUE_WORLD_API void WorldFormatAbsoluteBlockCoordinate(World* world,
    int32_t axis, int64_t localBlock, wchar_t* outText, uint32_t capacity);

LAIUE_WORLD_API BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z);
LAIUE_WORLD_API void      WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block);

LAIUE_WORLD_API WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks);

// Высота верхнего твёрдого блока в текущих локальных координатах.
LAIUE_WORLD_API int64_t WorldGetTerrainHeight(World* world, int64_t x, int64_t y);

// Младшие 32 бита абсолютных координат — стабильный хеш текстуры при rebasing.
LAIUE_WORLD_API void WorldGetAbsoluteBlockLow32(World* world,
    int64_t x, int64_t y, int64_t z, uint32_t outLow[3]);
