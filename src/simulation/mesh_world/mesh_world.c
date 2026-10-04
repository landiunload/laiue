#include "mesh_world/mesh_world_service.h"

#include "math/scalar.h"
#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Every query is evaluated in the frame of one reference cell: cell
 * differences are exact int64 values, and only differences inside the query
 * range ever become floats, so precision does not depend on how far from
 * the origin the query runs. */

#define MESH_DEFAULT_MAXIMUM_INSTANCES (1u << 20)
#define MESH_MAXIMUM_INSTANCES_LIMIT (1u << 28)
#define MESH_MAX_SHAPE_EXTENT 1048576.0f
#define MESH_MAX_SCALE 65536.0f
#define MESH_MAX_QUERY_RADIUS 16777216.0f
#define MESH_MAX_MOTION 65536.0f
#define MESH_MAX_BLOCK_SIZE 65536.0f
#define MESH_MAX_LOCAL_CELLS 1099511627776.0 /* 2^40 */
#define MESH_SKIN 1.0e-3f
#define MESH_GROUND_NORMAL 0.7f
#define MESH_MOVE_ITERATIONS 4u
#define MESH_NO_INDEX UINT32_MAX
#define MESH_MAX_STREAM_CELLS 64
#define MESH_MAX_PROVIDER_CELL_OFFSET (INT64_C(1) << 40)
#define MESH_KNOWN_INSTANCE_FLAGS (LAIUE_MESH_INSTANCE_VISIBLE | LAIUE_MESH_INSTANCE_COLLIDABLE)
#define MESH_BOX_BATCH_SIZE 8u

typedef struct MeshShape
{
    uint32_t model;
    uint32_t flags;
    uint32_t boxCount;
    float boundsMin[3];
    float boundsMax[3];
    LaiueMeshBoxV1 *boxes;
} MeshShape;

typedef struct MeshSlot
{
    uint32_t generation;
    uint32_t live;
    uint32_t model;
    uint32_t flags;
    uint64_t userData;
    LaiueMeshTransformV1 transform; /* normalized */
    uint32_t cell;
    uint32_t nextFree;
    float boundsMin[3]; /* relative to the cell origin */
    float boundsMax[3];
} MeshSlot;

typedef struct MeshCell
{
    LaiueMeshCellV1 coord;
    uint32_t *instances; /* slot indices in insertion order */
    uint32_t count;
    uint32_t capacity;
    uint64_t revision;
    float boundsMin[3];
    float boundsMax[3];
} MeshCell;

/* A cell the provider has filled and the instances it produced there. */
typedef struct MeshPopulated
{
    LaiueMeshCellV1 coord;
    LaiueMeshInstanceV1 *handles;
    uint32_t count;
} MeshPopulated;

/* Open addressing over any array whose elements start with their cell
 * coordinate; entries hold element index + 1, 0 is empty. */
typedef struct MeshTable
{
    uint32_t *entries;
    uint32_t capacity;
} MeshTable;

struct LaiueMeshWorldV1
{
    PlatformRwLock lock;
    float cellSize;
    uint32_t maximumInstances;
    MeshSlot *slots;
    uint32_t slotCount;
    uint32_t slotCapacity;
    uint32_t freeHead;
    uint32_t liveCount;
    MeshCell *cells;
    uint32_t cellCount;
    uint32_t cellCapacity;
    MeshTable cellTable;
    /* Bounds of the occupied cells.  Deleting a cell leaves them wide; they
     * are recomputed once deletions outnumber the remaining cells, which
     * keeps the cost amortized O(1) per deletion. */
    int64_t occupiedMin[3];
    int64_t occupiedMax[3];
    uint32_t deletionsSinceBounds;
    MeshShape *shapes; /* sorted by model */
    uint32_t shapeCount;
    uint32_t shapeCapacity;
    uint64_t revisionCounter;
    /* The farthest any cell's bounds have ever reached outside the cell.
     * Never shrinks; it only widens the cell range a query inspects. */
    float reach;

    uint32_t hasProvider;
    LaiueMeshCellProviderV1 provider;
    LaiueMeshInstanceDescV1 *providerScratch;
    MeshPopulated *populated;
    uint32_t populatedCount;
    uint32_t populatedCapacity;
    MeshTable populatedTable;
    /* The last full scan: repeated stream calls from the same cell skip it. */
    LaiueMeshCellV1 streamCenter;
    float streamRadius;
    uint32_t streamComplete;
};

typedef struct MeshObb
{
    float center[3];
    float axis[3][3];
    float half[3];
} MeshObb;

/* ---- scalar helpers ---------------------------------------------------- */

static bool FiniteFloat(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7F800000)) != UINT32_C(0x7F800000);
}

static bool FiniteVector(const float value[3])
{
    return FiniteFloat(value[0]) && FiniteFloat(value[1]) && FiniteFloat(value[2]);
}

/* Callers keep |value| below 2^62. */
static double FloorDouble(double value)
{
    const double truncated = (double)(int64_t)value;
    return truncated > value ? truncated - 1.0 : truncated;
}

static float AbsFloat(float value)
{
    return value < 0.0f ? -value : value;
}

static float MaxFloat(float left, float right)
{
    return left > right ? left : right;
}

static float MinFloat(float left, float right)
{
    return left < right ? left : right;
}

