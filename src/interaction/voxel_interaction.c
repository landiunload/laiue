#include "interaction/voxel_interaction.h"

#include "interaction/voxel_raycast.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool InteractionFiniteFloat(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

static bool InteractionFiniteDouble(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) !=
        UINT64_C(0x7ff0000000000000);
}

static bool BlockingBodiesValid(const PhysicalConstructAabb* bodies,
    uint32_t count)
{
    if (count > PHYSICAL_CONSTRUCT_MAX_DYNAMIC_BLOCKERS ||
        (count != 0U && bodies == NULL))
    {
        return false;
    }
    for (uint32_t index = 0U; index < count; ++index)
    {
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            if (!InteractionFiniteDouble(bodies[index].minimum[axis]) ||
                !InteractionFiniteDouble(bodies[index].maximum[axis]) ||
                bodies[index].minimum[axis] >= bodies[index].maximum[axis])
            {
                return false;
            }
        }
    }
    return true;
}

static bool CheckedAddInt64(int64_t value, int8_t offset,
    int64_t* output)
{
    if ((offset > 0 && value == INT64_MAX) ||
        (offset < 0 && value == INT64_MIN))
    {
        return false;
    }
    *output = value + offset;
    return true;
}

static bool CheckedAddInt32(int32_t value, int8_t offset,
    int32_t* output)
{
    if ((offset > 0 && value == INT32_MAX) ||
        (offset < 0 && value == INT32_MIN))
    {
        return false;
    }
    *output = value + offset;
    return true;
}

static bool AabbOverlaps(const PhysicalConstructAabb* left,
    const PhysicalConstructAabb* right)
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (left->maximum[axis] <= right->minimum[axis] ||
            left->minimum[axis] >= right->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

bool VoxelInteractionRaycastScene(World* world,
    const PhysicalConstructSystem* constructs,
    const double origin[3], const float direction[3],
    float maximumDistance, VoxelSceneHit* outHit)
{
    if (world == NULL || origin == NULL || direction == NULL ||
        outHit == NULL || !InteractionFiniteFloat(maximumDistance) ||
        maximumDistance <= 0.0f ||
        maximumDistance > VOXEL_RAYCAST_MAX_DISTANCE)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!InteractionFiniteDouble(origin[axis]) ||
            !InteractionFiniteFloat(direction[axis]) ||
            direction[axis] < -1.0f || direction[axis] > 1.0f)
        {
            return false;
        }
    }
    VoxelRaycastHit worldHit;
    memset(&worldHit, 0, sizeof(worldHit));
    bool hasWorld = VoxelRaycast(
        world, origin, direction, maximumDistance, &worldHit);
    double rayDirection[3] = {
        (double)direction[0],
        (double)direction[1],
        (double)direction[2],
    };
    PhysicalConstructRaycastHit constructHit;
    memset(&constructHit, 0, sizeof(constructHit));
    bool hasConstruct = constructs != NULL &&
        PhysicalConstructRaycast(constructs, origin, rayDirection,
            (double)maximumDistance, &constructHit);
    if (!hasWorld && !hasConstruct) return false;

    memset(outHit, 0, sizeof(*outHit));
    // Static world geometry wins an exact distance tie. This keeps an
    // imported/invalid overlap from exposing a dynamic handle through terrain.
    if (hasConstruct &&
        (!hasWorld || constructHit.distance < worldHit.distance))
    {
        outHit->distance = constructHit.distance;
        outHit->bodyId = constructHit.bodyId;
        outHit->localBlock[0] = constructHit.localBlock[0];
        outHit->localBlock[1] = constructHit.localBlock[1];
        outHit->localBlock[2] = constructHit.localBlock[2];
        outHit->normal[0] = constructHit.normal[0];
        outHit->normal[1] = constructHit.normal[1];
        outHit->normal[2] = constructHit.normal[2];
        outHit->material = constructHit.material;
        if (constructHit.hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE)
            outHit->kind = VOXEL_SCENE_HIT_LEVER_HANDLE;
        else if (constructHit.hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_BASE)
            outHit->kind = VOXEL_SCENE_HIT_LEVER_BASE;
        else
            outHit->kind = VOXEL_SCENE_HIT_CONSTRUCT_VOXEL;
        return true;
    }

    outHit->kind = VOXEL_SCENE_HIT_WORLD;
    outHit->distance = worldHit.distance;
    memcpy(outHit->normal, worldHit.normal, sizeof(outHit->normal));
    memcpy(outHit->worldBlock, worldHit.block,
        sizeof(outHit->worldBlock));
    outHit->material = WorldGetBlock(world,
        worldHit.block[0], worldHit.block[1], worldHit.block[2]);
    return true;
}

