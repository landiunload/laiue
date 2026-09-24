#pragma once

/* Fixed-step kinematic character service. Positions use an integer cell plus
 * a signed local coordinate, so rebasing/render precision never changes the
 * authoritative result. A game may use this without loading physics.dll. */

#include <stdint.h>

#define LAIUE_CHARACTER_ABI_VERSION_1 1u
#define LAIUE_CHARACTER_TICK_HZ 128u
#define LAIUE_CHARACTER_LOCAL_CELL_SIZE INT64_C(1024000)
#define LAIUE_CHARACTER_WALK_SPEED INT64_C(3500)
#define LAIUE_CHARACTER_SPRINT_SPEED INT64_C(7000)
#define LAIUE_CHARACTER_JUMP_SPEED INT64_C(5200)
#define LAIUE_CHARACTER_GRAVITY_PER_TICK (INT64_C(9800) / LAIUE_CHARACTER_TICK_HZ)

typedef struct LaiueCharacterPositionV1
{
    int64_t cellX;
    int64_t cellY;
    int64_t localX;
    int64_t localY;
    int64_t localZ;
} LaiueCharacterPositionV1;

typedef struct LaiueCharacterInputV1
{
    int32_t moveX;
    int32_t moveY;
    uint32_t flags;
} LaiueCharacterInputV1;

#define LAIUE_CHARACTER_INPUT_JUMP UINT32_C(1) << 0
#define LAIUE_CHARACTER_INPUT_SPRINT UINT32_C(1) << 1

typedef struct LaiueCharacterCollisionV1 LaiueCharacterCollisionV1;
typedef struct LaiueCharacterControllerV1 LaiueCharacterControllerV1;
typedef uint32_t (*LaiueCharacterSweepAabbFn)(const LaiueCharacterCollisionV1 *,
                                              const LaiueCharacterPositionV1 *position,
                                              int64_t halfExtent, int64_t deltaX, int64_t deltaY,
                                              int64_t deltaZ, LaiueCharacterPositionV1 *outPosition,
                                              uint32_t *outGrounded);
/* Translate the local origin without resetting velocity or fixed-point
 * remainders. The world and streaming providers call this between fixed
 * steps as part of one rebasing transaction. */
typedef uint32_t (*LaiueCharacterRebaseOriginFn)(
    LaiueCharacterControllerV1 *controller, int64_t cellDeltaX,
    int64_t cellDeltaY, int64_t localDeltaX, int64_t localDeltaY);

struct LaiueCharacterCollisionV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void *context;
    LaiueCharacterSweepAabbFn sweepAabb;
    uintptr_t reserved[8];
};
