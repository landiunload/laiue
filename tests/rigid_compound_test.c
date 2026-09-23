// Regression tests for the additive compound rigid-body API:
//   VoxelRigidCompoundMassProperties
//   VoxelRigidBodyStepCompoundScratchBytes
//   VoxelRigidBodyStepCompoundEx
//
// The contract is the one documented in src/simulation/physics/rigid_body.h. The test
// deliberately exercises real trajectories, mutual impulses, concave gaps,
// inversion of the full row-major 3x3 tensor, SPD/envelope rejection before
// mutation, children-aware waking, scaled primitive contact budgets, cache warm
// starting, executor independent replay and rebasing. single-child shapes are
// checked against the legacy box paths; boxCount == 0 must keep the legacy fast
// path unchanged.
//
// No-CRT builds must not emit a large stack probe, so every buffer is static.

#include "physics/rigid_body.h"
#include "physics/numeric_provider.h"
#include "task/task_pool.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#include <string.h>

#define TEST_BODY_COUNT 32u
#define TEST_CACHE_CAPACITY 64u
#define MAX_CHILDREN 32u
#define TEST_SCRATCH_BYTES 1048576u
#define TEST_CACHE_BYTES 262144u
#define LEGACY_TICKS 256u
#define REPLAY_TICKS 512u
#define EXECUTOR_TICKS 128u
#define FLAT_PLATE_BLOCKS 16u

// Large rotated rod regression: 256 adjacent unit children. Storage is kept
// separate from the 32-child fixtures so their replay hashes stay unchanged.
#define LARGE_ROD_CHILDREN 256u
#define LARGE_ROD_SCRATCH_BYTES 4194304u
#define LARGE_ROD_CACHE_BYTES 1048576u
#define LARGE_ROD_TICKS 512u

typedef struct CompoundScene
{
    VoxelRigidBody bodies[TEST_BODY_COUNT];
    VoxelRigidCompoundBox children[TEST_BODY_COUNT][MAX_CHILDREN];
    VoxelRigidCompoundShape shapes[TEST_BODY_COUNT];
    uint32_t count;
} CompoundScene;

static uint8_t stepScratch[TEST_SCRATCH_BYTES];
static uint8_t cacheStorage[TEST_CACHE_BYTES];
static uint8_t secondCacheStorage[TEST_CACHE_BYTES];
static uint8_t smallCacheStorage[4096];
static VoxelRigidContactCache contactCache;
static VoxelRigidContactCache secondContactCache;
static VoxelRigidContactCache smallContactCache;
static bool floorEnabled = true;
static int64_t worldOrigin[3];
static bool perturbFp;

static CompoundScene sceneA;
static CompoundScene sceneB;
static CompoundScene sceneWork;
static VoxelRigidCompoundBox temporaryBoxes[MAX_CHILDREN];
static VoxelRigidCompoundShape rejectShapes[TEST_BODY_COUNT];

static VoxelRigidBody largeRodBodies[1];
static VoxelRigidCompoundBox largeRodChildren[LARGE_ROD_CHILDREN];
static VoxelRigidCompoundShape largeRodShapes[1];
static uint8_t largeRodScratch[LARGE_ROD_SCRATCH_BYTES];
static uint8_t largeRodCacheStorage[LARGE_ROD_CACHE_BYTES];
static VoxelRigidContactCache largeRodCache;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Rigid compound failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static bool Near(double value, double expected, double tolerance)
{
    return Absolute(value - expected) <= tolerance;
}

static union
{
    double scalar;
    uint64_t bits;
} scalarBits;

static double NaNValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff8000000000000);
    return scalarBits.scalar;
}

static double InfValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff0000000000000);
    return scalarBits.scalar;
}

static uint64_t DoubleBits(double value)
{
    scalarBits.scalar = value;
    return scalarBits.bits;
}

static VoxelRigidCompoundBox MakeBox(double cx, double cy, double cz, double hx, double hy,
                                     double hz)
{
    VoxelRigidCompoundBox box;
    box.center[0] = cx;
    box.center[1] = cy;
    box.center[2] = cz;
    box.halfExtent[0] = hx;
    box.halfExtent[1] = hy;
    box.halfExtent[2] = hz;
    return box;
}

// Independent reference for the documented uniform-density formulas. It is not
// shared with production code on purpose: a wrong parallel-axis term, envelope
// or inverse must fail here.
static void ReferenceMassProperties(const VoxelRigidCompoundBox *boxes, uint32_t count,
                                    double mass, double outCenter[3], double outHalf[3],
                                    double outInverse[9])
{
    double volume = 0.0;
    for (uint32_t index = 0u; index < count; ++index)
    {
        volume += 8.0 * boxes[index].halfExtent[0] * boxes[index].halfExtent[1] *
                  boxes[index].halfExtent[2];
    }
    double density = mass / volume;

    double center[3] = {0.0, 0.0, 0.0};
    for (uint32_t index = 0u; index < count; ++index)
    {
        double childMass = density * 8.0 * boxes[index].halfExtent[0] * boxes[index].halfExtent[1] *
                           boxes[index].halfExtent[2];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            center[axis] += childMass * boxes[index].center[axis];
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        center[axis] /= mass;
        outCenter[axis] = center[axis];
        outHalf[axis] = 0.0;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            double reach = boxes[index].center[axis] + boxes[index].halfExtent[axis] - center[axis];
            double lower =
                center[axis] - (boxes[index].center[axis] - boxes[index].halfExtent[axis]);
            if (lower > reach)
            {
                reach = lower;
            }
            if (reach > outHalf[axis])
            {
                outHalf[axis] = reach;
            }
        }
    }

    double inertia[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (uint32_t index = 0u; index < count; ++index)
    {
        const double *half = boxes[index].halfExtent;
        double childMass = density * 8.0 * half[0] * half[1] * half[2];
        double offset[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            offset[axis] = boxes[index].center[axis] - center[axis];
        }
        inertia[0] += childMass * (half[1] * half[1] + half[2] * half[2]) / 3.0 +
                      childMass * (offset[1] * offset[1] + offset[2] * offset[2]);
        inertia[4] += childMass * (half[0] * half[0] + half[2] * half[2]) / 3.0 +
                      childMass * (offset[0] * offset[0] + offset[2] * offset[2]);
        inertia[8] += childMass * (half[0] * half[0] + half[1] * half[1]) / 3.0 +
                      childMass * (offset[0] * offset[0] + offset[1] * offset[1]);
        inertia[1] += -childMass * offset[0] * offset[1];
        inertia[2] += -childMass * offset[0] * offset[2];
        inertia[5] += -childMass * offset[1] * offset[2];
    }
    inertia[3] = inertia[1];
    inertia[6] = inertia[2];
    inertia[7] = inertia[5];

    double determinant = inertia[0] * (inertia[4] * inertia[8] - inertia[5] * inertia[5]) -
                         inertia[1] * (inertia[1] * inertia[8] - inertia[5] * inertia[2]) +
                         inertia[2] * (inertia[1] * inertia[5] - inertia[4] * inertia[2]);
    outInverse[0] = (inertia[4] * inertia[8] - inertia[5] * inertia[5]) / determinant;
    outInverse[1] = (inertia[2] * inertia[5] - inertia[1] * inertia[8]) / determinant;
    outInverse[2] = (inertia[1] * inertia[5] - inertia[2] * inertia[4]) / determinant;
    outInverse[3] = outInverse[1];
    outInverse[4] = (inertia[0] * inertia[8] - inertia[2] * inertia[2]) / determinant;
    outInverse[5] = (inertia[1] * inertia[2] - inertia[0] * inertia[5]) / determinant;
    outInverse[6] = outInverse[2];
    outInverse[7] = outInverse[5];
    outInverse[8] = (inertia[0] * inertia[4] - inertia[1] * inertia[1]) / determinant;
}

static void ExpectVectorNear(const double actual[3], const double expected[3], double tolerance,
                             const char *message)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        Expect(Absolute(actual[axis] - expected[axis]) <= tolerance, message);
    }
}

// Bit-exact sentinel check: a rejected call must not have touched the outputs.
static void ExpectVectorUnchanged(const double actual[3], double expected, const char *message)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        Expect(DoubleBits(actual[axis]) == DoubleBits(expected), message);
    }
}

static void ExpectMatrixNear(const double actual[9], const double expected[9], double tolerance,
                             const char *message)
{
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        double scale = 1.0 + Absolute(expected[index]);
        Expect(Absolute(actual[index] - expected[index]) <= tolerance * scale, message);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryFloor(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *block)
{
    (void)context;
    (void)x;
    (void)y;
    block->flags =
        floorEnabled && z + worldOrigin[2] < 0 ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    block->friction = 0.6f;
}

static VoxelCollisionSource Collision(void)
{
    VoxelCollisionSource collision = {NULL, QueryFloor, NULL};
    return collision;
}

static VoxelRigidStepOptions Options(VoxelRigidContactCache *cache,
                                     const LaiueTaskExecutor *executor)
{
    VoxelRigidStepOptions options = {0};
    options.structSize = sizeof(options);
    options.contactCache = cache;
    options.executor = executor;
    return options;
}

static uint64_t HashWord(uint64_t hash, uint64_t word)
{
    for (uint32_t byte = 0u; byte < 8u; ++byte)
    {
        hash = (hash ^ (word & 255u)) * UINT64_C(1099511628211);
        word >>= 8u;
    }
    return hash;
}

static uint64_t HashCoordinate(uint64_t hash, const InfiniteCoord *coordinate)
{
    hash = HashWord(hash, (uint64_t)(int64_t)coordinate->sign);
    hash = HashWord(hash, coordinate->limbCount);
    for (uint32_t limb = 0u; limb < coordinate->limbCount; ++limb)
    {
        hash = HashWord(hash, coordinate->limbs[limb]);
    }
    return hash;
}

static uint64_t HashBody(uint64_t hash, const VoxelRigidBody *body)
{
    hash = HashWord(hash, body->stableId);
    hash = HashWord(hash, body->active ? 1u : 0u);
    hash = HashWord(hash, body->sleeping ? 1u : 0u);
    hash = HashWord(hash, body->sleepCounter);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        hash = HashCoordinate(hash, &body->position[axis]);
        hash = HashCoordinate(hash, &body->linearVelocity[axis]);
        hash = HashCoordinate(hash, &body->angularVelocity[axis]);
        hash = HashWord(hash, DoubleBits(body->halfExtent[axis]));
        hash = HashWord(hash, DoubleBits(body->inverseInertia[axis]));
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        hash = HashWord(hash, DoubleBits(body->orientation[component]));
    }
    hash = HashWord(hash, DoubleBits(body->inverseMass));
    hash = HashWord(hash, DoubleBits(body->restitution));
    return HashWord(hash, DoubleBits(body->friction));
}

