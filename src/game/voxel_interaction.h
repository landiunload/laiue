#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game/camera.h"
#include "game/player_controller.h"
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

bool VoxelInteractionTryCreateEdit(World* world, const Camera* camera,
    const PlayerController* player, bool playerCollisionsEnabled,
    bool breakPressed, bool placePressed, float maximumDistance,
    VoxelEdit* outEdit);
