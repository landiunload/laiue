#include "physics/voxel_body.h"
#include "numeric/infinite_coord.h"
#include "world/world.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#include <limits.h>

// The reference below is a regression contract for supported x86_64 and ARM64
// binary64 builds compiled with the engine's precise, non-contracting policy.
// A new architecture/toolchain must match it before joining that contract.
#define DETERMINISM_TICKS 60000U
#define DETERMINISM_EXPECTED_HASH 0x4486debc2d00fab7ULL

typedef struct DeterminismProvider
{
    int64_t origin[3];
} DeterminismProvider;

typedef struct Simulation
{
    DeterminismProvider provider;
    WorldBaseProvider providerApi;
    World *world;
    VoxelCollisionSource collision;
    InfiniteCoord origin[3];
    double position[3];
    double velocity[3];
    uint64_t hash;
    uint32_t collisionMask;
} Simulation;

static uint32_t determinismChecks;

static void DeterminismWriteHex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0U; index < 16U; ++index)
    {
        uint32_t shift = (15U - index) * 4U;
        text[2U + index] = digits[(value >> shift) & 0xfU];
    }
    text[18] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void DeterminismExpect(bool condition, const char *name)
{
    ++determinismChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Physics determinism check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static double AbsoluteDouble(double value)
{
    return value < 0.0 ? -value : value;
}

static uint64_t DoubleBits(double value)
{
    union
    {
        double value;
        uint64_t bits;
    } view = {value};
    return view.bits;
}

static void HashU64(uint64_t *hash, uint64_t value)
{
    for (uint32_t byte = 0U; byte < 8U; ++byte)
    {
        *hash ^= (value >> (byte * 8U)) & 0xffU;
        *hash *= 1099511628211ULL;
    }
}

static int64_t FloorLocalDouble(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

static uint64_t FractionToFixed32(double fraction)
{
    return (uint64_t)(fraction * 4294967296.0 + 0.5);
}

static BlockType DeterminismGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    DeterminismProvider *context = (DeterminismProvider *)rawContext;
    int64_t absoluteX = context->origin[0] + x;
    int64_t absoluteY = context->origin[1] + y;
    int64_t absoluteZ = context->origin[2] + z;

    if (absoluteZ <= 0)
    {
        return (BlockType)1U;
    }
    if (absoluteZ <= 4 &&
        (absoluteX == -23 || absoluteX == 24 || absoluteY == -19 || absoluteY == 20))
    {
        return (BlockType)2U;
    }
    if (absoluteZ <= 2 && absoluteX == 7 && absoluteY >= -6 && absoluteY <= 6)
    {
        return (BlockType)3U;
    }
    if (absoluteZ == 1 && absoluteY == -8 && absoluteX >= -12 && absoluteX <= 12)
    {
        return (BlockType)4U;
    }
    return BLOCK_AIR;
}

static bool DeterminismRebase(void *rawContext, int64_t blockShiftX, int64_t blockShiftY,
                              int64_t blockShiftZ)
{
    DeterminismProvider *context = (DeterminismProvider *)rawContext;
    context->origin[0] += blockShiftX;
    context->origin[1] += blockShiftY;
    context->origin[2] += blockShiftZ;
    return true;
}

static void QueryWorldPhysics(void *rawContext, int64_t x, int64_t y, int64_t z,
                              VoxelBlockPhysics *outBlock)
{
    World *world = (World *)rawContext;
    BlockType material = WorldGetBlock(world, x, y, z);
    outBlock->flags = material == BLOCK_AIR ? 0U : VOXEL_BLOCK_PHYSICS_SOLID;
    outBlock->friction = material == (BlockType)2U ? 0.5f : 0.75f;
}

static VoxelBodyShape DeterminismShape(void)
{
    VoxelBodyShape shape = {
        .radius = 0.30,
        .height = 1.80,
        .eyeHeight = 1.75,
        .collisionEpsilon = 0.001,
    };
    return shape;
}

static void SimulationInitialize(Simulation *simulation)
{
    *simulation = (Simulation){0};
    simulation->providerApi.context = &simulation->provider;
    simulation->providerApi.getBlock = DeterminismGetBlock;
    simulation->providerApi.rebase = DeterminismRebase;
    simulation->world = WorldCreate(&simulation->providerApi);
    DeterminismExpect(simulation->world != NULL, "simulation world creation");
    simulation->collision.context = simulation->world;
    simulation->collision.queryBlockPhysics = QueryWorldPhysics;
    simulation->position[0] = 0.5;
    simulation->position[1] = 0.5;
    simulation->position[2] = 2.751;
    simulation->velocity[0] = 5.0 / 256.0;
    simulation->velocity[1] = -3.0 / 256.0;
    simulation->velocity[2] = 0.0;
    simulation->hash = 14695981039346656037ULL;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        InfiniteCoordInit(&simulation->origin[axis]);
    }

    // Sparse application edits participate in the same absolute-coordinate
    // lookup as provider terrain and must survive every origin schedule.
    WorldSetBlock(simulation->world, -5, 3, 1, (BlockType)5U);
    WorldSetBlock(simulation->world, 7, 0, 1, BLOCK_AIR);
}

