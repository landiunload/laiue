#pragma once

#include "scene/math.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_SCENE_MATH_SERVICE_NAME "laiue.scene_math"
#define LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueSceneMathServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void (*matrix4Multiply)(const float left[16], const float right[16],
                            float out[16]);
    void (*matrix4ExtractFrustumPlanes)(const float viewProjection[16],
                                        float outPlanes[6][4]);
    bool (*frustumIntersectsBox)(const float planes[6][4],
                                 const float minimum[3], const float maximum[3]);
} LaiueSceneMathServiceV1;

const LaiueModuleApiV1 *LaiueSceneMathGetStaticModuleApiV1(void);