static uint64_t HashBodies(const VoxelRigidBody *bodies, uint32_t count)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t index = 0u; index < count; ++index)
    {
        hash = HashBody(hash, &bodies[index]);
    }
    return hash;
}

static bool SameBody(const VoxelRigidBody *first, const VoxelRigidBody *second)
{
    if (first->stableId != second->stableId || first->active != second->active ||
        first->sleeping != second->sleeping || first->sleepCounter != second->sleepCounter ||
        DoubleBits(first->inverseMass) != DoubleBits(second->inverseMass) ||
        DoubleBits(first->restitution) != DoubleBits(second->restitution) ||
        DoubleBits(first->friction) != DoubleBits(second->friction))
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (InfiniteCoordCompare(&first->position[axis], &second->position[axis]) != 0 ||
            InfiniteCoordCompare(&first->linearVelocity[axis], &second->linearVelocity[axis]) != 0 ||
            InfiniteCoordCompare(&first->angularVelocity[axis], &second->angularVelocity[axis]) !=
                0 ||
            DoubleBits(first->halfExtent[axis]) != DoubleBits(second->halfExtent[axis]) ||
            DoubleBits(first->inverseInertia[axis]) != DoubleBits(second->inverseInertia[axis]))
        {
            return false;
        }
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        if (DoubleBits(first->orientation[component]) != DoubleBits(second->orientation[component]))
        {
            return false;
        }
    }
    return true;
}

static bool NearBody(const VoxelRigidBody *first, const VoxelRigidBody *second, double tolerance)
{
    double firstPosition[3];
    double secondPosition[3];
    double firstLinear[3];
    double secondLinear[3];
    double firstAngular[3];
    double secondAngular[3];
    if (!VoxelRigidBodyLocalPosition(first, firstPosition) ||
        !VoxelRigidBodyLocalPosition(second, secondPosition) ||
        !VoxelRigidBodyLinearVelocity(first, firstLinear) ||
        !VoxelRigidBodyLinearVelocity(second, secondLinear) ||
        !VoxelRigidBodyAngularVelocity(first, firstAngular) ||
        !VoxelRigidBodyAngularVelocity(second, secondAngular))
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (Absolute(firstPosition[axis] - secondPosition[axis]) > tolerance ||
            Absolute(firstLinear[axis] - secondLinear[axis]) > tolerance ||
            Absolute(firstAngular[axis] - secondAngular[axis]) > tolerance)
        {
            return false;
        }
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        if (Absolute(first->orientation[component] - second->orientation[component]) > tolerance)
        {
            return false;
        }
    }
    return true;
}

static void ReleaseScene(CompoundScene *scene)
{
    for (uint32_t index = 0u; index < scene->count; ++index)
    {
        VoxelRigidBodyRelease(&scene->bodies[index]);
    }
    scene->count = 0u;
}

static void InitPlainBody(CompoundScene *scene, uint32_t slot, double half, double x, double y,
                          double z, double mass, uint64_t stableId, double restitution,
                          double friction)
{
    VoxelRigidBodyDescription description = {0};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        description.halfExtent[axis] = half;
    }
    description.position[0] = x;
    description.position[1] = y;
    description.position[2] = z;
    description.mass = mass;
    description.restitution = restitution;
    description.friction = friction;
    Expect(VoxelRigidBodyInitialize(&scene->bodies[slot], stableId, &description),
           "plain body initialized");
    scene->shapes[slot].boxes = NULL;
    scene->shapes[slot].boxCount = 0u;
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        scene->shapes[slot].inverseInertia[index] = 0.0;
    }
}

// Builds a compound body whose children live in the local frame `boxes`.
// body.position becomes COM + offset, so the children occupy `boxes` translated
// by offset, and the shape stores child centers relative to COM.
static void InitCompoundBody(CompoundScene *scene, uint32_t slot,
                             const VoxelRigidCompoundBox *boxes, uint32_t boxCount, double mass,
                             const double offset[3], uint64_t stableId, double restitution,
                             double friction)
{
    double center[3];
    double envelope[3];
    double inverse[9];
    Expect(VoxelRigidCompoundMassProperties(boxes, boxCount, mass, center, envelope, inverse),
           "compound mass properties resolved");
    VoxelRigidBodyDescription description = {0};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        description.halfExtent[axis] = envelope[axis];
        description.position[axis] = center[axis] + offset[axis];
    }
    description.mass = mass;
    description.restitution = restitution;
    description.friction = friction;
    Expect(VoxelRigidBodyInitialize(&scene->bodies[slot], stableId, &description),
           "compound body initialized");
    for (uint32_t child = 0u; child < boxCount; ++child)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            scene->children[slot][child].center[axis] = boxes[child].center[axis] - center[axis];
            scene->children[slot][child].halfExtent[axis] = boxes[child].halfExtent[axis];
        }
    }
    scene->shapes[slot].boxes = scene->children[slot];
    scene->shapes[slot].boxCount = boxCount;
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        scene->shapes[slot].inverseInertia[index] = inverse[index];
    }
}

static uint32_t ScenePrimitiveCount(const CompoundScene *scene)
{
    uint32_t primitives = 0u;
    for (uint32_t index = 0u; index < scene->count; ++index)
    {
        primitives += scene->shapes[index].boxCount == 0u ? 1u : scene->shapes[index].boxCount;
    }
    return primitives;
}

static bool StepScene(CompoundScene *scene, const VoxelRigidStepSettings *settings,
                      const VoxelRigidStepOptions *options)
{
    VoxelCollisionSource collision = Collision();
    uint32_t required =
        VoxelRigidBodyStepCompoundScratchBytes(scene->count, ScenePrimitiveCount(scene));
    Expect(required != 0u && required <= sizeof(stepScratch), "compound scratch sizing fits");
    if (perturbFp)
    {
        LaiueTestSetHostileFpEnvironment();
    }
    bool ok = VoxelRigidBodyStepCompoundEx(scene->bodies, scene->count, &collision, settings,
                                           stepScratch, required, options, scene->shapes);
    if (perturbFp)
    {
        Expect(VoxelPhysicsThreadIsConfigured(), "compound step restores FP environment");
    }
    return ok;
}

static void InitializeCaches(void)
{
    uint32_t bytes = VoxelRigidContactCacheBytes(TEST_CACHE_CAPACITY);
    Expect(bytes != 0u && bytes < sizeof(cacheStorage), "cache storage requirement fits");
    Expect(VoxelRigidContactCacheInitialize(&contactCache, cacheStorage, TEST_CACHE_CAPACITY, bytes),
           "primary cache initialized");
    Expect(VoxelRigidContactCacheInitialize(&secondContactCache, secondCacheStorage,
                                            TEST_CACHE_CAPACITY, bytes),
           "secondary cache initialized");
    uint32_t smallBytes = VoxelRigidContactCacheBytes(1u);
    Expect(smallBytes != 0u && smallBytes < sizeof(smallCacheStorage), "small cache fits");
    Expect(VoxelRigidContactCacheInitialize(&smallContactCache, smallCacheStorage, 1u, smallBytes),
           "small cache initialized");
    uint32_t largeBytes = VoxelRigidContactCacheBytes(LARGE_ROD_CHILDREN);
    Expect(largeBytes != 0u && largeBytes <= sizeof(largeRodCacheStorage), "large cache fits");
    Expect(VoxelRigidContactCacheInitialize(&largeRodCache, largeRodCacheStorage, LARGE_ROD_CHILDREN,
                                            largeBytes),
           "large rod cache initialized");
}

// === Массовые свойства ===

