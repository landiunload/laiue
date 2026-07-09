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

static float Noise3D(int64_t seed, float x, float y, float z)
{
    int32_t integerX = FloorToInt(x);
    int32_t integerY = FloorToInt(y);
    int32_t integerZ = FloorToInt(z);

    float smoothX = SmoothStep(x - (float)integerX);
    float smoothY = SmoothStep(y - (float)integerY);
    float smoothZ = SmoothStep(z - (float)integerZ);

    float n000 = ToUnitFloat(HashCoordinates(integerX,     integerY,     integerZ,     seed));
    float n100 = ToUnitFloat(HashCoordinates(integerX + 1, integerY,     integerZ,     seed));
    float n010 = ToUnitFloat(HashCoordinates(integerX,     integerY + 1, integerZ,     seed));
    float n110 = ToUnitFloat(HashCoordinates(integerX + 1, integerY + 1, integerZ,     seed));
    float n001 = ToUnitFloat(HashCoordinates(integerX,     integerY,     integerZ + 1, seed));
    float n101 = ToUnitFloat(HashCoordinates(integerX + 1, integerY,     integerZ + 1, seed));
    float n011 = ToUnitFloat(HashCoordinates(integerX,     integerY + 1, integerZ + 1, seed));
    float n111 = ToUnitFloat(HashCoordinates(integerX + 1, integerY + 1, integerZ + 1, seed));

    float nx00 = n000 + (n100 - n000) * smoothX;
    float nx10 = n010 + (n110 - n010) * smoothX;
    float nx01 = n001 + (n101 - n001) * smoothX;
    float nx11 = n011 + (n111 - n011) * smoothX;

    float nxy0 = nx00 + (nx10 - nx00) * smoothY;
    float nxy1 = nx01 + (nx11 - nx01) * smoothY;

    return nxy0 + (nxy1 - nxy0) * smoothZ;
}

static float FractalBrownianMotion(int64_t seed, float x, float y, float z, int32_t octaves)
{
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maximumValue = 0.0f;

    for (int32_t octave = 0; octave < octaves; ++octave)
    {
        value += amplitude * Noise3D(seed, x * frequency, y * frequency, z * frequency);
        maximumValue += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return value / maximumValue;
}

float GenerateNoise(int64_t seed, float x, float y, float z)
{
    return FractalBrownianMotion(seed, x, y, z, 4);
}