static void SimulationDestroy(Simulation *simulation)
{
    WorldDestroy(simulation->world);
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        InfiniteCoordDestroy(&simulation->origin[axis]);
    }
}

static void SimulationApplyRebase(Simulation *simulation, int64_t shiftX, int64_t shiftY,
                                  int64_t shiftZ)
{
    int64_t shifts[3] = {shiftX, shiftY, shiftZ};
    DeterminismExpect(WorldRebase(simulation->world, shiftX, shiftY, shiftZ),
                      "scheduled world rebase");
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        InfiniteCoord next;
        DeterminismExpect(
            InfiniteCoordTryCopyAddInt64(&next, &simulation->origin[axis], shifts[axis]),
            "scheduled absolute origin update");
        InfiniteCoordDestroy(&simulation->origin[axis]);
        simulation->origin[axis] = next;
        simulation->position[axis] -= (double)shifts[axis];
    }
}

static void SimulationApplySchedule(Simulation *simulation, uint32_t tick)
{
    switch (tick % 2048U)
    {
    case 137U:
        SimulationApplyRebase(simulation, CHUNK_SIZE, 0, 0);
        break;
    case 521U:
        SimulationApplyRebase(simulation, 0, -CHUNK_SIZE, 0);
        break;
    case 907U:
        SimulationApplyRebase(simulation, -CHUNK_SIZE, 0, CHUNK_SIZE);
        break;
    case 1421U:
        SimulationApplyRebase(simulation, 0, CHUNK_SIZE, -CHUNK_SIZE);
        break;
    default:
        break;
    }
}

static void SimulationHashCanonicalState(Simulation *simulation, const VoxelGroundContact *contact)
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        int64_t integral = FloorLocalDouble(simulation->position[axis]);
        double fraction = simulation->position[axis] - (double)integral;
        uint64_t fixedFraction = FractionToFixed32(fraction);
        if (fixedFraction == 4294967296ULL)
        {
            ++integral;
            fixedFraction = 0U;
        }
        HashU64(&simulation->hash, InfiniteCoordHashOffset(&simulation->origin[axis], integral));
        // Canonical Q0.32 fractions make the hash origin-schedule invariant.
        // Raw binary64 positions are compared separately within 1e-9 block.
        HashU64(&simulation->hash, fixedFraction);
        HashU64(&simulation->hash, DoubleBits(simulation->velocity[axis]));
    }
    HashU64(&simulation->hash, (uint64_t)simulation->collisionMask);
    HashU64(&simulation->hash, contact->supported ? 1U : 0U);
    HashU64(&simulation->hash, (uint64_t)DoubleBits(contact->friction));
}

static void SimulationStep(Simulation *simulation, uint32_t tick)
{
    VoxelBodyShape shape = DeterminismShape();
    if (tick != 0U && tick % 911U == 0U)
    {
        simulation->velocity[0] = -simulation->velocity[0];
    }
    if (tick != 0U && tick % 1237U == 0U)
    {
        simulation->velocity[1] = -simulation->velocity[1];
    }

    VoxelGroundContact before;
    VoxelBodyQueryGroundContact(&simulation->collision, simulation->position, &shape, 0.01,
                                &before);
    if (before.supported && tick % 487U == 11U)
    {
        simulation->velocity[2] = 1.0 / 16.0;
    }
    else
    {
        simulation->velocity[2] -= 1.0 / 1024.0;
        if (simulation->velocity[2] < -1.0 / 16.0)
        {
            simulation->velocity[2] = -1.0 / 16.0;
        }
    }

    simulation->collisionMask = 0U;
    if (VoxelBodyMoveAxis(&simulation->collision, simulation->position, &shape, 0,
                          simulation->velocity[0]))
    {
        simulation->collisionMask |= 1U;
        simulation->velocity[0] = -simulation->velocity[0];
    }
    if (VoxelBodyMoveAxis(&simulation->collision, simulation->position, &shape, 1,
                          simulation->velocity[1]))
    {
        simulation->collisionMask |= 2U;
        simulation->velocity[1] = -simulation->velocity[1];
    }
    if (VoxelBodyMoveAxis(&simulation->collision, simulation->position, &shape, 2,
                          simulation->velocity[2]))
    {
        simulation->collisionMask |= 4U;
        simulation->velocity[2] = 0.0;
    }

    VoxelGroundContact after;
    VoxelBodyQueryGroundContact(&simulation->collision, simulation->position, &shape, 0.01, &after);
    SimulationHashCanonicalState(simulation, &after);
}

