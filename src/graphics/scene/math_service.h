#pragma once

#include "scene/math.h"
#include "mod/module_api.h"

#include <stddef.h>
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
    /* Optional tail: present when both the published size and structSize
     * reach LAIUE_SCENE_MATH_SERVICE_V1_CAMERA_SIZE. */
    bool (*matrix4Perspective)(float verticalFovRadians, float aspectRatio,
                               float nearPlane, float farPlane, float out[16]);
    bool (*matrix4LookAt)(const float eye[3], const float target[3],
                          const float up[3], float out[16]);
} LaiueSceneMathServiceV1;

#define LAIUE_SCENE_MATH_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueSceneMathServiceV1, matrix4Perspective))
#define LAIUE_SCENE_MATH_SERVICE_V1_CAMERA_SIZE ((uint32_t)sizeof(LaiueSceneMathServiceV1))

const LaiueModuleApiV1 *LaiueSceneMathGetStaticModuleApiV1(void);
