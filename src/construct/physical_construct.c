#include "construct/physical_construct.h"

#include "platform/system.h"
#include "world/world.h"

#include <stddef.h>
#include <string.h>

#define CONSTRUCT_COORDINATE_LIMIT 1048576
#define CONSTRUCT_POSITION_LIMIT 1099511627776.0
#define CONSTRUCT_VELOCITY_LIMIT 1048576.0
#define CONSTRUCT_BINARY_SWEEP_STEPS 16U
#define CONSTRUCT_MAX_STEP_DISTANCE (1.0 / 16.0)
#define CONSTRUCT_MAX_SWEEP_SEGMENTS 64U
#define CONSTRUCT_OCCUPANCY_CAPACITY 512U
#define CONSTRUCT_MAX_COLLIDER_PARTS \
    (PHYSICAL_CONSTRUCT_MAX_BLOCKS + PHYSICAL_CONSTRUCT_MODEL_PARTS - 1U)

_Static_assert((CONSTRUCT_OCCUPANCY_CAPACITY & (CONSTRUCT_OCCUPANCY_CAPACITY - 1U)) == 0U,
               "construct occupancy capacity must be a power of two");
_Static_assert(CONSTRUCT_OCCUPANCY_CAPACITY >= PHYSICAL_CONSTRUCT_MAX_BLOCKS * 2U,
               "construct occupancy must stay at or below 50 percent load");
_Static_assert(CONSTRUCT_MAX_COLLIDER_PARTS <= WORLD_MAX_BOUNDED_BLOCK_RANGES,
               "world range batch must hold every exact construct collider");
_Static_assert(CHUNK_SIZE == (1U << CHUNK_SIZE_LOG2),
               "construct chunk conversion requires the canonical world chunk size");

typedef struct PhysicalConstructBody
{
    bool active;
    uint64_t id;
    uint64_t topologyRevision;
    double origin[3];
    double velocity[3];
    double localMinimum[3];
    double localMaximum[3];
    PhysicalConstructBlock blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    // Open-addressed local-cell occupancy. Entries contain block index + 1;
    // the table stays at or below 50% load for bounded fixed-tick probes.
    uint16_t occupancy[CONSTRUCT_OCCUPANCY_CAPACITY];
    uint32_t blockCount;
    uint64_t grabOwner;
    double grabTarget[3];
    double grabPointLocal[3];
} PhysicalConstructBody;

struct PhysicalConstructSystem
{
    World *world;
    PhysicalConstructConfiguration configuration;
    PhysicalConstructBody bodies[PHYSICAL_CONSTRUCT_MAX_BODIES];
    uint64_t nextBodyId;

    // Shared main-thread scratch. Topology operations are intentionally not
    // re-entrant; the rest of the engine already mutates world state on its
    // main thread.
    int64_t worldScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS][3];
    uint8_t materialScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    PhysicalConstructBlock blockScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    WorldBlockMutation mutationScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    WorldBlockRange worldRangeScratch[CONSTRUCT_MAX_COLLIDER_PARTS];
    uint16_t queueScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    uint8_t componentScratch[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    PhysicalConstructBody splitScratch[PHYSICAL_CONSTRUCT_MAX_BODIES];
};

static bool IsFiniteBounded(double value, double limit)
{
    return value == value && value >= -limit && value <= limit;
}

static bool CheckedAddInt64(int64_t left, int64_t right, int64_t *output)
{
    if (output == NULL || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
    {
        return false;
    }
    *output = left + right;
    return true;
}

static double ClampDouble(double value, double minimum, double maximum)
{
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}

static int64_t FloorDoubleToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

static int32_t FloorDivChunkI32(int32_t value)
{
    int32_t quotient = value / CHUNK_SIZE;
    if (value % CHUNK_SIZE < 0)
    {
        --quotient;
    }
    return quotient;
}

static bool IsUnitNormal(const int8_t normal[3])
{
    if (normal == NULL)
    {
        return false;
    }
    uint32_t nonzero = 0;
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (normal[axis] < -1 || normal[axis] > 1)
        {
            return false;
        }
        if (normal[axis] != 0)
        {
            ++nonzero;
        }
    }
    return nonzero == 1U;
}

static bool ConstructMaterialValid(uint8_t material)
{
    return material == BLOCK_EARTH || material == BLOCK_GRASS;
}

static bool SameLocal(const int32_t left[3], const int32_t right[3])
{
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

static bool SameWorld(const int64_t left[3], const int64_t right[3])
{
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

static bool BlockLess(const PhysicalConstructBlock *left, const PhysicalConstructBlock *right)
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (left->local[axis] != right->local[axis])
        {
            return left->local[axis] < right->local[axis];
        }
    }
    return left->kind < right->kind;
}

static void SortBlocks(PhysicalConstructBlock *blocks, uint32_t count)
{
    for (uint32_t index = 1U; index < count; ++index)
    {
        PhysicalConstructBlock value = blocks[index];
        uint32_t position = index;
        while (position > 0U && BlockLess(&value, &blocks[position - 1U]))
        {
            blocks[position] = blocks[position - 1U];
            --position;
        }
        blocks[position] = value;
    }
}

static uint32_t LocalCoordinateHash(const int32_t local[3])
{
    uint32_t hash = (uint32_t)local[0] * 0x9e3779b1U;
    hash ^= (uint32_t)local[1] * 0x85ebca77U;
    hash ^= (uint32_t)local[2] * 0xc2b2ae3dU;
    hash ^= hash >> 16U;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15U;
    return hash;
}

static void BodyRefreshOccupancy(PhysicalConstructBody *body)
{
    memset(body->occupancy, 0, sizeof(body->occupancy));
    for (uint32_t index = 0U; index < body->blockCount; ++index)
    {
        uint32_t slot =
            LocalCoordinateHash(body->blocks[index].local) & (CONSTRUCT_OCCUPANCY_CAPACITY - 1U);
        while (body->occupancy[slot] != 0U)
        {
            slot = (slot + 1U) & (CONSTRUCT_OCCUPANCY_CAPACITY - 1U);
        }
        body->occupancy[slot] = (uint16_t)(index + 1U);
    }
}

static int32_t BodyOccupancyFind(const PhysicalConstructBody *body, const int32_t local[3])
{
    uint32_t slot = LocalCoordinateHash(local) & (CONSTRUCT_OCCUPANCY_CAPACITY - 1U);
    for (uint32_t probe = 0U; probe < CONSTRUCT_OCCUPANCY_CAPACITY; ++probe)
    {
        uint16_t entry = body->occupancy[slot];
        if (entry == 0U)
        {
            return -1;
        }
        uint32_t index = (uint32_t)entry - 1U;
        if (index < body->blockCount && SameLocal(body->blocks[index].local, local))
        {
            return (int32_t)index;
        }
        slot = (slot + 1U) & (CONSTRUCT_OCCUPANCY_CAPACITY - 1U);
    }
    return -1;
}

static int32_t FindBlockIndex(const PhysicalConstructBody *body, const int32_t local[3])
{
    uint32_t low = 0U;
    uint32_t high = body->blockCount;
    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2U;
        const PhysicalConstructBlock *block = &body->blocks[middle];
        bool less = false;
        bool greater = false;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            if (block->local[axis] < local[axis])
            {
                less = true;
                break;
            }
            if (block->local[axis] > local[axis])
            {
                greater = true;
                break;
            }
        }
        if (less)
        {
            low = middle + 1U;
        }
        else if (greater)
        {
            high = middle;
        }
        else
        {
            return (int32_t)middle;
        }
    }
    return -1;
}

static PhysicalConstructBody *FindBody(PhysicalConstructSystem *system, uint64_t id)
{
    if (system == NULL || id == 0U)
    {
        return NULL;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        if (system->bodies[index].active && system->bodies[index].id == id)
        {
            return &system->bodies[index];
        }
    }
    return NULL;
}

static const PhysicalConstructBody *FindBodyConst(const PhysicalConstructSystem *system,
                                                  uint64_t id)
{
    return FindBody((PhysicalConstructSystem *)system, id);
}

static PhysicalConstructBody *FindFreeBody(PhysicalConstructSystem *system)
{
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        if (!system->bodies[index].active)
        {
            return &system->bodies[index];
        }
    }
    return NULL;
}

static uint32_t ActiveBodyCount(const PhysicalConstructSystem *system)
{
    uint32_t count = 0U;
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        if (system->bodies[index].active)
        {
            ++count;
        }
    }
    return count;
}