static float Dot3(const float left[3], const float right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static bool CellInRange(int64_t value)
{
    return value >= -LAIUE_MESH_WORLD_MAX_CELL && value <= LAIUE_MESH_WORLD_MAX_CELL;
}

static bool CellValid(const LaiueMeshCellV1 *cell)
{
    return CellInRange(cell->x) && CellInRange(cell->y) && CellInRange(cell->z);
}

static bool CellEqual(const LaiueMeshCellV1 *left, const LaiueMeshCellV1 *right)
{
    return left->x == right->x && left->y == right->y && left->z == right->z;
}

static int64_t CellComponent(const LaiueMeshCellV1 *cell, uint32_t axis)
{
    return axis == 0u ? cell->x : (axis == 1u ? cell->y : cell->z);
}

static void SetCellComponent(LaiueMeshCellV1 *cell, uint32_t axis, int64_t value)
{
    if (axis == 0u)
        cell->x = value;
    else if (axis == 1u)
        cell->y = value;
    else
        cell->z = value;
}

/* ---- quaternions ------------------------------------------------------- */

static bool NormalizeQuaternion(const float input[4], float output[4])
{
    if (!FiniteFloat(input[0]) || !FiniteFloat(input[1]) || !FiniteFloat(input[2]) ||
        !FiniteFloat(input[3]))
        return false;
    const double lengthSquared = (double)input[0] * input[0] + (double)input[1] * input[1] +
                                 (double)input[2] * input[2] + (double)input[3] * input[3];
    if (!(lengthSquared > 1.0e-12))
        return false;
    const double inverse = 1.0 / ScalarSqrtDouble(lengthSquared);
    for (uint32_t i = 0u; i < 4u; ++i)
        output[i] = (float)(input[i] * inverse);
    return true;
}

static bool QuaternionIsIdentity(const float rotation[4])
{
    return AbsFloat(rotation[0]) < 1.0e-6f && AbsFloat(rotation[1]) < 1.0e-6f &&
           AbsFloat(rotation[2]) < 1.0e-6f;
}

/* Rows of the rotation matrix; column c is the rotated basis vector c. */
static void QuaternionMatrix(const float q[4], float m[3][3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    m[0][1] = 2.0f * (x * y - w * z);
    m[0][2] = 2.0f * (x * z + w * y);
    m[1][0] = 2.0f * (x * y + w * z);
    m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    m[1][2] = 2.0f * (y * z - w * x);
    m[2][0] = 2.0f * (x * z - w * y);
    m[2][1] = 2.0f * (y * z + w * x);
    m[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

/* left * right: rotate by right first, then by left. */
static void QuaternionMultiply(const float left[4], const float right[4], float output[4])
{
    const float ax = left[0], ay = left[1], az = left[2], aw = left[3];
    const float bx = right[0], by = right[1], bz = right[2], bw = right[3];
    output[0] = aw * bx + ax * bw + ay * bz - az * by;
    output[1] = aw * by - ax * bz + ay * bw + az * bx;
    output[2] = aw * bz + ax * by - ay * bx + az * bw;
    output[3] = aw * bw - ax * bx - ay * by - az * bz;
}

static void MatrixApply(float m[3][3], const float v[3], float output[3])
{
    for (uint32_t r = 0u; r < 3u; ++r)
        output[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
}

static void ObbFromMatrix(MeshObb *box, float m[3][3])
{
    for (uint32_t c = 0u; c < 3u; ++c)
        for (uint32_t r = 0u; r < 3u; ++r)
            box->axis[c][r] = m[r][c];
}

/* World-space half extent of an oriented box along each axis. */
static void ObbAabb(const MeshObb *box, float outMin[3], float outMax[3])
{
    for (uint32_t r = 0u; r < 3u; ++r)
    {
        const float extent = AbsFloat(box->axis[0][r]) * box->half[0] +
                             AbsFloat(box->axis[1][r]) * box->half[1] +
                             AbsFloat(box->axis[2][r]) * box->half[2];
        outMin[r] = box->center[r] - extent;
        outMax[r] = box->center[r] + extent;
    }
}

/* ---- positions --------------------------------------------------------- */

static bool NormalizePosition(float cellSize, const LaiueMeshPositionV1 *input,
                              LaiueMeshPositionV1 *output)
{
    if (input == NULL || output == NULL || !CellValid(&input->cell) || !FiniteVector(input->local))
        return false;
    LaiueMeshPositionV1 result = *input;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double value = input->local[axis];
        const double cells = value / (double)cellSize;
        if (cells > MESH_MAX_LOCAL_CELLS || cells < -MESH_MAX_LOCAL_CELLS)
            return false;
        const double whole = FloorDouble(cells);
        int64_t cell = CellComponent(&input->cell, axis) + (int64_t)whole;
        float local = (float)(value - whole * (double)cellSize);
        if (local >= cellSize)
        {
            local = 0.0f;
            ++cell;
        }
        if (local < 0.0f)
            local = 0.0f;
        if (!CellInRange(cell))
            return false;
        SetCellComponent(&result.cell, axis, cell);
        result.local[axis] = local;
    }
    *output = result;
    return true;
}

static bool NormalizeTransform(float cellSize, const LaiueMeshTransformV1 *input,
                               LaiueMeshTransformV1 *output)
{
    if (input == NULL)
        return false;
    LaiueMeshTransformV1 result;
    if (!NormalizePosition(cellSize, &input->position, &result.position) ||
        !NormalizeQuaternion(input->rotation, result.rotation))
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const float scale = input->scale[axis];
        if (!FiniteFloat(scale) || !(scale > 0.0f) || scale > MESH_MAX_SCALE)
            return false;
        result.scale[axis] = scale;
    }
    *output = result;
    return true;
}

/* Offset of a cell from the reference cell in metres, or false when the
 * difference is outside [low, high] cells on some axis. */
static bool CellOffset(const LaiueMeshCellV1 *cell, const LaiueMeshCellV1 *reference,
                       const int64_t low[3], const int64_t high[3], float cellSize,
                       float outOffset[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const int64_t difference = CellComponent(cell, axis) - CellComponent(reference, axis);
        if (difference < low[axis] || difference > high[axis])
            return false;
        outOffset[axis] = (float)((double)difference * (double)cellSize);
    }
    return true;
}

/* ---- shapes ------------------------------------------------------------ */

static uint32_t FindShapeIndex(const LaiueMeshWorldV1 *world, uint32_t model, bool *outFound)
{
    uint32_t low = 0u;
    uint32_t high = world->shapeCount;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        if (world->shapes[middle].model < model)
            low = middle + 1u;
        else
            high = middle;
    }
    *outFound = low < world->shapeCount && world->shapes[low].model == model;
    return low;
}

static const MeshShape *FindShape(const LaiueMeshWorldV1 *world, uint32_t model)
{
    bool found = false;
    const uint32_t index = FindShapeIndex(world, model, &found);
    return found ? &world->shapes[index] : NULL;
}

/* One transform per instance, one OBB at a time. Keeping all 64 OBBs on the
 * stack made inlined
 * Release visitors exceed a page and require __chkstk in
 * the no-CRT profile; early-out queries
 * also transformed unused boxes. */
typedef struct MeshBoxIterator
{
    const LaiueMeshTransformV1 *transform;
    const LaiueMeshBoxV1 *boxes;
    float instanceMatrix[3][3];
    LaiueMeshBoxV1 boundsBox;
    uint32_t count;
    bool uniform;
} MeshBoxIterator;

static void BeginInstanceBoxes(const MeshShape *shape, const LaiueMeshTransformV1 *transform,
                               MeshBoxIterator *iterator)
{
    iterator->count = 0u;
    if (shape == NULL)
        return;
    iterator->transform = transform;
    iterator->boxes = shape->boxes;
    iterator->count = shape->boxCount;
    if (iterator->count == 0u)
    {
        if ((shape->flags & LAIUE_MESH_SHAPE_COLLIDE_BOUNDS) == 0u)
            return;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            iterator->boundsBox.center[axis] =
                0.5f * (shape->boundsMin[axis] + shape->boundsMax[axis]);
            iterator->boundsBox.halfExtent[axis] =
                0.5f * (shape->boundsMax[axis] - shape->boundsMin[axis]);
        }
        iterator->boundsBox.rotation[0] = 0.0f;
        iterator->boundsBox.rotation[1] = 0.0f;
        iterator->boundsBox.rotation[2] = 0.0f;
        iterator->boundsBox.rotation[3] = 1.0f;
        iterator->boxes = &iterator->boundsBox;
        iterator->count = 1u;
    }
    QuaternionMatrix(transform->rotation, iterator->instanceMatrix);
    const float *scale = transform->scale;
    iterator->uniform = scale[0] == scale[1] && scale[1] == scale[2];
}

static inline void InstanceBox(MeshBoxIterator *iterator, uint32_t index, MeshObb *out)
{
    const LaiueMeshTransformV1 *transform = iterator->transform;
    const LaiueMeshBoxV1 *box = &iterator->boxes[index];
    const float *scale = transform->scale;
    const float scaledCenter[3] = {box->center[0] * scale[0], box->center[1] * scale[1],
                                   box->center[2] * scale[2]};
    float rotatedCenter[3];
    MatrixApply(iterator->instanceMatrix, scaledCenter, rotatedCenter);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        out->center[axis] = transform->position.local[axis] + rotatedCenter[axis];
    if (QuaternionIsIdentity(box->rotation))
    {
        ObbFromMatrix(out, iterator->instanceMatrix);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            out->half[axis] = box->halfExtent[axis] * scale[axis];
    }
    else if (iterator->uniform)
    {
        float combined[4];
        float matrix[3][3];
        QuaternionMultiply(transform->rotation, box->rotation, combined);
        QuaternionMatrix(combined, matrix);
        ObbFromMatrix(out, matrix);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            out->half[axis] = box->halfExtent[axis] * scale[0];
    }
    else
    {
        /* A rotated box under non-uniform scale is no longer a box; its
         * bounds in model
         * axes are a conservative stand-in. */
        float boxMatrix[3][3];
        QuaternionMatrix(box->rotation, boxMatrix);
        ObbFromMatrix(out, iterator->instanceMatrix);
        for (uint32_t r = 0u; r < 3u; ++r)
            out->half[r] = scale[r] * (AbsFloat(boxMatrix[r][0]) * box->halfExtent[0] +
                                       AbsFloat(boxMatrix[r][1]) * box->halfExtent[1] +
                                       AbsFloat(boxMatrix[r][2]) * box->halfExtent[2]);
    }
}

/* Transform a small contiguous batch before running its narrow phase. This
 * keeps the transform
 * and offset loops cache-friendly without a page-sized
 * scratch array; the source order and
 * arithmetic match the full-array path. */
static inline uint32_t InstanceBoxBatch(MeshBoxIterator *iterator, uint32_t first,
                                        const float offset[3], MeshObb output[MESH_BOX_BATCH_SIZE])
{
    const uint32_t remaining = iterator->count - first;
    const uint32_t count = remaining < MESH_BOX_BATCH_SIZE ? remaining : MESH_BOX_BATCH_SIZE;
    for (uint32_t index = 0u; index < count; ++index)
        InstanceBox(iterator, first + index, &output[index]);
    for (uint32_t index = 0u; index < count; ++index)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            output[index].center[axis] += offset[axis];
    return count;
}

static void ComputeInstanceBounds(const MeshShape *shape, const LaiueMeshTransformV1 *transform,
                                  float outMin[3], float outMax[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        outMin[axis] = transform->position.local[axis];
        outMax[axis] = transform->position.local[axis];
    }
    if (shape == NULL)
        return;
    MeshObb bounds;
    float matrix[3][3];
    QuaternionMatrix(transform->rotation, matrix);
    ObbFromMatrix(&bounds, matrix);
    float scaledCenter[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        scaledCenter[axis] =
            0.5f * (shape->boundsMin[axis] + shape->boundsMax[axis]) * transform->scale[axis];
        bounds.half[axis] =
            0.5f * (shape->boundsMax[axis] - shape->boundsMin[axis]) * transform->scale[axis];
    }
    MatrixApply(matrix, scaledCenter, bounds.center);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        bounds.center[axis] += transform->position.local[axis];
    ObbAabb(&bounds, outMin, outMax);
    MeshBoxIterator iterator;
    BeginInstanceBoxes(shape, transform, &iterator);
    for (uint32_t i = 0u; i < iterator.count; ++i)
    {
        MeshObb box;
        float boxMin[3];
        float boxMax[3];
        InstanceBox(&iterator, i, &box);
        ObbAabb(&box, boxMin, boxMax);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            outMin[axis] = MinFloat(outMin[axis], boxMin[axis]);
            outMax[axis] = MaxFloat(outMax[axis], boxMax[axis]);
        }
    }
}

/* ---- cell table -------------------------------------------------------- */

static uint64_t HashCell(const LaiueMeshCellV1 *cell)
{
    uint64_t hash = (uint64_t)cell->x * UINT64_C(0x9E3779B97F4A7C15);
    hash ^= (uint64_t)cell->y * UINT64_C(0xC2B2AE3D27D4EB4F) + (hash << 6) + (hash >> 2);
    hash ^= (uint64_t)cell->z * UINT64_C(0x165667B19E3779F9) + (hash << 6) + (hash >> 2);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xFF51AFD7ED558CCD);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xC4CEB9FE1A85EC53);
    hash ^= hash >> 33;
    return hash;
}

/* The world's cells and the provider's populated cells share one table
 * implementation; MeshKeys says where the coordinates live. */
typedef struct MeshKeys
{
    const void *items;
    size_t stride;
} MeshKeys;

static const LaiueMeshCellV1 *KeyAt(MeshKeys keys, uint32_t index)
{
    return (const LaiueMeshCellV1 *)((const unsigned char *)keys.items + keys.stride * index);
}

static uint32_t TableFind(const MeshTable *table, MeshKeys keys, const LaiueMeshCellV1 *coord)
{
    if (table->capacity == 0u)
        return MESH_NO_INDEX;
    const uint32_t mask = table->capacity - 1u;
    uint32_t position = (uint32_t)HashCell(coord) & mask;
    for (;;)
    {
        const uint32_t entry = table->entries[position];
        if (entry == 0u)
            return MESH_NO_INDEX;
        if (CellEqual(KeyAt(keys, entry - 1u), coord))
            return entry - 1u;
        position = (position + 1u) & mask;
    }
}

static void TableInsert(MeshTable *table, MeshKeys keys, uint32_t index)
{
    const uint32_t mask = table->capacity - 1u;
    uint32_t position = (uint32_t)HashCell(KeyAt(keys, index)) & mask;
    while (table->entries[position] != 0u)
        position = (position + 1u) & mask;
    table->entries[position] = index + 1u;
}

static void TableRebuild(MeshTable *table, MeshKeys keys, uint32_t count)
{
    if (table->capacity == 0u)
        return;
    memset(table->entries, 0, sizeof(uint32_t) * table->capacity);
    for (uint32_t i = 0u; i < count; ++i)
        TableInsert(table, keys, i);
}

static uint32_t TableFindEntry(const MeshTable *table, MeshKeys keys, uint32_t index)
{
    const uint32_t mask = table->capacity - 1u;
    uint32_t position = (uint32_t)HashCell(KeyAt(keys, index)) & mask;
    while (table->entries[position] != index + 1u)
        position = (position + 1u) & mask;
    return position;
}

/* Linear probing with backward-shift deletion keeps every chain intact
 * without tombstones. */
static void TableRemove(MeshTable *table, MeshKeys keys, uint32_t index)
{
    const uint32_t mask = table->capacity - 1u;
    uint32_t hole = TableFindEntry(table, keys, index);
    table->entries[hole] = 0u;
    uint32_t position = (hole + 1u) & mask;
    while (table->entries[position] != 0u)
    {
        const uint32_t entry = table->entries[position];
        const uint32_t home = (uint32_t)HashCell(KeyAt(keys, entry - 1u)) & mask;
        /* The entry may move into the hole when its home is not inside the
         * cyclic interval (hole, position]. */
        const bool homeInside = hole <= position ? (home > hole && home <= position)
                                                 : (home > hole || home <= position);
        if (!homeInside)
        {
            table->entries[hole] = entry;
            table->entries[position] = 0u;
            hole = position;
        }
        position = (position + 1u) & mask;
    }
}

/* Removes element index by moving the last element into its place; the
 * caller moves the array element itself afterwards. */
static void TableMoveLast(MeshTable *table, MeshKeys keys, uint32_t index, uint32_t last)
{
    TableRemove(table, keys, index);
    if (index != last)
        table->entries[TableFindEntry(table, keys, last)] = index + 1u;
}

/* Room for one element beyond existing, keeping the load at most 1/2. */
static bool TableEnsure(MeshTable *table, MeshKeys keys, uint32_t existing)
{
    const uint64_t count = (uint64_t)existing + 1u;
    if (table->capacity != 0u && count * 2u <= table->capacity)
        return true;
    uint64_t capacity = table->capacity == 0u ? 64u : (uint64_t)table->capacity * 2u;
    while (count * 2u > capacity)
        capacity *= 2u;
    if (capacity > (UINT64_C(1) << 31))
        return false;
    uint32_t *entries = (uint32_t *)PlatformAllocate(sizeof(uint32_t) * (size_t)capacity, true);
    if (entries == NULL)
        return false;
    PlatformFree(table->entries);
    table->entries = entries;
    table->capacity = (uint32_t)capacity;
    TableRebuild(table, keys, existing);
    return true;
}

