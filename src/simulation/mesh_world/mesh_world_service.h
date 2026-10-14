#pragma once

/* Infinite mesh world: a sparse grid of cells holding placed model instances.
 * It owns placement, spatial queries and collision against the instances'
 * oriented boxes; it does not load models, draw, or generate anything.  The
 * application registers a shape (bounds and collision boxes) per model id,
 * places instances, and decides what exists where.
 *
 * Coordinates are an integer cell plus a float offset inside the cell, in
 * metres, Z up.  Cells are int64 relative to an origin the application keeps
 * (for example as an InfiniteCoord); rebase() moves that origin by whole
 * cells, so the world is unbounded while every float stays small.
 *
 * Reads may run concurrently; mutations are serialized internally.  Handles
 * carry a generation, so a stale handle is refused instead of aliasing a
 * newer instance in the same slot.  Functions returning uint32_t report 1 on
 * success and 0 on invalid input, a stale handle, a full world or
 * exhausted memory; a failed call changes nothing. */

#include "api.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_MESH_WORLD_SERVICE_NAME "laiue.mesh_world"
#define LAIUE_MESH_WORLD_SERVICE_ABI_VERSION_1 1u

#define LAIUE_MESH_WORLD_MIN_CELL_SIZE 1.0f
#define LAIUE_MESH_WORLD_MAX_CELL_SIZE 4096.0f
#define LAIUE_MESH_WORLD_MAX_SHAPE_BOXES 64u
/* Stored cell coordinates stay within +-2^61 so differences never overflow. */
#define LAIUE_MESH_WORLD_MAX_CELL (INT64_C(1) << 61)

typedef struct LaiueMeshWorldV1 LaiueMeshWorldV1;
/* 0 is never a valid instance. */
typedef uint64_t LaiueMeshInstanceV1;

typedef struct LaiueMeshCellV1
{
    int64_t x;
    int64_t y;
    int64_t z;
} LaiueMeshCellV1;

/* Absolute position = origin + cell * cellSize + local.  Inputs may carry
 * any finite local offset; the world normalizes it into [0, cellSize). */
typedef struct LaiueMeshPositionV1
{
    LaiueMeshCellV1 cell;
    float local[3];
} LaiueMeshPositionV1;

/* Oriented box in model axes; the same layout as a model pack box. */
typedef struct LaiueMeshBoxV1
{
    float center[3];
    float halfExtent[3];
    float rotation[4]; /* unit quaternion (x, y, z, w) */
} LaiueMeshBoxV1;

/* Use the model bounds as one collision box when the shape has none. */
#define LAIUE_MESH_SHAPE_COLLIDE_BOUNDS (UINT32_C(1) << 0)

typedef struct LaiueMeshShapeV1
{
    uint32_t structSize;
    uint32_t flags;
    float boundsMin[3];
    float boundsMax[3];
    const LaiueMeshBoxV1 *boxes; /* copied by registerShape */
    uint32_t boxCount;
} LaiueMeshShapeV1;

typedef struct LaiueMeshTransformV1
{
    LaiueMeshPositionV1 position;
    float rotation[4]; /* unit quaternion (x, y, z, w) */
    float scale[3];    /* positive per-axis scale, applied before rotation */
} LaiueMeshTransformV1;

#define LAIUE_MESH_INSTANCE_VISIBLE (UINT32_C(1) << 0)
#define LAIUE_MESH_INSTANCE_COLLIDABLE (UINT32_C(1) << 1)

typedef struct LaiueMeshInstanceDescV1
{
    uint32_t structSize;
    uint32_t model;
    LaiueMeshTransformV1 transform;
    uint32_t flags;
    uint64_t userData;
} LaiueMeshInstanceDescV1;

typedef struct LaiueMeshInstanceInfoV1
{
    LaiueMeshInstanceV1 handle;
    uint32_t model;
    uint32_t flags;
    LaiueMeshTransformV1 transform;
    uint64_t userData;
} LaiueMeshInstanceInfoV1;

typedef struct LaiueMeshWorldConfigV1
{
    uint32_t structSize;
    float cellSize;            /* metres per cell, [1, 4096] */
    uint32_t maximumInstances; /* 0 selects 1 << 20 */
} LaiueMeshWorldConfigV1;

/* Result of a sweep or ray: time of impact in [0, 1] along the motion, the
 * contact normal pointing against the motion, and the instance that was
 * hit. */
typedef struct LaiueMeshHitV1
{
    float time;
    float normal[3];
    LaiueMeshInstanceV1 instance;
} LaiueMeshHitV1;

typedef struct LaiueMeshMoveResultV1
{
    LaiueMeshPositionV1 position;
    uint32_t grounded; /* a contact with an upward normal stopped the move */
    uint32_t collided;
} LaiueMeshMoveResultV1;

/* Axis-aligned bounds of one collidable box, in metres relative to the
 * reference position of the query. */
typedef struct LaiueMeshColliderV1
{
    float minimum[3];
    float maximum[3];
    LaiueMeshInstanceV1 instance;
} LaiueMeshColliderV1;

/* Fills the instances that belong to one cell.  Each desc's position is
 * relative to that cell: the world adds the cell to desc.transform.position.cell
 * (usually left zero) before normalizing.  Write at most capacity descs and
 * the number written; return 0 on failure and the cell is retried by a later
 * stream call.  The callback runs under the world's write lock and must not
 * call back into the world.  The cell is in the world's current frame, so a
 * provider working in absolute coordinates adds the origin it keeps across
 * rebase. */