static uint32_t SortedBodyIndices(const PhysicalConstructSystem *system, uint8_t output[16])
{
    uint32_t count = 0U;
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        if (!system->bodies[index].active)
        {
            continue;
        }
        uint32_t position = count;
        while (position > 0U && system->bodies[output[position - 1U]].id > system->bodies[index].id)
        {
            output[position] = output[position - 1U];
            --position;
        }
        output[position] = (uint8_t)index;
        ++count;
    }
    return count;
}

bool PhysicalConstructGetLeverModel(const int8_t mountNormal[3],
                                    PhysicalConstructModelPart output[3])
{
    if (!IsUnitNormal(mountNormal) || output == NULL)
    {
        return false;
    }

    const double sixteenth = 1.0 / 16.0;
    for (uint32_t part = 0U; part < PHYSICAL_CONSTRUCT_MODEL_PARTS; ++part)
    {
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            uint32_t minimumUnits;
            uint32_t maximumUnits;
            if (mountNormal[axis] == 0)
            {
                static const uint8_t transverseMinimum[3] = {3U, 7U, 6U};
                static const uint8_t transverseMaximum[3] = {13U, 9U, 10U};
                minimumUnits = transverseMinimum[part];
                maximumUnits = transverseMaximum[part];
            }
            else if (mountNormal[axis] > 0)
            {
                static const uint8_t positiveMinimum[3] = {0U, 2U, 11U};
                static const uint8_t positiveMaximum[3] = {2U, 12U, 15U};
                minimumUnits = positiveMinimum[part];
                maximumUnits = positiveMaximum[part];
            }
            else
            {
                static const uint8_t negativeMinimum[3] = {14U, 4U, 1U};
                static const uint8_t negativeMaximum[3] = {16U, 14U, 5U};
                minimumUnits = negativeMinimum[part];
                maximumUnits = negativeMaximum[part];
            }
            output[part].localBounds.minimum[axis] = (double)minimumUnits * sixteenth;
            output[part].localBounds.maximum[axis] = (double)maximumUnits * sixteenth;
        }
        output[part].hitPart =
            part == 0U ? PHYSICAL_CONSTRUCT_HIT_LEVER_BASE : PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE;
    }
    return true;
}

static uint32_t BlockPartCount(const PhysicalConstructBlock *block)
{
    return block->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER ? 3U : 1U;
}

static bool BlockPartLocalBounds(const PhysicalConstructBlock *block, uint32_t part,
                                 PhysicalConstructAabb *outBounds,
                                 PhysicalConstructHitPart *outHitPart)
{
    if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        if (part != 0U || block->material == 0U)
        {
            return false;
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            outBounds->minimum[axis] = (double)block->local[axis];
            outBounds->maximum[axis] = (double)block->local[axis] + 1.0;
        }
        if (outHitPart != NULL)
        {
            *outHitPart = PHYSICAL_CONSTRUCT_HIT_VOXEL;
        }
        return true;
    }
    if (block->kind != PHYSICAL_CONSTRUCT_BLOCK_LEVER || part >= 3U)
    {
        return false;
    }

    PhysicalConstructModelPart model[3];
    if (!PhysicalConstructGetLeverModel(block->mountNormal, model))
    {
        return false;
    }
    *outBounds = model[part].localBounds;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        outBounds->minimum[axis] += (double)block->local[axis];
        outBounds->maximum[axis] += (double)block->local[axis];
    }
    if (outHitPart != NULL)
    {
        *outHitPart = model[part].hitPart;
    }
    return true;
}

static void BodyRefreshBounds(PhysicalConstructBody *body)
{
    SortBlocks(body->blocks, body->blockCount);
    BodyRefreshOccupancy(body);
    bool initialized = false;
    for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
    {
        const PhysicalConstructBlock *block = &body->blocks[blockIndex];
        for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
        {
            PhysicalConstructAabb bounds;
            if (!BlockPartLocalBounds(block, part, &bounds, NULL))
            {
                continue;
            }
            if (!initialized)
            {
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    body->localMinimum[axis] = bounds.minimum[axis];
                    body->localMaximum[axis] = bounds.maximum[axis];
                }
                initialized = true;
            }
            else
            {
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    if (bounds.minimum[axis] < body->localMinimum[axis])
                    {
                        body->localMinimum[axis] = bounds.minimum[axis];
                    }
                    if (bounds.maximum[axis] > body->localMaximum[axis])
                    {
                        body->localMaximum[axis] = bounds.maximum[axis];
                    }
                }
            }
        }
    }
}

static bool AabbOverlaps(const PhysicalConstructAabb *left, const PhysicalConstructAabb *right,
                         double epsilon)
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (left->maximum[axis] <= right->minimum[axis] + epsilon ||
            left->minimum[axis] >= right->maximum[axis] - epsilon)
        {
            return false;
        }
    }
    return true;
}

static bool AabbValid(const PhysicalConstructAabb *bounds)
{
    if (bounds == NULL)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!IsFiniteBounded(bounds->minimum[axis], CONSTRUCT_POSITION_LIMIT) ||
            !IsFiniteBounded(bounds->maximum[axis], CONSTRUCT_POSITION_LIMIT) ||
            bounds->minimum[axis] >= bounds->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

static bool AabbArrayValid(const PhysicalConstructAabb *bounds, uint32_t count)
{
    if (count > PHYSICAL_CONSTRUCT_MAX_DYNAMIC_BLOCKERS || (count > 0U && bounds == NULL))
    {
        return false;
    }
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (!AabbValid(&bounds[index]))
        {
            return false;
        }
    }
    return true;
}

static PhysicalConstructAabb BodyWorldBoundsAt(const PhysicalConstructBody *body,
                                               const double origin[3])
{
    PhysicalConstructAabb bounds;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        bounds.minimum[axis] = origin[axis] + body->localMinimum[axis];
        bounds.maximum[axis] = origin[axis] + body->localMaximum[axis];
    }
    return bounds;
}

static PhysicalConstructAabb BlockPartWorldBoundsAt(const PhysicalConstructBlock *block,
                                                    uint32_t part, const double origin[3])
{
    PhysicalConstructAabb bounds;
    BlockPartLocalBounds(block, part, &bounds, NULL);
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        bounds.minimum[axis] += origin[axis];
        bounds.maximum[axis] += origin[axis];
    }
    return bounds;
}

static bool AabbToWorldRange(const PhysicalConstructSystem *system,
                             const PhysicalConstructAabb *bounds, WorldBlockRange *outRange,
                             bool *outEmpty)
{
    *outEmpty = false;
    double epsilon = system->configuration.collisionEpsilon;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        double minimum = bounds->minimum[axis] + epsilon;
        double maximum = bounds->maximum[axis] - epsilon;
        if (!IsFiniteBounded(minimum, CONSTRUCT_POSITION_LIMIT + CONSTRUCT_COORDINATE_LIMIT + 2.0) ||
            !IsFiniteBounded(maximum, CONSTRUCT_POSITION_LIMIT + CONSTRUCT_COORDINATE_LIMIT + 2.0))
        {
            return false;
        }
        outRange->minimum[axis] = FloorDoubleToInt64(minimum);
        outRange->maximum[axis] = FloorDoubleToInt64(maximum);
        if (outRange->minimum[axis] > outRange->maximum[axis])
        {
            *outEmpty = true;
        }
    }
    return true;
}

static bool AabbOverlapsWorld(const PhysicalConstructSystem *system,
                              const PhysicalConstructAabb *bounds)
{
    WorldBlockRange range;
    bool empty = false;
    if (!AabbToWorldRange(system, bounds, &range, &empty))
    {
        return true;
    }
    if (empty)
    {
        return false;
    }
    bool solid = false;
    return !WorldAnySolidBlockInRanges(system->world, &range, 1U, &solid) || solid;
}

