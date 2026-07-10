#include "world/noise.h"

static inline uint32_t HashCoordinates(int32_t x, int32_t y, int32_t z, int64_t seed)
{
    uint64_t hash = (uint64_t)(uint32_t)x * 374761393u
                  + (uint64_t)(uint32_t)y * 668265263u
                  + (uint64_t)(uint32_t)z * 1274126177u
                  + (uint64_t)seed * 1013904223u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
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

static inline int32_t FloorToInt(float value)
{
    int32_t truncated = (int32_t)value;
    return (value >= 0.0f || (float)truncated == value) ? truncated : truncated - 1;
}

// 2D-срез прежнего 3D-шума (плоскость y = 0): вклад узлов y = 1 был
// умножен на SmoothStep(0) = 0, поэтому результат идентичен старому.
static float Noise2D(int64_t seed, float x, float z)
{
    int32_t integerX = FloorToInt(x);
    int32_t integerZ = FloorToInt(z);

    float smoothX = SmoothStep(x - (float)integerX);
    float smoothZ = SmoothStep(z - (float)integerZ);

    float n00 = ToUnitFloat(HashCoordinates(integerX,     0, integerZ,     seed));
    float n10 = ToUnitFloat(HashCoordinates(integerX + 1, 0, integerZ,     seed));
    float n01 = ToUnitFloat(HashCoordinates(integerX,     0, integerZ + 1, seed));
    float n11 = ToUnitFloat(HashCoordinates(integerX + 1, 0, integerZ + 1, seed));

    float nx0 = n00 + (n10 - n00) * smoothX;
    float nx1 = n01 + (n11 - n01) * smoothX;

    return nx0 + (nx1 - nx0) * smoothZ;
}

static float FractalBrownianMotion(int64_t seed, float x, float z, int32_t octaves)
{
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maximumValue = 0.0f;

    for (int32_t octave = 0; octave < octaves; ++octave)
    {
        value += amplitude * Noise2D(seed, x * frequency, z * frequency);
        maximumValue += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return value / maximumValue;
}

float GenerateTerrainNoise(int64_t seed, float x, float z)
{
    return FractalBrownianMotion(seed, x, z, 4);
}