static MeshKeys CellKeys(const LaiueMeshWorldV1 *world)
{
    MeshKeys keys = {world->cells, sizeof(MeshCell)};
    return keys;
}

static MeshKeys PopulatedKeys(const LaiueMeshWorldV1 *world)
{
    MeshKeys keys = {world->populated, sizeof(MeshPopulated)};
    return keys;
}

static uint32_t FindCell(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *coord)
{
    return TableFind(&world->cellTable, CellKeys(world), coord);
}

/* Returns array grown to hold required elements (possibly the same pointer)
 * or NULL, leaving array untouched.  New elements are not zeroed. */
static void *GrowArray(void *array, uint32_t *capacity, uint32_t required, size_t elementSize,
                       uint32_t limit)
{
    if (required <= *capacity)
        return array;
    if (required > limit)
        return NULL;
    uint64_t next = *capacity == 0u ? 8u : (uint64_t)*capacity * 2u;
    while (next < required)
        next *= 2u;
    if (next > limit)
        next = limit;
    void *grown = PlatformReallocate(array, elementSize * (size_t)next, false);
    if (grown != NULL)
        *capacity = (uint32_t)next;
    return grown;
}

static void UpdateReach(LaiueMeshWorldV1 *world, const float boundsMin[3], const float boundsMax[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        world->reach = MaxFloat(world->reach, -boundsMin[axis]);
        world->reach = MaxFloat(world->reach, boundsMax[axis] - world->cellSize);
    }
}

static void RecomputeCellBounds(LaiueMeshWorldV1 *world, MeshCell *cell)
{
    for (uint32_t i = 0u; i < cell->count; ++i)
    {
        const MeshSlot *slot = &world->slots[cell->instances[i]];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (i == 0u || slot->boundsMin[axis] < cell->boundsMin[axis])
                cell->boundsMin[axis] = slot->boundsMin[axis];
            if (i == 0u || slot->boundsMax[axis] > cell->boundsMax[axis])
                cell->boundsMax[axis] = slot->boundsMax[axis];
        }
    }
    UpdateReach(world, cell->boundsMin, cell->boundsMax);
}

static void TouchCell(LaiueMeshWorldV1 *world, MeshCell *cell)
{
    cell->revision = ++world->revisionCounter;
}

/* Finds the cell for coord, or prepares an empty one, with room for one more
 * instance.  Nothing observable changes on failure. */
static uint32_t ReserveCell(LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *coord)
{
    uint32_t index = FindCell(world, coord);
    if (index != MESH_NO_INDEX)
    {
        MeshCell *cell = &world->cells[index];
        uint32_t *instances =
            (uint32_t *)GrowArray(cell->instances, &cell->capacity, cell->count + 1u,
                                  sizeof(uint32_t), MESH_MAXIMUM_INSTANCES_LIMIT);
        if (instances == NULL)
            return MESH_NO_INDEX;
        cell->instances = instances;
        return index;
    }
    MeshCell *cells =
        (MeshCell *)GrowArray(world->cells, &world->cellCapacity, world->cellCount + 1u,
                              sizeof(MeshCell), MESH_MAXIMUM_INSTANCES_LIMIT);
    if (cells == NULL)
        return MESH_NO_INDEX;
    world->cells = cells;
    if (!TableEnsure(&world->cellTable, CellKeys(world), world->cellCount))
        return MESH_NO_INDEX;
    uint32_t *instances = (uint32_t *)PlatformAllocate(sizeof(uint32_t) * 4u, false);
    if (instances == NULL)
        return MESH_NO_INDEX;
    index = world->cellCount++;
    MeshCell *cell = &world->cells[index];
    memset(cell, 0, sizeof(*cell));
    cell->coord = *coord;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const int64_t value = CellComponent(coord, axis);
        if (index == 0u || value < world->occupiedMin[axis])
            world->occupiedMin[axis] = value;
        if (index == 0u || value > world->occupiedMax[axis])
            world->occupiedMax[axis] = value;
    }
    cell->instances = instances;
    cell->capacity = 4u;
    TableInsert(&world->cellTable, CellKeys(world), index);
    return index;
}

static void AppendToCell(LaiueMeshWorldV1 *world, uint32_t cellIndex, uint32_t slotIndex)
{
    MeshCell *cell = &world->cells[cellIndex];
    MeshSlot *slot = &world->slots[slotIndex];
    cell->instances[cell->count++] = slotIndex;
    slot->cell = cellIndex;
    RecomputeCellBounds(world, cell);
    TouchCell(world, cell);
}

static void RecomputeOccupiedBounds(LaiueMeshWorldV1 *world)
{
    world->deletionsSinceBounds = 0u;
    for (uint32_t i = 0u; i < world->cellCount; ++i)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const int64_t value = CellComponent(&world->cells[i].coord, axis);
            if (i == 0u || value < world->occupiedMin[axis])
                world->occupiedMin[axis] = value;
            if (i == 0u || value > world->occupiedMax[axis])
                world->occupiedMax[axis] = value;
        }
}

static void DeleteCell(LaiueMeshWorldV1 *world, uint32_t cellIndex)
{
    const uint32_t last = world->cellCount - 1u;
    TableMoveLast(&world->cellTable, CellKeys(world), cellIndex, last);
    PlatformFree(world->cells[cellIndex].instances);
    if (cellIndex != last)
    {
        world->cells[cellIndex] = world->cells[last];
        const MeshCell *moved = &world->cells[cellIndex];
        for (uint32_t i = 0u; i < moved->count; ++i)
            world->slots[moved->instances[i]].cell = cellIndex;
    }
    memset(&world->cells[last], 0, sizeof(MeshCell));
    world->cellCount = last;
    if (++world->deletionsSinceBounds > world->cellCount)
        RecomputeOccupiedBounds(world);
}

static void RemoveFromCell(LaiueMeshWorldV1 *world, uint32_t cellIndex, uint32_t slotIndex)
{
    MeshCell *cell = &world->cells[cellIndex];
    uint32_t position = 0u;
    while (position < cell->count && cell->instances[position] != slotIndex)
        ++position;
    if (position == cell->count)
        return;
    for (uint32_t i = position + 1u; i < cell->count; ++i)
        cell->instances[i - 1u] = cell->instances[i];
    --cell->count;
    if (cell->count == 0u)
    {
        DeleteCell(world, cellIndex);
        return;
    }
    RecomputeCellBounds(world, cell);
    TouchCell(world, cell);
}

/* ---- handles ----------------------------------------------------------- */

static LaiueMeshInstanceV1 MakeHandle(const LaiueMeshWorldV1 *world, uint32_t slotIndex)
{
    return ((uint64_t)world->slots[slotIndex].generation << 32) | (uint64_t)(slotIndex + 1u);
}

static uint32_t ResolveHandle(const LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance)
{
    const uint32_t low = (uint32_t)(instance & UINT32_MAX);
    if (low == 0u || low > world->slotCount)
        return MESH_NO_INDEX;
    const uint32_t index = low - 1u;
    const MeshSlot *slot = &world->slots[index];
    if (slot->live == 0u || slot->generation != (uint32_t)(instance >> 32))
        return MESH_NO_INDEX;
    return index;
}

static void FillInfo(const LaiueMeshWorldV1 *world, uint32_t slotIndex,
                     LaiueMeshInstanceInfoV1 *output)
{
    const MeshSlot *slot = &world->slots[slotIndex];
    output->handle = MakeHandle(world, slotIndex);
    output->model = slot->model;
    output->flags = slot->flags;
    output->transform = slot->transform;
    output->userData = slot->userData;
}

/* ---- lifecycle ---------------------------------------------------------- */

static uint32_t WorldCreate(const LaiueMeshWorldConfigV1 *config, LaiueMeshWorldV1 **outWorld)
{
    if (outWorld == NULL)
        return 0u;
    *outWorld = NULL;
    if (config == NULL || config->structSize < sizeof(LaiueMeshWorldConfigV1) ||
        !FiniteFloat(config->cellSize) || config->cellSize < LAIUE_MESH_WORLD_MIN_CELL_SIZE ||
        config->cellSize > LAIUE_MESH_WORLD_MAX_CELL_SIZE ||
        config->maximumInstances > MESH_MAXIMUM_INSTANCES_LIMIT)
        return 0u;
    LaiueMeshWorldV1 *world = (LaiueMeshWorldV1 *)PlatformAllocate(sizeof(*world), true);
    if (world == NULL)
        return 0u;
    if (!PlatformRwLockInitialize(&world->lock))
    {
        PlatformFree(world);
        return 0u;
    }
    world->cellSize = config->cellSize;
    world->maximumInstances =
        config->maximumInstances == 0u ? MESH_DEFAULT_MAXIMUM_INSTANCES : config->maximumInstances;
    world->freeHead = MESH_NO_INDEX;
    *outWorld = world;
    return 1u;
}

static void WorldDestroy(LaiueMeshWorldV1 *world)
{
    if (world == NULL)
        return;
    for (uint32_t i = 0u; i < world->cellCount; ++i)
        PlatformFree(world->cells[i].instances);
    for (uint32_t i = 0u; i < world->shapeCount; ++i)
        PlatformFree(world->shapes[i].boxes);
    for (uint32_t i = 0u; i < world->populatedCount; ++i)
        PlatformFree(world->populated[i].handles);
    PlatformFree(world->populated);
    PlatformFree(world->populatedTable.entries);
    PlatformFree(world->providerScratch);
    PlatformFree(world->cells);
    PlatformFree(world->cellTable.entries);
    PlatformFree(world->slots);
    PlatformFree(world->shapes);
    PlatformRwLockDestroy(&world->lock);
    PlatformFree(world);
}

static float WorldCellSize(const LaiueMeshWorldV1 *world)
{
    return world != NULL ? world->cellSize : 0.0f;
}

static void LockShared(const LaiueMeshWorldV1 *world)
{
    PlatformRwLockAcquireShared((PlatformRwLock *)&world->lock);
}

static void UnlockShared(const LaiueMeshWorldV1 *world)
{
    PlatformRwLockReleaseShared((PlatformRwLock *)&world->lock);
}

/* ---- shapes and instances ---------------------------------------------- */

static bool ValidExtentVector(const float value[3])
{
    return FiniteVector(value) && AbsFloat(value[0]) <= MESH_MAX_SHAPE_EXTENT &&
           AbsFloat(value[1]) <= MESH_MAX_SHAPE_EXTENT &&
           AbsFloat(value[2]) <= MESH_MAX_SHAPE_EXTENT;
}

