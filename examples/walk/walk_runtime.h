#pragma once

#include "character/character_service.h"
#include "voxel/voxel_service.h"
#include <stdbool.h>
#include <stddef.h>

/* Shared generated terrain and swept-AABB collision, independent of shells. */
typedef struct WalkVoxelContext
{
    LaiueVoxelProviderV1 sparse;
    bool providerFailed;
} WalkVoxelContext;

uint32_t WalkGetBlock(const LaiueVoxelProviderV1 *provider, const LaiueVoxelCoordV1 *coordinate,
                      LaiueVoxelBlockV1 *outBlock);

uint32_t WalkPositionAxisToBlock(int64_t cell, int64_t local, int64_t *outBlock);

uint32_t WalkSweepAabb(const LaiueCharacterCollisionV1 *collision,
                       const LaiueCharacterPositionV1 *position, int64_t halfExtent, int64_t deltaX,
                       int64_t deltaY, int64_t deltaZ, LaiueCharacterPositionV1 *outPosition,
                       uint32_t *outGrounded);

/* Rebase the sparse world and character together between fixed steps.  The
 * service-size arguments are the sizes returned by the host query; both the
 * published size and each table's structSize are checked before using an
 * optional tail. */
uint32_t WalkRebaseWorldAndCharacter(const LaiueVoxelServiceV1 *voxel, uint32_t voxelServiceSize,
                                     LaiueVoxelWorldV1 *world,
                                     const LaiueCharacterServiceV1 *character,
                                     uint32_t characterServiceSize,
                                     LaiueCharacterControllerV1 *controller);
/* Bounds must both include the field before it is read. */
bool WalkServiceFieldPresent(uint32_t actualSize, uint32_t declaredSize, size_t offset,
                             size_t size);
