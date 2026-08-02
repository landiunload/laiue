#include "physics/voxel_body.h"

#include <stddef.h>

#define VOXEL_DYNAMIC_COORDINATE_LIMIT 1.0e15
#define VOXEL_DYNAMIC_VELOCITY_LIMIT 1048576.0
#define VOXEL_INT64_EXCLUSIVE_MAXIMUM 9223372036854775808.0

typedef struct DynamicColliderBatch
{
    VoxelDynamicCollider colliders[VOXEL_DYNAMIC_COLLIDER_CAPACITY];
    uint32_t count;
} DynamicColliderBatch;

static double MinimumDouble(double left, double right)
{
    return left < right ? left : right;
}

static double MaximumDouble(double left, double right)
{
    return left > right ? left : right;
}

static bool IsFiniteBoundedDouble(double value, double limit)
{
    return value == value && value >= -limit && value <= limit;
}

static bool BoundsAreValid(const VoxelBodyBounds* bounds)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!IsFiniteBoundedDouble(bounds->minimum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || !IsFiniteBoundedDouble(bounds->maximum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || bounds->minimum[axis] >= bounds->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

static bool QueryBoundsAreValid(const VoxelBodyBounds* bounds)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!IsFiniteBoundedDouble(bounds->minimum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || !IsFiniteBoundedDouble(bounds->maximum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || bounds->minimum[axis] > bounds->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

static bool BodyShapeIsValid(const VoxelBodyShape* shape)
{
    return IsFiniteBoundedDouble(
            shape->radius, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        && IsFiniteBoundedDouble(
            shape->height, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        && IsFiniteBoundedDouble(
            shape->eyeHeight, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        && IsFiniteBoundedDouble(
            shape->collisionEpsilon, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        && shape->radius > shape->collisionEpsilon
        && shape->height > shape->collisionEpsilon * 2.0
        && shape->eyeHeight >= 0.0
        && shape->eyeHeight <= shape->height
        && shape->collisionEpsilon >= 0.0;
}

static bool CalculateValidBodyBounds(const double position[3],
    const VoxelBodyShape* shape, VoxelBodyBounds* outBounds)
{
    if (!BodyShapeIsValid(shape))
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!IsFiniteBoundedDouble(
                position[axis], VOXEL_DYNAMIC_COORDINATE_LIMIT))
        {
            return false;
        }
    }
    VoxelBodyCalculateBounds(position, shape, outBounds);
    return BoundsAreValid(outBounds);
}

static bool TryFloorToInt64(double value, int64_t* outValue)
{
    // C leaves an out-of-range floating-to-integer conversion undefined.
    // The exclusive positive bound is exactly 2^63 as a double.
    if (!(value >= -VOXEL_INT64_EXCLUSIVE_MAXIMUM
            && value < VOXEL_INT64_EXCLUSIVE_MAXIMUM))
    {
        return false;
    }
    int64_t truncated = (int64_t)value;
    *outValue = (double)truncated > value
        ? truncated - 1 : truncated;
    return true;
}

static bool DynamicColliderIsValid(const VoxelDynamicCollider* collider)
{
    if (collider->stableId == 0u
        || !(collider->friction >= 0.0f && collider->friction <= 1.0f))
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!IsFiniteBoundedDouble(
                collider->bounds.minimum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || !IsFiniteBoundedDouble(
                collider->bounds.maximum[axis],
                VOXEL_DYNAMIC_COORDINATE_LIMIT)
            || collider->bounds.minimum[axis]
                >= collider->bounds.maximum[axis]
            || !IsFiniteBoundedDouble(
                collider->velocity[axis],
                VOXEL_DYNAMIC_VELOCITY_LIMIT))
        {
            return false;
        }
    }
    return true;
}

static void SortDynamicColliders(DynamicColliderBatch* batch)
{
    // Insertion sort keeps the bounded 32-element path allocation-free and
    // makes clipping/contact selection independent from callback order.
    for (uint32_t index = 1u; index < batch->count; ++index)
    {
        VoxelDynamicCollider value = batch->colliders[index];
        uint32_t insertion = index;
        while (insertion > 0u
            && batch->colliders[insertion - 1u].stableId > value.stableId)
        {
            batch->colliders[insertion] =
                batch->colliders[insertion - 1u];
            --insertion;
        }
        batch->colliders[insertion] = value;
    }
}

static bool QueryDynamicColliderBatch(
    const VoxelCollisionSource* collision,
    const VoxelBodyBounds* queryBounds,
    DynamicColliderBatch* outBatch)
{
    outBatch->count = 0u;
    if (!QueryBoundsAreValid(queryBounds))
    {
        return false;
    }
    if (collision->queryDynamicColliders == NULL)
    {
        return true;
    }

    // A callback which forgot to write count fails safely as well.
    uint32_t count = VOXEL_DYNAMIC_COLLIDER_CAPACITY + 1u;
    if (!collision->queryDynamicColliders(collision->context,
            queryBounds, outBatch->colliders,
            VOXEL_DYNAMIC_COLLIDER_CAPACITY, &count)
        || count > VOXEL_DYNAMIC_COLLIDER_CAPACITY)
    {
        outBatch->count = 0u;
        return false;
    }

    outBatch->count = count;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!DynamicColliderIsValid(&outBatch->colliders[index]))
        {
            outBatch->count = 0u;
            return false;
        }
        for (uint32_t previous = 0u; previous < index; ++previous)
        {
            if (outBatch->colliders[previous].stableId
                == outBatch->colliders[index].stableId)
            {
                outBatch->count = 0u;
                return false;
            }
        }
    }
    SortDynamicColliders(outBatch);
    return true;
}

static bool BoundsOverlapDynamicCollider(
    const VoxelBodyBounds* bounds,
    const VoxelDynamicCollider* collider, double epsilon)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (bounds->maximum[axis]
                <= collider->bounds.minimum[axis] - epsilon
            || bounds->minimum[axis]
                >= collider->bounds.maximum[axis] + epsilon)
        {
            return false;
        }
    }
    return true;
}

static bool BoundsOverlapDynamicOnOtherAxes(
    const VoxelBodyBounds* bounds,
    const VoxelDynamicCollider* collider,
    int32_t movementAxis, double epsilon)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (axis == movementAxis)
        {
            continue;
        }
        if (bounds->maximum[axis]
                <= collider->bounds.minimum[axis] - epsilon
            || bounds->minimum[axis]
                >= collider->bounds.maximum[axis] + epsilon)
        {
            return false;
        }
    }
    return true;
}