static uint32_t WorldRegisterShape(LaiueMeshWorldV1 *world, uint32_t model,
                                   const LaiueMeshShapeV1 *shape)
{
    if (world == NULL || shape == NULL || shape->structSize < sizeof(LaiueMeshShapeV1) ||
        (shape->flags & ~LAIUE_MESH_SHAPE_COLLIDE_BOUNDS) != 0u ||
        shape->boxCount > LAIUE_MESH_WORLD_MAX_SHAPE_BOXES ||
        (shape->boxCount != 0u && shape->boxes == NULL) || !ValidExtentVector(shape->boundsMin) ||
        !ValidExtentVector(shape->boundsMax))
        return 0u;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (shape->boundsMin[axis] > shape->boundsMax[axis])
            return 0u;
    LaiueMeshBoxV1 *boxes = NULL;
    if (shape->boxCount != 0u)
    {
        boxes = (LaiueMeshBoxV1 *)PlatformAllocate(sizeof(LaiueMeshBoxV1) * shape->boxCount, false);
        if (boxes == NULL)
            return 0u;
        for (uint32_t i = 0u; i < shape->boxCount; ++i)
        {
            const LaiueMeshBoxV1 *source = &shape->boxes[i];
            boxes[i] = *source;
            bool valid = ValidExtentVector(source->center) &&
                         ValidExtentVector(source->halfExtent) &&
                         NormalizeQuaternion(source->rotation, boxes[i].rotation);
            for (uint32_t axis = 0u; valid && axis < 3u; ++axis)
                valid = source->halfExtent[axis] >= 0.0f;
            if (!valid)
            {
                PlatformFree(boxes);
                return 0u;
            }
        }
    }

    PlatformRwLockAcquireExclusive(&world->lock);
    bool found = false;
    const uint32_t index = FindShapeIndex(world, model, &found);
    if (!found)
    {
        MeshShape *shapes =
            (MeshShape *)GrowArray(world->shapes, &world->shapeCapacity, world->shapeCount + 1u,
                                   sizeof(MeshShape), UINT32_MAX / 2u);
        if (shapes == NULL)
        {
            PlatformRwLockReleaseExclusive(&world->lock);
            PlatformFree(boxes);
            return 0u;
        }
        world->shapes = shapes;
        for (uint32_t i = world->shapeCount; i > index; --i)
            world->shapes[i] = world->shapes[i - 1u];
        ++world->shapeCount;
    }
    else
    {
        PlatformFree(world->shapes[index].boxes);
    }
    MeshShape *stored = &world->shapes[index];
    stored->model = model;
    stored->flags = shape->flags;
    stored->boxCount = shape->boxCount;
    stored->boxes = boxes;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        stored->boundsMin[axis] = shape->boundsMin[axis];
        stored->boundsMax[axis] = shape->boundsMax[axis];
    }

    /* Instances already placed with this model take the new shape. */
    for (uint32_t i = 0u; i < world->slotCount; ++i)
    {
        MeshSlot *slot = &world->slots[i];
        if (slot->live != 0u && slot->model == model)
            ComputeInstanceBounds(stored, &slot->transform, slot->boundsMin, slot->boundsMax);
    }
    for (uint32_t c = 0u; c < world->cellCount; ++c)
    {
        MeshCell *cell = &world->cells[c];
        for (uint32_t i = 0u; i < cell->count; ++i)
        {
            if (world->slots[cell->instances[i]].model == model)
            {
                RecomputeCellBounds(world, cell);
                TouchCell(world, cell);
                break;
            }
        }
    }
    PlatformRwLockReleaseExclusive(&world->lock);
    return 1u;
}

/* Caller holds the write lock; transform is normalized. */
static bool AddLocked(LaiueMeshWorldV1 *world, const LaiueMeshInstanceDescV1 *desc,
                      const LaiueMeshTransformV1 *transform, LaiueMeshInstanceV1 *outInstance)
{
    if (world->liveCount >= world->maximumInstances)
        return false;
    if (world->freeHead == MESH_NO_INDEX)
    {
        MeshSlot *slots =
            (MeshSlot *)GrowArray(world->slots, &world->slotCapacity, world->slotCount + 1u,
                                  sizeof(MeshSlot), world->maximumInstances);
        if (slots == NULL)
            return false;
        world->slots = slots;
    }
    const uint32_t cellIndex = ReserveCell(world, &transform->position.cell);
    if (cellIndex == MESH_NO_INDEX)
        return false;
    uint32_t slotIndex;
    if (world->freeHead != MESH_NO_INDEX)
    {
        slotIndex = world->freeHead;
        world->freeHead = world->slots[slotIndex].nextFree;
    }
    else
    {
        slotIndex = world->slotCount++;
        world->slots[slotIndex].generation = 1u;
    }
    MeshSlot *slot = &world->slots[slotIndex];
    slot->live = 1u;
    slot->model = desc->model;
    slot->flags = desc->flags;
    slot->userData = desc->userData;
    slot->transform = *transform;
    slot->nextFree = MESH_NO_INDEX;
    ComputeInstanceBounds(FindShape(world, desc->model), transform, slot->boundsMin,
                          slot->boundsMax);
    AppendToCell(world, cellIndex, slotIndex);
    ++world->liveCount;
    *outInstance = MakeHandle(world, slotIndex);
    return true;
}

static void RemoveLocked(LaiueMeshWorldV1 *world, uint32_t slotIndex)
{
    MeshSlot *slot = &world->slots[slotIndex];
    RemoveFromCell(world, slot->cell, slotIndex);
    slot->live = 0u;
    slot->generation = slot->generation == UINT32_MAX ? 1u : slot->generation + 1u;
    slot->nextFree = world->freeHead;
    world->freeHead = slotIndex;
    --world->liveCount;
}

static bool ValidDesc(const LaiueMeshInstanceDescV1 *desc)
{
    return desc != NULL && desc->structSize >= sizeof(LaiueMeshInstanceDescV1) &&
           (desc->flags & ~MESH_KNOWN_INSTANCE_FLAGS) == 0u;
}

static uint32_t WorldAdd(LaiueMeshWorldV1 *world, const LaiueMeshInstanceDescV1 *desc,
                         LaiueMeshInstanceV1 *outInstance)
{
    if (outInstance != NULL)
        *outInstance = 0u;
    LaiueMeshTransformV1 transform;
    if (world == NULL || outInstance == NULL || !ValidDesc(desc) ||
        !NormalizeTransform(world->cellSize, &desc->transform, &transform))
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    const bool added = AddLocked(world, desc, &transform, outInstance);
    PlatformRwLockReleaseExclusive(&world->lock);
    return added ? 1u : 0u;
}

static uint32_t WorldRemove(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance)
{
    if (world == NULL)
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    const uint32_t slotIndex = ResolveHandle(world, instance);
    if (slotIndex != MESH_NO_INDEX)
        RemoveLocked(world, slotIndex);
    PlatformRwLockReleaseExclusive(&world->lock);
    return slotIndex != MESH_NO_INDEX ? 1u : 0u;
}

static uint32_t WorldSetTransform(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance,
                                  const LaiueMeshTransformV1 *transform)
{
    LaiueMeshTransformV1 normalized;
    if (world == NULL || !NormalizeTransform(world->cellSize, transform, &normalized))
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    const uint32_t slotIndex = ResolveHandle(world, instance);
    if (slotIndex == MESH_NO_INDEX)
    {
        PlatformRwLockReleaseExclusive(&world->lock);
        return 0u;
    }
    MeshSlot *slot = &world->slots[slotIndex];
    const uint32_t oldCell = slot->cell;
    if (CellEqual(&world->cells[oldCell].coord, &normalized.position.cell))
    {
        slot->transform = normalized;
        ComputeInstanceBounds(FindShape(world, slot->model), &normalized, slot->boundsMin,
                              slot->boundsMax);
        RecomputeCellBounds(world, &world->cells[oldCell]);
        TouchCell(world, &world->cells[oldCell]);
        PlatformRwLockReleaseExclusive(&world->lock);
        return 1u;
    }
    const uint32_t newCell = ReserveCell(world, &normalized.position.cell);
    if (newCell == MESH_NO_INDEX)
    {
        PlatformRwLockReleaseExclusive(&world->lock);
        return 0u;
    }
    slot = &world->slots[slotIndex];
    slot->transform = normalized;
    ComputeInstanceBounds(FindShape(world, slot->model), &normalized, slot->boundsMin,
                          slot->boundsMax);
    AppendToCell(world, newCell, slotIndex);
    /* Removing the old cell may move the last cell into its slot; the move
     * also updates this instance when its new cell was the last one. */
    RemoveFromCell(world, oldCell, slotIndex);
    PlatformRwLockReleaseExclusive(&world->lock);
    return 1u;
}

static uint32_t WorldSetFlags(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance, uint32_t flags)
{
    if (world == NULL || (flags & ~MESH_KNOWN_INSTANCE_FLAGS) != 0u)
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    const uint32_t slotIndex = ResolveHandle(world, instance);
    uint32_t result = 0u;
    if (slotIndex != MESH_NO_INDEX)
    {
        MeshSlot *slot = &world->slots[slotIndex];
        if (slot->flags != flags)
        {
            slot->flags = flags;
            TouchCell(world, &world->cells[slot->cell]);
        }
        result = 1u;
    }
    PlatformRwLockReleaseExclusive(&world->lock);
    return result;
}

static uint32_t WorldGet(const LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance,
                         LaiueMeshInstanceInfoV1 *outInfo)
{
    if (world == NULL || outInfo == NULL)
        return 0u;
    LockShared(world);
    const uint32_t slotIndex = ResolveHandle(world, instance);
    if (slotIndex != MESH_NO_INDEX)
        FillInfo(world, slotIndex, outInfo);
    UnlockShared(world);
    return slotIndex != MESH_NO_INDEX ? 1u : 0u;
}

static uint32_t WorldInstanceCount(const LaiueMeshWorldV1 *world)
{
    if (world == NULL)
        return 0u;
    LockShared(world);
    const uint32_t count = world->liveCount;
    UnlockShared(world);
    return count;
}

static uint32_t WorldRebase(LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *shift)
{
    if (world == NULL || shift == NULL || !CellValid(shift))
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    for (uint32_t i = 0u; i < world->cellCount + world->populatedCount; ++i)
    {
        const LaiueMeshCellV1 *coord = i < world->cellCount
                                           ? &world->cells[i].coord
                                           : &world->populated[i - world->cellCount].coord;
        if (!CellInRange(coord->x - shift->x) || !CellInRange(coord->y - shift->y) ||
            !CellInRange(coord->z - shift->z))
        {
            PlatformRwLockReleaseExclusive(&world->lock);
            return 0u;
        }
    }
    for (uint32_t i = 0u; i < world->cellCount; ++i)
    {
        MeshCell *cell = &world->cells[i];
        cell->coord.x -= shift->x;
        cell->coord.y -= shift->y;
        cell->coord.z -= shift->z;
        for (uint32_t k = 0u; k < cell->count; ++k)
            world->slots[cell->instances[k]].transform.position.cell = cell->coord;
    }
    TableRebuild(&world->cellTable, CellKeys(world), world->cellCount);
    RecomputeOccupiedBounds(world);
    for (uint32_t i = 0u; i < world->populatedCount; ++i)
    {
        world->populated[i].coord.x -= shift->x;
        world->populated[i].coord.y -= shift->y;
        world->populated[i].coord.z -= shift->z;
    }
    TableRebuild(&world->populatedTable, PopulatedKeys(world), world->populatedCount);
    /* A stale scan centre only costs one extra scan; it never overflows. */
    world->streamComplete = 0u;
    PlatformRwLockReleaseExclusive(&world->lock);
    return 1u;
}

