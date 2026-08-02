#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct World World;
typedef struct PhysicalConstructSystem PhysicalConstructSystem;

#if !defined(LAIUE_CONSTRUCT_API)
#if defined(LAIUE_BUILD_CONSTRUCT)
#define LAIUE_CONSTRUCT_API LAIUE_EXPORT
#else
#define LAIUE_CONSTRUCT_API LAIUE_IMPORT
#endif
#endif

#define PHYSICAL_CONSTRUCT_MAX_BODIES 16U
#define PHYSICAL_CONSTRUCT_MAX_BLOCKS 256U
#define PHYSICAL_CONSTRUCT_MODEL_PARTS 3U
#define PHYSICAL_CONSTRUCT_MAX_DYNAMIC_BLOCKERS 64U

typedef enum PhysicalConstructResult
{
    PHYSICAL_CONSTRUCT_OK = 0,
    PHYSICAL_CONSTRUCT_INVALID_ARGUMENT,
    PHYSICAL_CONSTRUCT_NOT_FOUND,
    PHYSICAL_CONSTRUCT_OCCUPIED,
    PHYSICAL_CONSTRUCT_COLLISION,
    PHYSICAL_CONSTRUCT_NOT_CONNECTED,
    PHYSICAL_CONSTRUCT_CAPACITY,
    PHYSICAL_CONSTRUCT_WORLD_MUTATION_FAILED,
} PhysicalConstructResult;

typedef enum PhysicalConstructBlockKind
{
    PHYSICAL_CONSTRUCT_BLOCK_VOXEL = 0,
    PHYSICAL_CONSTRUCT_BLOCK_LEVER = 1,
} PhysicalConstructBlockKind;

typedef enum PhysicalConstructHitPart
{
    PHYSICAL_CONSTRUCT_HIT_VOXEL = 0,
    PHYSICAL_CONSTRUCT_HIT_LEVER_BASE,
    PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE,
} PhysicalConstructHitPart;

typedef struct PhysicalConstructAabb
{
    double minimum[3];
    double maximum[3];
} PhysicalConstructAabb;

// Dynamic blockers are sampled by the caller at the start of a fixed-step
// batch. ignoredBodyId lets a grab owner opt its held construct out without
// making the player transparent to every other construct.
typedef struct PhysicalConstructDynamicBlocker
{
    PhysicalConstructAabb bounds;
    uint64_t ignoredBodyId;
} PhysicalConstructDynamicBlocker;

typedef struct PhysicalConstructCollider
{
    PhysicalConstructAabb bounds;
    uint64_t bodyId;
    double velocity[3];
} PhysicalConstructCollider;

// A voxel carries the original world material. A lever is a construct-owned
// attachment rather than a BlockType: material is zero and mountNormal is the
// unit direction from the captured root block towards the lever cell.
typedef struct PhysicalConstructBlock
{
    int32_t local[3];
    int8_t mountNormal[3];
    uint8_t material;
    uint8_t kind;
} PhysicalConstructBlock;

typedef struct PhysicalConstructBodyState
{
    uint64_t id;
    uint64_t topologyRevision;
    double origin[3];
    double velocity[3];
    uint32_t blockCount;
    uint64_t grabOwner;
    bool grabbed;
} PhysicalConstructBodyState;

typedef struct PhysicalConstructConfiguration
{
    double fixedStepSeconds;
    double gravity;
    double collisionEpsilon;
    double grabStiffness;
    double grabDamping;
    double maximumGrabAcceleration;
    double maximumSpeed;
} PhysicalConstructConfiguration;

typedef struct PhysicalConstructModelPart
{
    PhysicalConstructAabb localBounds;
    PhysicalConstructHitPart hitPart;
} PhysicalConstructModelPart;

typedef struct PhysicalConstructRaycastHit
{
    uint64_t bodyId;
    int32_t localBlock[3];
    int8_t normal[3];
    uint8_t material;
    PhysicalConstructBlockKind blockKind;
    PhysicalConstructHitPart hitPart;
    double distance;
} PhysicalConstructRaycastHit;

LAIUE_CONSTRUCT_API void PhysicalConstructGetDefaultConfiguration(
    PhysicalConstructConfiguration *outConfiguration);

// All body, topology, solver and scratch storage is allocated here. Step,
// raycast, placement and splitting perform no allocation.
LAIUE_CONSTRUCT_API PhysicalConstructSystem *PhysicalConstructSystemCreate(
    World *world, const PhysicalConstructConfiguration *configuration);
LAIUE_CONSTRUCT_API void PhysicalConstructSystemDestroy(PhysicalConstructSystem *system);
LAIUE_CONSTRUCT_API void PhysicalConstructSystemReset(PhysicalConstructSystem *system);

// rootWorldBlock is always captured. The bounded flood fill continues only
// through face-connected, non-air blocks which already have a stored World
// edit; this prevents a lever placed on terrain from capturing the planet.
// leverWorldCell = rootWorldBlock + mountNormal and must be empty.
LAIUE_CONSTRUCT_API PhysicalConstructResult PhysicalConstructActivateLever(
    PhysicalConstructSystem *system, const int64_t rootWorldBlock[3], const int8_t mountNormal[3],
    const PhysicalConstructAabb *externalBlockers, uint32_t externalBlockerCount,
    uint64_t *outBodyId);

LAIUE_CONSTRUCT_API PhysicalConstructResult PhysicalConstructPlaceBlock(
    PhysicalConstructSystem *system, uint64_t bodyId, const int32_t localBlock[3], uint8_t material,
    const PhysicalConstructAabb *externalBlockers, uint32_t externalBlockerCount);