static bool BodyOverlapsAabbAt(const PhysicalConstructSystem *system,
                               const PhysicalConstructBody *body, const double bodyOrigin[3],
                               const PhysicalConstructAabb *bounds)
{
    double epsilon = system->configuration.collisionEpsilon;
    int64_t minimum[3];
    int64_t maximum[3];
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        minimum[axis] = FloorDoubleToInt64(bounds->minimum[axis] - bodyOrigin[axis] + epsilon);
        maximum[axis] = FloorDoubleToInt64(bounds->maximum[axis] - bodyOrigin[axis] - epsilon);
        if (minimum[axis] > maximum[axis])
        {
            return false;
        }
    }
    for (int64_t z = minimum[2]; z <= maximum[2]; ++z)
    {
        for (int64_t y = minimum[1]; y <= maximum[1]; ++y)
        {
            for (int64_t x = minimum[0]; x <= maximum[0]; ++x)
            {
                if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
                    z < INT32_MIN || z > INT32_MAX)
                {
                    continue;
                }
                const int32_t local[3] = {(int32_t)x, (int32_t)y, (int32_t)z};
                int32_t blockIndex = BodyOccupancyFind(body, local);
                if (blockIndex < 0)
                {
                    continue;
                }
                const PhysicalConstructBlock *block = &body->blocks[(uint32_t)blockIndex];
                for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
                {
                    PhysicalConstructAabb bodyBounds =
                        BlockPartWorldBoundsAt(block, part, bodyOrigin);
                    if (AabbOverlaps(bounds, &bodyBounds, epsilon))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool BodiesOverlapAt(const PhysicalConstructSystem *system,
                            const PhysicalConstructBody *left, const double leftOrigin[3],
                            const PhysicalConstructBody *right, const double rightOrigin[3])
{
    double epsilon = system->configuration.collisionEpsilon;
    PhysicalConstructAabb leftBroad = BodyWorldBoundsAt(left, leftOrigin);
    PhysicalConstructAabb rightBroad = BodyWorldBoundsAt(right, rightOrigin);
    if (!AabbOverlaps(&leftBroad, &rightBroad, epsilon))
    {
        return false;
    }

    for (uint32_t blockIndex = 0U; blockIndex < left->blockCount; ++blockIndex)
    {
        const PhysicalConstructBlock *block = &left->blocks[blockIndex];
        for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
        {
            PhysicalConstructAabb bounds = BlockPartWorldBoundsAt(block, part, leftOrigin);
            if (BodyOverlapsAabbAt(system, right, rightOrigin, &bounds))
            {
                return true;
            }
        }
    }
    return false;
}

static bool BodyOverlapsWorldAt(PhysicalConstructSystem *system,
                                const PhysicalConstructBody *body, const double origin[3])
{
    uint32_t rangeCount = 0U;
    for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
    {
        const PhysicalConstructBlock *block = &body->blocks[blockIndex];
        for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
        {
            PhysicalConstructAabb bounds = BlockPartWorldBoundsAt(block, part, origin);
            if (rangeCount >= CONSTRUCT_MAX_COLLIDER_PARTS)
            {
                return true;
            }
            bool empty = false;
            if (!AabbToWorldRange(system, &bounds, &system->worldRangeScratch[rangeCount], &empty))
            {
                return true;
            }
            if (!empty)
            {
                ++rangeCount;
            }
        }
    }
    bool solid = false;
    return !WorldAnySolidBlockInRanges(system->world, system->worldRangeScratch, rangeCount,
                                       &solid) ||
           solid;
}

static bool BodyOverlapsDynamicAt(
    const PhysicalConstructSystem *system, const PhysicalConstructBody *body, const double origin[3],
    const PhysicalConstructDynamicBlocker *blockers, uint32_t blockerCount)
{
    double epsilon = system->configuration.collisionEpsilon;
    PhysicalConstructAabb broad = BodyWorldBoundsAt(body, origin);
    for (uint32_t blockerIndex = 0U; blockerIndex < blockerCount; ++blockerIndex)
    {
        const PhysicalConstructDynamicBlocker *blocker = &blockers[blockerIndex];
        if (blocker->ignoredBodyId == body->id || !AabbOverlaps(&broad, &blocker->bounds, epsilon))
        {
            continue;
        }
        for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
        {
            const PhysicalConstructBlock *block = &body->blocks[blockIndex];
            for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
            {
                PhysicalConstructAabb bounds = BlockPartWorldBoundsAt(block, part, origin);
                if (AabbOverlaps(&bounds, &blocker->bounds, epsilon))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool BodyCollidesAt(PhysicalConstructSystem *system, const PhysicalConstructBody *body,
                           const double origin[3],
                           const PhysicalConstructDynamicBlocker *blockers,
                           uint32_t blockerCount)
{
    if (BodyOverlapsWorldAt(system, body, origin) ||
        BodyOverlapsDynamicAt(system, body, origin, blockers, blockerCount))
    {
        return true;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        const PhysicalConstructBody *other = &system->bodies[index];
        if (!other->active || other == body)
        {
            continue;
        }
        if (BodiesOverlapAt(system, body, origin, other, other->origin))
        {
            return true;
        }
    }
    return false;
}

static bool BodyOverlapsExternal(const PhysicalConstructSystem *system,
                                 const PhysicalConstructBody *body,
                                 const PhysicalConstructAabb *blockers, uint32_t blockerCount)
{
    double epsilon = system->configuration.collisionEpsilon;
    for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
    {
        const PhysicalConstructBlock *block = &body->blocks[blockIndex];
        for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
        {
            PhysicalConstructAabb bounds = BlockPartWorldBoundsAt(block, part, body->origin);
            for (uint32_t blocker = 0U; blocker < blockerCount; ++blocker)
            {
                if (AabbOverlaps(&bounds, &blockers[blocker], epsilon))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool AabbOverlapsAnyBody(const PhysicalConstructSystem *system,
                                const PhysicalConstructAabb *bounds, uint64_t ignoredBody)
{
    double epsilon = system->configuration.collisionEpsilon;
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        const PhysicalConstructBody *body = &system->bodies[index];
        if (!body->active || body->id == ignoredBody)
        {
            continue;
        }
        PhysicalConstructAabb broad = BodyWorldBoundsAt(body, body->origin);
        if (!AabbOverlaps(bounds, &broad, epsilon))
        {
            continue;
        }
        for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
        {
            const PhysicalConstructBlock *block = &body->blocks[blockIndex];
            for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
            {
                PhysicalConstructAabb bodyBounds =
                    BlockPartWorldBoundsAt(block, part, body->origin);
                if (AabbOverlaps(bounds, &bodyBounds, epsilon))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

void PhysicalConstructGetDefaultConfiguration(PhysicalConstructConfiguration *outConfiguration)
{
    if (outConfiguration == NULL)
    {
        return;
    }
    *outConfiguration = (PhysicalConstructConfiguration){
        .fixedStepSeconds = 1.0 / 240.0,
        .gravity = 26.0,
        .collisionEpsilon = 0.001,
        .grabStiffness = 42.0,
        .grabDamping = 13.0,
        .maximumGrabAcceleration = 96.0,
        .maximumSpeed = 12.0,
    };
}

static bool ConfigurationValid(const PhysicalConstructConfiguration *configuration)
{
    return configuration != NULL && IsFiniteBounded(configuration->fixedStepSeconds, 1.0) &&
           configuration->fixedStepSeconds > 0.0 &&
           IsFiniteBounded(configuration->gravity, 1024.0) && configuration->gravity >= 0.0 &&
           IsFiniteBounded(configuration->collisionEpsilon, 0.1) &&
           configuration->collisionEpsilon > 0.0 &&
           IsFiniteBounded(configuration->grabStiffness, 4096.0) &&
           configuration->grabStiffness >= 0.0 &&
           IsFiniteBounded(configuration->grabDamping, 4096.0) &&
           configuration->grabDamping >= 0.0 &&
           IsFiniteBounded(configuration->maximumGrabAcceleration, 65536.0) &&
           configuration->maximumGrabAcceleration > 0.0 &&
           IsFiniteBounded(configuration->maximumSpeed, 4096.0) &&
           configuration->maximumSpeed > 0.0 &&
           configuration->maximumSpeed * configuration->fixedStepSeconds <=
               CONSTRUCT_MAX_STEP_DISTANCE * (double)CONSTRUCT_MAX_SWEEP_SEGMENTS;
}

PhysicalConstructSystem *PhysicalConstructSystemCreate(
    World *world, const PhysicalConstructConfiguration *configuration)
{
    if (world == NULL)
    {
        return NULL;
    }
    PhysicalConstructConfiguration selected;
    if (configuration == NULL)
    {
        PhysicalConstructGetDefaultConfiguration(&selected);
    }
    else
    {
        selected = *configuration;
    }
    if (!ConfigurationValid(&selected))
    {
        return NULL;
    }

    PhysicalConstructSystem *system = PlatformAllocate(sizeof(*system), true);
    if (system == NULL)
    {
        return NULL;
    }
    system->world = world;
    system->configuration = selected;
    system->nextBodyId = 1U;
    return system;
}

void PhysicalConstructSystemDestroy(PhysicalConstructSystem *system)
{
    PlatformFree(system);
}

void PhysicalConstructSystemReset(PhysicalConstructSystem *system)
{
    if (system == NULL)
    {
        return;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        memset(&system->bodies[index], 0, sizeof(system->bodies[index]));
    }
    system->nextBodyId = 1U;
}

static bool WorldStoredEdit(PhysicalConstructSystem *system, const int64_t block[3],
                            bool *outEdited, uint8_t *outMaterial)
{
    WorldBlockState state;
    if (!WorldGetBlockState(system->world, block[0], block[1], block[2], &state))
    {
        return false;
    }
    *outEdited = state.edited;
    *outMaterial = state.edited ? state.block : BLOCK_AIR;
    return true;
}

static bool WorldScratchContains(const PhysicalConstructSystem *system, uint32_t count,
                                 const int64_t worldBlock[3])
{
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (SameWorld(system->worldScratch[index], worldBlock))
        {
            return true;
        }
    }
    return false;
}

static bool CoordinateDifferenceToInt32(int64_t value, int64_t origin, int32_t *output)
{
    if ((origin > 0 && value < INT64_MIN + origin) || (origin < 0 && value > INT64_MAX + origin))
    {
        return false;
    }
    int64_t difference = value - origin;
    if (difference < -CONSTRUCT_COORDINATE_LIMIT || difference > CONSTRUCT_COORDINATE_LIMIT)
    {
        return false;
    }
    *output = (int32_t)difference;
    return true;
}

static bool NextBodyId(const PhysicalConstructSystem *system, uint64_t *output)
{
    if (system->nextBodyId == 0U || system->nextBodyId == UINT64_MAX)
    {
        return false;
    }
    *output = system->nextBodyId;
    return true;
}

PhysicalConstructResult PhysicalConstructActivateLever(
    PhysicalConstructSystem *system, const int64_t rootWorldBlock[3], const int8_t mountNormal[3],
    const PhysicalConstructAabb *externalBlockers, uint32_t externalBlockerCount,
    uint64_t *outBodyId)
{
    if (outBodyId != NULL)
    {
        *outBodyId = 0U;
    }
    if (system == NULL || rootWorldBlock == NULL || !IsUnitNormal(mountNormal) ||
        !AabbArrayValid(externalBlockers, externalBlockerCount))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    PhysicalConstructBody *destination = FindFreeBody(system);
    if (destination == NULL)
    {
        return PHYSICAL_CONSTRUCT_CAPACITY;
    }

    WorldBlockState rootState;
    if (!WorldGetBlockState(system->world, rootWorldBlock[0], rootWorldBlock[1], rootWorldBlock[2],
                            &rootState))
    {
        return PHYSICAL_CONSTRUCT_WORLD_MUTATION_FAILED;
    }
    BlockType rootMaterial = rootState.block;
    if (rootMaterial == BLOCK_AIR)
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    if (!ConstructMaterialValid(rootMaterial))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    int64_t leverWorld[3];
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!CheckedAddInt64(rootWorldBlock[axis], (int64_t)mountNormal[axis], &leverWorld[axis]) ||
            !IsFiniteBounded((double)leverWorld[axis], CONSTRUCT_POSITION_LIMIT))
        {
            return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
        }
    }
    if (WorldGetBlock(system->world, leverWorld[0], leverWorld[1], leverWorld[2]) != BLOCK_AIR)
    {
        return PHYSICAL_CONSTRUCT_OCCUPIED;
    }

    uint32_t capturedCount = 1U;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        system->worldScratch[0][axis] = rootWorldBlock[axis];
    }
    system->materialScratch[0] = rootMaterial;
    uint32_t queueHead = 0U;
    static const int8_t neighborOffsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    while (queueHead < capturedCount)
    {
        int64_t current[3] = {
            system->worldScratch[queueHead][0],
            system->worldScratch[queueHead][1],
            system->worldScratch[queueHead][2],
        };
        ++queueHead;
        for (uint32_t neighbor = 0U; neighbor < 6U; ++neighbor)
        {
            int64_t candidate[3];
            bool representable = true;
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                representable = representable &&
                                CheckedAddInt64(current[axis], (int64_t)neighborOffsets[neighbor][axis],
                                                &candidate[axis]);
            }
            if (!representable)
            {
                continue;
            }
            if (SameWorld(candidate, leverWorld) ||
                WorldScratchContains(system, capturedCount, candidate))
            {
                continue;
            }
            bool edited = false;
            uint8_t material = BLOCK_AIR;
            if (!WorldStoredEdit(system, candidate, &edited, &material))
            {
                return PHYSICAL_CONSTRUCT_WORLD_MUTATION_FAILED;
            }
            if (!edited || material == BLOCK_AIR)
            {
                continue;
            }
            if (!ConstructMaterialValid(material))
            {
                return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
            }
            if (capturedCount >= PHYSICAL_CONSTRUCT_MAX_BLOCKS - 1U)
            {
                return PHYSICAL_CONSTRUCT_CAPACITY;
            }
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                system->worldScratch[capturedCount][axis] = candidate[axis];
            }
            system->materialScratch[capturedCount] = material;
            ++capturedCount;
        }
    }

    PhysicalConstructBody *prepared = &system->splitScratch[0];
    memset(prepared, 0, sizeof(*prepared));
    prepared->active = true;
    prepared->topologyRevision = 1U;
    prepared->blockCount = capturedCount + 1U;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        prepared->origin[axis] = (double)leverWorld[axis];
    }
    prepared->blocks[0].kind = PHYSICAL_CONSTRUCT_BLOCK_LEVER;
    prepared->blocks[0].material = 0U;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        prepared->blocks[0].local[axis] = 0;
        prepared->blocks[0].mountNormal[axis] = mountNormal[axis];
    }
    for (uint32_t index = 0U; index < capturedCount; ++index)
    {
        PhysicalConstructBlock *block = &prepared->blocks[index + 1U];
        block->kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL;
        block->material = system->materialScratch[index];
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            if (!CoordinateDifferenceToInt32(system->worldScratch[index][axis], leverWorld[axis],
                                             &block->local[axis]))
            {
                return PHYSICAL_CONSTRUCT_CAPACITY;
            }
            block->mountNormal[axis] = 0;
        }
    }
    BodyRefreshBounds(prepared);

    if (BodyOverlapsExternal(system, prepared, externalBlockers, externalBlockerCount))
    {
        return PHYSICAL_CONSTRUCT_COLLISION;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        const PhysicalConstructBody *body = &system->bodies[index];
        if (body->active &&
            BodiesOverlapAt(system, prepared, prepared->origin, body, body->origin))
        {
            return PHYSICAL_CONSTRUCT_COLLISION;
        }
    }

    if (!NextBodyId(system, &prepared->id))
    {
        return PHYSICAL_CONSTRUCT_CAPACITY;
    }
    for (uint32_t index = 0U; index < capturedCount; ++index)
    {
        WorldBlockMutation *mutation = &system->mutationScratch[index];
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            mutation->block[axis] = system->worldScratch[index][axis];
        }
        mutation->expected = system->materialScratch[index];
        mutation->replacement = BLOCK_AIR;
    }
    WorldBlockMutation *leverGuard = &system->mutationScratch[capturedCount];
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        leverGuard->block[axis] = leverWorld[axis];
    }
    leverGuard->expected = BLOCK_AIR;
    leverGuard->replacement = BLOCK_AIR;
    if (!WorldApplyBlockBatch(system->world, system->mutationScratch, capturedCount + 1U))
    {
        return PHYSICAL_CONSTRUCT_WORLD_MUTATION_FAILED;
    }
    ++system->nextBodyId;
    memcpy(destination, prepared, sizeof(*destination));
    if (outBodyId != NULL)
    {
        *outBodyId = prepared->id;
    }
    return PHYSICAL_CONSTRUCT_OK;
}

static bool LocalCoordinateValid(const int32_t local[3])
{
    if (local == NULL)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (local[axis] < -CONSTRUCT_COORDINATE_LIMIT || local[axis] > CONSTRUCT_COORDINATE_LIMIT)
        {
            return false;
        }
    }
    return true;
}

static bool LeverMountsVoxel(const PhysicalConstructBlock *lever,
                             const PhysicalConstructBlock *voxel)
{
    if (lever->kind != PHYSICAL_CONSTRUCT_BLOCK_LEVER ||
        voxel->kind != PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (voxel->local[axis] != lever->local[axis] - (int32_t)lever->mountNormal[axis])
        {
            return false;
        }
    }
    return true;
}

static bool BlocksStructurallyConnected(const PhysicalConstructBlock *left,
                                        const PhysicalConstructBlock *right)
{
    if (left->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
    {
        return LeverMountsVoxel(left, right);
    }
    if (right->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
    {
        return LeverMountsVoxel(right, left);
    }
    return left->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL &&
           right->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL;
}

static bool BlockHasNeighbor(const PhysicalConstructBody *body, const int32_t local[3])
{
    static const int8_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    PhysicalConstructBlock candidateBlock = {
        .local = {local[0], local[1], local[2]},
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    for (uint32_t neighbor = 0U; neighbor < 6U; ++neighbor)
    {
        int32_t candidate[3] = {
            local[0] + offsets[neighbor][0],
            local[1] + offsets[neighbor][1],
            local[2] + offsets[neighbor][2],
        };
        int32_t index = FindBlockIndex(body, candidate);
        if (index >= 0 &&
            BlocksStructurallyConnected(&candidateBlock, &body->blocks[(uint32_t)index]))
        {
            return true;
        }
    }
    return false;
}

PhysicalConstructResult PhysicalConstructPlaceBlock(PhysicalConstructSystem *system,
                                                    uint64_t bodyId, const int32_t localBlock[3],
                                                    uint8_t material,
                                                    const PhysicalConstructAabb *externalBlockers,
                                                    uint32_t externalBlockerCount)
{
    if (system == NULL || !ConstructMaterialValid(material) || !LocalCoordinateValid(localBlock) ||
        !AabbArrayValid(externalBlockers, externalBlockerCount))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL)
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    if (body->blockCount >= PHYSICAL_CONSTRUCT_MAX_BLOCKS)
    {
        return PHYSICAL_CONSTRUCT_CAPACITY;
    }
    if (FindBlockIndex(body, localBlock) >= 0)
    {
        return PHYSICAL_CONSTRUCT_OCCUPIED;
    }
    if (!BlockHasNeighbor(body, localBlock))
    {
        return PHYSICAL_CONSTRUCT_NOT_CONNECTED;
    }

    PhysicalConstructAabb candidate;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        candidate.minimum[axis] = body->origin[axis] + (double)localBlock[axis];
        candidate.maximum[axis] = candidate.minimum[axis] + 1.0;
    }
    if (AabbOverlapsWorld(system, &candidate) || AabbOverlapsAnyBody(system, &candidate, bodyId))
    {
        return PHYSICAL_CONSTRUCT_COLLISION;
    }
    for (uint32_t blocker = 0U; blocker < externalBlockerCount; ++blocker)
    {
        if (AabbOverlaps(&candidate, &externalBlockers[blocker],
                         system->configuration.collisionEpsilon))
        {
            return PHYSICAL_CONSTRUCT_COLLISION;
        }
    }

    PhysicalConstructBlock *block = &body->blocks[body->blockCount++];
    *block = (PhysicalConstructBlock){0};
    block->kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL;
    block->material = material;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        block->local[axis] = localBlock[axis];
    }
    if (body->topologyRevision != UINT64_MAX)
    {
        ++body->topologyRevision;
    }
    BodyRefreshBounds(body);
    return PHYSICAL_CONSTRUCT_OK;
}

static uint32_t BuildComponents(PhysicalConstructSystem *system,
                                const PhysicalConstructBlock *blocks, uint32_t count)
{
    for (uint32_t index = 0U; index < count; ++index)
    {
        system->componentScratch[index] = UINT8_MAX;
    }
    uint32_t componentCount = 0U;
    static const int8_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    for (uint32_t seed = 0U; seed < count; ++seed)
    {
        if (system->componentScratch[seed] != UINT8_MAX)
        {
            continue;
        }
        uint32_t head = 0U;
        uint32_t tail = 1U;
        system->queueScratch[0] = (uint16_t)seed;
        system->componentScratch[seed] = (uint8_t)componentCount;
        while (head < tail)
        {
            uint32_t currentIndex = system->queueScratch[head++];
            const PhysicalConstructBlock *current = &blocks[currentIndex];
            for (uint32_t neighbor = 0U; neighbor < 6U; ++neighbor)
            {
                int32_t wanted[3] = {
                    current->local[0] + offsets[neighbor][0],
                    current->local[1] + offsets[neighbor][1],
                    current->local[2] + offsets[neighbor][2],
                };
                for (uint32_t candidate = 0U; candidate < count; ++candidate)
                {
                    if (system->componentScratch[candidate] == UINT8_MAX &&
                        SameLocal(blocks[candidate].local, wanted) &&
                        BlocksStructurallyConnected(current, &blocks[candidate]))
                    {
                        system->componentScratch[candidate] = (uint8_t)componentCount;
                        system->queueScratch[tail++] = (uint16_t)candidate;
                        break;
                    }
                }
            }
        }
        ++componentCount;
    }
    return componentCount;
}

static uint32_t FreeBodyCount(const PhysicalConstructSystem *system)
{
    return PHYSICAL_CONSTRUCT_MAX_BODIES - ActiveBodyCount(system);
}

static void SortIds(uint64_t *values, uint32_t count)
{
    for (uint32_t index = 1U; index < count; ++index)
    {
        uint64_t value = values[index];
        uint32_t position = index;
        while (position > 0U && values[position - 1U] > value)
        {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
}

PhysicalConstructResult PhysicalConstructBreakBlock(PhysicalConstructSystem *system,
                                                    uint64_t bodyId, const int32_t localBlock[3],
                                                    PhysicalConstructBlock *outRemoved,
                                                    uint64_t *outBodyIds, uint32_t bodyIdCapacity,
                                                    uint32_t *outBodyIdCount)
{
    if (outBodyIdCount != NULL)
    {
        *outBodyIdCount = 0U;
    }
    if (system == NULL || !LocalCoordinateValid(localBlock) ||
        (bodyIdCapacity > 0U && outBodyIds == NULL))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL)
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    int32_t removedIndex = FindBlockIndex(body, localBlock);
    if (removedIndex < 0)
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    PhysicalConstructBlock removed = body->blocks[(uint32_t)removedIndex];

    uint32_t remainingCount = 0U;
    for (uint32_t index = 0U; index < body->blockCount; ++index)
    {
        if (index != (uint32_t)removedIndex)
        {
            system->blockScratch[remainingCount++] = body->blocks[index];
        }
    }
    if (remainingCount == 0U)
    {
        memset(body, 0, sizeof(*body));
        if (outRemoved != NULL)
        {
            *outRemoved = removed;
        }
        return PHYSICAL_CONSTRUCT_OK;
    }

    uint32_t componentCount = BuildComponents(system, system->blockScratch, remainingCount);
    if (componentCount > 1U && FreeBodyCount(system) < componentCount - 1U)
    {
        return PHYSICAL_CONSTRUCT_CAPACITY;
    }
    if (outBodyIds != NULL && bodyIdCapacity < componentCount)
    {
        return PHYSICAL_CONSTRUCT_CAPACITY;
    }

    uint32_t parentComponent = 0U;
    bool hasLever = false;
    for (uint32_t index = 0U; index < remainingCount; ++index)
    {
        if (system->blockScratch[index].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
        {
            parentComponent = system->componentScratch[index];
            hasLever = true;
            break;
        }
    }
    (void)hasLever;

    double oldOrigin[3] = {body->origin[0], body->origin[1], body->origin[2]};
    double oldVelocity[3] = {body->velocity[0], body->velocity[1], body->velocity[2]};
    double oldGrabTarget[3] = {body->grabTarget[0], body->grabTarget[1], body->grabTarget[2]};
    uint64_t oldId = body->id;
    uint64_t oldRevision = body->topologyRevision;
    uint64_t oldGrabOwner = body->grabOwner;
    uint64_t nextId = system->nextBodyId;

    for (uint32_t component = 0U; component < componentCount; ++component)
    {
        PhysicalConstructBody *output = &system->splitScratch[component];
        memset(output, 0, sizeof(*output));
        output->active = true;
        output->id = component == parentComponent ? oldId : nextId++;
        if (output->id == 0U || output->id == UINT64_MAX)
        {
            return PHYSICAL_CONSTRUCT_CAPACITY;
        }
        output->topologyRevision = component == parentComponent
                                       ? (oldRevision == UINT64_MAX ? UINT64_MAX : oldRevision + 1U)
                                       : 1U;
        int32_t anchor[3] = {0, 0, 0};
        bool componentHasLever = false;
        uint32_t firstIndex = UINT32_MAX;
        for (uint32_t index = 0U; index < remainingCount; ++index)
        {
            if (system->componentScratch[index] != component)
            {
                continue;
            }
            if (firstIndex == UINT32_MAX)
            {
                firstIndex = index;
            }
            if (system->blockScratch[index].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
            {
                componentHasLever = true;
            }
        }
        if (!componentHasLever)
        {
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                anchor[axis] = system->blockScratch[firstIndex].local[axis];
            }
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            output->origin[axis] = oldOrigin[axis] + (double)anchor[axis];
            output->velocity[axis] = oldVelocity[axis];
            output->grabTarget[axis] = oldGrabTarget[axis];
        }
        output->grabOwner = componentHasLever ? oldGrabOwner : 0U;
        for (uint32_t index = 0U; index < remainingCount; ++index)
        {
            if (system->componentScratch[index] != component)
            {
                continue;
            }
            PhysicalConstructBlock value = system->blockScratch[index];
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                value.local[axis] -= anchor[axis];
            }
            output->blocks[output->blockCount++] = value;
        }
        BodyRefreshBounds(output);
        if (componentHasLever)
        {
            PhysicalConstructModelPart model[3];
            for (uint32_t index = 0U; index < output->blockCount; ++index)
            {
                if (output->blocks[index].kind != PHYSICAL_CONSTRUCT_BLOCK_LEVER)
                {
                    continue;
                }
                PhysicalConstructGetLeverModel(output->blocks[index].mountNormal, model);
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    output->grabPointLocal[axis] =
                        (model[2].localBounds.minimum[axis] + model[2].localBounds.maximum[axis]) *
                        0.5;
                }
                break;
            }
        }
    }

    system->nextBodyId = nextId;
    uint32_t parentPrepared = 0U;
    for (uint32_t component = 0U; component < componentCount; ++component)
    {
        if (system->splitScratch[component].id == oldId)
        {
            parentPrepared = component;
            break;
        }
    }
    memcpy(body, &system->splitScratch[parentPrepared], sizeof(*body));
    for (uint32_t component = 0U; component < componentCount; ++component)
    {
        if (component == parentPrepared)
        {
            continue;
        }
        PhysicalConstructBody *freeBody = FindFreeBody(system);
        if (freeBody == NULL)
        {
            return PHYSICAL_CONSTRUCT_CAPACITY;
        }
        memcpy(freeBody, &system->splitScratch[component], sizeof(*freeBody));
    }

    if (outRemoved != NULL)
    {
        *outRemoved = removed;
    }
    if (outBodyIdCount != NULL)
    {
        *outBodyIdCount = componentCount;
    }
    if (outBodyIds != NULL)
    {
        for (uint32_t component = 0U; component < componentCount; ++component)
        {
            outBodyIds[component] = system->splitScratch[component].id;
        }
        SortIds(outBodyIds, componentCount);
    }
    return PHYSICAL_CONSTRUCT_OK;
}

static bool RayAabb(const double origin[3], const double direction[3],
                    const PhysicalConstructAabb *bounds, double maximumDistance,
                    double *outDistance, int8_t outNormal[3])
{
    double nearDistance = 0.0;
    double farDistance = CONSTRUCT_POSITION_LIMIT;
    int8_t nearNormal[3] = {0, 0, 0};
    int8_t farNormal[3] = {0, 0, 0};
    bool hasNearNormal = false;
    bool hasFarNormal = false;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        double axisDirection = direction[axis];
        if (axisDirection > -1e-12 && axisDirection < 1e-12)
        {
            if (origin[axis] < bounds->minimum[axis] || origin[axis] > bounds->maximum[axis])
            {
                return false;
            }
            continue;
        }
        double first = (bounds->minimum[axis] - origin[axis]) / axisDirection;
        double second = (bounds->maximum[axis] - origin[axis]) / axisDirection;
        int8_t nearSign = -1;
        int8_t farSign = 1;
        if (first > second)
        {
            double temporary = first;
            first = second;
            second = temporary;
            nearSign = 1;
            farSign = -1;
        }
        if (first > nearDistance || (first == nearDistance && !hasNearNormal))
        {
            nearDistance = first;
            nearNormal[0] = 0;
            nearNormal[1] = 0;
            nearNormal[2] = 0;
            nearNormal[axis] = nearSign;
            hasNearNormal = true;
        }
        if (second < farDistance || (second == farDistance && !hasFarNormal))
        {
            farDistance = second;
            farNormal[0] = 0;
            farNormal[1] = 0;
            farNormal[2] = 0;
            farNormal[axis] = farSign;
            hasFarNormal = true;
        }
        if (nearDistance > farDistance)
        {
            return false;
        }
    }
    if (farDistance < 0.0 || nearDistance > maximumDistance)
    {
        return false;
    }
    if (hasNearNormal)
    {
        *outDistance = nearDistance;
        outNormal[0] = nearNormal[0];
        outNormal[1] = nearNormal[1];
        outNormal[2] = nearNormal[2];
    }
    else if (hasFarNormal)
    {
        // An origin inside (or exactly on the outward-facing boundary of) a
        // collider has no entering slab. Select its first exit face so
        // placement still receives a real unit normal instead of {0,0,0}.
        *outDistance = 0.0;
        outNormal[0] = farNormal[0];
        outNormal[1] = farNormal[1];
        outNormal[2] = farNormal[2];
    }
    else
    {
        return false;
    }
    return true;
}

bool PhysicalConstructRaycast(const PhysicalConstructSystem *system, const double origin[3],
                              const double direction[3], double maximumDistance,
                              PhysicalConstructRaycastHit *outHit)
{
    if (system == NULL || origin == NULL || direction == NULL || outHit == NULL ||
        !IsFiniteBounded(maximumDistance, CONSTRUCT_POSITION_LIMIT) || maximumDistance <= 0.0)
    {
        return false;
    }
    bool hasDirection = false;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!IsFiniteBounded(origin[axis], CONSTRUCT_POSITION_LIMIT) ||
            !IsFiniteBounded(direction[axis], 1.0))
        {
            return false;
        }
        if (direction[axis] <= -1e-12 || direction[axis] >= 1e-12)
        {
            hasDirection = true;
        }
    }
    if (!hasDirection)
    {
        return false;
    }

    bool found = false;
    double nearest = maximumDistance;
    uint8_t bodyIndices[16];
    uint32_t bodyCount = SortedBodyIndices(system, bodyIndices);
    for (uint32_t ordered = 0U; ordered < bodyCount; ++ordered)
    {
        const PhysicalConstructBody *body = &system->bodies[bodyIndices[ordered]];
        for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
        {
            const PhysicalConstructBlock *block = &body->blocks[blockIndex];
            for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
            {
                PhysicalConstructHitPart hitPart;
                PhysicalConstructAabb localBounds;
                if (!BlockPartLocalBounds(block, part, &localBounds, &hitPart))
                {
                    continue;
                }
                PhysicalConstructAabb worldBounds = localBounds;
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    worldBounds.minimum[axis] += body->origin[axis];
                    worldBounds.maximum[axis] += body->origin[axis];
                }
                double distance;
                int8_t normal[3];
                if (!RayAabb(origin, direction, &worldBounds, nearest, &distance, normal))
                {
                    continue;
                }
                if (found && distance == nearest)
                {
                    continue;
                }
                found = true;
                nearest = distance;
                outHit->bodyId = body->id;
                outHit->distance = distance;
                outHit->material = block->material;
                outHit->blockKind = (PhysicalConstructBlockKind)block->kind;
                outHit->hitPart = hitPart;
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    outHit->localBlock[axis] = block->local[axis];
                    outHit->normal[axis] = normal[axis];
                }
            }
        }
    }
    return found;
}