static void TestMassPropertiesAnalytic(void)
{
    double center[3];
    double half[3];
    double inverse[9];
    double referenceCenter[3];
    double referenceHalf[3];
    double referenceInverse[9];

    const double origin[3] = {0.0, 0.0, 0.0};
    const double wideEnvelope[3] = {1.0, 2.0, 3.0};
    const double shiftedCentre[3] = {1.5, -2.5, 0.5};
    const double unitHalf[3] = {0.5, 0.5, 0.5};

    // Single centred box against the textbook formula.
    VoxelRigidCompoundBox single[1];
    single[0] = MakeBox(0.0, 0.0, 0.0, 1.0, 2.0, 3.0);
    Expect(VoxelRigidCompoundMassProperties(single, 1u, 6.0, center, half, inverse),
           "single box mass properties");
    ExpectVectorNear(center, origin, 1e-12, "single box COM at centre");
    ExpectVectorNear(half, wideEnvelope, 1e-12, "single box envelope");
    Expect(Near(inverse[0], 1.0 / 26.0, 1e-12), "single box inverse Ixx");
    Expect(Near(inverse[4], 1.0 / 20.0, 1e-12), "single box inverse Iyy");
    Expect(Near(inverse[8], 1.0 / 10.0, 1e-12), "single box inverse Izz");
    Expect(Near(inverse[1], 0.0, 1e-15) && Near(inverse[2], 0.0, 1e-15) &&
               Near(inverse[5], 0.0, 1e-15),
           "single box inverse is diagonal");
    ReferenceMassProperties(single, 1u, 6.0, referenceCenter, referenceHalf, referenceInverse);
    ExpectMatrixNear(inverse, referenceInverse, 1e-12, "single box inverse matches reference");

    // Off-centre single box: the COM follows the child, the envelope stays tight.
    VoxelRigidCompoundBox shifted[1];
    shifted[0] = MakeBox(1.5, -2.5, 0.5, 0.5, 0.5, 0.5);
    Expect(VoxelRigidCompoundMassProperties(shifted, 1u, 1.0, center, half, inverse),
           "shifted box mass properties");
    ExpectVectorNear(center, shiftedCentre, 1e-12, "single box COM equals its centre");
    ExpectVectorNear(half, unitHalf, 1e-12, "envelope is relative to COM");

    // Two equal boxes symmetric about the origin: COM at origin, inertia diagonal.
    VoxelRigidCompoundBox pair[2];
    pair[0] = MakeBox(-1.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    pair[1] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    Expect(VoxelRigidCompoundMassProperties(pair, 2u, 2.0, center, half, inverse),
           "symmetric pair mass properties");
    ReferenceMassProperties(pair, 2u, 2.0, referenceCenter, referenceHalf, referenceInverse);
    ExpectVectorNear(center, referenceCenter, 1e-12, "symmetric pair COM");
    ExpectVectorNear(half, referenceHalf, 1e-12, "symmetric pair envelope");
    ExpectMatrixNear(inverse, referenceInverse, 1e-12, "symmetric pair inverse");
    ExpectVectorNear(center, origin, 1e-12, "symmetric pair COM is the midpoint");
    Expect(Near(half[0], 1.5, 1e-12), "symmetric pair envelope reaches the far faces");

    // Three boxes that force genuinely non-diagonal products of inertia.
    VoxelRigidCompoundBox diagonal[3];
    diagonal[0] = MakeBox(1.0, 1.0, 0.0, 0.4, 0.4, 0.4);
    diagonal[1] = MakeBox(1.0, 0.0, 1.0, 0.4, 0.4, 0.4);
    diagonal[2] = MakeBox(0.0, 1.0, 1.0, 0.4, 0.4, 0.4);
    Expect(VoxelRigidCompoundMassProperties(diagonal, 3u, 3.0, center, half, inverse),
           "non-diagonal mass properties");
    ReferenceMassProperties(diagonal, 3u, 3.0, referenceCenter, referenceHalf, referenceInverse);
    ExpectVectorNear(center, referenceCenter, 1e-12, "non-diagonal COM matches reference");
    ExpectVectorNear(half, referenceHalf, 1e-12, "non-diagonal envelope matches reference");
    ExpectMatrixNear(inverse, referenceInverse, 1e-9,
                     "full inverse tensor matches independent reference");
    Expect(Near(inverse[1], inverse[3], 1e-15) && Near(inverse[2], inverse[6], 1e-15) &&
               Near(inverse[5], inverse[7], 1e-15),
           "inverse tensor is symmetric");
    Expect(Absolute(inverse[1]) > 1e-9 && Absolute(inverse[2]) > 1e-9 &&
               Absolute(inverse[5]) > 1e-9,
           "non-diagonal inverse really has off-diagonal terms");
    for (uint32_t child = 0u; child < 3u; ++child)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            double reach = Absolute(diagonal[child].center[axis] - center[axis]) +
                           diagonal[child].halfExtent[axis];
            Expect(reach <= half[axis] + 1e-12, "envelope covers every child corner");
        }
    }

    // Touching faces do not count as overlap and must be accepted.
    VoxelRigidCompoundBox touching[2];
    touching[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    touching[1] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    Expect(VoxelRigidCompoundMassProperties(touching, 2u, 2.0, center, half, inverse),
           "face-touching boxes are not overlapping");

    // Determinism: the same input must produce the same bits.
    double secondCenter[3];
    double secondHalf[3];
    double secondInverse[9];
    Expect(VoxelRigidCompoundMassProperties(diagonal, 3u, 3.0, center, half, inverse),
           "repeat mass properties primary");
    Expect(VoxelRigidCompoundMassProperties(diagonal, 3u, 3.0, secondCenter, secondHalf,
                                            secondInverse),
           "repeat mass properties secondary");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        Expect(DoubleBits(center[axis]) == DoubleBits(secondCenter[axis]) &&
                   DoubleBits(half[axis]) == DoubleBits(secondHalf[axis]),
               "mass properties repeat bit for bit");
    }
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        Expect(DoubleBits(inverse[index]) == DoubleBits(secondInverse[index]),
               "inverse tensor repeats bit for bit");
    }
}

