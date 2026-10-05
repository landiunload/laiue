#include "walk_runtime.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    WALK_VOXEL_SIZE = 1000,
    WALK_BLOCKS_PER_CELL = LAIUE_CHARACTER_LOCAL_CELL_SIZE / WALK_VOXEL_SIZE,
    WALK_REBASE_RADIUS_BLOCKS = 8192,
};

static int64_t FloorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder < 0)
        --quotient;
    return quotient;
}

static bool AddChecked(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static bool MulAddChecked(int64_t left, int64_t multiplier, int64_t addend, int64_t *out)
{
    if (out == NULL || multiplier < 0 || (left > 0 && left > INT64_MAX / multiplier) ||
        (left < 0 && left < INT64_MIN / multiplier))
        return false;
    return AddChecked(left * multiplier, addend, out);
}

static bool PositionAxisToBlock(int64_t cell, int64_t local, int64_t *out)
{
    const int64_t localBlock = FloorDiv(local, WALK_VOXEL_SIZE);
    return MulAddChecked(cell, WALK_BLOCKS_PER_CELL, localBlock, out);
}

uint32_t WalkPositionAxisToBlock(int64_t cell, int64_t local, int64_t *outBlock)
{
    return PositionAxisToBlock(cell, local, outBlock) ? 1u : 0u;
}

static bool WalkIsSolid(const LaiueVoxelProviderV1 *provider, int64_t x, int64_t y, int32_t z);

static bool NormalizeWalkAxis(int64_t *cell, int64_t *local)
{
    if (cell == NULL || local == NULL)
        return false;
    int64_t quotient = *local / LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    int64_t remainder = *local % LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    if (remainder < 0)
    {
        if (quotient == INT64_MIN)
            return false;
        --quotient;
        remainder += LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    if (!AddChecked(*cell, quotient, cell))
        return false;
    *local = remainder;
    return true;
}

static bool AddWalkAxis(LaiueCharacterPositionV1 *position, uint32_t axis, int64_t delta)
{
    if (position == NULL)
        return false;
    if (axis == 0u)
    {
        if (!AddChecked(position->localX, delta, &position->localX))
            return false;
        return NormalizeWalkAxis(&position->cellX, &position->localX);
    }
    if (axis == 1u)
    {
        if (!AddChecked(position->localY, delta, &position->localY))
            return false;
        return NormalizeWalkAxis(&position->cellY, &position->localY);
    }
    return AddChecked(position->localZ, delta, &position->localZ);
}

static bool WalkAxisBlockRange(const LaiueCharacterPositionV1 *position, uint32_t axis,
                               int64_t halfExtent, int64_t *outMinimum, int64_t *outMaximum)
{
    if (position == NULL || outMinimum == NULL || outMaximum == NULL || halfExtent < 0)
        return false;
    int64_t cell = 0;
    int64_t local = 0;
    if (axis == 0u)
    {
        cell = position->cellX;
        local = position->localX;
    }
    else if (axis == 1u)
    {
        cell = position->cellY;
        local = position->localY;
    }
    else if (axis == 2u)
    {
        local = position->localZ;
    }
    else
        return false;
    int64_t centerBlock = 0;
    if (axis == 2u)
    {
        centerBlock = FloorDiv(local, WALK_VOXEL_SIZE);
    }
    else if (!PositionAxisToBlock(cell, local, &centerBlock))
        return false;
    int64_t withinBlock = local % WALK_VOXEL_SIZE;
    if (withinBlock < 0)
        withinBlock += WALK_VOXEL_SIZE;
    int64_t minimumLocal = 0;
    int64_t maximumLocal = 0;
    if (!AddChecked(withinBlock, -halfExtent, &minimumLocal) ||
        !AddChecked(withinBlock, halfExtent, &maximumLocal))
        return false;
    if (halfExtent != 0 && !AddChecked(maximumLocal, -1, &maximumLocal))
        return false;
    const int64_t minimumOffset = FloorDiv(minimumLocal, WALK_VOXEL_SIZE);
    const int64_t maximumOffset = FloorDiv(maximumLocal, WALK_VOXEL_SIZE);
    return AddChecked(centerBlock, minimumOffset, outMinimum) &&
           AddChecked(centerBlock, maximumOffset, outMaximum);
}

static bool WalkRangeCount(int64_t minimum, int64_t maximum, uint32_t *outCount)
{
    if (outCount == NULL || maximum < minimum)
        return false;
    /* A range spanning INT64_MIN cannot be represented by the checked
     * subtraction below. It is also far outside the bounded swept-AABB
     * neighbourhood, so reject it before evaluating -minimum. */
    if (minimum == INT64_MIN)
        return false;
    int64_t distance = 0;
    if (!AddChecked(maximum, -minimum, &distance) || distance > 64)
        return false;
    *outCount = (uint32_t)distance + 1u;
    return true;
}

static bool WalkBlockFacePosition(LaiueCharacterPositionV1 *position, uint32_t axis, int64_t block,
                                  int64_t offset)
{
    if (position == NULL)
        return false;
    if (axis == 2u)
    {
        return MulAddChecked(block, WALK_VOXEL_SIZE, offset, &position->localZ);
    }
    const int64_t blocksPerCell = WALK_BLOCKS_PER_CELL;
    int64_t cell = block / blocksPerCell;
    int64_t remainder = block % blocksPerCell;
    if (remainder < 0)
    {
        --cell;
        remainder += blocksPerCell;
    }
    int64_t local = remainder * WALK_VOXEL_SIZE;
    if (!AddChecked(local, offset, &local))
        return false;
    if (axis == 0u)
    {
        position->cellX = cell;
        position->localX = local;
        return NormalizeWalkAxis(&position->cellX, &position->localX);
    }
    if (axis == 1u)
    {
        position->cellY = cell;
        position->localY = local;
        return NormalizeWalkAxis(&position->cellY, &position->localY);
    }
    return false;
}

static bool WalkAxisCenterBlock(const LaiueCharacterPositionV1 *position, uint32_t axis,
                                int64_t *outBlock)
{
    int64_t minimum = 0;
    int64_t maximum = 0;
    return WalkAxisBlockRange(position, axis, 0, &minimum, &maximum) && minimum == maximum &&
           (*outBlock = minimum, true);
}

static uint32_t WalkSweepAxis(const LaiueVoxelProviderV1 *provider,
                              const LaiueCharacterPositionV1 *position, int64_t halfExtent,
                              uint32_t axis, int64_t delta, LaiueCharacterPositionV1 *outPosition,
                              uint32_t *outCollided)
{
    if (provider == NULL || position == NULL || outPosition == NULL || outCollided == NULL ||
        halfExtent < 0)
        return 0u;
    /* The public caller intentionally reuses one position buffer for the
     * three sequential axes.  Keep an immutable start snapshot so aliasing
     * position/outPosition cannot erase the swept interval before it is
     * inspected. */
    const LaiueCharacterPositionV1 startPosition = *position;
    *outPosition = startPosition;
    *outCollided = 0u;
    if (!AddWalkAxis(outPosition, axis, delta))
        return 0u;
    if (delta == 0)
        return 1u;

    int64_t startCenterBlock = 0;
    if (!WalkAxisCenterBlock(&startPosition, axis, &startCenterBlock))
        return 0u;
    int64_t startRanges[3][2];
    int64_t endRanges[3][2];
    int64_t ranges[3][2];
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
        if (!WalkAxisBlockRange(&startPosition, currentAxis, halfExtent,
                                &startRanges[currentAxis][0], &startRanges[currentAxis][1]) ||
            !WalkAxisBlockRange(outPosition, currentAxis, halfExtent, &endRanges[currentAxis][0],
                                &endRanges[currentAxis][1]))
            return 0u;
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
    {
        if (currentAxis != axis)
        {
            ranges[currentAxis][0] = endRanges[currentAxis][0];
            ranges[currentAxis][1] = endRanges[currentAxis][1];
            continue;
        }
        /* The old implementation inspected only the final AABB.  A fast
         * fixed-step movement could therefore jump over a one-block wall.
         * Sweep the complete integer interval between the start and end
         * occupied ranges, then choose the nearest hit below. */
        ranges[currentAxis][0] = startRanges[currentAxis][0] < endRanges[currentAxis][0]
                                     ? startRanges[currentAxis][0]
                                     : endRanges[currentAxis][0];
        ranges[currentAxis][1] = startRanges[currentAxis][1] > endRanges[currentAxis][1]
                                     ? startRanges[currentAxis][1]
                                     : endRanges[currentAxis][1];
    }
    uint32_t counts[3];
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
        if (!WalkRangeCount(ranges[currentAxis][0], ranges[currentAxis][1], &counts[currentAxis]))
            return 0u;

    bool haveCollision = false;
    int64_t bestBlock = 0;
    for (uint32_t ix = 0u; ix < counts[0]; ++ix)
    {
        int64_t blockX = 0;
        if (!AddChecked(ranges[0][0], (int64_t)ix, &blockX))
            return 0u;
        for (uint32_t iy = 0u; iy < counts[1]; ++iy)
        {
            int64_t blockY = 0;
            if (!AddChecked(ranges[1][0], (int64_t)iy, &blockY))
                return 0u;
            for (uint32_t iz = 0u; iz < counts[2]; ++iz)
            {
                int64_t blockZ = 0;
                if (!AddChecked(ranges[2][0], (int64_t)iz, &blockZ))
                    return 0u;
                if (blockZ < INT32_MIN || blockZ > INT32_MAX ||
                    (axis == 0u && delta > 0 && blockX < startCenterBlock) ||
                    (axis == 1u && delta > 0 && blockY < startCenterBlock) ||
                    (axis == 2u && delta > 0 && blockZ < startCenterBlock) ||
                    (axis == 0u && delta < 0 && blockX > startCenterBlock) ||
                    (axis == 1u && delta < 0 && blockY > startCenterBlock) ||
                    (axis == 2u && delta < 0 && blockZ > startCenterBlock) ||
                    !WalkIsSolid(provider, blockX, blockY, (int32_t)blockZ))
                    continue;
                const int64_t candidate = axis == 0u ? blockX : (axis == 1u ? blockY : blockZ);
                if (!haveCollision || (delta > 0 ? candidate < bestBlock : candidate > bestBlock))
                {
                    haveCollision = true;
                    bestBlock = candidate;
                }
            }
        }
    }
    if (!haveCollision)
        return 1u;
    int64_t offset = 0;
    if (delta > 0)
        offset = -halfExtent;
    else if (!AddChecked(halfExtent, WALK_VOXEL_SIZE, &offset))
        return 0u;
    if (!WalkBlockFacePosition(outPosition, axis, bestBlock, offset))
        return 0u;
    *outCollided = 1u;
    return 1u;
}

static uint32_t BaseGetBlock(const LaiueVoxelCoordV1 *coordinate, LaiueVoxelBlockV1 *outBlock)
{
    if (coordinate == NULL || outBlock == NULL)
        return 0u;
    outBlock->flags = 0u;
    if (coordinate->z > 0)
        outBlock->material = 0u; /* air above the surface */
    else if (coordinate->z == 0)
        outBlock->material = 1u; /* grass */
    else if (coordinate->z >= -3)
        outBlock->material = 2u; /* earth */
    else
        outBlock->material = 3u; /* stone */
    return 1u;
}

uint32_t WalkGetBlock(const LaiueVoxelProviderV1 *provider, const LaiueVoxelCoordV1 *coordinate,
                      LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL)
        return 0u;
    WalkVoxelContext *context = (WalkVoxelContext *)provider->context;
    LaiueVoxelBlockV1 overrideBlock = {0u, 0u};
    uint32_t explicitEdit = 0u;
    if (context != NULL && context->sparse.getBlockState != NULL)
    {
        if (context->sparse.getBlockState(&context->sparse, coordinate, &overrideBlock,
                                          &explicitEdit) == 0u)
        {
            context->providerFailed = true;
            return 0u;
        }
    }
    else if (context != NULL && context->sparse.getBlock != NULL)
    {
        if (context->sparse.getBlock(&context->sparse, coordinate, &overrideBlock) == 0u)
        {
            context->providerFailed = true;
            return 0u;
        }
        /* Compatibility providers predating getBlockState have no explicit
         * air bit, so retain their non-air-only override behavior. */
        explicitEdit = overrideBlock.material != 0u || overrideBlock.flags != 0u;
    }
    /* The sparse module is configured with air as its default. An explicit
     * entry, including air, masks the game-owned infinite strata. */
    if (explicitEdit != 0u)
    {
        *outBlock = overrideBlock;
        return 1u;
    }
    return BaseGetBlock(coordinate, outBlock);
}

static bool WalkIsSolid(const LaiueVoxelProviderV1 *provider, int64_t x, int64_t y, int32_t z)
{
    LaiueVoxelCoordV1 coordinate = {x, y, z};
    LaiueVoxelBlockV1 block = {0u, 0u};
    if (provider == NULL || provider->getBlock == NULL)
        return true;
    /* A provider error is a collision failure, not air.  Conservatively
     * treating it as solid prevents the character from tunnelling through a
     * missing/failed voxel service. */
    if (provider->getBlock(provider, &coordinate, &block) == 0u)
        return true;
    return block.material != 0u;
}

uint32_t WalkSweepAabb(const LaiueCharacterCollisionV1 *collision,
                       const LaiueCharacterPositionV1 *position, int64_t halfExtent, int64_t deltaX,
                       int64_t deltaY, int64_t deltaZ, LaiueCharacterPositionV1 *outPosition,
                       uint32_t *outGrounded)
{
    if (collision == NULL || position == NULL || outPosition == NULL || outGrounded == NULL)
        return 0u;
    const LaiueVoxelProviderV1 *provider = (const LaiueVoxelProviderV1 *)collision->context;
    *outGrounded = 0u;
    if (provider == NULL || halfExtent < 0)
        return 0u;
    LaiueCharacterPositionV1 next = *position;
    uint32_t collided = 0u;
    if (!WalkSweepAxis(provider, &next, halfExtent, 0u, deltaX, &next, &collided) ||
        !WalkSweepAxis(provider, &next, halfExtent, 1u, deltaY, &next, &collided) ||
        !WalkSweepAxis(provider, &next, halfExtent, 2u, deltaZ, &next, &collided))
        return 0u;
    if (deltaZ <= 0)
    {
        int64_t bottom = 0;
        int64_t sample = 0;
        if (!AddChecked(next.localZ, -halfExtent, &bottom) || !AddChecked(bottom, -1, &sample))
            return 0u;
        const int64_t supportBlock = FloorDiv(sample, WALK_VOXEL_SIZE);
        int64_t blockX = 0;
        int64_t blockY = 0;
        if (!PositionAxisToBlock(next.cellX, next.localX, &blockX) ||
            !PositionAxisToBlock(next.cellY, next.localY, &blockY))
            return 0u;
        if (supportBlock >= INT32_MIN && supportBlock <= INT32_MAX &&
            WalkIsSolid(provider, blockX, blockY, (int32_t)supportBlock))
            *outGrounded = 1u;
    }
    *outPosition = next;
    return 1u;
}

bool WalkServiceFieldPresent(uint32_t actualSize, uint32_t declaredSize, size_t offset, size_t size)
{
    return (size_t)actualSize >= offset && (size_t)actualSize - offset >= size &&
           (size_t)declaredSize >= offset && (size_t)declaredSize - offset >= size;
}

static bool WalkBlockShiftToCharacterDelta(int64_t blockShift, int64_t *outCellDelta,
                                           int64_t *outLocalDelta)
{
    if (outCellDelta == NULL || outLocalDelta == NULL)
        return false;
    const int64_t cellShift = FloorDiv(blockShift, WALK_BLOCKS_PER_CELL);
    const int64_t remainder = blockShift - cellShift * WALK_BLOCKS_PER_CELL;
    if (remainder < 0 || remainder >= WALK_BLOCKS_PER_CELL || cellShift == INT64_MIN)
        return false;
    *outCellDelta = -cellShift;
    *outLocalDelta = -remainder * WALK_VOXEL_SIZE;
    return true;
}

uint32_t WalkRebaseWorldAndCharacter(const LaiueVoxelServiceV1 *voxel, uint32_t voxelServiceSize,
                                     LaiueVoxelWorldV1 *world,
                                     const LaiueCharacterServiceV1 *character,
                                     uint32_t characterServiceSize,
                                     LaiueCharacterControllerV1 *controller)
{
    if (voxel == NULL || world == NULL || character == NULL || controller == NULL ||
        voxelServiceSize < 2u * sizeof(uint32_t) || characterServiceSize < 2u * sizeof(uint32_t) ||
        voxel->abiVersion != LAIUE_VOXEL_SERVICE_ABI_VERSION_1 ||
        character->abiVersion != LAIUE_CHARACTER_SERVICE_ABI_VERSION_1 ||
        !WalkServiceFieldPresent(characterServiceSize, character->structSize,
                                 offsetof(LaiueCharacterServiceV1, getPosition),
                                 sizeof(character->getPosition)) ||
        character->getPosition == NULL ||
        !WalkServiceFieldPresent(voxelServiceSize, voxel->structSize,
                                 LAIUE_VOXEL_SERVICE_V1_REBASE_OFFSET, sizeof(voxel->rebase)) ||
        voxel->rebase == NULL ||
        !WalkServiceFieldPresent(characterServiceSize, character->structSize,
                                 LAIUE_CHARACTER_SERVICE_V1_REBASE_ORIGIN_OFFSET,
                                 sizeof(character->rebaseOrigin)) ||
        character->rebaseOrigin == NULL)
        return 1u;

    LaiueCharacterPositionV1 position;
    if (character->getPosition(controller, &position) == 0u)
        return 0u;
    int64_t blockX = 0;
    int64_t blockY = 0;
    if (!PositionAxisToBlock(position.cellX, position.localX, &blockX) ||
        !PositionAxisToBlock(position.cellY, position.localY, &blockY))
        return 0u;
    const bool farX = blockX > WALK_REBASE_RADIUS_BLOCKS || blockX < -WALK_REBASE_RADIUS_BLOCKS;
    const bool farY = blockY > WALK_REBASE_RADIUS_BLOCKS || blockY < -WALK_REBASE_RADIUS_BLOCKS;
    if (!farX && !farY)
        return 1u;

    const int64_t shiftX = farX ? FloorDiv(blockX, 64) * 64 : 0;
    const int64_t shiftY = farY ? FloorDiv(blockY, 64) * 64 : 0;
    int64_t cellDeltaX = 0;
    int64_t cellDeltaY = 0;
    int64_t localDeltaX = 0;
    int64_t localDeltaY = 0;
    if (!WalkBlockShiftToCharacterDelta(shiftX, &cellDeltaX, &localDeltaX) ||
        !WalkBlockShiftToCharacterDelta(shiftY, &cellDeltaY, &localDeltaY))
        return 0u;

    /* Move the consumer first, then the provider.  If the world rejects the
     * shift, restore the character exactly; no fixed-point state is touched by
     * the origin callback. */
    if (character->rebaseOrigin(controller, cellDeltaX, cellDeltaY, localDeltaX, localDeltaY) == 0u)
        return 0u;
    if (voxel->rebase(world, shiftX, shiftY, 0) != 0u)
        return 1u;
    if (character->rebaseOrigin(controller, -cellDeltaX, -cellDeltaY, -localDeltaX, -localDeltaY) ==
        0u)
        return 0u;
    return 0u;
}