static bool FindLever(const PhysicalConstructBody *body, uint32_t *outIndex)
{
    for (uint32_t index = 0U; index < body->blockCount; ++index)
    {
        if (body->blocks[index].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
        {
            if (outIndex != NULL)
            {
                *outIndex = index;
            }
            return true;
        }
    }
    return false;
}

static bool TargetValid(const double target[3])
{
    if (target == NULL)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!IsFiniteBounded(target[axis], CONSTRUCT_POSITION_LIMIT))
        {
            return false;
        }
    }
    return true;
}

PhysicalConstructResult PhysicalConstructBeginGrab(PhysicalConstructSystem *system, uint64_t bodyId,
                                                   uint64_t owner, const double target[3])
{
    if (system == NULL || owner == 0U || !TargetValid(target))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL)
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    uint32_t leverIndex;
    if (!FindLever(body, &leverIndex))
    {
        return PHYSICAL_CONSTRUCT_NOT_FOUND;
    }
    if (body->grabOwner != 0U && body->grabOwner != owner)
    {
        return PHYSICAL_CONSTRUCT_OCCUPIED;
    }
    PhysicalConstructModelPart model[3];
    PhysicalConstructGetLeverModel(body->blocks[leverIndex].mountNormal, model);
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        body->grabPointLocal[axis] =
            (double)body->blocks[leverIndex].local[axis] +
            (model[2].localBounds.minimum[axis] + model[2].localBounds.maximum[axis]) * 0.5;
        body->grabTarget[axis] = target[axis];
    }
    body->grabOwner = owner;
    return PHYSICAL_CONSTRUCT_OK;
}