static void TestMassPropertiesReject(void)
{
    double center[3] = {7.0, 7.0, 7.0};
    double half[3] = {7.0, 7.0, 7.0};
    double inverse[9] = {7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0, 7.0};
    VoxelRigidCompoundBox valid[2];
    valid[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    valid[1] = MakeBox(2.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    // Every argument failure must leave the outputs untouched.
#define REJECT_MASS(boxes_, count_, mass_, message_)                                              \
    do                                                                                            \
    {                                                                                             \
        Expect(!VoxelRigidCompoundMassProperties((boxes_), (count_), (mass_), center, half,       \
                                                 inverse),                                        \
               (message_));                                                                      \
        ExpectVectorUnchanged(center, 7.0, "reject keeps center");                               \
        ExpectVectorUnchanged(half, 7.0, "reject keeps envelope");                               \
        for (uint32_t rejectIndex = 0u; rejectIndex < 9u; ++rejectIndex)                          \
        {                                                                                         \
            Expect(DoubleBits(inverse[rejectIndex]) == DoubleBits(7.0), "reject keeps inverse"); \
        }                                                                                         \
    } while (0)

    REJECT_MASS(NULL, 2u, 1.0, "null boxes rejected");
    REJECT_MASS(valid, 0u, 1.0, "zero box count rejected");
    REJECT_MASS(valid, 2u, 0.0, "zero mass rejected");
    REJECT_MASS(valid, 2u, -1.0, "negative mass rejected");
    REJECT_MASS(valid, 2u, NaNValue(), "NaN mass rejected");
    REJECT_MASS(valid, 2u, InfValue(), "infinite mass rejected");

    VoxelRigidCompoundBox bad[2];
    bad[0] = MakeBox(0.0, 0.0, 0.0, 0.0, 0.5, 0.5);
    bad[1] = MakeBox(2.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "zero half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, -0.5, 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "negative half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, NaNValue(), 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "NaN half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, InfValue(), 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "infinite half extent rejected");
    bad[0] = MakeBox(NaNValue(), 0.0, 0.0, 0.5, 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "NaN child center rejected");
    // Strictly overlapping boxes double volume and inertia, so they are refused.
    bad[0] = MakeBox(0.0, 0.0, 0.0, 0.6, 0.5, 0.5);
    bad[1] = MakeBox(0.5, 0.0, 0.0, 0.6, 0.5, 0.5);
    REJECT_MASS(bad, 2u, 1.0, "overlapping boxes rejected");
    // Degenerate union: zero volume is not a valid body.
    bad[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    bad[1] = MakeBox(2.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    Expect(!VoxelRigidCompoundMassProperties(valid, 2u, 1.0, NULL, half, inverse),
           "null center output rejected");
    Expect(!VoxelRigidCompoundMassProperties(valid, 2u, 1.0, center, NULL, inverse),
           "null envelope output rejected");
    Expect(!VoxelRigidCompoundMassProperties(valid, 2u, 1.0, center, half, NULL),
           "null inverse output rejected");
#undef REJECT_MASS
}

// MassProperties must normalize the host FP environment before any arithmetic.
// The hostile mode is installed BEFORE the call (not after Initialize), so a
// missing configure would change rounding and fail the bit comparison.
static void TestMassPropertiesHostileFp(void)
{
    VoxelRigidCompoundBox boxes[4];
    boxes[0] = MakeBox(0.0, 0.0, 0.0, 0.7, 0.4, 0.5);
    boxes[1] = MakeBox(2.0, 0.3, 0.2, 0.5, 0.6, 0.4);
    boxes[2] = MakeBox(-2.0, 1.0, 0.5, 0.4, 0.5, 0.7);
    boxes[3] = MakeBox(0.0, 3.0, 0.9, 0.3, 0.3, 0.3);

    double normalCenter[3];
    double normalHalf[3];
    double normalInverse[9];
    Expect(VoxelRigidCompoundMassProperties(boxes, 4u, 3.5, normalCenter, normalHalf,
                                            normalInverse),
           "normal FP mass properties resolved");

    LaiueTestSetHostileFpEnvironment();
    double hostileCenter[3];
    double hostileHalf[3];
    double hostileInverse[9];
    Expect(VoxelRigidCompoundMassProperties(boxes, 4u, 3.5, hostileCenter, hostileHalf,
                                            hostileInverse),
           "hostile FP mass properties resolved");
    Expect(VoxelPhysicsThreadIsConfigured(), "MassProperties normalizes FP before arithmetic");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        Expect(DoubleBits(normalCenter[axis]) == DoubleBits(hostileCenter[axis]) &&
                   DoubleBits(normalHalf[axis]) == DoubleBits(hostileHalf[axis]),
               "MassProperties is host-FP independent");
    }
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        Expect(DoubleBits(normalInverse[index]) == DoubleBits(hostileInverse[index]),
               "MassProperties inverse is host-FP independent");
    }
}

// === Путь одиночного ребёнка и legacy fast path ===

static void BuildLegacyScene(CompoundScene *scene, bool singleChild)
{
    ReleaseScene(scene);
    scene->count = 4u;
    const double masses[4] = {1.0, 2.0, 0.5, 1.5};
    for (uint32_t slot = 0u; slot < scene->count; ++slot)
    {
        double x = (double)slot * 1.3 - 1.5;
        double z = 2.0 + (double)slot * 1.1;
        if (singleChild)
        {
            VoxelRigidCompoundBox box = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
            const double offset[3] = {x, 0.0, z};
            InitCompoundBody(scene, slot, &box, 1u, masses[slot], offset, (uint64_t)slot + 1u, 0.1,
                             0.5);
        }
        else
        {
            InitPlainBody(scene, slot, 0.5, x, 0.0, z, masses[slot], (uint64_t)slot + 1u, 0.1, 0.5);
        }
        const double spin[3] = {(double)slot * 0.1, 0.03, -0.02};
        Expect(VoxelRigidBodyAddAngularVelocity(&scene->bodies[slot], spin), "legacy spin applied");
    }
}

static void TestBoxCountZeroMatchesLegacy(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(NULL, NULL);

    BuildLegacyScene(&sceneA, false);
    BuildLegacyScene(&sceneB, false);
    for (uint32_t tick = 0u; tick < LEGACY_TICKS; ++tick)
    {
        VoxelCollisionSource collision = Collision();
        Expect(VoxelRigidBodyStep(sceneA.bodies, sceneA.count, &collision, &settings, stepScratch,
                                  (uint32_t)sizeof(stepScratch)),
               "legacy step succeeded");
        Expect(StepScene(&sceneB, &settings, &options), "boxCount zero compound step succeeded");
        for (uint32_t slot = 0u; slot < sceneA.count; ++slot)
        {
            Expect(SameBody(&sceneA.bodies[slot], &sceneB.bodies[slot]),
                   "boxCount zero keeps the legacy trajectory bit for bit");
        }
    }
    ReleaseScene(&sceneA);
    ReleaseScene(&sceneB);
}

static void TestSingleChildNearLegacy(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(NULL, NULL);

    BuildLegacyScene(&sceneA, false);
    BuildLegacyScene(&sceneB, true);
    for (uint32_t tick = 0u; tick < LEGACY_TICKS; ++tick)
    {
        VoxelCollisionSource collision = Collision();
        Expect(VoxelRigidBodyStep(sceneA.bodies, sceneA.count, &collision, &settings, stepScratch,
                                  (uint32_t)sizeof(stepScratch)),
               "single child legacy step succeeded");
        Expect(StepScene(&sceneB, &settings, &options), "single child compound step succeeded");
        for (uint32_t slot = 0u; slot < sceneA.count; ++slot)
        {
            Expect(NearBody(&sceneA.bodies[slot], &sceneB.bodies[slot], 1e-6),
                   "single child follows the legacy trajectory");
        }
    }
    ReleaseScene(&sceneA);
    ReleaseScene(&sceneB);
}

static void TestCompoundCacheMatchesLegacyCache(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions legacyOptions = Options(&contactCache, NULL);
    VoxelRigidStepOptions compoundOptions = Options(&secondContactCache, NULL);

    BuildLegacyScene(&sceneA, false);
    BuildLegacyScene(&sceneB, false);
    for (uint32_t tick = 0u; tick < LEGACY_TICKS; ++tick)
    {
        VoxelCollisionSource collision = Collision();
        Expect(VoxelRigidBodyStepCached(sceneA.bodies, sceneA.count, &collision, &settings,
                                        stepScratch, (uint32_t)sizeof(stepScratch), &contactCache),
               "legacy cached step succeeded");
        Expect(VoxelRigidBodyStepCompoundEx(sceneB.bodies, sceneB.count, &collision, &settings,
                                            stepScratch, (uint32_t)sizeof(stepScratch),
                                            &compoundOptions, sceneB.shapes),
               "compound cached step succeeded");
        Expect(contactCache.contactCount == secondContactCache.contactCount &&
                   contactCache.matchedContactCount == secondContactCache.matchedContactCount,
               "boxCount zero reuses exactly the same cached contacts");
        for (uint32_t slot = 0u; slot < sceneA.count; ++slot)
        {
            Expect(SameBody(&sceneA.bodies[slot], &sceneB.bodies[slot]),
                   "cached boxCount zero trajectory matches legacy bit for bit");
        }
    }
    Expect(contactCache.contactCount > 0u, "legacy cache fixture has contacts");
    ReleaseScene(&sceneA);
    ReleaseScene(&sceneB);
    (void)legacyOptions;
}

// === L-образная форма: пустота не должна стать enclosing box ===

// Три кубика в форме L: вертикальное плечо и горизонтальная нога. Между ними
// внутри общего AABB остаётся пустой квадрант, который не является телом.
static void TestLShapeGapVersusSolidLimb(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox limb[3];
    limb[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    limb[1] = MakeBox(0.0, 0.0, 1.0, 0.5, 0.5, 0.5);
    limb[2] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    // The probe sits inside the L envelope but outside every limb.
    for (uint32_t sample = 0u; sample < 2u; ++sample)
    {
        double probeX = sample == 0u ? 1.0 : 0.0;
        double probeZ = sample == 0u ? 1.0 : 1.0;
        ReleaseScene(&sceneWork);
        sceneWork.count = 2u;
        const double offset[3] = {0.0, 0.0, 0.0};
        InitCompoundBody(&sceneWork, 0u, limb, 3u, 3.0, offset, 1u, 0.0, 0.0);
        InitPlainBody(&sceneWork, 1u, 0.2, probeX, 0.0, probeZ, 1.0, 2u, 0.0, 0.0);

        VoxelCollisionSource collision = Collision();
        Expect(VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision,
                                            &settings, stepScratch,
                                            (uint32_t)sizeof(stepScratch), &options,
                                            sceneWork.shapes),
               "L-shape step succeeded");
        VoxelRigidStepStats stats;
        Expect(VoxelRigidBodyReadStepStats(stepScratch, sceneWork.count,
                                           (uint32_t)sizeof(stepScratch), &stats),
               "L-shape statistics readable");
        if (sample == 0u)
        {
            Expect(stats.candidatePairCount == 1u,
                   "gap probe is inside the broadphase envelope");
            Expect(stats.contactCount == 0u,
                   "empty L gap does not collide with the enclosing box");
        }
        else
        {
            Expect(stats.contactCount > 0u, "probe against a solid limb collides");
        }
    }
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

// The same concavity check on a nine-child L (five stacked cubes plus a
// four-cube foot): the larger empty quadrant must stay empty.
static void TestL9ShapeGapVersusSolidLimb(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox l9[9];
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        l9[index] = MakeBox(0.0, 0.0, (double)index, 0.5, 0.5, 0.5);
    }
    for (uint32_t index = 1u; index < 5u; ++index)
    {
        l9[4u + index] = MakeBox((double)index, 0.0, 0.0, 0.5, 0.5, 0.5);
    }

    for (uint32_t sample = 0u; sample < 2u; ++sample)
    {
        double probeX = sample == 0u ? 3.0 : 0.0;
        double probeZ = sample == 0u ? 3.0 : 4.0;
        ReleaseScene(&sceneWork);
        sceneWork.count = 2u;
        const double offset[3] = {0.0, 0.0, 0.0};
        InitCompoundBody(&sceneWork, 0u, l9, 9u, 9.0, offset, 1u, 0.0, 0.0);
        InitPlainBody(&sceneWork, 1u, 0.3, probeX, 0.0, probeZ, 1.0, 2u, 0.0, 0.0);

        Expect(StepScene(&sceneWork, &settings, &options), "L9 step succeeded");
        VoxelRigidStepStats stats;
        uint32_t scratchBytes =
            VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, ScenePrimitiveCount(&sceneWork));
        Expect(VoxelRigidBodyReadStepStats(stepScratch, sceneWork.count, scratchBytes, &stats),
               "L9 statistics readable");
        if (sample == 0u)
        {
            Expect(stats.candidatePairCount >= 1u, "L9 gap probe is inside the envelope");
            Expect(stats.contactCount == 0u, "L9 empty quadrant stays empty");
        }
        else
        {
            Expect(stats.contactCount > 0u, "L9 limb probe collides");
        }
    }
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

// === Пробуждение по настоящим детям ===
// WakeContactPair must use the actual child overlap, not the body envelope.
// A sleeping L must not wake for an awake box sitting inside its empty gap,
// but must wake for the same box touching a solid limb.
static void TestWakeChildrenAware(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox limb[3];
    limb[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    limb[1] = MakeBox(0.0, 0.0, 1.0, 0.5, 0.5, 0.5);
    limb[2] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    for (uint32_t sample = 0u; sample < 2u; ++sample)
    {
        double probeX = sample == 0u ? 1.0 : 0.0;
        ReleaseScene(&sceneWork);
        sceneWork.count = 2u;
        const double offset[3] = {0.0, 0.0, 0.0};
        InitCompoundBody(&sceneWork, 0u, limb, 3u, 3.0, offset, 1u, 0.0, 0.0);
        sceneWork.bodies[0].sleeping = true;
        InitPlainBody(&sceneWork, 1u, 0.2, probeX, 0.0, 1.0, 1.0, 2u, 0.0, 0.0);

        Expect(StepScene(&sceneWork, &settings, &options), "wake probe step succeeded");
        VoxelRigidStepStats stats;
        uint32_t scratchBytes =
            VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, ScenePrimitiveCount(&sceneWork));
        Expect(VoxelRigidBodyReadStepStats(stepScratch, sceneWork.count, scratchBytes, &stats),
               "wake probe statistics readable");
        if (sample == 0u)
        {
            Expect(stats.contactCount == 0u, "gap probe makes no contact");
            Expect(sceneWork.bodies[0].sleeping,
                   "sleeping L is not woken by an awake box inside its empty gap");
        }
        else
        {
            Expect(stats.contactCount > 0u, "limb probe makes a real contact");
            Expect(!sceneWork.bodies[0].sleeping,
                   "sleeping L wakes on a real child collision");
        }
    }
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

// === Взаимный импульс и вращение ===

static void TestMutualImpulse(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    settings.penetrationCorrection = 0.0;
    settings.sleepFrames = 0u;
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox bar[2];
    bar[0] = MakeBox(-0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    bar[1] = MakeBox(0.5, 0.0, 0.0, 0.5, 0.5, 0.5);

    ReleaseScene(&sceneWork);
    sceneWork.count = 2u;
    const double offset[3] = {0.0, 0.0, 0.0};
    InitCompoundBody(&sceneWork, 0u, bar, 2u, 2.0, offset, 1u, 0.0, 0.0);
    InitPlainBody(&sceneWork, 1u, 0.5, -1.49, 0.0, 0.0, 1.0, 2u, 0.0, 0.0);
    const double incoming[3] = {10.0, 0.0, 0.0};
    Expect(VoxelRigidBodyAddLinearVelocity(&sceneWork.bodies[1], incoming), "probe velocity applied");

    Expect(StepScene(&sceneWork, &settings, &options), "mutual impulse step succeeded");
    double compoundVelocity[3];
    double probeVelocity[3];
    Expect(VoxelRigidBodyLinearVelocity(&sceneWork.bodies[0], compoundVelocity) &&
               VoxelRigidBodyLinearVelocity(&sceneWork.bodies[1], probeVelocity),
           "mutual impulse velocities readable");
    double momentum = 2.0 * compoundVelocity[0] + 1.0 * probeVelocity[0];
    Expect(Near(momentum, 10.0, 1e-6), "cube and compound conserve linear momentum");
    Expect(compoundVelocity[0] > 0.0 && probeVelocity[0] < 10.0,
           "compound receives impulse and the cube slows");
    Expect(VoxelRigidBodyAngularSpeed(&sceneWork.bodies[0]) < 1e-4,
           "head-on symmetric impact does not invent rotation");
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

static void TestOffCenterImpactRotates(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    settings.penetrationCorrection = 0.0;
    settings.sleepFrames = 0u;
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox bar[3];
    bar[0] = MakeBox(-1.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    bar[1] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    bar[2] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    ReleaseScene(&sceneWork);
    sceneWork.count = 2u;
    const double offset[3] = {0.0, 0.0, 0.0};
    InitCompoundBody(&sceneWork, 0u, bar, 3u, 3.0, offset, 1u, 0.0, 0.0);
    // Cube dropped over the far-left child: the lever arm has an X component
    // perpendicular to the vertical impulse, so the bar must pick up spin.
    InitPlainBody(&sceneWork, 1u, 0.5, -1.0, 0.0, 0.99, 1.0, 2u, 0.0, 0.0);
    const double downward[3] = {0.0, 0.0, -10.0};
    Expect(VoxelRigidBodyAddLinearVelocity(&sceneWork.bodies[1], downward), "drop velocity applied");

    Expect(StepScene(&sceneWork, &settings, &options), "off-centre impact step succeeded");
    double angular[3];
    Expect(VoxelRigidBodyAngularVelocity(&sceneWork.bodies[0], angular),
           "off-centre angular velocity readable");
    Expect(Absolute(angular[1]) > 0.01,
           "off-centre impact converts linear impulse into rotation");
    double linear[3];
    Expect(VoxelRigidBodyLinearVelocity(&sceneWork.bodies[0], linear),
           "off-centre linear velocity readable");
    Expect(linear[0] < 1e-6 && linear[2] < 0.0,
           "off-centre impulse is mostly vertical and does not inject X momentum");
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

// === Гравитация и покой на полу ===

static void TestCompoundRestsOnFloor(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    floorEnabled = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    VoxelRigidStepOptions options = Options(NULL, NULL);

    VoxelRigidCompoundBox plate[2];
    plate[0] = MakeBox(-0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    plate[1] = MakeBox(0.5, 0.0, 0.0, 0.5, 0.5, 0.5);

    ReleaseScene(&sceneWork);
    sceneWork.count = 1u;
    const double offset[3] = {0.0, 0.0, 2.0};
    InitCompoundBody(&sceneWork, 0u, plate, 2u, 2.0, offset, 1u, 0.0, 0.6);
    for (uint32_t tick = 0u; tick < REPLAY_TICKS; ++tick)
    {
        Expect(StepScene(&sceneWork, &settings, &options), "compound resting step succeeded");
    }
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneWork.bodies[0], position),
           "resting compound position readable");
    Expect(position[2] > 0.4 && position[2] < 0.7, "flat compound rests on the floor");
    Expect(VoxelRigidBodyLinearSpeed(&sceneWork.bodies[0]) < 0.3, "resting compound is nearly still");
    Expect(VoxelRigidBodyAngularSpeed(&sceneWork.bodies[0]) < 0.3,
           "resting compound does not spin on a flat floor");
    ReleaseScene(&sceneWork);
}

// === Отказы до мутаций ===

static void TestStepRejectBeforeMutation(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(NULL, NULL);
    VoxelCollisionSource collision = Collision();
    floorEnabled = true;

    VoxelRigidCompoundBox box = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    ReleaseScene(&sceneWork);
    sceneWork.count = 1u;
    const double offset[3] = {0.0, 0.0, 1.0};
    InitCompoundBody(&sceneWork, 0u, &box, 1u, 1.0, offset, 1u, 0.0, 0.5);
    uint64_t before = HashBodies(sceneWork.bodies, sceneWork.count);

    rejectShapes[0] = sceneWork.shapes[0];
#define REJECT_STEP(message_)                                                                     \
    do                                                                                            \
    {                                                                                             \
        Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision,       \
                                             &settings, stepScratch,                               \
                                             (uint32_t)sizeof(stepScratch), &options, rejectShapes),\
               (message_));                                                                       \
        Expect(HashBodies(sceneWork.bodies, sceneWork.count) == before,                            \
               "shape validation failure leaves bodies unchanged");                               \
    } while (0)

    rejectShapes[0].boxCount = UINT32_MAX;
    REJECT_STEP("unrepresentable compound scratch rejected before child access");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].boxes = NULL;
    rejectShapes[0].boxCount = 1u;
    REJECT_STEP("null child array rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].boxes = sceneWork.children[0];
    rejectShapes[0].boxCount = 0u;
    REJECT_STEP("non-null child array with zero count rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].inverseInertia[0] = NaNValue();
    REJECT_STEP("NaN inverse inertia rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].inverseInertia[4] = InfValue();
    REJECT_STEP("infinite inverse inertia rejected");
    // Finite but non-SPD or asymmetric tensors are refused as well: the angular
    // response must never trust garbage storage.
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].inverseInertia[0] = -rejectShapes[0].inverseInertia[0];
    REJECT_STEP("negative tensor rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].inverseInertia[0] = -6.0;
    rejectShapes[0].inverseInertia[4] = -6.0;
    rejectShapes[0].inverseInertia[8] = -6.0;
    REJECT_STEP("negative definite tensor rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    for (uint32_t component = 0u; component < 9u; ++component)
    {
        rejectShapes[0].inverseInertia[component] = 0.0;
    }
    REJECT_STEP("singular tensor rejected");
    rejectShapes[0] = sceneWork.shapes[0];
    rejectShapes[0].inverseInertia[1] = rejectShapes[0].inverseInertia[3] + 1.0;
    REJECT_STEP("asymmetric tensor rejected");

    // The body envelope must actually cover every child; an underreported or
    // corrupt envelope would silently drop broadphase contacts.
    double savedEnvelope[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        savedEnvelope[axis] = sceneWork.bodies[0].halfExtent[axis];
    }
    sceneWork.bodies[0].halfExtent[0] = 0.1;
    rejectShapes[0] = sceneWork.shapes[0];
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, (uint32_t)sizeof(stepScratch), &options,
                                         rejectShapes),
           "underreported envelope rejected");
    sceneWork.bodies[0].halfExtent[0] = NaNValue();
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, (uint32_t)sizeof(stepScratch), &options,
                                         rejectShapes),
           "non-finite envelope rejected");
    sceneWork.bodies[0].halfExtent[0] = InfValue();
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, (uint32_t)sizeof(stepScratch), &options,
                                         rejectShapes),
           "positive-infinite envelope rejected");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        sceneWork.bodies[0].halfExtent[axis] = savedEnvelope[axis];
    }
    Expect(HashBodies(sceneWork.bodies, sceneWork.count) == before,
           "envelope validation leaves bodies unchanged");

    // A child with a degenerate or non-finite extent must be refused too.
    VoxelRigidCompoundBox badChildren[1];
    badChildren[0] = MakeBox(0.0, 0.0, 0.0, 0.0, 0.5, 0.5);
    VoxelRigidCompoundBox saved = sceneWork.children[0][0];
    sceneWork.children[0][0] = badChildren[0];
    rejectShapes[0] = sceneWork.shapes[0];
    REJECT_STEP("zero child extent rejected");
    sceneWork.children[0][0] = saved;
    sceneWork.children[0][0] = MakeBox(NaNValue(), 0.0, 0.0, 0.5, 0.5, 0.5);
    REJECT_STEP("NaN child center rejected");
    sceneWork.children[0][0] = MakeBox(0.0, 0.0, 0.0, InfValue(), 0.5, 0.5);
    REJECT_STEP("infinite child extent rejected");
    sceneWork.children[0][0] = saved;
#undef REJECT_STEP

    // Sanity: the untouched fixture still steps successfully.
    rejectShapes[0] = sceneWork.shapes[0];
    Expect(StepScene(&sceneWork, &settings, &options), "valid compound still steps after rejects");
    ReleaseScene(&sceneWork);
}

// === Бюджет контактов и масштабируемый scratch ===

static void BuildFlatPlateScene(CompoundScene *scene, uint32_t blocks)
{
    ReleaseScene(scene);
    scene->count = 1u;
    for (uint32_t index = 0u; index < blocks; ++index)
    {
        temporaryBoxes[index] =
            MakeBox(((double)index - (double)(blocks - 1u) * 0.5) * 1.2, 0.0, 0.0, 0.5, 0.5, 0.5);
    }
    const double offset[3] = {0.0, 0.0, 0.49};
    InitCompoundBody(scene, 0u, temporaryBoxes, blocks, (double)blocks, offset, 1u, 0.0, 0.6);
}

// A flat 16-block plate must rest with more than 16 supporting contacts. The
// scaled primitive budget is what makes this legal; the old one-body budget
// could only reach 16 contacts and would have refused the whole step.
static void TestFlatPlate16RestsAndReplays(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    floorEnabled = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }

    BuildFlatPlateScene(&sceneWork, FLAT_PLATE_BLOCKS);
    VoxelRigidStepOptions options = Options(NULL, NULL);
    Expect(StepScene(&sceneWork, &settings, &options), "flat 16-block plate step succeeded");
    VoxelRigidStepStats stats;
    uint32_t legacyBytes = VoxelRigidBodyStepScratchBytes(sceneWork.count);
    Expect(VoxelRigidBodyReadStepStats(stepScratch, sceneWork.count, legacyBytes, &stats),
           "legacy ReadStepStats works after a compound step");
    Expect(stats.contactCount > FLAT_PLATE_BLOCKS,
           "flat plate rests with more than 16 supporting contacts");

    // Successful multi-tick replay with a cache sized for primitives.
    VoxelRigidContactCacheReset(&contactCache);
    VoxelRigidStepOptions cached = Options(&contactCache, NULL);
    for (uint32_t tick = 0u; tick < 256u; ++tick)
    {
        Expect(StepScene(&sceneWork, &settings, &cached), "flat plate replay step succeeded");
    }
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneWork.bodies[0], position),
           "flat plate position readable");
    Expect(position[2] > 0.4 && position[2] < 0.7, "flat plate stays on the floor");
    double linear[3];
    Expect(VoxelRigidBodyLinearVelocity(&sceneWork.bodies[0], linear),
           "flat plate velocity readable");
    Expect(Absolute(linear[2]) < 0.5, "flat plate stopped sinking");
    ReleaseScene(&sceneWork);
}

