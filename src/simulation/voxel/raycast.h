#pragma once

#include "api.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define VOXEL_RAYCAST_MAX_DISTANCE 1024.0f

typedef struct VoxelRaycastHit
{
    int64_t block[3];
    int64_t previousBlock[3];
    int8_t normal[3];
    double distance;
} VoxelRaycastHit;

/* Runtime-neutral block query used by the dynamically loaded provider. The
 * callback is borrowed for the duration of the call and is never retained. */
typedef BlockType (*VoxelRaycastGetBlockFn)(
    void *context, int64_t x, int64_t y, int64_t z);

LAIUE_VOXEL_RAYCAST_API bool VoxelRaycastWithBlockQuery(
    void *context, VoxelRaycastGetBlockFn getBlock,
    const double origin[3], const float direction[3],
    float maximumDistance, VoxelRaycastHit *outHit);

/* Compatibility wrapper for static/legacy callers. Dynamic module builds
 * use VoxelRaycastWithBlockQuery and resolve laiue.world through the host. */
LAIUE_VOXEL_RAYCAST_API bool VoxelRaycast(World* world,
    const double origin[3],
    const float direction[3], float maximumDistance,
    VoxelRaycastHit* outHit);
