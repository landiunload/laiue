#include "physics/voxel_body.h"

#include <stddef.h>

static int64_t FloorToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

void VoxelBodyCalculateBounds(const double position[3],
    const VoxelBodyShape* shape, VoxelBodyBounds* outBounds)
{
    double feet = position[2] - shape->eyeHeight;

    outBounds->minimum[0] = position[0] - shape->radius;
    outBounds->maximum[0] = position[0] + shape->radius;
    outBounds->minimum[1] = position[1] - shape->radius;
    outBounds->maximum[1] = position[1] + shape->radius;
    outBounds->minimum[2] = feet;
    outBounds->maximum[2] = feet + shape->height;
}

static bool BoundsContainSolidBlock(
    const VoxelCollisionSource* collision,
    const VoxelBodyShape* shape, const VoxelBodyBounds* bounds)
{
    int64_t minimumBlock[3];
    int64_t maximumBlock[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        minimumBlock[axis] = FloorToInt64(
            bounds->minimum[axis] + shape->collisionEpsilon);
        maximumBlock[axis] = FloorToInt64(
            bounds->maximum[axis] - shape->collisionEpsilon);
    }

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (collision->isSolidBlock(collision->context, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool VoxelBodyCollides(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape)
{
    VoxelBodyBounds bounds;
    VoxelBodyCalculateBounds(position, shape, &bounds);
    return BoundsContainSolidBlock(collision, shape, &bounds);
}

static bool BlockPlaneCollides(
    const VoxelCollisionSource* collision,
    const VoxelBodyShape* shape, int32_t axis, int64_t plane,
    const VoxelBodyBounds* bounds)
{
    int64_t minimumBlock[3];
    int64_t maximumBlock[3];

    for (int32_t currentAxis = 0; currentAxis < 3; ++currentAxis)
    {
        minimumBlock[currentAxis] = FloorToInt64(
            bounds->minimum[currentAxis] + shape->collisionEpsilon);
        maximumBlock[currentAxis] = FloorToInt64(
            bounds->maximum[currentAxis] - shape->collisionEpsilon);
    }
    minimumBlock[axis] = plane;
    maximumBlock[axis] = plane;

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (collision->isSolidBlock(collision->context, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool VoxelBodyMoveAxis(const VoxelCollisionSource* collision,
    double position[3], const VoxelBodyShape* shape,
    int32_t axis, double distance)
{
    if (distance == 0.0)
    {
        return false;
    }

    VoxelBodyBounds oldBounds;
    VoxelBodyCalculateBounds(position, shape, &oldBounds);

    double targetPosition[3] = {
        position[0], position[1], position[2]
    };
    targetPosition[axis] += distance;

    VoxelBodyBounds newBounds;
    VoxelBodyCalculateBounds(targetPosition, shape, &newBounds);

    double negativeExtent = axis == 2
        ? shape->eyeHeight
        : shape->radius;
    double positiveExtent = axis == 2
        ? shape->height - shape->eyeHeight
        : shape->radius;
    double epsilon = shape->collisionEpsilon;

    if (distance > 0.0)
    {
        int64_t firstPlane =
            FloorToInt64(oldBounds.maximum[axis] - epsilon) + 1;
        int64_t lastPlane =
            FloorToInt64(newBounds.maximum[axis] - epsilon);

        for (int64_t plane = firstPlane; plane <= lastPlane; ++plane)
        {
            if (BlockPlaneCollides(
                    collision, shape, axis, plane, &newBounds))
            {
                position[axis] =
                    (double)plane - positiveExtent - epsilon;
                return true;
            }
        }
    }
    else
    {
        int64_t firstPlane =
            FloorToInt64(oldBounds.minimum[axis] + epsilon) - 1;
        int64_t lastPlane =
            FloorToInt64(newBounds.minimum[axis] + epsilon);

        for (int64_t plane = firstPlane; plane >= lastPlane; --plane)
        {
            if (BlockPlaneCollides(
                    collision, shape, axis, plane, &newBounds))
            {
                position[axis] =
                    (double)plane + 1.0 + negativeExtent + epsilon;
                return true;
            }
        }
    }

    position[axis] = targetPosition[axis];
    return false;
}

bool VoxelBodyHasGroundContact(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth)
{
    VoxelBodyBounds bounds;
    VoxelBodyCalculateBounds(position, shape, &bounds);

    int64_t minimumX = FloorToInt64(
        bounds.minimum[0] + shape->collisionEpsilon);
    int64_t maximumX = FloorToInt64(
        bounds.maximum[0] - shape->collisionEpsilon);
    int64_t minimumY = FloorToInt64(
        bounds.minimum[1] + shape->collisionEpsilon);
    int64_t maximumY = FloorToInt64(
        bounds.maximum[1] - shape->collisionEpsilon);
    int64_t supportZ = FloorToInt64(bounds.minimum[2] - probeDepth);

    for (int64_t y = minimumY; y <= maximumY; ++y)
    {
        for (int64_t x = minimumX; x <= maximumX; ++x)
        {
            if (collision->isSolidBlock(collision->context, x, y, supportZ))
            {
                return true;
            }
        }
    }
    return false;
}

bool VoxelBodyHasStableGround(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double supportRadius)
{
    if (supportRadius < 0.0)
    {
        supportRadius = 0.0;
    }
    if (supportRadius > shape->radius)
    {
        supportRadius = shape->radius;
    }

    double feet = position[2] - shape->eyeHeight;
    int64_t supportZ = FloorToInt64(feet - probeDepth);
    const double offsets[3] = {
        -supportRadius, 0.0, supportRadius
    };

    for (uint32_t yIndex = 0; yIndex < 3u; ++yIndex)
    {
        for (uint32_t xIndex = 0; xIndex < 3u; ++xIndex)
        {
            int64_t x = FloorToInt64(position[0] + offsets[xIndex]);
            int64_t y = FloorToInt64(position[1] + offsets[yIndex]);
            if (collision->isSolidBlock(collision->context, x, y, supportZ))
            {
                return true;
            }
        }
    }
    return false;
}

bool VoxelBodyOverlapsBlock(const double position[3],
    const VoxelBodyShape* shape, const int64_t block[3])
{
    VoxelBodyBounds bounds;
    VoxelBodyCalculateBounds(position, shape, &bounds);

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double blockMinimum = (double)block[axis];
        double blockMaximum = blockMinimum + 1.0;
        if (bounds.maximum[axis]
                <= blockMinimum + shape->collisionEpsilon
            || bounds.minimum[axis]
                >= blockMaximum - shape->collisionEpsilon)
        {
            return false;
        }
    }
    return true;
}

static bool HasSupportBelowOffset(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double xOffset, double yOffset)
{
    if (probeDepth <= 0.0)
    {
        return false;
    }

    double shiftedPosition[3] = {
        position[0] + xOffset,
        position[1] + yOffset,
        position[2],
    };
    VoxelBodyBounds bounds;
    VoxelBodyCalculateBounds(shiftedPosition, shape, &bounds);

    double epsilon = shape->collisionEpsilon;
    int64_t minimumX = FloorToInt64(bounds.minimum[0] + epsilon);
    int64_t maximumX = FloorToInt64(bounds.maximum[0] - epsilon);
    int64_t minimumY = FloorToInt64(bounds.minimum[1] + epsilon);
    int64_t maximumY = FloorToInt64(bounds.maximum[1] - epsilon);
    int64_t minimumZ = FloorToInt64(bounds.minimum[2] - probeDepth + epsilon);
    int64_t maximumZ = FloorToInt64(bounds.minimum[2] - epsilon);

    for (int64_t z = minimumZ; z <= maximumZ; ++z)
    {
        for (int64_t y = minimumY; y <= maximumY; ++y)
        {
            for (int64_t x = minimumX; x <= maximumX; ++x)
            {
                if (collision->isSolidBlock(collision->context, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static double ReduceSneakDistance(double value, double step)
{
    if (value > 0.0)
    {
        return value <= step ? 0.0 : value - step;
    }
    if (value < 0.0)
    {
        return value >= -step ? 0.0 : value + step;
    }
    return 0.0;
}

void VoxelBodyClipSneakingMovement(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double* xDistance, double* yDistance)
{
    if (collision == NULL || collision->isSolidBlock == NULL
        || position == NULL || shape == NULL
        || xDistance == NULL || yDistance == NULL)
    {
        return;
    }

    // Уже падающее или вытолкнутое тело не приклеивается обратно к краю.
    if (!HasSupportBelowOffset(
            collision, position, shape, probeDepth, 0.0, 0.0))
    {
        return;
    }

    // Vanilla Minecraft уменьшает компоненты движения небольшими шагами,
    // сначала отдельно, затем диагональ. Это сохраняет скольжение вдоль края.
    const double reductionStep = 0.05;
    double x = *xDistance;
    double y = *yDistance;

    while (x != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, x, 0.0))
    {
        x = ReduceSneakDistance(x, reductionStep);
    }

    while (y != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, 0.0, y))
    {
        y = ReduceSneakDistance(y, reductionStep);
    }

    while (x != 0.0 && y != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, x, y))
    {
        x = ReduceSneakDistance(x, reductionStep);
        y = ReduceSneakDistance(y, reductionStep);
    }

    *xDistance = x;
    *yDistance = y;
}
