#include "game/voxel_interaction.h"

#include "core/numeric.h"

static bool VoxelRaycast(World* world, const double origin[3],
    const float direction[3], float maximumDistance,
    int64_t outHitBlock[3], int64_t outPreviousBlock[3])
{
    int64_t block[3];
    int64_t step[3];
    double tMaximum[3];
    double tDelta[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        block[axis] = FloorDoubleToInt64(origin[axis]);
        double axisDirection = (double)direction[axis];

        if (axisDirection > 1e-6)
        {
            step[axis] = 1;
            tDelta[axis] = 1.0 / axisDirection;
            tMaximum[axis] =
                ((double)(block[axis] + 1) - origin[axis]) / axisDirection;
        }
        else if (axisDirection < -1e-6)
        {
            step[axis] = -1;
            tDelta[axis] = -1.0 / axisDirection;
            tMaximum[axis] =
                ((double)block[axis] - origin[axis]) / axisDirection;
        }
        else
        {
            step[axis] = 0;
            tDelta[axis] = 1e30;
            tMaximum[axis] = 1e30;
        }
    }

    for (;;)
    {
        int32_t axis = 0;
        if (tMaximum[1] < tMaximum[axis]) axis = 1;
        if (tMaximum[2] < tMaximum[axis]) axis = 2;
        if (tMaximum[axis] > (double)maximumDistance)
        {
            return false;
        }

        outPreviousBlock[0] = block[0];
        outPreviousBlock[1] = block[1];
        outPreviousBlock[2] = block[2];

        block[axis] += step[axis];
        tMaximum[axis] += tDelta[axis];

        if (WorldGetBlock(world, block[0], block[1], block[2]) != BLOCK_AIR)
        {
            outHitBlock[0] = block[0];
            outHitBlock[1] = block[1];
            outHitBlock[2] = block[2];
            return true;
        }
    }
}

bool VoxelInteractionTryCreateEdit(World* world, const Camera* camera,
    const PlayerController* player, bool playerCollisionsEnabled,
    bool breakPressed, bool placePressed, float maximumDistance,
    VoxelEdit* outEdit)
{
    outEdit->type = VOXEL_EDIT_NONE;
    if (!breakPressed && !placePressed)
    {
        return false;
    }

    float direction[3];
    CameraGetForwardVector(camera, direction);

    int64_t hitBlock[3];
    int64_t previousBlock[3];
    if (!VoxelRaycast(world, camera->position, direction, maximumDistance,
            hitBlock, previousBlock))
    {
        return false;
    }

    if (breakPressed)
    {
        outEdit->type = VOXEL_EDIT_BREAK;
        outEdit->replacement = BLOCK_AIR;
        outEdit->block[0] = hitBlock[0];
        outEdit->block[1] = hitBlock[1];
        outEdit->block[2] = hitBlock[2];
        return true;
    }

    if (playerCollisionsEnabled
        && PlayerControllerOverlapsBlock(player, camera, previousBlock))
    {
        return false;
    }

    outEdit->type = VOXEL_EDIT_PLACE;
    outEdit->replacement = BLOCK_EARTH;
    outEdit->block[0] = previousBlock[0];
    outEdit->block[1] = previousBlock[1];
    outEdit->block[2] = previousBlock[2];
    return true;
}