/* ---- cell provider ------------------------------------------------------ */

static void RemovePopulatedInstances(LaiueMeshWorldV1 *world, const MeshPopulated *populated)
{
    /* The application may have removed some already; their handles are
     * stale by now and resolve to nothing. */
    for (uint32_t i = 0u; i < populated->count; ++i)
    {
        const uint32_t slotIndex = ResolveHandle(world, populated->handles[i]);
        if (slotIndex != MESH_NO_INDEX)
            RemoveLocked(world, slotIndex);
    }
}

static void DropPopulated(LaiueMeshWorldV1 *world, uint32_t index)
{
    RemovePopulatedInstances(world, &world->populated[index]);
    PlatformFree(world->populated[index].handles);
    const uint32_t last = world->populatedCount - 1u;
    TableMoveLast(&world->populatedTable, PopulatedKeys(world), index, last);
    if (index != last)
        world->populated[index] = world->populated[last];
    world->populatedCount = last;
}

static void ClearProvider(LaiueMeshWorldV1 *world)
{
    while (world->populatedCount != 0u)
        DropPopulated(world, world->populatedCount - 1u);
    PlatformFree(world->providerScratch);
    world->providerScratch = NULL;
    world->hasProvider = 0u;
    world->streamComplete = 0u;
}

static uint32_t WorldSetProvider(LaiueMeshWorldV1 *world, const LaiueMeshCellProviderV1 *provider)
{
    if (world == NULL)
        return 0u;
    LaiueMeshInstanceDescV1 *scratch = NULL;
    if (provider != NULL)
    {
        if (provider->structSize < sizeof(LaiueMeshCellProviderV1) || provider->populate == NULL ||
            provider->maximumInstancesPerCell == 0u ||
            provider->maximumInstancesPerCell > LAIUE_MESH_WORLD_MAX_PROVIDER_INSTANCES)
            return 0u;
        scratch = (LaiueMeshInstanceDescV1 *)PlatformAllocate(
            sizeof(LaiueMeshInstanceDescV1) * provider->maximumInstancesPerCell, false);
        if (scratch == NULL)
            return 0u;
    }
    PlatformRwLockAcquireExclusive(&world->lock);
    ClearProvider(world);
    if (provider != NULL)
    {
        world->provider = *provider;
        world->provider.structSize = sizeof(LaiueMeshCellProviderV1);
        world->providerScratch = scratch;
        world->hasProvider = 1u;
    }
    PlatformRwLockReleaseExclusive(&world->lock);
    return 1u;
}

/* Squared distance between the boxes of two cells offset by (dx, dy, dz)
 * cells, in cells.  Gaps are clamped so far-apart cells cannot overflow. */
static int64_t CellGapSquared(int64_t dx, int64_t dy, int64_t dz)
{
    const int64_t offsets[3] = {dx, dy, dz};
    int64_t sum = 0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int64_t gap = offsets[axis] < 0 ? -offsets[axis] : offsets[axis];
        gap = gap > 0 ? gap - 1 : 0;
        if (gap > (INT64_C(1) << 20))
            gap = INT64_C(1) << 20;
        sum += gap * gap;
    }
    return sum;
}

/* Fills one cell; on failure nothing it produced stays behind. */
static bool PopulateCell(LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell)
{
    MeshPopulated *grownList = (MeshPopulated *)GrowArray(
        world->populated, &world->populatedCapacity, world->populatedCount + 1u,
        sizeof(MeshPopulated), MESH_MAXIMUM_INSTANCES_LIMIT);
    if (grownList == NULL)
        return false;
    world->populated = grownList;
    if (!TableEnsure(&world->populatedTable, PopulatedKeys(world), world->populatedCount))
        return false;
    uint32_t count = 0u;
    if (world->provider.populate(world->provider.context, cell, world->providerScratch,
                                 world->provider.maximumInstancesPerCell, &count) == 0u ||
        count > world->provider.maximumInstancesPerCell)
        return false;
    LaiueMeshInstanceV1 *handles = NULL;
    if (count != 0u)
    {
        handles =
            (LaiueMeshInstanceV1 *)PlatformAllocate(sizeof(LaiueMeshInstanceV1) * count, false);
        if (handles == NULL)
            return false;
    }
    MeshPopulated record = {*cell, handles, 0u};
    for (uint32_t i = 0u; i < count; ++i)
    {
        const LaiueMeshInstanceDescV1 *desc = &world->providerScratch[i];
        LaiueMeshTransformV1 relative = desc->transform;
        const LaiueMeshCellV1 *offset = &relative.position.cell;
        bool valid = ValidDesc(desc) && offset->x >= -MESH_MAX_PROVIDER_CELL_OFFSET &&
                     offset->x <= MESH_MAX_PROVIDER_CELL_OFFSET &&
                     offset->y >= -MESH_MAX_PROVIDER_CELL_OFFSET &&
                     offset->y <= MESH_MAX_PROVIDER_CELL_OFFSET &&
                     offset->z >= -MESH_MAX_PROVIDER_CELL_OFFSET &&
                     offset->z <= MESH_MAX_PROVIDER_CELL_OFFSET;
        LaiueMeshTransformV1 transform;
        if (valid)
        {
            relative.position.cell.x += cell->x;
            relative.position.cell.y += cell->y;
            relative.position.cell.z += cell->z;
            valid = NormalizeTransform(world->cellSize, &relative, &transform) &&
                    AddLocked(world, desc, &transform, &handles[record.count]);
        }
        if (!valid)
        {
            RemovePopulatedInstances(world, &record);
            PlatformFree(handles);
            return false;
        }
        ++record.count;
    }
    world->populated[world->populatedCount] = record;
    TableInsert(&world->populatedTable, PopulatedKeys(world), world->populatedCount);
    ++world->populatedCount;
    return true;
}

static uint32_t WorldStream(LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *center,
                            float radius, uint32_t cellBudget, uint32_t *outPending)
{
    if (outPending != NULL)
        *outPending = 0u;
    LaiueMeshPositionV1 normalized;
    if (world == NULL || !FiniteFloat(radius) || radius < 0.0f ||
        radius > (float)MESH_MAX_STREAM_CELLS * world->cellSize ||
        !NormalizePosition(world->cellSize, center, &normalized))
        return 0u;
    PlatformRwLockAcquireExclusive(&world->lock);
    if (world->hasProvider == 0u || (world->streamComplete != 0u && world->streamRadius == radius &&
                                     CellEqual(&world->streamCenter, &normalized.cell)))
    {
        PlatformRwLockReleaseExclusive(&world->lock);
        return 1u;
    }
    const LaiueMeshCellV1 origin = normalized.cell;
    /* Every cell in range of the previous complete scan is still filled:
     * removal keeps one cell more than the fill range, so only the new
     * shell needs the populated-set lookup. */
    const bool previousComplete = world->streamComplete != 0u && world->streamRadius == radius;
    const int64_t movedX = origin.x - world->streamCenter.x;
    const int64_t movedY = origin.y - world->streamCenter.y;
    const int64_t movedZ = origin.z - world->streamCenter.z;
    /* A cell is in range when its box comes within radius of the centre
     * cell's box; integer gaps in cells compare against (radius/cell)^2. */
    const double radiusCells = (double)radius / (double)world->cellSize;
    const int64_t fillSquared = (int64_t)FloorDouble(radiusCells * radiusCells);
    const int64_t keepSquared = (int64_t)FloorDouble((radiusCells + 1.0) * (radiusCells + 1.0));

    for (uint32_t i = world->populatedCount; i-- > 0u;)
    {
        const LaiueMeshCellV1 *coord = &world->populated[i].coord;
        const int64_t dx = coord->x - origin.x;
        const int64_t dy = coord->y - origin.y;
        const int64_t dz = coord->z - origin.z;
        const int64_t limit = MESH_MAX_STREAM_CELLS + 2;
        if (dx < -limit || dx > limit || dy < -limit || dy > limit || dz < -limit || dz > limit ||
            CellGapSquared(dx, dy, dz) > keepSquared)
            DropPopulated(world, i);
    }

    /* Rings of growing Chebyshev distance, so the nearest cells fill first. */
    const int64_t range = (int64_t)radiusCells + 1;
    uint32_t filled = 0u;
    uint32_t pending = 0u;
    uint32_t result = 1u;
    for (int64_t ring = 0; ring <= range && result != 0u; ++ring)
        for (int64_t dz = -ring; dz <= ring && result != 0u; ++dz)
            for (int64_t dy = -ring; dy <= ring && result != 0u; ++dy)
            {
                const bool shell = dz == -ring || dz == ring || dy == -ring || dy == ring;
                const int64_t step = shell || ring == 0 ? 1 : 2 * ring;
                for (int64_t dx = -ring; dx <= ring; dx += step)
                {
                    if (CellGapSquared(dx, dy, dz) > fillSquared ||
                        (previousComplete &&
                         CellGapSquared(dx + movedX, dy + movedY, dz + movedZ) <= fillSquared))
                        continue;
                    const LaiueMeshCellV1 cell = {origin.x + dx, origin.y + dy, origin.z + dz};
                    if (!CellValid(&cell) || TableFind(&world->populatedTable, PopulatedKeys(world),
                                                       &cell) != MESH_NO_INDEX)
                        continue;
                    if (cellBudget != 0u && filled >= cellBudget)
                    {
                        ++pending;
                        continue;
                    }
                    if (!PopulateCell(world, &cell))
                    {
                        result = 0u;
                        break;
                    }
                    ++filled;
                }
            }
    world->streamCenter = origin;
    world->streamRadius = radius;
    world->streamComplete = result != 0u && pending == 0u ? 1u : 0u;
    PlatformRwLockReleaseExclusive(&world->lock);
    if (outPending != NULL)
        *outPending = pending;
    return result;
}

/* ---- sorting ------------------------------------------------------------ */

typedef int (*MeshCompareFn)(const void *left, const void *right);

static void SwapBytes(unsigned char *left, unsigned char *right, size_t size)
{
    for (size_t i = 0u; i < size; ++i)
    {
        const unsigned char value = left[i];
        left[i] = right[i];
        right[i] = value;
    }
}

static void SiftDown(unsigned char *base, size_t size, uint32_t root, uint32_t count,
                     MeshCompareFn compare)
{
    for (;;)
    {
        uint32_t child = root * 2u + 1u;
        if (child >= count)
            return;
        if (child + 1u < count && compare(base + child * size, base + (child + 1u) * size) < 0)
            ++child;
        if (compare(base + root * size, base + child * size) >= 0)
            return;
        SwapBytes(base + root * size, base + child * size, size);
        root = child;
    }
}

static void HeapSort(void *array, uint32_t count, size_t size, MeshCompareFn compare)
{
    unsigned char *base = (unsigned char *)array;
    if (count < 2u)
        return;
    for (uint32_t i = count / 2u; i-- > 0u;)
        SiftDown(base, size, i, count, compare);
    for (uint32_t end = count - 1u; end > 0u; --end)
    {
        SwapBytes(base, base + end * size, size);
        SiftDown(base, size, 0u, end, compare);
    }
}