// The scaled sizing is a hard requirement: the legacy scratch size must fail
// before any mutation, and only the new sizing may pass.
static void TestOldScratchSizingRejectedBeforeMutation(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    floorEnabled = true;
    VoxelRigidStepOptions options = Options(NULL, NULL);
    VoxelCollisionSource collision = Collision();

    BuildFlatPlateScene(&sceneWork, FLAT_PLATE_BLOCKS);
    uint32_t primitiveCount = ScenePrimitiveCount(&sceneWork);
    uint32_t legacyBytes = VoxelRigidBodyStepScratchBytes(sceneWork.count);
    uint32_t compoundBytes = VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, primitiveCount);
    Expect(legacyBytes != 0u && compoundBytes != 0u, "both sizing functions resolve");
    Expect(legacyBytes < compoundBytes,
           "compound shape needs extra scratch beyond the legacy box size");
    uint64_t before = HashBodies(sceneWork.bodies, sceneWork.count);

    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, legacyBytes, &options, sceneWork.shapes),
           "legacy-sized scratch rejected");
    Expect(HashBodies(sceneWork.bodies, sceneWork.count) == before,
           "undersized scratch leaves bodies unchanged before any mutation");
    Expect(StepScene(&sceneWork, &settings, &options),
           "new primitive sizing accepts the same compound scene");
    ReleaseScene(&sceneWork);
}