bool PhysicalConstructUpdateGrab(PhysicalConstructSystem *system, uint64_t bodyId, uint64_t owner,
                                 const double target[3])
{
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL || owner == 0U || body->grabOwner != owner || !TargetValid(target))
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        body->grabTarget[axis] = target[axis];
    }
    return true;
}

bool PhysicalConstructEndGrab(PhysicalConstructSystem *system, uint64_t bodyId, uint64_t owner)
{
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL || owner == 0U || body->grabOwner != owner)
    {
        return false;
    }
    body->grabOwner = 0U;
    return true;
}

void PhysicalConstructReleaseOwner(PhysicalConstructSystem *system, uint64_t owner)
{
    if (system == NULL || owner == 0U)
    {
        return;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        if (system->bodies[index].active && system->bodies[index].grabOwner == owner)
        {
            system->bodies[index].grabOwner = 0U;
        }
    }
}

static bool DynamicBlockersValid(const PhysicalConstructDynamicBlocker *blockers,
                                 uint32_t blockerCount)
{
    if (blockerCount > PHYSICAL_CONSTRUCT_MAX_DYNAMIC_BLOCKERS ||
        (blockerCount > 0U && blockers == NULL))
    {
        return false;
    }
    for (uint32_t index = 0U; index < blockerCount; ++index)
    {
        if (!AabbValid(&blockers[index].bounds))
        {
            return false;
        }
    }
    return true;
}

