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

// Итог пакетной выборки региона: однородные регионы позволяют
// потребителю (например, мешеру) пропустить обработку целиком.
typedef enum WorldRegionContents
{
    WORLD_REGION_ALL_AIR,
    WORLD_REGION_ALL_SOLID,
    WORLD_REGION_MIXED
} WorldRegionContents;

LAIUE_WORLD_API World* WorldCreate(int64_t seed);
LAIUE_WORLD_API void   WorldDestroy(World* world);

LAIUE_WORLD_API BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z);
LAIUE_WORLD_API void      WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block);

// Пакетная выборка блоков региона одним вызовом: высота колонны считается
// один раз, дельты накладываются пачкой — на порядки быстрее, чем
// поблочные вызовы WorldGetBlock через границу DLL.
//
// Раскладка outBlocks: индекс = ((z * sizeX) + x) * sizeY + y.
// Если регион однороден (весь воздух или весь камень), outBlocks
// НЕ заполняется — потребитель обрабатывает такой регион без данных.
LAIUE_WORLD_API WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks);

// Y-координата верхнего твёрдого блока колонны процедурного ландшафта.
LAIUE_WORLD_API int32_t WorldGetTerrainHeight(World* world, int64_t x, int64_t z);