static void QueryBlockPhysics(const VoxelCollisionSource* collision,
    int64_t x, int64_t y, int64_t z, VoxelBlockPhysics* outBlock)
{
    collision->queryBlockPhysics(
        collision->context, x, y, z, outBlock);
}

static bool IsSolidBlock(const VoxelCollisionSource* collision,
    int64_t x, int64_t y, int64_t z)
{
    VoxelBlockPhysics block;
    QueryBlockPhysics(collision, x, y, z, &block);
    return (block.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u;
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
        if (!TryFloorToInt64(
                bounds->minimum[axis] + shape->collisionEpsilon,
                &minimumBlock[axis])
            || !TryFloorToInt64(
                bounds->maximum[axis] - shape->collisionEpsilon,
                &maximumBlock[axis]))
        {
            return true;
        }
    }

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (IsSolidBlock(collision, x, y, z))
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
    if (!CalculateValidBodyBounds(position, shape, &bounds))
    {
        return true;
    }
    if (BoundsContainSolidBlock(collision, shape, &bounds))
    {
        return true;
    }
    if (collision->queryDynamicColliders == NULL)
    {
        return false;
    }

    VoxelBodyBounds queryBounds = bounds;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        queryBounds.minimum[axis] -= shape->collisionEpsilon;
        queryBounds.maximum[axis] += shape->collisionEpsilon;
    }
    DynamicColliderBatch batch;
    if (!QueryDynamicColliderBatch(collision, &queryBounds, &batch))
    {
        return true;
    }
    for (uint32_t index = 0u; index < batch.count; ++index)
    {
        if (BoundsOverlapDynamicCollider(
                &bounds, &batch.colliders[index],
                shape->collisionEpsilon))
        {
            return true;
        }
    }
    return false;
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
        if (!TryFloorToInt64(
                bounds->minimum[currentAxis]
                    + shape->collisionEpsilon,
                &minimumBlock[currentAxis])
            || !TryFloorToInt64(
                bounds->maximum[currentAxis]
                    - shape->collisionEpsilon,
                &maximumBlock[currentAxis]))
        {
            return true;
        }
    }
    minimumBlock[axis] = plane;
    maximumBlock[axis] = plane;

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (IsSolidBlock(collision, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool MoveAxisAgainstBlocks(const VoxelCollisionSource* collision,
    double position[3], const VoxelBodyShape* shape,
    int32_t axis, double distance)
{
    if (axis < 0 || axis > 2
        || !IsFiniteBoundedDouble(
            distance, VOXEL_DYNAMIC_COORDINATE_LIMIT))
    {
        return true;
    }
    if (distance == 0.0)
    {
        return false;
    }

    VoxelBodyBounds oldBounds;
    if (!CalculateValidBodyBounds(position, shape, &oldBounds))
    {
        return true;
    }

    double targetPosition[3] = {
        position[0], position[1], position[2]
    };
    targetPosition[axis] += distance;

    VoxelBodyBounds newBounds;
    if (!CalculateValidBodyBounds(targetPosition, shape, &newBounds))
    {
        return true;
    }

    double negativeExtent = axis == 2
        ? shape->eyeHeight
        : shape->radius;
    double positiveExtent = axis == 2
        ? shape->height - shape->eyeHeight
        : shape->radius;
    double epsilon = shape->collisionEpsilon;

    if (distance > 0.0)
    {
        int64_t firstPlane;
        int64_t lastPlane;
        if (!TryFloorToInt64(
                oldBounds.maximum[axis] - epsilon, &firstPlane)
            || !TryFloorToInt64(
                newBounds.maximum[axis] - epsilon, &lastPlane))
        {
            return true;
        }
        ++firstPlane;

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
        int64_t firstPlane;
        int64_t lastPlane;
        if (!TryFloorToInt64(
                oldBounds.minimum[axis] + epsilon, &firstPlane)
            || !TryFloorToInt64(
                newBounds.minimum[axis] + epsilon, &lastPlane))
        {
            return true;
        }
        --firstPlane;

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

bool VoxelBodyMoveAxis(const VoxelCollisionSource* collision,
    double position[3], const VoxelBodyShape* shape,
    int32_t axis, double distance)
{
    if (axis < 0 || axis > 2
        || !IsFiniteBoundedDouble(
            distance, VOXEL_DYNAMIC_COORDINATE_LIMIT))
    {
        return true;
    }
    if (distance == 0.0)
    {
        return false;
    }
    if (collision->queryDynamicColliders == NULL)
    {
        return MoveAxisAgainstBlocks(
            collision, position, shape, axis, distance);
    }

    VoxelBodyBounds oldBounds;
    if (!CalculateValidBodyBounds(position, shape, &oldBounds))
    {
        return true;
    }
    double requestedPosition[3] = {
        position[0], position[1], position[2]
    };
    requestedPosition[axis] += distance;
    VoxelBodyBounds requestedBounds;
    if (!CalculateValidBodyBounds(
            requestedPosition, shape, &requestedBounds))
    {
        return true;
    }

    VoxelBodyBounds queryBounds;
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
    {
        // Narrowphase reports an exact end contact at one epsilon of
        // separation. A strict-overlap broadphase needs one more epsilon on
        // the movement axis or it can omit that touching collider entirely.
        double padding = shape->collisionEpsilon
            * (currentAxis == (uint32_t)axis ? 2.0 : 1.0);
        queryBounds.minimum[currentAxis] = MinimumDouble(
            oldBounds.minimum[currentAxis],
            requestedBounds.minimum[currentAxis])
            - padding;
        queryBounds.maximum[currentAxis] = MaximumDouble(
            oldBounds.maximum[currentAxis],
            requestedBounds.maximum[currentAxis])
            + padding;
    }

    DynamicColliderBatch batch;
    if (!QueryDynamicColliderBatch(collision, &queryBounds, &batch))
    {
        // Unknown colliders never permit movement into an unchecked region.
        return true;
    }

    double clippedPosition[3] = {
        position[0], position[1], position[2]
    };
    bool collided = MoveAxisAgainstBlocks(
        collision, clippedPosition, shape, axis, distance);
    VoxelBodyBounds clippedBounds;
    VoxelBodyCalculateBounds(clippedPosition, shape, &clippedBounds);

    double epsilon = shape->collisionEpsilon;
    double negativeExtent = axis == 2
        ? shape->eyeHeight
        : shape->radius;
    double positiveExtent = axis == 2
        ? shape->height - shape->eyeHeight
        : shape->radius;
    uint64_t clippingStableId = 0u;

    for (uint32_t index = 0u; index < batch.count; ++index)
    {
        const VoxelDynamicCollider* collider = &batch.colliders[index];
        if (!BoundsOverlapDynamicOnOtherAxes(
                &oldBounds, collider, axis, epsilon))
        {
            continue;
        }

        if (BoundsOverlapDynamicCollider(
                &oldBounds, collider, epsilon))
        {
            // Starting penetration is ambiguous. Do not make it deeper or
            // tunnel through the opposite face in one axis operation.
            clippedPosition[axis] = position[axis];
            collided = true;
            clippingStableId = collider->stableId;
            break;
        }

        if (distance > 0.0)
        {
            double collisionPlane =
                collider->bounds.minimum[axis] - epsilon;
            if (oldBounds.maximum[axis] <= collisionPlane
                && clippedBounds.maximum[axis] >= collisionPlane)
            {
                double candidate = collisionPlane - positiveExtent;
                if (candidate < clippedPosition[axis]
                    || (candidate == clippedPosition[axis]
                        && (clippingStableId == 0u
                            || collider->stableId < clippingStableId)))
                {
                    clippedPosition[axis] = candidate;
                    clippedBounds.maximum[axis] = collisionPlane;
                    clippedBounds.minimum[axis] =
                        candidate - negativeExtent;
                    clippingStableId = collider->stableId;
                }
                collided = true;
            }
        }
        else
        {
            double collisionPlane =
                collider->bounds.maximum[axis] + epsilon;
            if (oldBounds.minimum[axis] >= collisionPlane
                && clippedBounds.minimum[axis] <= collisionPlane)
            {
                double candidate = collisionPlane + negativeExtent;
                if (candidate > clippedPosition[axis]
                    || (candidate == clippedPosition[axis]
                        && (clippingStableId == 0u
                            || collider->stableId < clippingStableId)))
                {
                    clippedPosition[axis] = candidate;
                    clippedBounds.minimum[axis] = collisionPlane;
                    clippedBounds.maximum[axis] =
                        candidate + positiveExtent;
                    clippingStableId = collider->stableId;
                }
                collided = true;
            }
        }
    }

    position[axis] = clippedPosition[axis];
    return collided;
}

static void SetFailClosedGroundContact(VoxelGroundContact* outContact)
{
    outContact->friction = 1.0f;
    outContact->supported = true;
    outContact->surfaceVelocity[0] = 0.0;
    outContact->surfaceVelocity[1] = 0.0;
    outContact->surfaceVelocity[2] = 0.0;
    outContact->surfaceStableId = 0u;
}

void VoxelBodyQueryGroundContact(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, VoxelGroundContact* outContact)
{
    outContact->friction = 0.0f;
    outContact->supported = false;
    outContact->surfaceVelocity[0] = 0.0;
    outContact->surfaceVelocity[1] = 0.0;
    outContact->surfaceVelocity[2] = 0.0;
    outContact->surfaceStableId = 0u;

    VoxelBodyBounds bounds;
    if (!IsFiniteBoundedDouble(
            probeDepth, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || probeDepth < 0.0
        || !CalculateValidBodyBounds(position, shape, &bounds))
    {
        SetFailClosedGroundContact(outContact);
        return;
    }

    double footprintMinimumX =
        bounds.minimum[0] + shape->collisionEpsilon;
    double footprintMaximumX =
        bounds.maximum[0] - shape->collisionEpsilon;
    double footprintMinimumY =
        bounds.minimum[1] + shape->collisionEpsilon;
    double footprintMaximumY =
        bounds.maximum[1] - shape->collisionEpsilon;
    int64_t minimumX;
    int64_t maximumX;
    int64_t minimumY;
    int64_t maximumY;
    int64_t supportZ;
    if (!TryFloorToInt64(footprintMinimumX, &minimumX)
        || !TryFloorToInt64(footprintMaximumX, &maximumX)
        || !TryFloorToInt64(footprintMinimumY, &minimumY)
        || !TryFloorToInt64(footprintMaximumY, &maximumY)
        || !TryFloorToInt64(
            bounds.minimum[2] - probeDepth, &supportZ))
    {
        SetFailClosedGroundContact(outContact);
        return;
    }

    double weightedFriction = 0.0;
    double supportedArea = 0.0;

    for (int64_t y = minimumY; y <= maximumY; ++y)
    {
        for (int64_t x = minimumX; x <= maximumX; ++x)
        {
            VoxelBlockPhysics block;
            QueryBlockPhysics(collision, x, y, supportZ, &block);
            if ((block.flags & VOXEL_BLOCK_PHYSICS_SOLID) == 0u)
            {
                continue;
            }

            double blockMinimumX = (double)x;
            double blockMaximumX = blockMinimumX + 1.0;
            double blockMinimumY = (double)y;
            double blockMaximumY = blockMinimumY + 1.0;
            double overlapMinimumX = footprintMinimumX > blockMinimumX
                ? footprintMinimumX : blockMinimumX;
            double overlapMaximumX = footprintMaximumX < blockMaximumX
                ? footprintMaximumX : blockMaximumX;
            double overlapMinimumY = footprintMinimumY > blockMinimumY
                ? footprintMinimumY : blockMinimumY;
            double overlapMaximumY = footprintMaximumY < blockMaximumY
                ? footprintMaximumY : blockMaximumY;
            double width = overlapMaximumX - overlapMinimumX;
            double height = overlapMaximumY - overlapMinimumY;
            if (width <= 0.0 || height <= 0.0)
            {
                continue;
            }

            double friction = block.friction >= 0.0f
                    && block.friction <= 1.0f
                ? (double)block.friction : 0.0;
            double area = width * height;
            weightedFriction += friction * area;
            supportedArea += area;
        }
    }

    if (supportedArea > 0.0)
    {
        outContact->friction =
            (float)(weightedFriction / supportedArea);
        outContact->supported = true;
    }

    if (collision->queryDynamicColliders == NULL)
    {
        return;
    }

    double feet = bounds.minimum[2];
    double epsilon = shape->collisionEpsilon;
    VoxelBodyBounds queryBounds = {
        {
            footprintMinimumX,
            footprintMinimumY,
            feet - probeDepth - epsilon,
        },
        {
            footprintMaximumX,
            footprintMaximumY,
            feet + epsilon,
        },
    };
    DynamicColliderBatch batch;
    if (!QueryDynamicColliderBatch(collision, &queryBounds, &batch))
    {
        SetFailClosedGroundContact(outContact);
        return;
    }

    double selectedTop = outContact->supported
        ? (double)supportZ + 1.0
        : -VOXEL_DYNAMIC_COORDINATE_LIMIT;
    double selectedArea = supportedArea;
    for (uint32_t index = 0u; index < batch.count; ++index)
    {
        const VoxelDynamicCollider* collider = &batch.colliders[index];
        double top = collider->bounds.maximum[2];
        double gap = feet - top;
        if (gap < -epsilon || gap > probeDepth + epsilon)
        {
            continue;
        }

        double overlapMinimumX = MaximumDouble(
            footprintMinimumX, collider->bounds.minimum[0]);
        double overlapMaximumX = MinimumDouble(
            footprintMaximumX, collider->bounds.maximum[0]);
        double overlapMinimumY = MaximumDouble(
            footprintMinimumY, collider->bounds.minimum[1]);
        double overlapMaximumY = MinimumDouble(
            footprintMaximumY, collider->bounds.maximum[1]);
        double width = overlapMaximumX - overlapMinimumX;
        double height = overlapMaximumY - overlapMinimumY;
        if (width <= 0.0 || height <= 0.0)
        {
            continue;
        }

        double area = width * height;
        bool higher = !outContact->supported
            || top > selectedTop + epsilon;
        bool sameHeight = top >= selectedTop - epsilon
            && top <= selectedTop + epsilon;
        bool betterTie = sameHeight
            && outContact->surfaceStableId != 0u
            && (area > selectedArea
                || (area == selectedArea
                    && collider->stableId
                        < outContact->surfaceStableId));
        if (!higher && !betterTie)
        {
            continue;
        }

        outContact->friction = collider->friction;
        outContact->supported = true;
        outContact->surfaceVelocity[0] = collider->velocity[0];
        outContact->surfaceVelocity[1] = collider->velocity[1];
        outContact->surfaceVelocity[2] = collider->velocity[2];
        outContact->surfaceStableId = collider->stableId;
        selectedTop = top;
        selectedArea = area;
    }
}

bool VoxelBodyHasGroundContact(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth)
{
    VoxelGroundContact contact;
    VoxelBodyQueryGroundContact(
        collision, position, shape, probeDepth, &contact);
    return contact.supported;
}

bool VoxelBodyHasStableGround(const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double supportRadius)
{
    VoxelBodyBounds bodyBounds;
    if (!IsFiniteBoundedDouble(
            probeDepth, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || !IsFiniteBoundedDouble(
            supportRadius, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || probeDepth < 0.0
        || !CalculateValidBodyBounds(
            position, shape, &bodyBounds))
    {
        return true;
    }
    if (supportRadius < 0.0)
    {
        supportRadius = 0.0;
    }
    if (supportRadius > shape->radius)
    {
        supportRadius = shape->radius;
    }

    double feet = bodyBounds.minimum[2];
    int64_t supportZ;
    if (!TryFloorToInt64(feet - probeDepth, &supportZ))
    {
        return true;
    }
    const double offsets[3] = {
        -supportRadius, 0.0, supportRadius
    };

    for (uint32_t yIndex = 0; yIndex < 3u; ++yIndex)
    {
        for (uint32_t xIndex = 0; xIndex < 3u; ++xIndex)
        {
            int64_t x;
            int64_t y;
            if (!TryFloorToInt64(
                    position[0] + offsets[xIndex], &x)
                || !TryFloorToInt64(
                    position[1] + offsets[yIndex], &y))
            {
                return true;
            }
            if (IsSolidBlock(collision, x, y, supportZ))
            {
                return true;
            }
        }
    }
    if (collision->queryDynamicColliders == NULL)
    {
        return false;
    }

    double epsilon = shape->collisionEpsilon;
    VoxelBodyBounds queryBounds = {
        {
            position[0] - supportRadius - epsilon,
            position[1] - supportRadius - epsilon,
            feet - probeDepth - epsilon,
        },
        {
            position[0] + supportRadius + epsilon,
            position[1] + supportRadius + epsilon,
            feet + epsilon,
        },
    };
    DynamicColliderBatch batch;
    if (!QueryDynamicColliderBatch(collision, &queryBounds, &batch))
    {
        return true;
    }

    for (uint32_t index = 0u; index < batch.count; ++index)
    {
        const VoxelDynamicCollider* collider = &batch.colliders[index];
        double gap = feet - collider->bounds.maximum[2];
        if (gap < -epsilon || gap > probeDepth + epsilon)
        {
            continue;
        }
        for (uint32_t yIndex = 0u; yIndex < 3u; ++yIndex)
        {
            double y = position[1] + offsets[yIndex];
            if (y <= collider->bounds.minimum[1] + epsilon
                || y >= collider->bounds.maximum[1] - epsilon)
            {
                continue;
            }
            for (uint32_t xIndex = 0u; xIndex < 3u; ++xIndex)
            {
                double x = position[0] + offsets[xIndex];
                if (x > collider->bounds.minimum[0] + epsilon
                    && x < collider->bounds.maximum[0] - epsilon)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool VoxelBodyOverlapsBlock(const double position[3],
    const VoxelBodyShape* shape, const int64_t block[3])
{
    VoxelBodyBounds bounds;
    if (!CalculateValidBodyBounds(position, shape, &bounds))
    {
        return true;
    }

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
    double probeDepth, double xOffset, double yOffset,
    const DynamicColliderBatch* dynamicColliders)
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
    if (!CalculateValidBodyBounds(
            shiftedPosition, shape, &bounds))
    {
        return false;
    }

    double epsilon = shape->collisionEpsilon;
    int64_t minimumX;
    int64_t maximumX;
    int64_t minimumY;
    int64_t maximumY;
    int64_t minimumZ;
    int64_t maximumZ;
    if (!TryFloorToInt64(bounds.minimum[0] + epsilon, &minimumX)
        || !TryFloorToInt64(bounds.maximum[0] - epsilon, &maximumX)
        || !TryFloorToInt64(bounds.minimum[1] + epsilon, &minimumY)
        || !TryFloorToInt64(bounds.maximum[1] - epsilon, &maximumY)
        || !TryFloorToInt64(
            bounds.minimum[2] - probeDepth + epsilon, &minimumZ)
        || !TryFloorToInt64(
            bounds.minimum[2] - epsilon, &maximumZ))
    {
        return false;
    }

    for (int64_t z = minimumZ; z <= maximumZ; ++z)
    {
        for (int64_t y = minimumY; y <= maximumY; ++y)
        {
            for (int64_t x = minimumX; x <= maximumX; ++x)
            {
                if (IsSolidBlock(collision, x, y, z))
                {
                    return true;
                }
            }
        }
    }

    if (dynamicColliders != NULL)
    {
        double footprintMinimumX = bounds.minimum[0] + epsilon;
        double footprintMaximumX = bounds.maximum[0] - epsilon;
        double footprintMinimumY = bounds.minimum[1] + epsilon;
        double footprintMaximumY = bounds.maximum[1] - epsilon;
        double feet = bounds.minimum[2];
        for (uint32_t index = 0u;
            index < dynamicColliders->count; ++index)
        {
            const VoxelDynamicCollider* collider =
                &dynamicColliders->colliders[index];
            double gap = feet - collider->bounds.maximum[2];
            if (gap < -epsilon || gap > probeDepth + epsilon)
            {
                continue;
            }
            if (footprintMaximumX
                    > collider->bounds.minimum[0] + epsilon
                && footprintMinimumX
                    < collider->bounds.maximum[0] - epsilon
                && footprintMaximumY
                    > collider->bounds.minimum[1] + epsilon
                && footprintMinimumY
                    < collider->bounds.maximum[1] - epsilon)
            {
                return true;
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
    if (collision == NULL || collision->queryBlockPhysics == NULL
        || position == NULL || shape == NULL
        || xDistance == NULL || yDistance == NULL)
    {
        return;
    }

    VoxelBodyBounds currentBounds;
    if (!IsFiniteBoundedDouble(
            probeDepth, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || !IsFiniteBoundedDouble(
            *xDistance, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || !IsFiniteBoundedDouble(
            *yDistance, VOXEL_DYNAMIC_COORDINATE_LIMIT)
        || !CalculateValidBodyBounds(
            position, shape, &currentBounds))
    {
        *xDistance = 0.0;
        *yDistance = 0.0;
        return;
    }

    DynamicColliderBatch dynamicColliders;
    const DynamicColliderBatch* dynamicColliderView = NULL;
    if (collision->queryDynamicColliders != NULL)
    {
        double xMinimumOffset = MinimumDouble(0.0, *xDistance);
        double xMaximumOffset = MaximumDouble(0.0, *xDistance);
        double yMinimumOffset = MinimumDouble(0.0, *yDistance);
        double yMaximumOffset = MaximumDouble(0.0, *yDistance);
        VoxelBodyBounds queryBounds = {
            {
                currentBounds.minimum[0] + xMinimumOffset,
                currentBounds.minimum[1] + yMinimumOffset,
                currentBounds.minimum[2] - probeDepth
                    - shape->collisionEpsilon,
            },
            {
                currentBounds.maximum[0] + xMaximumOffset,
                currentBounds.maximum[1] + yMaximumOffset,
                currentBounds.minimum[2] + shape->collisionEpsilon,
            },
        };
        if (!QueryDynamicColliderBatch(
                collision, &queryBounds, &dynamicColliders))
        {
            *xDistance = 0.0;
            *yDistance = 0.0;
            return;
        }
        dynamicColliderView = &dynamicColliders;
    }

    // Уже падающее или вытолкнутое тело не приклеивается обратно к краю.
    if (!HasSupportBelowOffset(
            collision, position, shape, probeDepth, 0.0, 0.0,
            dynamicColliderView))
    {
        return;
    }

    // Vanilla Minecraft уменьшает компоненты движения небольшими шагами,
    // сначала отдельно, затем диагональ. Это сохраняет скольжение вдоль края.
    const double reductionStep = 0.05;
    double x = *xDistance;
    double y = *yDistance;

    while (x != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, x, 0.0,
            dynamicColliderView))
    {
        x = ReduceSneakDistance(x, reductionStep);
    }

    while (y != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, 0.0, y,
            dynamicColliderView))
    {
        y = ReduceSneakDistance(y, reductionStep);
    }

    while (x != 0.0 && y != 0.0 && !HasSupportBelowOffset(
            collision, position, shape, probeDepth, x, y,
            dynamicColliderView))
    {
        x = ReduceSneakDistance(x, reductionStep);
        y = ReduceSneakDistance(y, reductionStep);
    }

    *xDistance = x;
    *yDistance = y;
}
