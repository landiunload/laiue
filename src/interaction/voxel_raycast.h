#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "world/world.h"

typedef struct VoxelRaycastHit
{
    int64_t block[3];
    int64_t previousBlock[3];
} VoxelRaycastHit;

bool VoxelRaycast(World* world, const double origin[3],
    const float direction[3], float maximumDistance,
    VoxelRaycastHit* outHit);
