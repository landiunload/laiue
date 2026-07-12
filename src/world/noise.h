#pragma once

#include <stdint.h>
#include "world/bigcoord.h"

// Абсолютный origin мира, разложенный на int64-остатки по клеткам решётки
// шума (по одному набору на ось X и Z, на каждую из 4 октав). Так шум считает
// АБСОЛЮТНУЮ координату (origin + локальная) без bignum в горячем пути:
// bignum-деление делается один раз при установке origin, дальше — только int64.
typedef struct TerrainOrigin
{
    uint64_t moduloX[4];   // originX mod cell
    uint64_t cellLowX[4];  // (originX / cell) низшие 64 бита
    uint64_t moduloZ[4];
    uint64_t cellLowZ[4];
} TerrainOrigin;

// Раскладывает абсолютный origin (X, Z) в остатки. Вызывается при создании мира.
void TerrainOriginInit(TerrainOrigin* out, BigCoord originX, BigCoord originZ);

// Высота ландшафта [0..1] в ЛОКАЛЬНЫХ координатах (относительно origin).
// FBM 4 октавы, клетка 40 блоков. Точен на любых координатах; уникальный
// рельеф в пределах 2^64 клеток (период недостижим), адресуется мир до origin.
float GenerateTerrainNoise(int64_t seed, const TerrainOrigin* origin, int64_t localX, int64_t localZ);