static int CompareInt64(int64_t left, int64_t right)
{
    return left < right ? -1 : (left > right ? 1 : 0);
}

static int CompareCells(const void *left, const void *right)
{
    const LaiueMeshCellV1 *a = (const LaiueMeshCellV1 *)left;
    const LaiueMeshCellV1 *b = (const LaiueMeshCellV1 *)right;
    int result = CompareInt64(a->x, b->x);
    if (result == 0)
        result = CompareInt64(a->y, b->y);
    if (result == 0)
        result = CompareInt64(a->z, b->z);
    return result;
}

static int CompareFloat(float left, float right)
{
    return left < right ? -1 : (left > right ? 1 : 0);
}

static int CompareColliders(const void *left, const void *right)
{
    const LaiueMeshColliderV1 *a = (const LaiueMeshColliderV1 *)left;
    const LaiueMeshColliderV1 *b = (const LaiueMeshColliderV1 *)right;
    if (a->instance != b->instance)
        return a->instance < b->instance ? -1 : 1;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int result = CompareFloat(a->minimum[axis], b->minimum[axis]);
        if (result == 0)
            result = CompareFloat(a->maximum[axis], b->maximum[axis]);
        if (result != 0)
            return result;
    }
    return 0;
}

/* ---- cell visiting ------------------------------------------------------ */

typedef bool (*MeshCellVisitorFn)(void *context, const LaiueMeshWorldV1 *world,
                                  const MeshCell *cell, const float offset[3]);

static bool BoundsOverlap(const float aMin[3], const float aMax[3], const float bMin[3],
                          const float bMax[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (aMin[axis] > bMax[axis] || aMax[axis] < bMin[axis])
            return false;
    return true;
}

static bool VisitOne(const LaiueMeshWorldV1 *world, const MeshCell *cell, const float offset[3],
                     const float queryMin[3], const float queryMax[3], MeshCellVisitorFn visit,
                     void *context)
{
    float cellMin[3];
    float cellMax[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        cellMin[axis] = offset[axis] + cell->boundsMin[axis];
        cellMax[axis] = offset[axis] + cell->boundsMax[axis];
    }
    if (!BoundsOverlap(cellMin, cellMax, queryMin, queryMax))
        return true;
    return visit(context, world, cell, offset);
}

/* Visits each occupied cell whose bounds overlap the query box, given in
 * metres relative to the reference cell origin.  Walks the cell range when
 * it is small and every occupied cell otherwise; visitors must not depend
 * on the order. */
static void VisitCells(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *reference,
                       const float queryMin[3], const float queryMax[3], MeshCellVisitorFn visit,
                       void *context)
{
    if (world->cellCount == 0u)
        return;
    const double cellSize = world->cellSize;
    int64_t low[3];
    int64_t high[3];
    double volume = 1.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        low[axis] = (int64_t)FloorDouble(((double)queryMin[axis] - world->reach) / cellSize);
        high[axis] = (int64_t)FloorDouble(((double)queryMax[axis] + world->reach) / cellSize);
        /* No occupied cell lies outside the occupied bounds. */
        const int64_t referenceValue = CellComponent(reference, axis);
        const int64_t occupiedLow = world->occupiedMin[axis] - referenceValue;
        const int64_t occupiedHigh = world->occupiedMax[axis] - referenceValue;
        if (low[axis] < occupiedLow)
            low[axis] = occupiedLow;
        if (high[axis] > occupiedHigh)
            high[axis] = occupiedHigh;
        if (low[axis] > high[axis])
            return;
        volume *= (double)(high[axis] - low[axis] + 1);
    }
    float offset[3];
    if (volume <= (double)world->cellCount)
    {
        for (int64_t z = low[2]; z <= high[2]; ++z)
            for (int64_t y = low[1]; y <= high[1]; ++y)
                for (int64_t x = low[0]; x <= high[0]; ++x)
                {
                    const LaiueMeshCellV1 coord = {reference->x + x, reference->y + y,
                                                   reference->z + z};
                    if (!CellValid(&coord))
                        continue;
                    const uint32_t index = FindCell(world, &coord);
                    if (index == MESH_NO_INDEX)
                        continue;
                    offset[0] = (float)((double)x * cellSize);
                    offset[1] = (float)((double)y * cellSize);
                    offset[2] = (float)((double)z * cellSize);
                    if (!VisitOne(world, &world->cells[index], offset, queryMin, queryMax, visit,
                                  context))
                        return;
                }
        return;
    }
    for (uint32_t i = 0u; i < world->cellCount; ++i)
    {
        const MeshCell *cell = &world->cells[i];
        if (!CellOffset(&cell->coord, reference, low, high, world->cellSize, offset))
            continue;
        if (!VisitOne(world, cell, offset, queryMin, queryMax, visit, context))
            return;
    }
}

static bool SlotBoundsOverlap(const MeshSlot *slot, const float offset[3], const float queryMin[3],
                              const float queryMax[3])
{
    float slotMin[3];
    float slotMax[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        slotMin[axis] = offset[axis] + slot->boundsMin[axis];
        slotMax[axis] = offset[axis] + slot->boundsMax[axis];
    }
    return BoundsOverlap(slotMin, slotMax, queryMin, queryMax);
}

/* ---- narrow phase ------------------------------------------------------- */

/* Separating axes between an axis-aligned box and an oriented box: three
 * world axes, three box axes and their nine cross products.  Degenerate
 * cross products (parallel edges) are skipped. */
static uint32_t SeparatingAxes(const MeshObb *box, float axes[15][3])
{
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        axes[count][0] = i == 0u ? 1.0f : 0.0f;
        axes[count][1] = i == 1u ? 1.0f : 0.0f;
        axes[count][2] = i == 2u ? 1.0f : 0.0f;
        ++count;
    }
    for (uint32_t j = 0u; j < 3u; ++j)
    {
        axes[count][0] = box->axis[j][0];
        axes[count][1] = box->axis[j][1];
        axes[count][2] = box->axis[j][2];
        ++count;
    }
    for (uint32_t i = 0u; i < 3u; ++i)
        for (uint32_t j = 0u; j < 3u; ++j)
        {
            const float *u = box->axis[j];
            float cross[3];
            /* e_i x u */
            if (i == 0u)
            {
                cross[0] = 0.0f;
                cross[1] = -u[2];
                cross[2] = u[1];
            }
            else if (i == 1u)
            {
                cross[0] = u[2];
                cross[1] = 0.0f;
                cross[2] = -u[0];
            }
            else
            {
                cross[0] = -u[1];
                cross[1] = u[0];
                cross[2] = 0.0f;
            }
            const float lengthSquared = Dot3(cross, cross);
            if (lengthSquared < 1.0e-6f)
                continue;
            const float inverse = 1.0f / ScalarSqrt(lengthSquared);
            axes[count][0] = cross[0] * inverse;
            axes[count][1] = cross[1] * inverse;
            axes[count][2] = cross[2] * inverse;
            ++count;
        }
    return count;
}

static float ProjectedRadius(const float axis[3], const float half[3], const MeshObb *box)
{
    return half[0] * AbsFloat(axis[0]) + half[1] * AbsFloat(axis[1]) + half[2] * AbsFloat(axis[2]) +
           box->half[0] * AbsFloat(Dot3(axis, box->axis[0])) +
           box->half[1] * AbsFloat(Dot3(axis, box->axis[1])) +
           box->half[2] * AbsFloat(Dot3(axis, box->axis[2]));
}

/* Swept separating-axis test.  Returns true with the time of first contact
 * in [0, 1] and a unit normal pointing against the motion.  A box that
 * already overlaps the mover counts only while the motion pushes deeper
 * along the axis of least penetration, so a mover can always leave. */
static bool SweepAgainstObb(const float start[3], const float half[3], const float delta[3],
                            const MeshObb *box, float *outTime, float outNormal[3])
{
    float axes[15][3];
    const uint32_t axisCount = SeparatingAxes(box, axes);
    const float relative[3] = {box->center[0] - start[0], box->center[1] - start[1],
                               box->center[2] - start[2]};
    float enter = -1.0e30f;
    float exit = 1.0e30f;
    float enterNormal[3] = {0.0f, 0.0f, 0.0f};
    float leastPenetration = 1.0e30f;
    float penetrationNormal[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0u; i < axisCount; ++i)
    {
        const float *axis = axes[i];
        const float radius = ProjectedRadius(axis, half, box);
        const float separation = Dot3(axis, relative);
        const float speed = Dot3(axis, delta);
        const float penetration = radius - AbsFloat(separation);
        if (penetration < leastPenetration)
        {
            leastPenetration = penetration;
            const float sign = separation > 0.0f ? -1.0f : 1.0f;
            penetrationNormal[0] = axis[0] * sign;
            penetrationNormal[1] = axis[1] * sign;
            penetrationNormal[2] = axis[2] * sign;
        }
        if (AbsFloat(speed) < 1.0e-12f)
        {
            if (penetration < 0.0f)
                return false;
            continue;
        }
        float first = (separation - radius) / speed;
        float last = (separation + radius) / speed;
        if (first > last)
        {
            const float swap = first;
            first = last;
            last = swap;
        }
        if (first > enter)
        {
            enter = first;
            const float sign = speed > 0.0f ? -1.0f : 1.0f;
            enterNormal[0] = axis[0] * sign;
            enterNormal[1] = axis[1] * sign;
            enterNormal[2] = axis[2] * sign;
        }
        if (last < exit)
            exit = last;
        if (enter > exit)
            return false;
    }
    if (exit < 0.0f || enter > 1.0f)
        return false;
    if (enter >= 0.0f)
    {
        *outTime = enter;
        outNormal[0] = enterNormal[0];
        outNormal[1] = enterNormal[1];
        outNormal[2] = enterNormal[2];
        return true;
    }
    if (Dot3(delta, penetrationNormal) >= 0.0f)
        return false;
    *outTime = 0.0f;
    outNormal[0] = penetrationNormal[0];
    outNormal[1] = penetrationNormal[1];
    outNormal[2] = penetrationNormal[2];
    return true;
}

/* Strict overlap: boxes that only touch do not overlap. */
static bool OverlapObb(const float center[3], const float half[3], const MeshObb *box)
{
    float axes[15][3];
    const uint32_t axisCount = SeparatingAxes(box, axes);
    const float relative[3] = {box->center[0] - center[0], box->center[1] - center[1],
                               box->center[2] - center[2]};
    for (uint32_t i = 0u; i < axisCount; ++i)
        if (AbsFloat(Dot3(axes[i], relative)) >= ProjectedRadius(axes[i], half, box))
            return false;
    return true;
}