// A cache whose bodyCapacity cannot hold the primitive contact load must make
// the step fail explicitly instead of silently truncating cached contacts.
static void TestCompoundCacheCapacityExplicitFalse(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    floorEnabled = true;
    BuildFlatPlateScene(&sceneWork, FLAT_PLATE_BLOCKS);
    uint32_t scratchBytes =
        VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, ScenePrimitiveCount(&sceneWork));
    VoxelCollisionSource collision = Collision();

    VoxelRigidContactCacheReset(&smallContactCache);
    VoxelRigidStepOptions small = Options(&smallContactCache, NULL);
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, scratchBytes, &small, sceneWork.shapes),
           "cache bodyCapacity below the contact load is rejected");

    VoxelRigidContactCacheReset(&contactCache);
    VoxelRigidStepOptions adequate = Options(&contactCache, NULL);
    Expect(VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                        stepScratch, scratchBytes, &adequate, sceneWork.shapes),
           "cache bodyCapacity covering primitiveCount succeeds");
    ReleaseScene(&sceneWork);
}

// A genuinely over-dense scene keeps every contact or refuses explicitly; it
// must never return success with a truncated set. Overlapping plain boxes go
// through the compound entry point with boxCount == 0 shapes.
static void TestTrulyDenseOverflowRefused(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    settings.penetrationCorrection = 0.0;
    settings.sleepFrames = 0u;
    floorEnabled = false;
    VoxelRigidStepOptions options = Options(NULL, NULL);
    VoxelCollisionSource collision = Collision();

    const uint32_t denseCount = 12u;
    ReleaseScene(&sceneWork);
    sceneWork.count = denseCount;
    for (uint32_t slot = 0u; slot < denseCount; ++slot)
    {
        InitPlainBody(&sceneWork, slot, 0.5, 0.0, 0.0, 0.0, 1.0, (uint64_t)slot + 1u, 0.0, 0.0);
    }
    uint32_t scratchBytes =
        VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, ScenePrimitiveCount(&sceneWork));
    Expect(scratchBytes != 0u && scratchBytes <= sizeof(stepScratch), "dense scratch fits");
    bool ok = VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                           stepScratch, scratchBytes, &options, sceneWork.shapes);
    Expect(!ok, "truly over-dense contact overflow is an explicit false");
    ReleaseScene(&sceneWork);
    floorEnabled = true;
}

// === Крупная повёрнутая составная форма: мировые контакты ===

static uint32_t LargeRodScratchBytes(void)
{
    return VoxelRigidBodyStepCompoundScratchBytes(1u, LARGE_ROD_CHILDREN);
}

// Rotated 45 degrees about Z, the 256x1x1 rod's world AABB spans far more than
// VOXEL_RIGID_MAX_WORLD_CELLS cells. The legacy sampler skipped such a body
// entirely, so it fell through the floor.
static double LargeRodEnvelopeCells(void)
{
    const double c = 0.7071067811865476;
    const double halfX = largeRodBodies[0].halfExtent[0];
    const double halfY = largeRodBodies[0].halfExtent[1];
    const double halfZ = largeRodBodies[0].halfExtent[2];
    double spanX = 2.0 * (c * halfX + c * halfY) + 1.0;
    double spanY = 2.0 * (c * halfX + c * halfY) + 1.0;
    double spanZ = 2.0 * halfZ + 1.0;
    return spanX * spanY * spanZ;
}

static void BuildLargeRotatedRod(void)
{
    VoxelRigidBodyRelease(&largeRodBodies[0]);
    for (uint32_t index = 0u; index < LARGE_ROD_CHILDREN; ++index)
    {
        largeRodChildren[index] =
            MakeBox((double)index - (double)(LARGE_ROD_CHILDREN - 1u) * 0.5, 0.0, 0.0, 0.5, 0.5,
                    0.5);
    }
    double center[3];
    double envelope[3];
    double inverse[9];
    Expect(VoxelRigidCompoundMassProperties(largeRodChildren, LARGE_ROD_CHILDREN,
                                            (double)LARGE_ROD_CHILDREN, center, envelope, inverse),
           "large rod mass properties resolved");
    for (uint32_t index = 0u; index < LARGE_ROD_CHILDREN; ++index)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            largeRodChildren[index].center[axis] -= center[axis];
        }
    }
    VoxelRigidBodyDescription description = {0};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        description.halfExtent[axis] = envelope[axis];
        description.position[axis] = center[axis];
    }
    // Start slightly sunk into the floor so the very first step must produce a
    // real world contact instead of a free fall.
    description.position[2] += 0.49;
    description.mass = (double)LARGE_ROD_CHILDREN;
    description.restitution = 0.0;
    description.friction = 0.6;
    Expect(VoxelRigidBodyInitialize(&largeRodBodies[0], 1u, &description),
           "large rod initialized");
    const double quaternion[4] = {0.0, 0.0, 0.3826834323650898, 0.9238795325112867};
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        largeRodBodies[0].orientation[component] = quaternion[component];
    }
    largeRodShapes[0].boxes = largeRodChildren;
    largeRodShapes[0].boxCount = LARGE_ROD_CHILDREN;
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        largeRodShapes[0].inverseInertia[index] = inverse[index];
    }
}

static bool StepLargeRod(const VoxelRigidStepSettings *settings,
                         const VoxelRigidStepOptions *options)
{
    VoxelCollisionSource collision = Collision();
    uint32_t required = LargeRodScratchBytes();
    Expect(required != 0u && required <= sizeof(largeRodScratch),
           "large rod scratch fits static storage");
    return VoxelRigidBodyStepCompoundEx(largeRodBodies, 1u, &collision, settings, largeRodScratch,
                                        required, options, largeRodShapes);
}

static uint64_t RunLargeRod(uint32_t ticks, bool *outOk)
{
    VoxelRigidContactCacheReset(&largeRodCache);
    BuildLargeRotatedRod();
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(&largeRodCache, NULL);
    uint64_t hash = UINT64_C(14695981039346656037);
    *outOk = false;
    for (uint32_t tick = 0u; tick < ticks; ++tick)
    {
        if (!StepLargeRod(&settings, &options))
        {
            return hash;
        }
        hash = HashBody(hash, &largeRodBodies[0]);
    }
    *outOk = true;
    return hash;
}

static void TestLargeRotatedRodWorldContacts(void)
{
    floorEnabled = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }

    BuildLargeRotatedRod();
    Expect(LargeRodEnvelopeCells() > (double)VOXEL_RIGID_MAX_WORLD_CELLS,
           "large rotated rod envelope exceeds the legacy world cell limit");
    uint32_t required = LargeRodScratchBytes();
    Expect(required > VoxelRigidBodyStepScratchBytes(1u),
           "large rod uses the scaled compound budget");
    Expect(required <= sizeof(largeRodScratch), "large rod scratch is adequately sized");

    VoxelRigidContactCacheReset(&largeRodCache);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(&largeRodCache, NULL);
    Expect(StepLargeRod(&settings, &options), "large rotated rod first step succeeded");
    VoxelRigidStepStats stats;
    Expect(VoxelRigidBodyReadStepStats(largeRodScratch, 1u, required, &stats),
           "large rod statistics readable");
    Expect(stats.contactCount > 0u, "large rotated compound has world contacts");
    Expect(stats.contactCount <= LARGE_ROD_CHILDREN * VOXEL_RIGID_CONTACTS_PER_BODY,
           "large rod contacts stay inside the primitive budget");

    bool firstOk = false;
    uint64_t first = RunLargeRod(LARGE_ROD_TICKS, &firstOk);
    Expect(firstOk, "large rod replay completed");
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&largeRodBodies[0], position),
           "large rod position readable");
    Expect(Near(position[2], 0.5, 0.05), "large rotated rod rests on the floor");
    Expect(VoxelRigidBodyLinearSpeed(&largeRodBodies[0]) < 0.2, "large rod settles");
    Expect(VoxelRigidBodyAngularSpeed(&largeRodBodies[0]) < 0.5, "large rod stops spinning");

    bool secondOk = false;
    uint64_t second = RunLargeRod(LARGE_ROD_TICKS, &secondOk);
    Expect(secondOk && first == second, "large rod replay repeats exactly");
    VoxelRigidBodyRelease(&largeRodBodies[0]);
}

// === Точный replay 512 тиков ===