static void CompareCanonicalPositions(const Simulation *left, const Simulation *right)
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        int64_t leftIntegral = FloorLocalDouble(left->position[axis]);
        int64_t rightIntegral = FloorLocalDouble(right->position[axis]);
        DeterminismExpect(InfiniteCoordEqualsOffsets(&left->origin[axis], leftIntegral,
                                                     &right->origin[axis], rightIntegral),
                          "rebase schedule changed absolute body cell");
        double leftFraction = left->position[axis] - (double)leftIntegral;
        double rightFraction = right->position[axis] - (double)rightIntegral;
        DeterminismExpect(AbsoluteDouble(leftFraction - rightFraction) <= 1.0e-9,
                          "rebase schedule exceeded binary64 position tolerance");
        DeterminismExpect(DoubleBits(left->velocity[axis]) == DoubleBits(right->velocity[axis]),
                          "rebase schedule changed velocity bits");
    }
    DeterminismExpect(left->collisionMask == right->collisionMask,
                      "rebase schedule changed collision topology");
}

static uint64_t RunLongSimulation(bool scheduledRebases)
{
    Simulation baseline;
    SimulationInitialize(&baseline);
    Simulation rebased;
    SimulationInitialize(&rebased);

    for (uint32_t tick = 0U; tick < DETERMINISM_TICKS; ++tick)
    {
        if (scheduledRebases)
        {
            SimulationApplySchedule(&rebased, tick);
        }
        SimulationStep(&baseline, tick);
        SimulationStep(&rebased, tick);
        if (scheduledRebases)
        {
            CompareCanonicalPositions(&baseline, &rebased);
        }
    }

    DeterminismExpect(!scheduledRebases || baseline.collisionMask == rebased.collisionMask,
                      "final collision state differs after rebasing");
    uint64_t result = scheduledRebases ? rebased.hash : baseline.hash;
    SimulationDestroy(&baseline);
    SimulationDestroy(&rebased);
    return result;
}

typedef struct BoundaryWorld
{
    int64_t wallX;
} BoundaryWorld;

typedef struct DynamicOrderWorld
{
    bool reversed;
} DynamicOrderWorld;

static void QueryEmptyWorld(void *rawContext, int64_t x, int64_t y, int64_t z,
                            VoxelBlockPhysics *outBlock)
{
    (void)rawContext;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = 0U;
    outBlock->friction = 0.0f;
}

static bool QueryOrderedDynamicColliders(void *rawContext, const VoxelBodyBounds *queryBounds,
                                         VoxelDynamicCollider *outColliders,
                                         uint32_t colliderCapacity, uint32_t *outColliderCount)
{
    DynamicOrderWorld *world = rawContext;
    (void)queryBounds;
    if (colliderCapacity < 2U)
    {
        *outColliderCount = 0U;
        return false;
    }
    VoxelDynamicCollider lowId = {
        .bounds =
            {
                .minimum = {-1.0, -1.0, 0.0},
                .maximum = {1.0, 1.0, 0.5},
            },
        .velocity = {0.125, -0.25, 0.0},
        .friction = 0.875f,
        .stableId = 3U,
    };
    VoxelDynamicCollider highId = lowId;
    highId.velocity[0] = -0.5;
    highId.friction = 0.25f;
    highId.stableId = 9U;
    outColliders[world->reversed ? 1U : 0U] = lowId;
    outColliders[world->reversed ? 0U : 1U] = highId;
    *outColliderCount = 2U;
    return true;
}