static bool RayAgainstObb(const float origin[3], const float segment[3], const MeshObb *box,
                          float *outTime, float outNormal[3])
{
    const float relative[3] = {origin[0] - box->center[0], origin[1] - box->center[1],
                               origin[2] - box->center[2]};
    float enter = -1.0e30f;
    float exit = 1.0e30f;
    int32_t enterAxis = -1;
    float enterSign = 0.0f;
    for (uint32_t j = 0u; j < 3u; ++j)
    {
        const float start = Dot3(relative, box->axis[j]);
        const float direction = Dot3(segment, box->axis[j]);
        if (AbsFloat(direction) < 1.0e-12f)
        {
            if (AbsFloat(start) > box->half[j])
                return false;
            continue;
        }
        float first = (-box->half[j] - start) / direction;
        float last = (box->half[j] - start) / direction;
        if (first > last)
        {
            const float swap = first;
            first = last;
            last = swap;
        }
        if (first > enter)
        {
            enter = first;
            enterAxis = (int32_t)j;
            enterSign = direction > 0.0f ? -1.0f : 1.0f;
        }
        if (last < exit)
            exit = last;
        if (enter > exit)
            return false;
    }
    if (enterAxis < 0 || enter < 0.0f || enter > 1.0f)
        return false;
    *outTime = enter;
    for (uint32_t r = 0u; r < 3u; ++r)
        outNormal[r] = box->axis[enterAxis][r] * enterSign;
    return true;
}

/* ---- queries ------------------------------------------------------------ */

typedef struct MeshHitState
{
    const float *start;
    const float *half;
    const float *delta;
    float queryMin[3];
    float queryMax[3];
    bool hit;
    LaiueMeshHitV1 best;
} MeshHitState;

static void OfferHit(MeshHitState *state, float time, const float normal[3],
                     LaiueMeshInstanceV1 instance)
{
    if (state->hit &&
        (time > state->best.time || (time == state->best.time && instance >= state->best.instance)))
        return;
    state->hit = true;
    state->best.time = time;
    state->best.normal[0] = normal[0];
    state->best.normal[1] = normal[1];
    state->best.normal[2] = normal[2];
    state->best.instance = instance;
}

static bool SweepVisitor(void *context, const LaiueMeshWorldV1 *world, const MeshCell *cell,
                         const float offset[3])
{
    MeshHitState *state = (MeshHitState *)context;
    for (uint32_t i = 0u; i < cell->count; ++i)
    {
        const uint32_t slotIndex = cell->instances[i];
        const MeshSlot *slot = &world->slots[slotIndex];
        if ((slot->flags & LAIUE_MESH_INSTANCE_COLLIDABLE) == 0u ||
            !SlotBoundsOverlap(slot, offset, state->queryMin, state->queryMax))
            continue;
        MeshBoxIterator iterator;
        BeginInstanceBoxes(FindShape(world, slot->model), &slot->transform, &iterator);
        for (uint32_t first = 0u; first < iterator.count; first += MESH_BOX_BATCH_SIZE)
        {
            MeshObb boxes[MESH_BOX_BATCH_SIZE];
            const uint32_t count = InstanceBoxBatch(&iterator, first, offset, boxes);
            for (uint32_t b = 0u; b < count; ++b)
            {
                float time;
                float normal[3];
                if (SweepAgainstObb(state->start, state->half, state->delta, &boxes[b], &time,
                                    normal))
                    OfferHit(state, time, normal, MakeHandle(world, slotIndex));
            }
        }
    }
    return true;
}

static bool ValidHalfExtent(const float halfExtent[3])
{
    return FiniteVector(halfExtent) && halfExtent[0] >= 0.0f && halfExtent[1] >= 0.0f &&
           halfExtent[2] >= 0.0f && halfExtent[0] <= MESH_MAX_MOTION &&
           halfExtent[1] <= MESH_MAX_MOTION && halfExtent[2] <= MESH_MAX_MOTION;
}

static bool ValidMotion(const float delta[3])
{
    return FiniteVector(delta) && AbsFloat(delta[0]) <= MESH_MAX_MOTION &&
           AbsFloat(delta[1]) <= MESH_MAX_MOTION && AbsFloat(delta[2]) <= MESH_MAX_MOTION;
}

/* Caller holds the lock; start is normalized. */
static bool SweepLocked(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *start,
                        const float halfExtent[3], const float delta[3], LaiueMeshHitV1 *outHit)
{
    MeshHitState state;
    memset(&state, 0, sizeof(state));
    state.start = start->local;
    state.half = halfExtent;
    state.delta = delta;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        state.queryMin[axis] = start->local[axis] - halfExtent[axis] + MinFloat(0.0f, delta[axis]);
        state.queryMax[axis] = start->local[axis] + halfExtent[axis] + MaxFloat(0.0f, delta[axis]);
    }
    VisitCells(world, &start->cell, state.queryMin, state.queryMax, SweepVisitor, &state);
    if (state.hit)
        *outHit = state.best;
    return state.hit;
}

static uint32_t WorldSweepBox(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *start,
                              const float halfExtent[3], const float delta[3],
                              LaiueMeshHitV1 *outHit)
{
    LaiueMeshPositionV1 normalized;
    if (world == NULL || outHit == NULL || halfExtent == NULL || delta == NULL ||
        !ValidHalfExtent(halfExtent) || !ValidMotion(delta) ||
        !NormalizePosition(world->cellSize, start, &normalized))
        return 0u;
    LockShared(world);
    const bool hit = SweepLocked(world, &normalized, halfExtent, delta, outHit);
    UnlockShared(world);
    return hit ? 1u : 0u;
}

static uint32_t WorldMoveBox(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *start,
                             const float halfExtent[3], const float delta[3],
                             LaiueMeshMoveResultV1 *outResult)
{
    LaiueMeshPositionV1 position;
    if (world == NULL || outResult == NULL || halfExtent == NULL || delta == NULL ||
        !ValidHalfExtent(halfExtent) || !ValidMotion(delta) ||
        !NormalizePosition(world->cellSize, start, &position))
        return 0u;
    float remaining[3] = {delta[0], delta[1], delta[2]};
    uint32_t grounded = 0u;
    uint32_t collided = 0u;
    LockShared(world);
    for (uint32_t iteration = 0u; iteration < MESH_MOVE_ITERATIONS; ++iteration)
    {
        const float lengthSquared = Dot3(remaining, remaining);
        if (lengthSquared < 1.0e-12f)
            break;
        LaiueMeshHitV1 hit;
        if (!SweepLocked(world, &position, halfExtent, remaining, &hit))
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                position.local[axis] += remaining[axis];
            break;
        }
        collided = 1u;
        if (hit.normal[2] > MESH_GROUND_NORMAL)
            grounded = 1u;
        /* Stop a skin short of the contact so the next sweep starts apart. */
        const float length = ScalarSqrt(lengthSquared);
        const float travel = MaxFloat(0.0f, hit.time * length - MESH_SKIN) / length;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            position.local[axis] += remaining[axis] * travel;
            remaining[axis] *= 1.0f - hit.time;
        }
        const float into = Dot3(remaining, hit.normal);
        if (into < 0.0f)
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                remaining[axis] -= hit.normal[axis] * into;
        LaiueMeshPositionV1 renormalized;
        if (!NormalizePosition(world->cellSize, &position, &renormalized))
            break;
        position = renormalized;
    }
    UnlockShared(world);
    LaiueMeshPositionV1 finished;
    if (!NormalizePosition(world->cellSize, &position, &finished))
        return 0u;
    outResult->position = finished;
    outResult->grounded = grounded;
    outResult->collided = collided;
    return 1u;
}

typedef struct MeshRayState
{
    float origin[3];
    float segment[3];
    float queryMin[3];
    float queryMax[3];
    MeshHitState hits;
} MeshRayState;

static bool RayVisitor(void *context, const LaiueMeshWorldV1 *world, const MeshCell *cell,
                       const float offset[3])
{
    MeshRayState *state = (MeshRayState *)context;
    for (uint32_t i = 0u; i < cell->count; ++i)
    {
        const uint32_t slotIndex = cell->instances[i];
        const MeshSlot *slot = &world->slots[slotIndex];
        if ((slot->flags & LAIUE_MESH_INSTANCE_COLLIDABLE) == 0u ||
            !SlotBoundsOverlap(slot, offset, state->queryMin, state->queryMax))
            continue;
        MeshBoxIterator iterator;
        BeginInstanceBoxes(FindShape(world, slot->model), &slot->transform, &iterator);
        for (uint32_t first = 0u; first < iterator.count; first += MESH_BOX_BATCH_SIZE)
        {
            MeshObb boxes[MESH_BOX_BATCH_SIZE];
            const uint32_t count = InstanceBoxBatch(&iterator, first, offset, boxes);
            for (uint32_t b = 0u; b < count; ++b)
            {
                float time;
                float normal[3];
                if (RayAgainstObb(state->origin, state->segment, &boxes[b], &time, normal))
                    OfferHit(&state->hits, time, normal, MakeHandle(world, slotIndex));
            }
        }
    }
    return true;
}

static uint32_t WorldRaycast(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *origin,
                             const float direction[3], float maximumDistance,
                             LaiueMeshHitV1 *outHit)
{
    LaiueMeshPositionV1 normalized;
    if (world == NULL || outHit == NULL || direction == NULL || !FiniteVector(direction) ||
        !FiniteFloat(maximumDistance) || !(maximumDistance > 0.0f) ||
        maximumDistance > MESH_MAX_QUERY_RADIUS ||
        !NormalizePosition(world->cellSize, origin, &normalized))
        return 0u;
    const float lengthSquared = Dot3(direction, direction);
    if (!(lengthSquared > 1.0e-12f))
        return 0u;
    const float scale = maximumDistance / ScalarSqrt(lengthSquared);
    MeshRayState state;
    memset(&state, 0, sizeof(state));
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        state.origin[axis] = normalized.local[axis];
        state.segment[axis] = direction[axis] * scale;
        state.queryMin[axis] = state.origin[axis] + MinFloat(0.0f, state.segment[axis]);
        state.queryMax[axis] = state.origin[axis] + MaxFloat(0.0f, state.segment[axis]);
    }
    LockShared(world);
    VisitCells(world, &normalized.cell, state.queryMin, state.queryMax, RayVisitor, &state);
    UnlockShared(world);
    if (state.hits.hit)
        *outHit = state.hits.best;
    return state.hits.hit ? 1u : 0u;
}

typedef struct MeshCellQueryState
{
    float center[3];
    float radiusSquared;
    LaiueMeshCellV1 *output;
    uint32_t capacity;
    uint32_t count;
} MeshCellQueryState;

static bool CellQueryVisitor(void *context, const LaiueMeshWorldV1 *world, const MeshCell *cell,
                             const float offset[3])
{
    (void)world;
    MeshCellQueryState *state = (MeshCellQueryState *)context;
    float distanceSquared = 0.0f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const float low = offset[axis] + cell->boundsMin[axis];
        const float high = offset[axis] + cell->boundsMax[axis];
        float gap = 0.0f;
        if (state->center[axis] < low)
            gap = low - state->center[axis];
        else if (state->center[axis] > high)
            gap = state->center[axis] - high;
        distanceSquared += gap * gap;
    }
    if (distanceSquared > state->radiusSquared)
        return true;
    if (state->count < state->capacity)
        state->output[state->count] = cell->coord;
    ++state->count;
    return true;
}