static void BuildReplayScene(CompoundScene *scene)
{
    ReleaseScene(scene);
    scene->count = 7u;
    VoxelRigidCompoundBox boxes[1];
    const double offsetSingle[3] = {-3.5, 0.0, 1.6};
    const double offsetPlate[3] = {0.0, 0.0, 2.0};
    const double offsetElbow[3] = {3.5, 0.0, 2.4};
    const double offsetTower[3] = {7.0, 0.0, 2.2};
    const double offsetFlat[3] = {0.0, 9.0, 1.2};

    InitPlainBody(scene, 0u, 0.5, -7.0, 0.0, 1.6, 1.0, 1u, 0.1, 0.5);
    boxes[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    InitCompoundBody(scene, 1u, boxes, 1u, 1.0, offsetSingle, 2u, 0.1, 0.5);
    VoxelRigidCompoundBox plate[2];
    plate[0] = MakeBox(-0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    plate[1] = MakeBox(0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    InitCompoundBody(scene, 2u, plate, 2u, 2.0, offsetPlate, 3u, 0.1, 0.5);
    // L9: five cubes stacked along Z plus a four-cube foot along X.
    VoxelRigidCompoundBox elbow[9];
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        elbow[index] = MakeBox(0.0, 0.0, (double)index, 0.5, 0.5, 0.5);
    }
    for (uint32_t index = 1u; index < 5u; ++index)
    {
        elbow[4u + index] = MakeBox((double)index, 0.0, 0.0, 0.5, 0.5, 0.5);
    }
    InitCompoundBody(scene, 3u, elbow, 9u, 9.0, offsetElbow, 4u, 0.1, 0.5);
    VoxelRigidCompoundBox tower[2];
    tower[0] = MakeBox(0.0, 0.0, -0.5, 0.5, 0.5, 0.5);
    tower[1] = MakeBox(0.0, 0.0, 0.5, 0.5, 0.5, 0.5);
    InitCompoundBody(scene, 4u, tower, 2u, 2.0, offsetTower, 5u, 0.1, 0.5);
    InitPlainBody(scene, 5u, 0.5, 10.5, 0.0, 3.0, 1.0, 6u, 0.1, 0.5);
    VoxelRigidCompoundBox flat[FLAT_PLATE_BLOCKS];
    for (uint32_t index = 0u; index < FLAT_PLATE_BLOCKS; ++index)
    {
        flat[index] = MakeBox(((double)index - (double)(FLAT_PLATE_BLOCKS - 1u) * 0.5) * 1.2, 0.0,
                              0.0, 0.5, 0.5, 0.5);
    }
    InitCompoundBody(scene, 6u, flat, FLAT_PLATE_BLOCKS, (double)FLAT_PLATE_BLOCKS, offsetFlat, 7u,
                     0.1, 0.5);

    for (uint32_t slot = 0u; slot < scene->count; ++slot)
    {
        const double spin[3] = {(double)(slot % 3u) * 0.1, 0.02, -0.01};
        Expect(VoxelRigidBodyAddAngularVelocity(&scene->bodies[slot], spin), "replay spin applied");
    }
}

static void WriteCount(const char *label, uint32_t value)
{
    char text[11] = {0};
    uint32_t cursor = 10u;
    do
    {
        text[--cursor] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(text + cursor);
    LaiueTestRuntimeWrite("\n");
}

static uint64_t ReplayCompound(void)
{
    VoxelRigidContactCacheReset(&contactCache);
    BuildReplayScene(&sceneWork);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(&contactCache, NULL);
    uint64_t hash = UINT64_C(14695981039346656037);
    uint64_t matchedTotal = 0u;
    for (uint32_t tick = 0u; tick < REPLAY_TICKS; ++tick)
    {
        if (tick == 200u)
        {
            const int64_t shift[3] = {64, -32, 0};
            for (uint32_t slot = 0u; slot < sceneWork.count; ++slot)
            {
                Expect(VoxelRigidBodyTranslateBlocks(&sceneWork.bodies[slot], shift),
                       "replay rebase succeeded");
            }
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                worldOrigin[axis] += shift[axis];
            }
        }
        if (tick == 300u)
        {
            const double impact[3] = {3.0, 0.0, 1.0};
            Expect(VoxelRigidBodyAddLinearVelocity(&sceneWork.bodies[3], impact),
                   "replay impulse applied");
        }
        Expect(StepScene(&sceneWork, &settings, &options), "compound replay step succeeded");
        matchedTotal += contactCache.matchedContactCount;
        for (uint32_t slot = 0u; slot < sceneWork.count; ++slot)
        {
            hash = HashBody(hash, &sceneWork.bodies[slot]);
        }
    }
    Expect(contactCache.contactCount > 0u && matchedTotal > 0u,
           "compound replay exercises warm starting");
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneWork.bodies[2], position),
           "replay body position readable");
    Expect(position[2] > 0.4, "replay plate did not fall through the floor");
    ReleaseScene(&sceneWork);
    return hash;
}

static void TestCompoundReplayDeterminism(void)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    floorEnabled = true;
    uint64_t first = ReplayCompound();
    uint64_t second = ReplayCompound();
    Expect(first == second, "compound replay with cache repeats bit for bit");
    uint64_t third = ReplayCompound();
    Expect(first == third, "compound replay repeats across a third fresh run");
}

// === Executor: serial, split and worker pool ===

#define EXECUTOR_BODY_COUNT 24u

static LaiueTaskExecutor splitExecutor;
static LaiueTaskExecutor poolExecutor;
static uint32_t splitDispatches;
static uint32_t poolDispatches;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void SplitRun(void *context, uint32_t count, uint32_t grain, LaiueTaskRangeFunction function,
                     void *jobContext)
{
    (void)context;
    ++splitDispatches;
    uint32_t rangeSize = grain == 0u ? 1u : grain;
    if (rangeSize > 3u)
    {
        rangeSize = 3u;
    }
    for (uint32_t begin = 0u; begin < count;)
    {
        uint32_t end = count - begin < rangeSize ? count : begin + rangeSize;
        LaiueTestSetHostileFpEnvironment();
        function(jobContext, begin, end);
        Expect(VoxelPhysicsThreadIsConfigured(), "split range restores FP environment");
        begin = end;
    }
}

// The pool executor is wrapped so the test can prove the physics step actually
// dispatched real parallel ranges instead of silently staying serial.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void PoolRun(void *context, uint32_t count, uint32_t grain, LaiueTaskRangeFunction function,
                    void *jobContext)
{
    (void)context;
    ++poolDispatches;
    poolExecutor.run(poolExecutor.context, count, grain, function, jobContext);
}

// A grid of resting legacy cubes and compound plates produces far more than 64
// contacts, which is the threshold where the canonical step hands contact
// preparation and cache storage to the executor.
static void BuildExecutorScene(CompoundScene *scene)
{
    ReleaseScene(scene);
    scene->count = EXECUTOR_BODY_COUNT;
    for (uint32_t slot = 0u; slot < EXECUTOR_BODY_COUNT; ++slot)
    {
        double x = (double)(slot % 6u) * 2.5 - 6.0;
        double y = (double)(slot / 6u) * 2.5 - 3.5;
        if ((slot & 1u) == 0u)
        {
            InitPlainBody(scene, slot, 0.5, x, y, 0.49, 1.0, (uint64_t)slot + 1u, 0.0, 0.6);
        }
        else
        {
            VoxelRigidCompoundBox plate[2];
            plate[0] = MakeBox(-0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
            plate[1] = MakeBox(0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
            const double offset[3] = {x, y, 0.49};
            InitCompoundBody(scene, slot, plate, 2u, 2.0, offset, (uint64_t)slot + 1u, 0.0, 0.6);
        }
    }
}

static uint64_t RunExecutorHash(const LaiueTaskExecutor *executor, const char *label)
{
    VoxelRigidContactCacheReset(&contactCache);
    BuildExecutorScene(&sceneWork);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepFrames = 0u;
    VoxelRigidStepOptions options = Options(&contactCache, executor);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < EXECUTOR_TICKS; ++tick)
    {
        if (!StepScene(&sceneWork, &settings, &options))
        {
            LaiueTestRuntimeWrite("executor failure: ");
            LaiueTestRuntimeWrite(label);
            WriteCount(" tick: ", tick);
            Expect(false, "executor compound step succeeded");
        }
        for (uint32_t slot = 0u; slot < sceneWork.count; ++slot)
        {
            hash = HashBody(hash, &sceneWork.bodies[slot]);
        }
    }
    Expect(contactCache.contactCount > 64u,
           "executor scene crosses the parallel contact threshold");
    ReleaseScene(&sceneWork);
    return hash;
}

static void TestExecutorEquivalence(void)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    floorEnabled = true;
    LaiueTaskPool *pool = LaiueTaskPoolCreate(4u);
    Expect(pool != NULL, "four-participant task pool created");
    memset(&poolExecutor, 0, sizeof(poolExecutor));
    poolExecutor.structSize = sizeof(poolExecutor);
    Expect(LaiueTaskPoolGetExecutor(pool, &poolExecutor), "pool executor obtained");
    memset(&splitExecutor, 0, sizeof(splitExecutor));
    splitExecutor.structSize = sizeof(splitExecutor);
    splitExecutor.context = &splitExecutor;
    splitExecutor.run = SplitRun;
    LaiueTaskExecutor poolWrapper = {0};
    poolWrapper.structSize = sizeof(poolWrapper);
    poolWrapper.context = &poolExecutor;
    poolWrapper.run = PoolRun;

    splitDispatches = 0u;
    poolDispatches = 0u;
    uint64_t serial = RunExecutorHash(NULL, "serial");
    uint64_t split = RunExecutorHash(&splitExecutor, "split");
    uint64_t workers = RunExecutorHash(&poolWrapper, "workers");
    Expect(serial == split, "compound replay independent of serial range split");
    Expect(serial == workers, "compound replay independent of worker count");
    Expect(splitDispatches > 0u && poolDispatches > 0u,
           "both executors actually dispatched physics ranges");
    WriteCount("compound executor split dispatches: ", splitDispatches);
    WriteCount("compound executor pool dispatches: ", poolDispatches);

    LaiueTaskPoolDestroy(pool);
}

// === Rebasing и огромные абсолютные координаты ===

