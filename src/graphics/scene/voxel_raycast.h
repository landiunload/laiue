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

LAIUE_SCENE_API bool VoxelRaycast(World* world,
    const double origin[3],
    const float direction[3], float maximumDistance,
    VoxelRaycastHit* outHit);