static uint32_t WorldQueryCells(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *center,
                                float radius, LaiueMeshCellV1 *outCells, uint32_t capacity,
                                uint32_t *outCount)
{
    LaiueMeshPositionV1 normalized;
    if (outCount != NULL)
        *outCount = 0u;
    if (world == NULL || outCount == NULL || (capacity != 0u && outCells == NULL) ||
        !FiniteFloat(radius) || radius < 0.0f || radius > MESH_MAX_QUERY_RADIUS ||
        !NormalizePosition(world->cellSize, center, &normalized))
        return 0u;
    MeshCellQueryState state;
    memset(&state, 0, sizeof(state));
    float queryMin[3];
    float queryMax[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        state.center[axis] = normalized.local[axis];
        queryMin[axis] = normalized.local[axis] - radius;
        queryMax[axis] = normalized.local[axis] + radius;
    }
    state.radiusSquared = radius * radius;
    state.output = outCells;
    state.capacity = capacity;
    LockShared(world);
    VisitCells(world, &normalized.cell, queryMin, queryMax, CellQueryVisitor, &state);
    uint32_t result = 1u;
    if (state.count > capacity && capacity != 0u)
    {
        /* Collect everything so the truncated prefix is still the sorted
         * prefix of the full answer. */
        const uint32_t total = state.count;
        LaiueMeshCellV1 *all =
            (LaiueMeshCellV1 *)PlatformAllocate(sizeof(LaiueMeshCellV1) * total, false);
        if (all == NULL)
        {
            result = 0u;
        }
        else
        {
            state.output = all;
            state.capacity = total;
            state.count = 0u;
            VisitCells(world, &normalized.cell, queryMin, queryMax, CellQueryVisitor, &state);
            HeapSort(all, state.count, sizeof(LaiueMeshCellV1), CompareCells);
            if (capacity != 0u)
                memcpy(outCells, all, sizeof(LaiueMeshCellV1) * capacity);
            PlatformFree(all);
        }
    }
    else if (state.count <= capacity)
    {
        HeapSort(outCells, state.count, sizeof(LaiueMeshCellV1), CompareCells);
    }
    UnlockShared(world);
    if (result != 0u)
        *outCount = state.count;
    return result;
}

static uint32_t WorldCellInstances(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell,
                                   LaiueMeshInstanceInfoV1 *outInstances, uint32_t capacity,
                                   uint32_t *outCount)
{
    if (outCount != NULL)
        *outCount = 0u;
    if (world == NULL || cell == NULL || outCount == NULL ||
        (capacity != 0u && outInstances == NULL) || !CellValid(cell))
        return 0u;
    LockShared(world);
    const uint32_t index = FindCell(world, cell);
    if (index != MESH_NO_INDEX)
    {
        const MeshCell *stored = &world->cells[index];
        const uint32_t written = stored->count < capacity ? stored->count : capacity;
        for (uint32_t i = 0u; i < written; ++i)
            FillInfo(world, stored->instances[i], &outInstances[i]);
        *outCount = stored->count;
    }
    UnlockShared(world);
    return 1u;
}

static uint64_t WorldCellRevision(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell)
{
    if (world == NULL || cell == NULL || !CellValid(cell))
        return 0u;
    LockShared(world);
    const uint32_t index = FindCell(world, cell);
    const uint64_t revision = index != MESH_NO_INDEX ? world->cells[index].revision : 0u;
    UnlockShared(world);
    return revision;
}

typedef struct MeshOverlapState
{
    float queryMin[3];
    float queryMax[3];
    float reference[3];
    LaiueMeshColliderV1 *output;
    uint32_t capacity;
    uint32_t count;
} MeshOverlapState;

static bool OverlapVisitor(void *context, const LaiueMeshWorldV1 *world, const MeshCell *cell,
                           const float offset[3])
{
    MeshOverlapState *state = (MeshOverlapState *)context;
    for (uint32_t i = 0u; i < cell->count; ++i)
    {
        const uint32_t slotIndex = cell->instances[i];
        const MeshSlot *slot = &world->slots[slotIndex];
        if ((slot->flags & LAIUE_MESH_INSTANCE_COLLIDABLE) == 0u ||
            !SlotBoundsOverlap(slot, offset, state->queryMin, state->queryMax))
            continue;
        MeshBoxIterator iterator;
        BeginInstanceBoxes(FindShape(world, slot->model), &slot->transform, &iterator);
        for (uint32_t first = 0u; first < iterator.count; first += MESH_BOX_BATCH_SIZE)
        {
            MeshObb boxes[MESH_BOX_BATCH_SIZE];
            const uint32_t count = InstanceBoxBatch(&iterator, first, offset, boxes);
            for (uint32_t b = 0u; b < count; ++b)
            {
                float boxMin[3];
                float boxMax[3];
                ObbAabb(&boxes[b], boxMin, boxMax);
                if (!BoundsOverlap(boxMin, boxMax, state->queryMin, state->queryMax))
                    continue;
                if (state->count < state->capacity)
                {
                    LaiueMeshColliderV1 *out = &state->output[state->count];
                    for (uint32_t axis = 0u; axis < 3u; ++axis)
                    {
                        out->minimum[axis] = boxMin[axis] - state->reference[axis];
                        out->maximum[axis] = boxMax[axis] - state->reference[axis];
                    }
                    out->instance = MakeHandle(world, slotIndex);
                }
                ++state->count;
            }
        }
    }
    return true;
}

static uint32_t WorldOverlapBoxes(const LaiueMeshWorldV1 *world,
                                  const LaiueMeshPositionV1 *reference, const float minimum[3],
                                  const float maximum[3], LaiueMeshColliderV1 *outColliders,
                                  uint32_t capacity, uint32_t *outCount)
{
    LaiueMeshPositionV1 normalized;
    if (outCount != NULL)
        *outCount = 0u;
    if (world == NULL || outCount == NULL || minimum == NULL || maximum == NULL ||
        (capacity != 0u && outColliders == NULL) || !FiniteVector(minimum) ||
        !FiniteVector(maximum) || !NormalizePosition(world->cellSize, reference, &normalized))
        return 0u;
    MeshOverlapState state;
    memset(&state, 0, sizeof(state));
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (minimum[axis] > maximum[axis] || AbsFloat(minimum[axis]) > MESH_MAX_QUERY_RADIUS ||
            AbsFloat(maximum[axis]) > MESH_MAX_QUERY_RADIUS)
            return 0u;
        state.reference[axis] = normalized.local[axis];
        state.queryMin[axis] = normalized.local[axis] + minimum[axis];
        state.queryMax[axis] = normalized.local[axis] + maximum[axis];
    }
    state.output = outColliders;
    state.capacity = capacity;
    LockShared(world);
    VisitCells(world, &normalized.cell, state.queryMin, state.queryMax, OverlapVisitor, &state);
    uint32_t result = 1u;
    if (state.count > capacity && capacity != 0u)
    {
        const uint32_t total = state.count;
        LaiueMeshColliderV1 *all =
            (LaiueMeshColliderV1 *)PlatformAllocate(sizeof(LaiueMeshColliderV1) * total, false);
        if (all == NULL)
        {
            result = 0u;
        }
        else
        {
            state.output = all;
            state.capacity = total;
            state.count = 0u;
            VisitCells(world, &normalized.cell, state.queryMin, state.queryMax, OverlapVisitor,
                       &state);
            HeapSort(all, state.count, sizeof(LaiueMeshColliderV1), CompareColliders);
            if (capacity != 0u)
                memcpy(outColliders, all, sizeof(LaiueMeshColliderV1) * capacity);
            PlatformFree(all);
        }
    }
    else if (state.count <= capacity)
    {
        HeapSort(outColliders, state.count, sizeof(LaiueMeshColliderV1), CompareColliders);
    }
    UnlockShared(world);
    if (result != 0u)
        *outCount = state.count;
    return result;
}

typedef struct MeshBlockState
{
    float center[3];
    float half[3];
    float queryMin[3];
    float queryMax[3];
    bool solid;
} MeshBlockState;

static bool BlockVisitor(void *context, const LaiueMeshWorldV1 *world, const MeshCell *cell,
                         const float offset[3])
{
    MeshBlockState *state = (MeshBlockState *)context;
    for (uint32_t i = 0u; i < cell->count; ++i)
    {
        const MeshSlot *slot = &world->slots[cell->instances[i]];
        if ((slot->flags & LAIUE_MESH_INSTANCE_COLLIDABLE) == 0u ||
            !SlotBoundsOverlap(slot, offset, state->queryMin, state->queryMax))
            continue;
        MeshBoxIterator iterator;
        BeginInstanceBoxes(FindShape(world, slot->model), &slot->transform, &iterator);
        for (uint32_t b = 0u; b < iterator.count; ++b)
        {
            MeshObb box;
            InstanceBox(&iterator, b, &box);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                box.center[axis] += offset[axis];
            if (OverlapObb(state->center, state->half, &box))
            {
                state->solid = true;
                return false;
            }
        }
    }
    return true;
}

static uint32_t WorldBlockSolid(const LaiueMeshWorldV1 *world, int64_t blockX, int64_t blockY,
                                int64_t blockZ, float blockSize)
{
    if (world == NULL || !FiniteFloat(blockSize) || !(blockSize > 0.0f) ||
        blockSize > MESH_MAX_BLOCK_SIZE)
        return 0u;
    const int64_t blocks[3] = {blockX, blockY, blockZ};
    LaiueMeshCellV1 reference;
    MeshBlockState state;
    memset(&state, 0, sizeof(state));
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double block = (double)blocks[axis];
        if (block > MESH_MAX_LOCAL_CELLS || block < -MESH_MAX_LOCAL_CELLS)
            return 0u;
        const double minimum = block * (double)blockSize;
        const double cell = FloorDouble(minimum / (double)world->cellSize);
        SetCellComponent(&reference, axis, (int64_t)cell);
        const float local = (float)(minimum - cell * (double)world->cellSize);
        state.half[axis] = 0.5f * blockSize;
        state.center[axis] = local + state.half[axis];
        state.queryMin[axis] = local;
        state.queryMax[axis] = local + blockSize;
    }
    if (!CellValid(&reference))
        return 0u;
    LockShared(world);
    VisitCells(world, &reference, state.queryMin, state.queryMax, BlockVisitor, &state);
    UnlockShared(world);
    return state.solid ? 1u : 0u;
}

static const LaiueMeshWorldServiceV1 g_service = {
    .structSize = sizeof(LaiueMeshWorldServiceV1),
    .abiVersion = LAIUE_MESH_WORLD_SERVICE_ABI_VERSION_1,
    .create = WorldCreate,
    .destroy = WorldDestroy,
    .cellSize = WorldCellSize,
    .registerShape = WorldRegisterShape,
    .add = WorldAdd,
    .remove = WorldRemove,
    .setTransform = WorldSetTransform,
    .setFlags = WorldSetFlags,
    .get = WorldGet,
    .instanceCount = WorldInstanceCount,
    .queryCells = WorldQueryCells,
    .cellInstances = WorldCellInstances,
    .cellRevision = WorldCellRevision,
    .rebase = WorldRebase,
    .sweepBox = WorldSweepBox,
    .moveBox = WorldMoveBox,
    .raycast = WorldRaycast,
    .overlapBoxes = WorldOverlapBoxes,
    .blockSolid = WorldBlockSolid,
    .setProvider = WorldSetProvider,
    .stream = WorldStream,
};

const LaiueMeshWorldServiceV1 *LaiueMeshWorldGetStaticServiceV1(void)
{
    return &g_service;
}
