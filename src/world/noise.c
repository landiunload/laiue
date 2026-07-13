#include "world/noise.h"

// Размеры клеток решётки по октавам (базовая 40 = прежняя частота 1/0.025).
static const int64_t g_cellBlocks[4] = { 40, 20, 10, 5 };

static inline uint32_t HashCoordinates(uint64_t x, uint64_t y, int64_t seed)
{
    uint64_t hash = x * 374761393ULL
                  + y * 1274126177ULL
                  + (uint64_t)seed * 1013904223ULL;
    hash = (hash ^ (hash >> 13)) * 1274126177ULL;
    return (uint32_t)(hash ^ (hash >> 16));
}

static inline float ToUnitFloat(uint32_t hash)
{
    return (float)(hash >> 9) / 8388607.0f;
}

static inline float SmoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static void InitializeTerrainAxis(
    uint64_t modulo[4], uint64_t cellLow[4], const BigCoord* origin)
{
    for (int32_t octave = 0; octave < 4; ++octave)
    {
        cellLow[octave] = BigCoordDivSmallLow(
            origin, (uint64_t)g_cellBlocks[octave], &modulo[octave]);
    }
}

void TerrainOriginInit(TerrainOrigin* out, const BigCoord* originX, const BigCoord* originY)
{
    InitializeTerrainAxis(out->moduloX, out->cellLowX, originX);

    if (originY == originX)
    {
        for (int32_t octave = 0; octave < 4; ++octave)
        {
            out->moduloY[octave] = out->moduloX[octave];
            out->cellLowY[octave] = out->cellLowX[octave];
        }
    }
    else
    {
        InitializeTerrainAxis(out->moduloY, out->cellLowY, originY);
    }
}

// Разлагает originModulo + local на клетку и долю внутри клетки без
// сложения signed int64, которое переполнялось у краёв локального диапазона.
static inline void ResolveCoordinate(
    uint64_t originModulo, uint64_t originCellLow,
    int64_t local, int64_t cell,
    uint64_t* outNode, float* outFraction)
{
    int64_t quotient = local / cell;
    int64_t remainder = local % cell;
    if (remainder < 0)
    {
        remainder += cell;
        --quotient;
    }

    uint64_t combinedRemainder = (uint64_t)remainder + originModulo;
    quotient += (int64_t)(combinedRemainder / (uint64_t)cell);
    combinedRemainder %= (uint64_t)cell;

    *outNode = originCellLow + (uint64_t)quotient;
    *outFraction = (float)combinedRemainder / (float)cell;
}

static float NoiseLayer(int64_t seed, const TerrainOrigin* origin, int32_t octave,
    int64_t localX, int64_t localY)
{
    int64_t cell = g_cellBlocks[octave];

    uint64_t nodeX;
    uint64_t nodeY;
    float fractionX;
    float fractionY;

    ResolveCoordinate(
        origin->moduloX[octave], origin->cellLowX[octave],
        localX, cell, &nodeX, &fractionX);
    ResolveCoordinate(
        origin->moduloY[octave], origin->cellLowY[octave],
        localY, cell, &nodeY, &fractionY);

    float smoothX = SmoothStep(fractionX);
    float smoothY = SmoothStep(fractionY);

    float n00 = ToUnitFloat(HashCoordinates(nodeX,     nodeY,     seed));
    float n10 = ToUnitFloat(HashCoordinates(nodeX + 1, nodeY,     seed));
    float n01 = ToUnitFloat(HashCoordinates(nodeX,     nodeY + 1, seed));
    float n11 = ToUnitFloat(HashCoordinates(nodeX + 1, nodeY + 1, seed));

    float nx0 = n00 + (n10 - n00) * smoothX;
    float nx1 = n01 + (n11 - n01) * smoothX;
    return nx0 + (nx1 - nx0) * smoothY;
}

float GenerateTerrainNoise(int64_t seed, const TerrainOrigin* origin, int64_t localX, int64_t localY)
{
    float value = 0.0f;
    float amplitude = 1.0f;
    float maximumValue = 0.0f;

    for (int32_t octave = 0; octave < 4; ++octave)
    {
        value += amplitude * NoiseLayer(seed, origin, octave, localX, localY);
        maximumValue += amplitude;
        amplitude *= 0.5f;
    }

    return value / maximumValue;
}
