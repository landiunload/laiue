#pragma once

#include <stdint.h>
#include "world/infinite_coord.h"

typedef struct TerrainOrigin
{
    uint64_t moduloX[4];
    uint64_t cellLowX[4];
    uint64_t moduloY[4];
    uint64_t cellLowY[4];
} TerrainOrigin;

void TerrainOriginInit(TerrainOrigin* out, const InfiniteCoord* originX, const InfiniteCoord* originY);

// FBM 4 октавы по горизонтальным координатам (x, y).
float GenerateTerrainNoise(int64_t seed, const TerrainOrigin* origin, int64_t localX, int64_t localY);