// A successful removal may create several bodies. outBodyIds receives all
// resulting IDs in ascending order, including the preserved parent ID.
LAIUE_CONSTRUCT_API PhysicalConstructResult PhysicalConstructBreakBlock(
    PhysicalConstructSystem *system, uint64_t bodyId, const int32_t localBlock[3],
    PhysicalConstructBlock *outRemoved, uint64_t *outBodyIds, uint32_t bodyIdCapacity,
    uint32_t *outBodyIdCount);

LAIUE_CONSTRUCT_API bool PhysicalConstructRaycast(const PhysicalConstructSystem *system,
                                                  const double origin[3], const double direction[3],
                                                  double maximumDistance,
                                                  PhysicalConstructRaycastHit *outHit);

// Only the lever handle is draggable. One owner token controls a body at a
// time; callers should use the peer ID online and a stable local token offline.
LAIUE_CONSTRUCT_API PhysicalConstructResult PhysicalConstructBeginGrab(
    PhysicalConstructSystem *system, uint64_t bodyId, uint64_t owner, const double target[3]);
LAIUE_CONSTRUCT_API bool PhysicalConstructUpdateGrab(PhysicalConstructSystem *system,
                                                     uint64_t bodyId, uint64_t owner,
                                                     const double target[3]);
LAIUE_CONSTRUCT_API bool PhysicalConstructEndGrab(PhysicalConstructSystem *system, uint64_t bodyId,
                                                  uint64_t owner);
LAIUE_CONSTRUCT_API void PhysicalConstructReleaseOwner(PhysicalConstructSystem *system,
                                                       uint64_t owner);

LAIUE_CONSTRUCT_API void PhysicalConstructStep(PhysicalConstructSystem *system);
LAIUE_CONSTRUCT_API void PhysicalConstructSimulateFixedSteps(PhysicalConstructSystem *system,
                                                             uint32_t steps);
LAIUE_CONSTRUCT_API bool PhysicalConstructStepWithBlockers(
    PhysicalConstructSystem *system, const PhysicalConstructDynamicBlocker *blockers,
    uint32_t blockerCount);
LAIUE_CONSTRUCT_API bool PhysicalConstructSimulateFixedStepsWithBlockers(
    PhysicalConstructSystem *system, uint32_t steps,
    const PhysicalConstructDynamicBlocker *blockers, uint32_t blockerCount);
// Rebasing is all-or-nothing. A shift which would leave the bounded local
// simulation coordinate range is rejected without changing any body.
LAIUE_CONSTRUCT_API bool PhysicalConstructRebase(PhysicalConstructSystem *system,
                                                 const int64_t blockShift[3]);

// Caller-owned copy/import boundary for save/network adapters. Blocks are
// copied in deterministic lexicographic order. Older topology revisions are
// accepted as no-op and never roll a replica back.
LAIUE_CONSTRUCT_API uint32_t PhysicalConstructCopyBodyStates(const PhysicalConstructSystem *system,
                                                             PhysicalConstructBodyState *output,
                                                             uint32_t capacity, bool *outTruncated);
// Exact voxel/lever-part colliders are copied in deterministic body-ID,
// local-block and model-part order. Touching the query boundary is not an
// overlap. The caller owns output and may pass zero capacity to count presence.
LAIUE_CONSTRUCT_API uint32_t PhysicalConstructCopyCollidersInAabb(
    const PhysicalConstructSystem *system, const PhysicalConstructAabb *query,
    PhysicalConstructCollider *output, uint32_t capacity, bool *outTruncated);
LAIUE_CONSTRUCT_API bool PhysicalConstructCopyBlocks(const PhysicalConstructSystem *system,
                                                     uint64_t bodyId,
                                                     PhysicalConstructBlock *output,
                                                     uint32_t capacity, uint32_t *outCount);
LAIUE_CONSTRUCT_API PhysicalConstructResult PhysicalConstructImportBody(
    PhysicalConstructSystem *system, const PhysicalConstructBodyState *state,
    const PhysicalConstructBlock *blocks, uint32_t blockCount);
LAIUE_CONSTRUCT_API bool PhysicalConstructApplyBodyMotion(PhysicalConstructSystem *system,
                                                          const PhysicalConstructBodyState *state);
LAIUE_CONSTRUCT_API bool PhysicalConstructRemoveBody(PhysicalConstructSystem *system,
                                                     uint64_t bodyId);

// Conservative composite-collision query: true when the given world-grid
// cell overlaps a voxel or any of the three lever model parts.
LAIUE_CONSTRUCT_API bool PhysicalConstructWorldCellOccupied(const PhysicalConstructSystem *system,
                                                            int64_t x, int64_t y, int64_t z);

// Shared lever geometry for collision/raycast and the client model adapter.
// Bounds are expressed in the lever cell [0,1]^3.
LAIUE_CONSTRUCT_API bool PhysicalConstructGetLeverModel(
    const int8_t mountNormal[3], PhysicalConstructModelPart output[PHYSICAL_CONSTRUCT_MODEL_PARTS]);

// Signed local coordinates use mathematical floor division. Chunk zero starts
// at the lever cell; negative cells therefore belong to chunk -1.
LAIUE_CONSTRUCT_API void PhysicalConstructLocalToChunk(const int32_t localBlock[3],
                                                       int32_t outChunk[3], uint32_t outLocal[3]);