static bool MoveBodyAxis(PhysicalConstructSystem *system, PhysicalConstructBody *body,
                         uint32_t axis, double distance,
                         const PhysicalConstructDynamicBlocker *blockers, uint32_t blockerCount)
{
    if (distance == 0.0)
    {
        return true;
    }
    double remaining = distance;
    for (uint32_t segmentIndex = 0U; segmentIndex < CONSTRUCT_MAX_SWEEP_SEGMENTS;
         ++segmentIndex)
    {
        double segment =
            ClampDouble(remaining, -CONSTRUCT_MAX_STEP_DISTANCE, CONSTRUCT_MAX_STEP_DISTANCE);
        double candidate[3] = {body->origin[0], body->origin[1], body->origin[2]};
        candidate[axis] += segment;
        if (!IsFiniteBounded(candidate[axis], CONSTRUCT_POSITION_LIMIT))
        {
            return false;
        }
        if (!BodyCollidesAt(system, body, candidate, blockers, blockerCount))
        {
            body->origin[axis] = candidate[axis];
            if (segment == remaining)
            {
                return true;
            }
            remaining -= segment;
            continue;
        }

        double safeFraction = 0.0;
        double blockedFraction = 1.0;
        for (uint32_t iteration = 0U; iteration < CONSTRUCT_BINARY_SWEEP_STEPS; ++iteration)
        {
            double fraction = (safeFraction + blockedFraction) * 0.5;
            candidate[axis] = body->origin[axis] + segment * fraction;
            if (BodyCollidesAt(system, body, candidate, blockers, blockerCount))
            {
                blockedFraction = fraction;
            }
            else
            {
                safeFraction = fraction;
            }
        }
        body->origin[axis] += segment * safeFraction;
        return false;
    }
    return remaining == 0.0;
}

