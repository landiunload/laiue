#include "walk_world.h"
#include "walk_math.h"

#include "math/scalar.h"

#include <math.h>
#include <stddef.h>

static bool AddCoordinate(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static bool FloorLocal(double value, int64_t *out)
{
    if (!WalkMathFinite(value) || value < -9223372036854775808.0 ||
        value >= 9223372036854775808.0)
        return false;
    const int64_t truncated = (int64_t)value;
    *out = (double)truncated > value ? truncated - 1 : truncated;
    return true;
}

bool WalkWorldRaycast(WalkWorldGetBlockFn getBlock, void *context,
                      const double origin[3], const int64_t offset[3],
                      const float direction[3], float maximumDistance,
                      WalkWorldBlockHit *outHit)
{
    if (getBlock == NULL || context == NULL || origin == NULL || offset == NULL ||
        direction == NULL || outHit == NULL || !WalkMathFinite(maximumDistance) ||
        maximumDistance <= 0.0f || maximumDistance > 1024.0f)
        return false;

    int64_t block[3], previous[3], step[3];
    double localFloor[3], tMaximum[3], tDelta[3];
    double lengthSquared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!WalkMathFinite(direction[axis]) || direction[axis] < -1.0f ||
            direction[axis] > 1.0f || !FloorLocal(origin[axis], &block[axis]) ||
            !AddCoordinate(block[axis], offset[axis], &block[axis]))
            return false;
        localFloor[axis] = WalkMathFloor(origin[axis]);
        lengthSquared += (double)direction[axis] * direction[axis];
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        previous[axis] = block[axis];
    if (!(lengthSquared > 1.0e-12) || !WalkMathFinite(lengthSquared))
        return false;
    const double inverseLength = 1.0 / ScalarSqrtDouble(lengthSquared);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double normalized = (double)direction[axis] * inverseLength;
        if (normalized > 1.0e-6)
        {
            step[axis] = 1;
            tDelta[axis] = 1.0 / normalized;
            tMaximum[axis] = (localFloor[axis] + 1.0 - origin[axis]) / normalized;
        }
        else if (normalized < -1.0e-6)
        {
            step[axis] = -1;
            tDelta[axis] = -1.0 / normalized;
            const bool onBoundary = origin[axis] == localFloor[axis];
            if (onBoundary && !AddCoordinate(block[axis], -1, &block[axis]))
                return false;
            tMaximum[axis] = (origin[axis] - localFloor[axis] +
                              (onBoundary ? 1.0 : 0.0)) / -normalized;
        }
        else
        {
            step[axis] = 0;
            tDelta[axis] = 1.0e30;
            tMaximum[axis] = 1.0e30;
        }
    }

    for (uint32_t visited = 0u; visited < 3075u; ++visited)
    {
        if (getBlock(context, block[0], block[1], block[2]) != 0u)
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                outHit->block[axis] = block[axis];
                outHit->previousBlock[axis] = previous[axis];
            }
            return true;
        }
        double nextDistance = tMaximum[0];
        if (tMaximum[1] < nextDistance) nextDistance = tMaximum[1];
        if (tMaximum[2] < nextDistance) nextDistance = tMaximum[2];
        if (nextDistance > (double)maximumDistance)
            return false;
        for (uint32_t component = 0u; component < 3u; ++component)
            previous[component] = block[component];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (tMaximum[axis] <= nextDistance + 1.0e-12)
            {
                if (!AddCoordinate(block[axis], step[axis], &block[axis]))
                    return false;
                tMaximum[axis] += tDelta[axis];
            }
    }
    return false;
}

bool WalkWorldEditHit(WalkWorldSetBlockFn setBlock, void *context,
                      const WalkWorldBlockHit *hit, bool place,
                      uint8_t material)
{
    if (setBlock == NULL || context == NULL || hit == NULL ||
        (place && (material == 0u ||
         (hit->previousBlock[0] == hit->block[0] &&
          hit->previousBlock[1] == hit->block[1] &&
          hit->previousBlock[2] == hit->block[2]))))
        return false;
    const int64_t *target = place ? hit->previousBlock : hit->block;
    return setBlock(context, target[0], target[1], target[2], place ? material : 0u);
}