static void TestDynamicColliderOrderIndependence(void)
{
    DynamicOrderWorld world = {0};
    VoxelCollisionSource source = {
        .context = &world,
        .queryBlockPhysics = QueryEmptyWorld,
        .queryDynamicColliders = QueryOrderedDynamicColliders,
    };
    VoxelBodyShape shape = DeterminismShape();
    const double body[3] = {0.0, 0.0, 2.251};
    VoxelGroundContact forward;
    VoxelBodyQueryGroundContact(&source, body, &shape, 0.01, &forward);
    world.reversed = true;
    VoxelGroundContact reversed;
    VoxelBodyQueryGroundContact(&source, body, &shape, 0.01, &reversed);
    DeterminismExpect(forward.supported && reversed.supported && forward.surfaceStableId == 3U &&
                          reversed.surfaceStableId == 3U,
                      "dynamic collider stable-id tie break changed with callback order");
    DeterminismExpect(
        DoubleBits(forward.friction) == DoubleBits(reversed.friction) &&
            DoubleBits(forward.surfaceVelocity[0]) == DoubleBits(reversed.surfaceVelocity[0]) &&
            DoubleBits(forward.surfaceVelocity[1]) == DoubleBits(reversed.surfaceVelocity[1]),
        "dynamic contact properties changed with callback order");
}

static void QueryBoundaryWorld(void *rawContext, int64_t x, int64_t y, int64_t z,
                               VoxelBlockPhysics *outBlock)
{
    BoundaryWorld *world = (BoundaryWorld *)rawContext;
    bool solid = z == 0 || (x == world->wallX && z >= 1 && z <= 3);
    outBlock->flags = solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0U;
    outBlock->friction = 0.75f;
    (void)y;
}

static void TestChunkEdgesAndContactStability(void)
{
    VoxelBodyShape shape = DeterminismShape();
    BoundaryWorld boundary = {.wallX = CHUNK_SIZE};
    VoxelCollisionSource source = {
        .context = &boundary,
        .queryBlockPhysics = QueryBoundaryWorld,
    };
    double positive[3] = {62.5, 0.5, 2.751};
    DeterminismExpect(VoxelBodyMoveAxis(&source, positive, &shape, 0, 3.0),
                      "positive chunk-edge wall was missed");
    DeterminismExpect(AbsoluteDouble(positive[0] - 63.699) <= 1.0e-12,
                      "positive chunk-edge clipping is inaccurate");

    boundary.wallX = -CHUNK_SIZE - 1;
    double negative[3] = {-62.5, 0.5, 2.751};
    DeterminismExpect(VoxelBodyMoveAxis(&source, negative, &shape, 0, -3.0),
                      "negative chunk-edge wall was missed");
    DeterminismExpect(AbsoluteDouble(negative[0] - -63.699) <= 1.0e-12,
                      "negative chunk-edge clipping is inaccurate");

    double resting[3] = {-0.5, -0.5, 4.0};
    DeterminismExpect(VoxelBodyMoveAxis(&source, resting, &shape, 2, -4.0),
                      "initial floor landing was missed");
    uint64_t stableBits = DoubleBits(resting[2]);
    for (uint32_t step = 0U; step < 100000U; ++step)
    {
        DeterminismExpect(VoxelBodyMoveAxis(&source, resting, &shape, 2, -1.0 / 64.0),
                          "resting floor contact was lost");
        DeterminismExpect(DoubleBits(resting[2]) == stableBits,
                          "resting body accumulated vertical drift");
        DeterminismExpect(!VoxelBodyCollides(&source, resting, &shape),
                          "resting collision margin became penetrating");
    }
}

static void TestInsufficientLocalResolutionFailsClosed(void)
{
    BoundaryWorld boundary = {.wallX = 0};
    VoxelCollisionSource source = {
        .context = &boundary,
        .queryBlockPhysics = QueryBoundaryWorld,
    };
    VoxelBodyShape shape = DeterminismShape();
    double normalLocal[3] = {0.5, 0.5, 2.751};
    double resolvedBinade[3] = {1099511627776.0, 0.5, 2.751};
    double unresolvedBinade[3] = {2199023255552.0, 0.5, 2.751};
    DeterminismExpect(VoxelBodyLocalRangeIsResolved(normalLocal, &shape),
                      "ordinary local coordinates were rejected");
    DeterminismExpect(VoxelBodyLocalRangeIsResolved(resolvedBinade, &shape),
                      "four-ULP collision margin boundary was rejected");
    DeterminismExpect(!VoxelBodyLocalRangeIsResolved(unresolvedBinade, &shape),
                      "sub-four-ULP collision margin boundary was accepted");
    double farLocal[3] = {1.0e15, 1.0e15, 1.0e15};
    double original[3] = {farLocal[0], farLocal[1], farLocal[2]};
    DeterminismExpect(!VoxelBodyLocalRangeIsResolved(farLocal, &shape),
                      "far local precision probe incorrectly succeeded");
    DeterminismExpect(VoxelBodyCollides(&source, farLocal, &shape),
                      "unresolvable local collision margin did not fail closed");
    DeterminismExpect(VoxelBodyMoveAxis(&source, farLocal, &shape, 0, 0.001) &&
                          DoubleBits(farLocal[0]) == DoubleBits(original[0]) &&
                          DoubleBits(farLocal[1]) == DoubleBits(original[1]) &&
                          DoubleBits(farLocal[2]) == DoubleBits(original[2]),
                      "sub-ULP far-local movement did not fail closed atomically");
}

