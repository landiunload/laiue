#pragma once

/* Service table for the optional fixed-step kinematic character provider.
 * The provider consumes only the collision callback from character_api.h;
 * it has no knowledge of voxels, scenes, renderers, or rigid-body physics. */

#include "character/character_api.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_CHARACTER_SERVICE_NAME "laiue.character"
#define LAIUE_CHARACTER_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueCharacterControllerV1 LaiueCharacterControllerV1;

typedef uint32_t (*LaiueCharacterCreateFn)(
    const LaiueCharacterCollisionV1 *collision,
    int64_t halfExtent,
    LaiueCharacterControllerV1 **outController);
typedef void (*LaiueCharacterDestroyFn)(LaiueCharacterControllerV1 *controller);
typedef uint32_t (*LaiueCharacterStepFn)(
    LaiueCharacterControllerV1 *controller,
    const LaiueCharacterInputV1 *input);
typedef uint32_t (*LaiueCharacterGetPositionFn)(
    const LaiueCharacterControllerV1 *controller,
    LaiueCharacterPositionV1 *outPosition);
typedef uint32_t (*LaiueCharacterSetPositionFn)(
    LaiueCharacterControllerV1 *controller,
    const LaiueCharacterPositionV1 *position,
    uint32_t grounded);
typedef uint32_t (*LaiueCharacterIsGroundedFn)(
    const LaiueCharacterControllerV1 *controller);

typedef struct LaiueCharacterServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueCharacterCreateFn create;
    LaiueCharacterDestroyFn destroy;
    LaiueCharacterStepFn step;
    LaiueCharacterGetPositionFn getPosition;
    LaiueCharacterSetPositionFn setPosition;
    LaiueCharacterIsGroundedFn isGrounded;
    uintptr_t reserved[8];
} LaiueCharacterServiceV1;

const LaiueModuleApiV1 *LaiueCharacterGetStaticModuleApiV1(void);
