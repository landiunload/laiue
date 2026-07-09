#include <stdint.h>

static inline uint32_t Hash4(int32_t x, int32_t y, int32_t z, int64_t seed)
{
    uint64_t h = (uint64_t)(uint32_t)x * 374761393u
               + (uint64_t)(uint32_t)y * 668265263u
               + (uint64_t)(uint32_t)z * 1274126177u
               + (uint64_t)seed * 1013904223u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (uint32_t)(h ^ (h >> 16));
}

static inline float ToFloat(uint32_t hash)
{
    return (float)(hash >> 9) / 8388607.0f;
}

static inline float SmoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static inline int32_t FloorToInt(float x)
{
    int32_t ix = (int32_t)x;
    return (x >= 0.0f || (float)ix == x) ? ix : ix - 1;
}

static float Noise3D(int64_t seed, float x, float y, float z)
{
    int32_t ix = FloorToInt(x);
    int32_t iy = FloorToInt(y);
    int32_t iz = FloorToInt(z);

    float fx = x - (float)ix;
    float fy = y - (float)iy;
    float fz = z - (float)iz;

    float sx = SmoothStep(fx);
    float sy = SmoothStep(fy);
    float sz = SmoothStep(fz);

    float n000 = ToFloat(Hash4(ix,     iy,     iz,     seed));
    float n100 = ToFloat(Hash4(ix + 1, iy,     iz,     seed));
    float n010 = ToFloat(Hash4(ix,     iy + 1, iz,     seed));
    float n110 = ToFloat(Hash4(ix + 1, iy + 1, iz,     seed));
    float n001 = ToFloat(Hash4(ix,     iy,     iz + 1, seed));
    float n101 = ToFloat(Hash4(ix + 1, iy,     iz + 1, seed));
    float n011 = ToFloat(Hash4(ix,     iy + 1, iz + 1, seed));
    float n111 = ToFloat(Hash4(ix + 1, iy + 1, iz + 1, seed));

    float nx00 = n000 + (n100 - n000) * sx;
    float nx10 = n010 + (n110 - n010) * sx;
    float nx01 = n001 + (n101 - n001) * sx;
    float nx11 = n011 + (n111 - n011) * sx;

    float nxy0 = nx00 + (nx10 - nx00) * sy;
    float nxy1 = nx01 + (nx11 - nx01) * sy;

    return nxy0 + (nxy1 - nxy0) * sz;
}

static float FBM(int64_t seed, float x, float y, float z, int octaves)
{
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; ++i)
    {
        value += amplitude * Noise3D(seed, x * frequency, y * frequency, z * frequency);
        maxValue += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return value / maxValue;
}

float GenerateNoise(int64_t seed, float x, float y, float z)
{
    return FBM(seed, x, y, z, 4);
}
