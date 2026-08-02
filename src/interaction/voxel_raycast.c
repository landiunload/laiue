#include "interaction/voxel_raycast.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool FiniteFloat(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

static bool FiniteDouble(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) !=
        UINT64_C(0x7ff0000000000000);
}

static bool TryFloorToInt64(double value, int64_t* outValue)
{
    // INT64_MAX rounds to 2^63 as a double.  Use the exclusive power-of-two
    // bound so the cast is defined even for hostile/network-derived origins.
    if (outValue == NULL || !FiniteDouble(value) ||
        value < -9223372036854775808.0 ||
        value >= 9223372036854775808.0)
    {
        return false;
    }
    int64_t truncated = (int64_t)value;
    *outValue = (double)truncated > value ? truncated - 1 : truncated;
    return true;
}

static bool CheckedAddStep(int64_t value, int64_t step, int64_t* output)
{
    if ((step > 0 && value == INT64_MAX) ||
        (step < 0 && value == INT64_MIN))
    {
        return false;
    }
    *output = value + step;
    return true;
}

bool VoxelRaycast(World* world, const double origin[3],
    const float direction[3], float maximumDistance,
    VoxelRaycastHit* outHit)
{
    if (world == NULL || origin == NULL || direction == NULL ||
        outHit == NULL || !FiniteFloat(maximumDistance) ||
        !(maximumDistance > 0.0f) ||
        maximumDistance > VOXEL_RAYCAST_MAX_DISTANCE)
    {
        return false;
    }

    int64_t block[3];
    int64_t step[3];
    double tMaximum[3];
    double tDelta[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!TryFloorToInt64(origin[axis], &block[axis]) ||
            !FiniteFloat(direction[axis]) ||
            direction[axis] < -1.0f || direction[axis] > 1.0f)
        {
            return false;
        }
        double axisDirection = (double)direction[axis];
        if (axisDirection > 1e-6)
        {
            step[axis] = 1;
            tDelta[axis] = 1.0 / axisDirection;
            tMaximum[axis] =
                (((double)block[axis] + 1.0) - origin[axis]) /
                    axisDirection;
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

        double hitDistance = tMaximum[axis];
        outHit->previousBlock[0] = block[0];
        outHit->previousBlock[1] = block[1];
        outHit->previousBlock[2] = block[2];
        if (!CheckedAddStep(block[axis], step[axis], &block[axis]))
        {
            return false;
        }
        tMaximum[axis] += tDelta[axis];

        if (WorldGetBlock(world, block[0], block[1], block[2]) != BLOCK_AIR)
        {
            outHit->block[0] = block[0];
            outHit->block[1] = block[1];
            outHit->block[2] = block[2];
            outHit->normal[0] = 0;
            outHit->normal[1] = 0;
            outHit->normal[2] = 0;
            outHit->normal[axis] = (int8_t)-step[axis];
            outHit->distance = hitDistance;
            return true;
        }
    }
}
