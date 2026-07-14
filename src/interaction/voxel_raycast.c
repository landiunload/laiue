#include "interaction/voxel_raycast.h"

static int64_t FloorToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

bool VoxelRaycast(World* world, const double origin[3],
    const float direction[3], float maximumDistance,
    VoxelRaycastHit* outHit)
{
    int64_t block[3];
    int64_t step[3];
    double tMaximum[3];
    double tDelta[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        block[axis] = FloorToInt64(origin[axis]);
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

        outHit->previousBlock[0] = block[0];
        outHit->previousBlock[1] = block[1];
        outHit->previousBlock[2] = block[2];
        block[axis] += step[axis];
        tMaximum[axis] += tDelta[axis];

        if (WorldGetBlock(world, block[0], block[1], block[2]) != BLOCK_AIR)
        {
            outHit->block[0] = block[0];
            outHit->block[1] = block[1];
            outHit->block[2] = block[2];
            return true;
        }
    }
}
