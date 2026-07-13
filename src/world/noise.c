#include "world/noise.h"

// Размеры клеток решётки по октавам (базовая 40 = прежняя частота 1/0.025).
static const int64_t g_cellBlocks[4] = { 40, 20, 10, 5 };

static inline uint32_t HashCoordinates(uint64_t x, uint64_t z, int64_t seed)
{
    uint64_t hash = x * 374761393ULL
                  + z * 1274126177ULL
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

// Целочисленное деление с округлением вниз (корректно для отрицательных).
static inline int64_t FloorDiv(int64_t a, int64_t b)
{
    int64_t quotient = a / b;
    int64_t remainder = a % b;
    return (remainder != 0 && (remainder < 0) != (b < 0)) ? quotient - 1 : quotient;
}

void TerrainOriginInit(TerrainOrigin* out, BigCoord originX, BigCoord originY)
{
    for (int32_t octave = 0; octave < 4; ++octave)
    {
        BigCoord quotientX = originX;
        out->moduloX[octave]  = BigCoordDivSmall(&quotientX, (uint64_t)g_cellBlocks[octave]);
        out->cellLowX[octave] = quotientX.limb[0];

        BigCoord quotientY = originY;
        out->moduloY[octave]  = BigCoordDivSmall(&quotientY, (uint64_t)g_cellBlocks[octave]);
        out->cellLowY[octave] = quotientY.limb[0];
    }
}

static float NoiseLayer(int64_t seed, const TerrainOrigin* origin, int32_t octave,
    int64_t localX, int64_t localY)
{
    int64_t cell = g_cellBlocks[octave];

    int64_t combinedX = (int64_t)origin->moduloX[octave] + localX;
    int64_t cellOffsetX = FloorDiv(combinedX, cell);
    float fractionX = (float)(combinedX - cellOffsetX * cell) / (float)cell;
    uint64_t nodeX = origin->cellLowX[octave] + (uint64_t)cellOffsetX;

    int64_t combinedY = (int64_t)origin->moduloY[octave] + localY;
    int64_t cellOffsetY = FloorDiv(combinedY, cell);
    float fractionY = (float)(combinedY - cellOffsetY * cell) / (float)cell;
    uint64_t nodeY = origin->cellLowY[octave] + (uint64_t)cellOffsetY;

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