static void TestPhysicsAtAbsoluteOriginBeyondInt64(void)
{
    World *world = WorldCreate(NULL);
    DeterminismExpect(world != NULL, "far absolute world creation");
    const int64_t hugeAligned = INT64_MAX - (CHUNK_SIZE - 1);
    DeterminismExpect(WorldRebase(world, hugeAligned, 0, 0) &&
                          WorldRebase(world, hugeAligned, 0, 0) &&
                          WorldRebase(world, hugeAligned, 0, 0),
                      "absolute origin did not grow beyond int64");
    WorldSetBlock(world, 0, 0, 0, (BlockType)1U);

    VoxelCollisionSource source = {
        .context = world,
        .queryBlockPhysics = QueryWorldPhysics,
    };
    VoxelBodyShape shape = DeterminismShape();
    double body[3] = {0.5, 0.5, 4.0};
    DeterminismExpect(VoxelBodyMoveAxis(&source, body, &shape, 2, -4.0) &&
                          AbsoluteDouble(body[2] - 2.751) <= 1.0e-12,
                      "local collision failed beyond int64 absolute origin");

    DeterminismExpect(WorldRebase(world, CHUNK_SIZE, -CHUNK_SIZE, 0), "far-world local rebase");
    body[0] -= CHUNK_SIZE;
    body[1] += CHUNK_SIZE;
    DeterminismExpect(WorldGetBlock(world, -CHUNK_SIZE, CHUNK_SIZE, 0) == (BlockType)1U,
                      "far sparse block lost its absolute coordinate");
    DeterminismExpect(VoxelBodyMoveAxis(&source, body, &shape, 2, -1.0 / 64.0) &&
                          AbsoluteDouble(body[2] - 2.751) <= 1.0e-12,
                      "far collision changed after local rebase");
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(PhysicsDeterminismTestEntryPoint)
{
    LaiueTestSetHostileFpEnvironment();
    DeterminismExpect(!VoxelPhysicsThreadIsConfigured(), "hostile FP environment was not detected");
    VoxelPhysicsConfigureThread();
    DeterminismExpect(VoxelPhysicsThreadIsConfigured(),
                      "deterministic FP environment was not installed");

    TestDynamicColliderOrderIndependence();
    TestChunkEdgesAndContactStability();
    TestInsufficientLocalResolutionFailsClosed();
    TestPhysicsAtAbsoluteOriginBeyondInt64();

    LaiueTestSetHostileFpEnvironment();
    uint64_t referenceHash = RunLongSimulation(false);
    DeterminismExpect(VoxelPhysicsThreadIsConfigured(),
                      "physics API did not self-normalize the FP environment");
    LaiueTestSetHostileFpEnvironment();
    uint64_t rebasedHash = RunLongSimulation(true);
    LaiueTestSetHostileFpEnvironment();
    uint64_t repeatedHash = RunLongSimulation(false);
    LaiueTestRuntimeWrite("physics-determinism-hash: ");
    DeterminismWriteHex64(referenceHash);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeWrite("physics-rebased-hash: ");
    DeterminismWriteHex64(rebasedHash);
    LaiueTestRuntimeWrite("\r\n");
    DeterminismExpect(referenceHash == repeatedHash,
                      "identical fixed-step run changed reference hash");
    DeterminismExpect(referenceHash == rebasedHash,
                      "rebase schedule changed canonical reference hash");
    DeterminismExpect(DETERMINISM_EXPECTED_HASH == 0ULL ||
                          referenceHash == DETERMINISM_EXPECTED_HASH,
                      "supported-platform reference hash changed");
    LAIUE_TEST_SUCCESS();
}