static void BuildRebaseScene(CompoundScene *scene, double z)
{
    ReleaseScene(scene);
    scene->count = 1u;
    VoxelRigidCompoundBox plate[2];
    plate[0] = MakeBox(-0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    plate[1] = MakeBox(0.5, 0.0, 0.0, 0.5, 0.5, 0.5);
    const double offset[3] = {0.0, 0.0, z};
    InitCompoundBody(scene, 0u, plate, 2u, 2.0, offset, 1u, 0.0, 0.6);
}

static void TestRebaseAndHugeCoordinates(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    floorEnabled = true;
    VoxelRigidStepOptions options = Options(NULL, NULL);

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    BuildRebaseScene(&sceneA, 2.0);
    for (uint32_t tick = 0u; tick < 256u; ++tick)
    {
        Expect(StepScene(&sceneA, &settings, &options), "control rebase step succeeded");
    }
    double controlPosition[3];
    double controlLinear[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneA.bodies[0], controlPosition) &&
               VoxelRigidBodyLinearVelocity(&sceneA.bodies[0], controlLinear),
           "control rebase state readable");

    BuildRebaseScene(&sceneB, 2.0);
    const int64_t shift[3] = {INT64_C(1) << 28, -(INT64_C(1) << 28), 0};
    Expect(VoxelRigidBodyTranslateBlocks(&sceneB.bodies[0], shift), "huge rebase applied");
    for (uint32_t tick = 0u; tick < 256u; ++tick)
    {
        Expect(StepScene(&sceneB, &settings, &options), "rebased step succeeded");
    }
    const int64_t unshift[3] = {-(INT64_C(1) << 28), INT64_C(1) << 28, 0};
    Expect(VoxelRigidBodyTranslateBlocks(&sceneB.bodies[0], unshift), "huge rebase undone");
    double rebasedPosition[3];
    double rebasedLinear[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneB.bodies[0], rebasedPosition) &&
               VoxelRigidBodyLinearVelocity(&sceneB.bodies[0], rebasedLinear),
           "rebased state readable");
    ExpectVectorNear(rebasedPosition, controlPosition, 1e-3, "rebase preserves trajectory");
    ExpectVectorNear(rebasedLinear, controlLinear, 1e-3, "rebase preserves velocity");
    ReleaseScene(&sceneA);
    ReleaseScene(&sceneB);

    // 2^32 blocks still goes through the arbitrary-precision coordinates and
    // must remain finite and steppable after a rebase back.
    BuildRebaseScene(&sceneWork, 2.0);
    const int64_t farShift[3] = {INT64_C(4294967296), 0, 0};
    const int64_t backShift[3] = {INT64_C(-4294967296), 0, 0};
    Expect(VoxelRigidBodyTranslateBlocks(&sceneWork.bodies[0], farShift), "2^32 rebase applied");
    for (uint32_t tick = 0u; tick < 16u; ++tick)
    {
        Expect(StepScene(&sceneWork, &settings, &options), "2^32 rebased step succeeded");
    }
    Expect(VoxelRigidBodyTranslateBlocks(&sceneWork.bodies[0], backShift), "2^32 rebase undone");
    double farPosition[3];
    Expect(VoxelRigidBodyLocalPosition(&sceneWork.bodies[0], farPosition),
           "2^32 position readable");
    Expect(Near(farPosition[0], 0.0, 1e-3) && farPosition[2] > 0.4,
           "2^32 rebase round-trips exactly enough to stay on the floor");
    ReleaseScene(&sceneWork);
}

// === FP-окружение ===

static void TestHostileFpReplay(void)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    floorEnabled = true;
    perturbFp = false;
    VoxelRigidContactCacheReset(&contactCache);
    BuildReplayScene(&sceneWork);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(&contactCache, NULL);
    uint64_t normalHash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < 128u; ++tick)
    {
        Expect(StepScene(&sceneWork, &settings, &options), "normal FP step succeeded");
        for (uint32_t slot = 0u; slot < sceneWork.count; ++slot)
        {
            normalHash = HashBody(normalHash, &sceneWork.bodies[slot]);
        }
    }
    ReleaseScene(&sceneWork);
    perturbFp = true;
    VoxelRigidContactCacheReset(&contactCache);
    BuildReplayScene(&sceneWork);
    uint64_t hostileHash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < 128u; ++tick)
    {
        Expect(StepScene(&sceneWork, &settings, &options), "hostile FP step succeeded");
        for (uint32_t slot = 0u; slot < sceneWork.count; ++slot)
        {
            hostileHash = HashBody(hostileHash, &sceneWork.bodies[slot]);
        }
    }
    Expect(normalHash == hostileHash, "hostile FP modes do not change compound replay");
    ReleaseScene(&sceneWork);
    perturbFp = false;
}

// === Scratch ===

static void TestCompoundScratchSizingContract(void)
{
    Expect(VoxelRigidBodyStepScratchBytes(1u) > 0u &&
               VoxelRigidBodyStepScratchBytes(TEST_BODY_COUNT) <= sizeof(stepScratch),
           "legacy scratch sizing still resolves");
    // A scene made only of plain boxes must keep the legacy size exactly.
    const uint32_t plainCounts[] = {1u, 2u, 7u, TEST_BODY_COUNT};
    for (uint32_t sample = 0u; sample < sizeof(plainCounts) / sizeof(plainCounts[0]); ++sample)
    {
        uint32_t count = plainCounts[sample];
        Expect(VoxelRigidBodyStepCompoundScratchBytes(count, count) ==
                   VoxelRigidBodyStepScratchBytes(count),
               "count0 scene keeps the legacy scratch size");
    }
    // More primitives may only grow the buffer.
    Expect(VoxelRigidBodyStepCompoundScratchBytes(2u, 9u) >=
               VoxelRigidBodyStepCompoundScratchBytes(2u, 2u),
           "compound scratch grows with primitive count");
    // Invalid or overflowing requests fail closed.
    Expect(VoxelRigidBodyStepCompoundScratchBytes(0u, 0u) == 0u, "zero body count rejected");
    Expect(VoxelRigidBodyStepCompoundScratchBytes(2u, 1u) == 0u,
           "primitive count below body count rejected");
    Expect(VoxelRigidBodyStepCompoundScratchBytes(UINT32_MAX, UINT32_MAX) == 0u,
           "scratch uint32 overflow rejected");
    Expect(VoxelRigidBodyStepCompoundScratchBytes(1u, UINT32_MAX) == 0u,
           "scratch count overflow rejected");

    // The replay scene needs the scaled size; its primitive count is explicit.
    BuildReplayScene(&sceneWork);
    uint32_t primitives = ScenePrimitiveCount(&sceneWork);
    uint32_t compoundBytes = VoxelRigidBodyStepCompoundScratchBytes(sceneWork.count, primitives);
    Expect(primitives == 32u, "replay scene primitive count is explicit");
    Expect(compoundBytes > VoxelRigidBodyStepScratchBytes(sceneWork.count),
           "compound scene exceeds the legacy box scratch");
    Expect(compoundBytes <= sizeof(stepScratch), "compound scratch fits the static buffer");

    // A compound step must not accept less than the reported requirement.
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelRigidStepOptions options = Options(NULL, NULL);
    VoxelCollisionSource collision = Collision();
    uint64_t before = HashBodies(sceneWork.bodies, sceneWork.count);
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, 0u, &options, sceneWork.shapes),
           "undersized compound scratch rejected");
    Expect(!VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision, &settings,
                                         stepScratch, compoundBytes - 1u, &options,
                                         sceneWork.shapes),
           "one byte short of the compound sizing rejected");
    Expect(HashBodies(sceneWork.bodies, sceneWork.count) == before,
           "undersized scratch leaves bodies unchanged");
    ReleaseScene(&sceneWork);
}

static void TestCompoundScratchAlignment(void)
{
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelCollisionSource collision = Collision();
    uint64_t reference = 0u;
    for (uint32_t offset = 0u; offset < 64u; ++offset)
    {
        BuildReplayScene(&sceneWork);
        uint32_t bytes = VoxelRigidBodyStepCompoundScratchBytes(
            sceneWork.count, ScenePrimitiveCount(&sceneWork));
        Expect((uint64_t)bytes + offset <= sizeof(stepScratch), "alignment fixture fits");
        Expect(VoxelRigidBodyStepCompoundEx(sceneWork.bodies, sceneWork.count, &collision,
                                            &settings, stepScratch + offset, bytes, NULL,
                                            sceneWork.shapes),
               "compound accepts every caller scratch alignment");
        uint64_t hash = HashBodies(sceneWork.bodies, sceneWork.count);
        if (offset == 0u)
            reference = hash;
        else
            Expect(hash == reference, "scratch alignment preserves compound trajectory");
        ReleaseScene(&sceneWork);
    }
}

static void WriteHash(const char *label, uint64_t hash)
{
    const char digits[] = "0123456789abcdef";
    char text[19] = {'0', 'x'};
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[2u + index] = digits[(hash >> (60u - index * 4u)) & 15u];
    }
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
}

LAIUE_TEST_ENTRY(RigidCompoundTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    InitializeCaches();
    TestMassPropertiesAnalytic();
    TestMassPropertiesReject();
    TestMassPropertiesHostileFp();
    TestCompoundScratchSizingContract();
    TestCompoundScratchAlignment();
    TestBoxCountZeroMatchesLegacy();
    TestSingleChildNearLegacy();
    TestCompoundCacheMatchesLegacyCache();
    TestLShapeGapVersusSolidLimb();
    TestL9ShapeGapVersusSolidLimb();
    TestWakeChildrenAware();
    TestMutualImpulse();
    TestOffCenterImpactRotates();
    TestCompoundRestsOnFloor();
    TestStepRejectBeforeMutation();
    TestFlatPlate16RestsAndReplays();
    TestOldScratchSizingRejectedBeforeMutation();
    TestCompoundCacheCapacityExplicitFalse();
    TestTrulyDenseOverflowRefused();
    TestLargeRotatedRodWorldContacts();
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        worldOrigin[axis] = 0;
    }
    TestCompoundReplayDeterminism();
    TestExecutorEquivalence();
    TestRebaseAndHugeCoordinates();
    TestHostileFpReplay();
    uint64_t replay = ReplayCompound();
    WriteHash("rigid-compound-replay-hash: ", replay);
    LAIUE_TEST_SUCCESS();
}