void PhysicalConstructStep(PhysicalConstructSystem *system)
{
    (void)PhysicalConstructStepWithBlockers(system, NULL, 0U);
}

bool PhysicalConstructStepWithBlockers(
    PhysicalConstructSystem *system, const PhysicalConstructDynamicBlocker *blockers,
    uint32_t blockerCount)
{
    if (system == NULL || !DynamicBlockersValid(blockers, blockerCount))
    {
        return false;
    }
    double step = system->configuration.fixedStepSeconds;
    uint8_t bodyIndices[16];
    uint32_t bodyCount = SortedBodyIndices(system, bodyIndices);
    for (uint32_t ordered = 0U; ordered < bodyCount; ++ordered)
    {
        PhysicalConstructBody *body = &system->bodies[bodyIndices[ordered]];
        double acceleration[3] = {0.0, 0.0, -system->configuration.gravity};
        if (body->grabOwner != 0U)
        {
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                double handle = body->origin[axis] + body->grabPointLocal[axis];
                double spring =
                    (body->grabTarget[axis] - handle) * system->configuration.grabStiffness -
                    body->velocity[axis] * system->configuration.grabDamping;
                acceleration[axis] +=
                    ClampDouble(spring, -system->configuration.maximumGrabAcceleration,
                                system->configuration.maximumGrabAcceleration);
            }
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            body->velocity[axis] += acceleration[axis] * step;
            body->velocity[axis] =
                ClampDouble(body->velocity[axis], -system->configuration.maximumSpeed,
                            system->configuration.maximumSpeed);
            double requested = body->velocity[axis] * step;
            if (!MoveBodyAxis(system, body, axis, requested, blockers, blockerCount))
            {
                body->velocity[axis] = 0.0;
            }
        }
    }
    return true;
}

void PhysicalConstructSimulateFixedSteps(PhysicalConstructSystem *system, uint32_t steps)
{
    (void)PhysicalConstructSimulateFixedStepsWithBlockers(system, steps, NULL, 0U);
}

bool PhysicalConstructSimulateFixedStepsWithBlockers(
    PhysicalConstructSystem *system, uint32_t steps,
    const PhysicalConstructDynamicBlocker *blockers, uint32_t blockerCount)
{
    if (system == NULL || !DynamicBlockersValid(blockers, blockerCount))
    {
        return false;
    }
    for (uint32_t step = 0U; step < steps; ++step)
    {
        if (!PhysicalConstructStepWithBlockers(system, blockers, blockerCount))
        {
            return false;
        }
    }
    return true;
}

bool PhysicalConstructRebase(PhysicalConstructSystem *system, const int64_t blockShift[3])
{
    if (system == NULL || blockShift == NULL)
    {
        return false;
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        const PhysicalConstructBody *body = &system->bodies[index];
        if (!body->active)
        {
            continue;
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            double shiftedOrigin = body->origin[axis] - (double)blockShift[axis];
            if (!IsFiniteBounded(shiftedOrigin, CONSTRUCT_POSITION_LIMIT))
            {
                return false;
            }
            if (body->grabOwner != 0U &&
                !IsFiniteBounded(body->grabTarget[axis] - (double)blockShift[axis],
                                 CONSTRUCT_POSITION_LIMIT))
            {
                return false;
            }
        }
    }
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        PhysicalConstructBody *body = &system->bodies[index];
        if (!body->active)
        {
            continue;
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            body->origin[axis] -= (double)blockShift[axis];
            if (body->grabOwner != 0U)
            {
                body->grabTarget[axis] -= (double)blockShift[axis];
            }
        }
    }
    return true;
}

uint32_t PhysicalConstructCopyBodyStates(const PhysicalConstructSystem *system,
                                         PhysicalConstructBodyState *output, uint32_t capacity,
                                         bool *outTruncated)
{
    if (outTruncated != NULL)
    {
        *outTruncated = false;
    }
    if (system == NULL || (capacity > 0U && output == NULL))
    {
        return 0U;
    }
    uint8_t bodyIndices[16];
    uint32_t count = SortedBodyIndices(system, bodyIndices);
    uint32_t copied = count < capacity ? count : capacity;
    for (uint32_t index = 0U; index < copied; ++index)
    {
        const PhysicalConstructBody *body = &system->bodies[bodyIndices[index]];
        PhysicalConstructBodyState *state = &output[index];
        state->id = body->id;
        state->topologyRevision = body->topologyRevision;
        state->blockCount = body->blockCount;
        state->grabOwner = body->grabOwner;
        state->grabbed = body->grabOwner != 0U;
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            state->origin[axis] = body->origin[axis];
            state->velocity[axis] = body->velocity[axis];
        }
    }
    if (outTruncated != NULL)
    {
        *outTruncated = copied < count;
    }
    return copied;
}