typedef uint32_t (*LaiueMeshCellProviderFn)(void *context, const LaiueMeshCellV1 *cell,
                                            LaiueMeshInstanceDescV1 *outInstances,
                                            uint32_t capacity, uint32_t *outCount);

#define LAIUE_MESH_WORLD_MAX_PROVIDER_INSTANCES 4096u

typedef struct LaiueMeshCellProviderV1
{
    uint32_t structSize;
    uint32_t maximumInstancesPerCell; /* 1..4096 */
    void *context;
    LaiueMeshCellProviderFn populate;
} LaiueMeshCellProviderV1;

typedef struct LaiueMeshWorldServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t (*create)(const LaiueMeshWorldConfigV1 *config, LaiueMeshWorldV1 **outWorld);
    void (*destroy)(LaiueMeshWorldV1 *world);
    float (*cellSize)(const LaiueMeshWorldV1 *world);

    uint32_t (*registerShape)(LaiueMeshWorldV1 *world, uint32_t model,
                              const LaiueMeshShapeV1 *shape);

    uint32_t (*add)(LaiueMeshWorldV1 *world, const LaiueMeshInstanceDescV1 *desc,
                    LaiueMeshInstanceV1 *outInstance);
    uint32_t (*remove)(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance);
    uint32_t (*setTransform)(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance,
                             const LaiueMeshTransformV1 *transform);
    uint32_t (*setFlags)(LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance, uint32_t flags);
    uint32_t (*get)(const LaiueMeshWorldV1 *world, LaiueMeshInstanceV1 instance,
                    LaiueMeshInstanceInfoV1 *outInfo);
    uint32_t (*instanceCount)(const LaiueMeshWorldV1 *world);

    /* Occupied cells whose instance bounds come within radius of center.
     * outCount receives the full number even when capacity is smaller; the
     * order is deterministic (by cell coordinates). */
    uint32_t (*queryCells)(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *center,
                           float radius, LaiueMeshCellV1 *outCells, uint32_t capacity,
                           uint32_t *outCount);
    /* Instances placed in a cell, in a stable order.  outCount receives the
     * full number. */
    uint32_t (*cellInstances)(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell,
                              LaiueMeshInstanceInfoV1 *outInstances, uint32_t capacity,
                              uint32_t *outCount);
    /* Changes whenever the cell's contents, transforms, flags or shapes
     * change; 0 for a cell that holds nothing.  Values come from one
     * world-wide counter and are never reused, so a revision names one state
     * of one cell's contents and survives rebase. */
    uint64_t (*cellRevision)(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell);

    /* Moves the origin by whole cells: every stored cell coordinate has
     * shift subtracted from it, so absolute positions stay where they were. */
    uint32_t (*rebase)(LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *shift);

    /* An axis-aligned box of halfExtent centred at start moves by delta
     * metres; the earliest contact with a collidable instance is reported.
     * Returns 1 on a hit, 0 when the path is free or on invalid input. */
    uint32_t (*sweepBox)(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *start,
                         const float halfExtent[3], const float delta[3], LaiueMeshHitV1 *outHit);
    /* Collide-and-slide: up to four sweeps, each continuing along the
     * surface that stopped the previous one. */
    uint32_t (*moveBox)(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *start,
                        const float halfExtent[3], const float delta[3],
                        LaiueMeshMoveResultV1 *outResult);
    /* A ray starting inside a box does not hit that box.  The hit time is a
     * fraction of maximumDistance. */
    uint32_t (*raycast)(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *origin,
                        const float direction[3], float maximumDistance, LaiueMeshHitV1 *outHit);
    /* Collidable boxes whose bounds overlap [minimum, maximum] metres around
     * reference, as axis-aligned bounds relative to reference, ordered by
     * instance handle.  outCount receives the full number; the result is
     * complete only when it does not exceed capacity.  This is the shape a
     * physics dynamic-collider query expects. */
    uint32_t (*overlapBoxes)(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *reference,
                             const float minimum[3], const float maximum[3],
                             LaiueMeshColliderV1 *outColliders, uint32_t capacity,
                             uint32_t *outCount);
    /* Whether a collidable box overlaps the cube [block, block + 1) *
     * blockSize metres measured from the origin.  This lets a voxel physics
     * collision callback see the mesh world at block resolution.  Blocks
     * farther than 2^40 block sizes from the origin report empty. */
    uint32_t (*blockSolid)(const LaiueMeshWorldV1 *world, int64_t blockX, int64_t blockY,
                           int64_t blockZ, float blockSize);

    /* Installs the cell provider, or removes it with NULL.  Either way every
     * instance the previous provider produced is removed, so the next stream
     * call fills cells with the new content.  Instances the application
     * placed itself are never touched. */
    uint32_t (*setProvider)(LaiueMeshWorldV1 *world, const LaiueMeshCellProviderV1 *provider);
    /* Keeps the cells around center filled: cells whose box comes within
     * radius are populated nearest first, at most cellBudget of them per call
     * (0 means no limit), and the provider instances of cells farther than
     * radius plus one cell are removed.  outPending, when not NULL, receives
     * the number of cells in range still waiting, so a caller spreading the
     * work over frames knows to call again.  Without a provider this only
     * succeeds. */
    uint32_t (*stream)(LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *center, float radius,
                       uint32_t cellBudget, uint32_t *outPending);
    uintptr_t reserved[8];
} LaiueMeshWorldServiceV1;

LAIUE_MESH_WORLD_API const LaiueModuleApiV1 *LaiueMeshWorldGetStaticModuleApiV1(void);
LAIUE_MESH_WORLD_API const LaiueMeshWorldServiceV1 *LaiueMeshWorldGetStaticServiceV1(void);
