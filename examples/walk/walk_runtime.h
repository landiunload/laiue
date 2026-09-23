#pragma once

#include "character/character_service.h"
#include "voxel/voxel_service.h"

/* The terrain and swept-AABB adapter belong to the walk game provider, not to
 * the Android or desktop shell.  A runtime-only build of walk.c exports these
 * two callbacks so every client uses exactly the same infinite-coordinate and
 * collision implementation. */
typedef struct WalkVoxelContext
{
    LaiueVoxelProviderV1 sparse;
} WalkVoxelContext;

#if defined(LAIUE_WALK_RUNTIME_ONLY)
#define LAIUE_WALK_RUNTIME_API
#else
#define LAIUE_WALK_RUNTIME_API static
#endif

LAIUE_WALK_RUNTIME_API uint32_t WalkGetBlock(
    const LaiueVoxelProviderV1 *provider,
    const LaiueVoxelCoordV1 *coordinate,
    LaiueVoxelBlockV1 *outBlock);
LAIUE_WALK_RUNTIME_API uint32_t WalkSweepAabb(
    const LaiueCharacterCollisionV1 *collision,
    const LaiueCharacterPositionV1 *position,
    int64_t halfExtent,
    int64_t deltaX,
    int64_t deltaY,
    int64_t deltaZ,
    LaiueCharacterPositionV1 *outPosition,
    uint32_t *outGrounded);