uint32_t PhysicalConstructCopyCollidersInAabb(const PhysicalConstructSystem *system,
                                              const PhysicalConstructAabb *query,
                                              PhysicalConstructCollider *output,
                                              uint32_t capacity, bool *outTruncated)
{
    if (outTruncated != NULL)
    {
        *outTruncated = false;
    }
    if (system == NULL || !AabbValid(query) || (capacity > 0U && output == NULL))
    {
        return 0U;
    }

    uint32_t copied = 0U;
    uint8_t bodyIndices[PHYSICAL_CONSTRUCT_MAX_BODIES];
    uint32_t bodyCount = SortedBodyIndices(system, bodyIndices);
    for (uint32_t ordered = 0U; ordered < bodyCount; ++ordered)
    {
        const PhysicalConstructBody *body = &system->bodies[bodyIndices[ordered]];
        PhysicalConstructAabb broad = BodyWorldBoundsAt(body, body->origin);
        if (!AabbOverlaps(query, &broad, 0.0))
        {
            continue;
        }
        for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
        {
            const PhysicalConstructBlock *block = &body->blocks[blockIndex];
            for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
            {
                PhysicalConstructAabb bounds =
                    BlockPartWorldBoundsAt(block, part, body->origin);
                if (!AabbOverlaps(query, &bounds, 0.0))
                {
                    continue;
                }
                if (copied == capacity)
                {
                    if (outTruncated != NULL)
                    {
                        *outTruncated = true;
                    }
                    return copied;
                }
                PhysicalConstructCollider *collider = &output[copied++];
                collider->bounds = bounds;
                collider->bodyId = body->id;
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    collider->velocity[axis] = body->velocity[axis];
                }
            }
        }
    }
    return copied;
}

bool PhysicalConstructCopyBlocks(const PhysicalConstructSystem *system, uint64_t bodyId,
                                 PhysicalConstructBlock *output, uint32_t capacity,
                                 uint32_t *outCount)
{
    if (outCount != NULL)
    {
        *outCount = 0U;
    }
    const PhysicalConstructBody *body = FindBodyConst(system, bodyId);
    if (body == NULL || outCount == NULL || (capacity > 0U && output == NULL))
    {
        return false;
    }
    *outCount = body->blockCount;
    if (capacity < body->blockCount)
    {
        return false;
    }
    for (uint32_t index = 0U; index < body->blockCount; ++index)
    {
        output[index] = body->blocks[index];
    }
    return true;
}

static bool StateValid(const PhysicalConstructBodyState *state)
{
    if (state == NULL || state->id == 0U || state->topologyRevision == 0U ||
        state->blockCount == 0U || state->blockCount > PHYSICAL_CONSTRUCT_MAX_BLOCKS ||
        state->grabbed != (state->grabOwner != 0U))
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!IsFiniteBounded(state->origin[axis], CONSTRUCT_POSITION_LIMIT) ||
            !IsFiniteBounded(state->velocity[axis], CONSTRUCT_VELOCITY_LIMIT))
        {
            return false;
        }
    }
    return true;
}

static bool ImportedBlocksValid(PhysicalConstructSystem *system, PhysicalConstructBlock *blocks,
                                uint32_t count)
{
    uint32_t leverCount = 0U;
    for (uint32_t index = 0U; index < count; ++index)
    {
        PhysicalConstructBlock *block = &blocks[index];
        if (!LocalCoordinateValid(block->local))
        {
            return false;
        }
        if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
        {
            ++leverCount;
            if (leverCount > 1U || block->material != 0U || block->local[0] != 0 ||
                block->local[1] != 0 || block->local[2] != 0 || !IsUnitNormal(block->mountNormal))
            {
                return false;
            }
        }
        else if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
        {
            if (!ConstructMaterialValid(block->material) || block->mountNormal[0] != 0 ||
                block->mountNormal[1] != 0 || block->mountNormal[2] != 0)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    SortBlocks(blocks, count);
    for (uint32_t index = 1U; index < count; ++index)
    {
        if (SameLocal(blocks[index - 1U].local, blocks[index].local))
        {
            return false;
        }
    }
    return BuildComponents(system, blocks, count) == 1U;
}

PhysicalConstructResult PhysicalConstructImportBody(PhysicalConstructSystem *system,
                                                    const PhysicalConstructBodyState *state,
                                                    const PhysicalConstructBlock *blocks,
                                                    uint32_t blockCount)
{
    if (system == NULL || !StateValid(state) || blocks == NULL || blockCount != state->blockCount)
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0U; index < blockCount; ++index)
    {
        system->blockScratch[index] = blocks[index];
    }
    if (!ImportedBlocksValid(system, system->blockScratch, blockCount))
    {
        return PHYSICAL_CONSTRUCT_INVALID_ARGUMENT;
    }

    PhysicalConstructBody *body = FindBody(system, state->id);
    if (body != NULL && body->topologyRevision > state->topologyRevision)
    {
        return PHYSICAL_CONSTRUCT_OK;
    }
    if (body == NULL)
    {
        body = FindFreeBody(system);
        if (body == NULL)
        {
            return PHYSICAL_CONSTRUCT_CAPACITY;
        }
    }
    memset(body, 0, sizeof(*body));
    body->active = true;
    body->id = state->id;
    body->topologyRevision = state->topologyRevision;
    body->blockCount = blockCount;
    body->grabOwner = state->grabOwner;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        body->origin[axis] = state->origin[axis];
        body->velocity[axis] = state->velocity[axis];
    }
    for (uint32_t index = 0U; index < blockCount; ++index)
    {
        body->blocks[index] = system->blockScratch[index];
    }
    BodyRefreshBounds(body);
    uint32_t leverIndex;
    if (FindLever(body, &leverIndex))
    {
        PhysicalConstructModelPart model[3];
        PhysicalConstructGetLeverModel(body->blocks[leverIndex].mountNormal, model);
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            body->grabPointLocal[axis] =
                (model[2].localBounds.minimum[axis] + model[2].localBounds.maximum[axis]) * 0.5;
            body->grabTarget[axis] = body->origin[axis] + body->grabPointLocal[axis];
        }
    }
    if (state->id >= system->nextBodyId && state->id != UINT64_MAX)
    {
        system->nextBodyId = state->id + 1U;
    }
    return PHYSICAL_CONSTRUCT_OK;
}

bool PhysicalConstructApplyBodyMotion(PhysicalConstructSystem *system,
                                      const PhysicalConstructBodyState *state)
{
    if (system == NULL || !StateValid(state))
    {
        return false;
    }
    PhysicalConstructBody *body = FindBody(system, state->id);
    if (body == NULL || body->topologyRevision != state->topologyRevision ||
        body->blockCount != state->blockCount)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        body->origin[axis] = state->origin[axis];
        body->velocity[axis] = state->velocity[axis];
    }
    body->grabOwner = state->grabOwner;
    return true;
}

bool PhysicalConstructRemoveBody(PhysicalConstructSystem *system, uint64_t bodyId)
{
    PhysicalConstructBody *body = FindBody(system, bodyId);
    if (body == NULL)
    {
        return false;
    }
    memset(body, 0, sizeof(*body));
    return true;
}

bool PhysicalConstructWorldCellOccupied(const PhysicalConstructSystem *system, int64_t x, int64_t y,
                                        int64_t z)
{
    if (system == NULL)
    {
        return false;
    }
    PhysicalConstructAabb cell = {
        .minimum = {(double)x, (double)y, (double)z},
        .maximum = {(double)x + 1.0, (double)y + 1.0, (double)z + 1.0},
    };
    for (uint32_t index = 0U; index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        const PhysicalConstructBody *body = &system->bodies[index];
        if (!body->active)
        {
            continue;
        }
        PhysicalConstructAabb broad = BodyWorldBoundsAt(body, body->origin);
        if (!AabbOverlaps(&cell, &broad, 0.0))
        {
            continue;
        }
        for (uint32_t blockIndex = 0U; blockIndex < body->blockCount; ++blockIndex)
        {
            const PhysicalConstructBlock *block = &body->blocks[blockIndex];
            for (uint32_t part = 0U; part < BlockPartCount(block); ++part)
            {
                PhysicalConstructAabb bounds = BlockPartWorldBoundsAt(block, part, body->origin);
                if (AabbOverlaps(&cell, &bounds, 0.0))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

void PhysicalConstructLocalToChunk(const int32_t localBlock[3], int32_t outChunk[3],
                                   uint32_t outLocal[3])
{
    if (localBlock == NULL || outChunk == NULL || outLocal == NULL)
    {
        return;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        outChunk[axis] = FloorDivChunkI32(localBlock[axis]);
        outLocal[axis] =
            (uint32_t)(localBlock[axis] - outChunk[axis] * CHUNK_SIZE);
    }
}