bool VoxelInteractionTryCreateEdit(
    World* world, PhysicalConstructSystem* constructs,
    const double origin[3], const float direction[3],
    const PhysicalConstructAabb* blockingBodies,
    uint32_t blockingBodyCount,
    bool breakPressed, bool placePressed, uint16_t placementItem,
    float maximumDistance,
    VoxelEdit* outEdit)
{
    if (outEdit == NULL || world == NULL ||
        !BlockingBodiesValid(blockingBodies, blockingBodyCount))
    {
        return false;
    }
    memset(outEdit, 0, sizeof(*outEdit));
    outEdit->type = VOXEL_EDIT_NONE;
    if (!breakPressed && !placePressed)
    {
        return false;
    }

    VoxelSceneHit hit;
    if (!VoxelInteractionRaycastScene(
            world, constructs, origin, direction,
            maximumDistance, &hit))
    {
        return false;
    }

    if ((hit.kind == VOXEL_SCENE_HIT_LEVER_HANDLE ||
         hit.kind == VOXEL_SCENE_HIT_LEVER_BASE) &&
        !breakPressed)
    {
        return false;
    }

    if (breakPressed)
    {
        outEdit->type = VOXEL_EDIT_BREAK;
        // A lever is an attachment rather than a world BlockType. Earth is a
        // shared non-zero hardness proxy for client/server break timing; the
        // actual kind and material are returned by PhysicalConstructBreakBlock.
        outEdit->original =
            hit.kind == VOXEL_SCENE_HIT_LEVER_HANDLE ||
                    hit.kind == VOXEL_SCENE_HIT_LEVER_BASE
                ? BLOCK_EARTH
                : hit.material;
        outEdit->replacement = BLOCK_AIR;
        if (hit.kind == VOXEL_SCENE_HIT_WORLD)
        {
            outEdit->space = VOXEL_EDIT_SPACE_WORLD;
            memcpy(outEdit->block, hit.worldBlock,
                sizeof(outEdit->block));
        }
        else
        {
            outEdit->space = VOXEL_EDIT_SPACE_CONSTRUCT;
            outEdit->bodyId = hit.bodyId;
            memcpy(outEdit->localBlock, hit.localBlock,
                sizeof(outEdit->localBlock));
        }
        return true;
    }

    if (placementItem == VOXEL_INTERACTION_PHYSICS_LEVER_ITEM)
    {
        if (hit.kind != VOXEL_SCENE_HIT_WORLD) return false;
        outEdit->type = VOXEL_EDIT_ACTIVATE_LEVER;
        outEdit->space = VOXEL_EDIT_SPACE_WORLD;
        memcpy(outEdit->block, hit.worldBlock, sizeof(outEdit->block));
        memcpy(outEdit->mountNormal, hit.normal,
            sizeof(outEdit->mountNormal));
        return true;
    }

    if (placementItem < BLOCK_EARTH || placementItem > BLOCK_GRASS)
        return false;
    outEdit->type = VOXEL_EDIT_PLACE;
    outEdit->replacement = (BlockType)placementItem;
    if (hit.kind == VOXEL_SCENE_HIT_WORLD)
    {
        outEdit->space = VOXEL_EDIT_SPACE_WORLD;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            if (!CheckedAddInt64(hit.worldBlock[axis], hit.normal[axis],
                    &outEdit->block[axis]))
            {
                return false;
            }
        }
        if (WorldGetBlock(world, outEdit->block[0], outEdit->block[1],
                outEdit->block[2]) != BLOCK_AIR ||
            (constructs != NULL &&
                PhysicalConstructWorldCellOccupied(constructs,
                    outEdit->block[0], outEdit->block[1],
                    outEdit->block[2])))
        {
            return false;
        }
        PhysicalConstructAabb candidate = {
            .minimum = {
                (double)outEdit->block[0],
                (double)outEdit->block[1],
                (double)outEdit->block[2],
            },
            .maximum = {
                (double)outEdit->block[0] + 1.0,
                (double)outEdit->block[1] + 1.0,
                (double)outEdit->block[2] + 1.0,
            },
        };
        for (uint32_t index = 0; index < blockingBodyCount; ++index)
        {
            if (AabbOverlaps(&candidate, &blockingBodies[index]))
                return false;
        }
    }
    else
    {
        outEdit->space = VOXEL_EDIT_SPACE_CONSTRUCT;
        outEdit->bodyId = hit.bodyId;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            if (!CheckedAddInt32(hit.localBlock[axis], hit.normal[axis],
                    &outEdit->localBlock[axis]))
            {
                return false;
            }
        }
    }
    return true;
}
