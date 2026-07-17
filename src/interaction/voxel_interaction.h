#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "physics/voxel_body.h"
#include "world/world.h"

typedef enum VoxelEditType
{
    VOXEL_EDIT_NONE,
    VOXEL_EDIT_BREAK,
    VOXEL_EDIT_PLACE,
} VoxelEditType;

typedef struct VoxelEdit
{
    VoxelEditType type;
    int64_t block[3];
    BlockType replacement;
} VoxelEdit;

LAIUE_INTERACTION_API bool VoxelInteractionTryCreateEdit(
    World* world, const double origin[3], const float direction[3],
    const double blockingBodyPosition[3],
    const VoxelBodyShape* blockingBodyShape,
    bool breakPressed, bool placePressed, BlockType placementBlock,
    float maximumDistance,
    VoxelEdit* outEdit);
