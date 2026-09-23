#pragma once

#include "voxel/raycast.h"

#include <stdint.h>

#define LAIUE_VOXEL_RAYCAST_SERVICE_NAME "laiue.voxel_raycast"
#define LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueVoxelRaycastServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    bool (*raycast)(World *world, const double origin[3],
                    const float direction[3], float maximumDistance,
                    VoxelRaycastHit *outHit);
} LaiueVoxelRaycastServiceV1;
