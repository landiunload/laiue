#include "interaction/voxel_interaction.h"

#include "interaction/voxel_raycast.h"

#include <stddef.h>

bool VoxelInteractionTryCreateEdit(
    World* world, const double origin[3], const float direction[3],
    const double blockingBodyPosition[3],
    const VoxelBodyShape* blockingBodyShape,
    bool breakPressed, bool placePressed, BlockType placementBlock,
    float maximumDistance,
    VoxelEdit* outEdit)
{
    outEdit->type = VOXEL_EDIT_NONE;
    if (!breakPressed && !placePressed)
    {
        return false;
    }

    VoxelRaycastHit hit;
    if (!VoxelRaycast(world, origin, direction, maximumDistance, &hit))
    {
        return false;
    }

    if (breakPressed)
    {
        outEdit->type = VOXEL_EDIT_BREAK;
        outEdit->replacement = BLOCK_AIR;
        outEdit->block[0] = hit.block[0];
        outEdit->block[1] = hit.block[1];
        outEdit->block[2] = hit.block[2];
        return true;
    }

    if (blockingBodyShape != NULL && blockingBodyPosition != NULL
        && VoxelBodyOverlapsBlock(blockingBodyPosition,
            blockingBodyShape, hit.previousBlock))
    {
        return false;
    }

    outEdit->type = VOXEL_EDIT_PLACE;
    if (placementBlock == BLOCK_AIR) return false;
    outEdit->replacement = placementBlock;
    outEdit->block[0] = hit.previousBlock[0];
    outEdit->block[1] = hit.previousBlock[1];
    outEdit->block[2] = hit.previousBlock[2];
    return true;
}
