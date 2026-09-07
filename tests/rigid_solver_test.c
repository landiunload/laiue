#include "physics/rigid_body.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#include <string.h>

static uint8_t solverScratch[262144];
static bool perturbFp;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Rigid solver failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void Floor(void *context, int64_t x, int64_t y, int64_t z,
                   VoxelBlockPhysics *block)
{
    (void)context;
    (void)x;
    (void)y;
    block->flags = z < 0 ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    block->friction = 0.6f;
}

static VoxelRigidBodyDescription Description(void)
{
    VoxelRigidBodyDescription description = {0};
    description.halfExtent[0] = 0.5;
    description.halfExtent[1] = 0.5;
    description.halfExtent[2] = 0.5;
    description.mass = 1.0;
    description.position[2] = 10.0;
    return description;
}

static void Step(VoxelRigidBody *bodies, uint32_t count, const VoxelRigidStepSettings *settings)
{
    VoxelCollisionSource collision = {NULL, Floor, NULL};
    if (perturbFp) LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyStep(bodies, count, &collision, settings, solverScratch,
                              (uint32_t)sizeof(solverScratch)), "step succeeded");
    Expect(VoxelPhysicsThreadIsConfigured(), "FP environment restored including sleeping steps");
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static void TestRestitution(void)
{
    const double coefficients[] = {0.0, 0.5, 1.0};
    for (uint32_t sample = 0u; sample < 6u; ++sample)
    {
        VoxelRigidBody bodies[2];
        VoxelRigidBodyDescription description = Description();
        description.restitution = coefficients[sample % 3u];
        Expect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "impact first body");
        description.position[0] = 0.99;
        Expect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description), "impact second body");
        const double incoming[3] = {10.0, 0.0, 0.0};
        Expect(VoxelRigidBodyAddLinearVelocity(&bodies[0], incoming), "impact velocity");
        VoxelRigidStepSettings settings;
        VoxelRigidStepSettingsDefault(&settings);
        settings.gravity[2] = 0.0;
        settings.penetrationCorrection = 0.0;
        settings.sleepFrames = 0u;
        settings.solverIterations = sample < 3u ? 8u : 16u;
        Step(bodies, 2u, &settings);
        double first[3];
        double second[3];
        Expect(VoxelRigidBodyLinearVelocity(&bodies[0], first) &&
               VoxelRigidBodyLinearVelocity(&bodies[1], second), "impact result");
        Expect(Absolute(first[0] + second[0] - 10.0) < 1e-7, "linear momentum conserved");
        Expect(Absolute(second[0] - first[0] - 10.0 * coefficients[sample % 3u]) < 0.03,
               "restitution uses pre-solve velocity through all iterations");
        VoxelRigidBodyRelease(&bodies[0]);
        VoxelRigidBodyRelease(&bodies[1]);
    }
}

static void TestCoulombFriction(void)
{
    const uint32_t iterations[] = {1u, 8u, 16u};
    const double masses[] = {1.0, 1e-100, 1e100, 1e200};
    for (uint32_t sample = 0u; sample < 12u; ++sample)
    {
        VoxelRigidBody body;
        VoxelRigidBodyDescription description = Description();
        description.position[2] = 0.49;
        description.friction = 0.3;
        description.mass = masses[sample / 3u];
        Expect(VoxelRigidBodyInitialize(&body, 1u, &description), "friction body");
        const double incoming[3] = {10.0, 10.0, -1.0};
        Expect(VoxelRigidBodyAddLinearVelocity(&body, incoming), "friction velocity");
        VoxelRigidStepSettings settings;
        VoxelRigidStepSettingsDefault(&settings);
        settings.gravity[2] = 0.0;
        settings.penetrationCorrection = 0.0;
        settings.sleepFrames = 0u;
        settings.solverIterations = iterations[sample % 3u];
        Step(&body, 1u, &settings);
        double velocity[3];
        Expect(VoxelRigidBodyLinearVelocity(&body, velocity), "friction result");
        double deltaX = incoming[0] - velocity[0];
        double deltaY = incoming[1] - velocity[1];
        double normalImpulse = velocity[2] - incoming[2];
        double limit = description.friction * normalImpulse;
        Expect(normalImpulse > 0.0 && deltaX > 0.0 && deltaY > 0.0, "friction opposes slip");
        Expect(deltaX * deltaX + deltaY * deltaY <= limit * limit + 1e-7,
               "total 2D friction bounded by mu times total normal impulse");
        VoxelRigidBodyRelease(&body);
    }
}

static void TestEdgeNormal(void)
{
    VoxelRigidBody bodies[2];
    VoxelRigidBodyDescription description = Description();
    description.halfExtent[0] = 1.0;
    description.halfExtent[1] = 0.25;
    description.halfExtent[2] = 0.25;
    Expect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description), "edge second body");
    description.position[0] = -0.6135573401115835;
    description.position[1] = 0.47029335610568523;
    description.position[2] = 10.284639788558707;
    Expect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "edge first body");
    const double orientation[4] = {-0.8284206418927017, 0.20436453443438132,
                                   -0.47628664764572104, 0.21238033436715184};
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        bodies[1].orientation[component] = orientation[component];
    }
    const double normal[3] = {0.0, 0.7922655995521599, 0.610176384143353};
    const double incoming[3] = {0.0, -normal[1] * 2.0, -normal[2] * 2.0};
    Expect(VoxelRigidBodyAddLinearVelocity(&bodies[0], incoming), "edge impact velocity");
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.gravity[2] = 0.0;
    settings.penetrationCorrection = 0.0;
    settings.sleepFrames = 0u;
    Step(bodies, 2u, &settings);
    double result[3];
    Expect(VoxelRigidBodyLinearVelocity(&bodies[0], result), "edge result");
    double deltaY = result[1] - incoming[1];
    double deltaZ = result[2] - incoming[2];
    Expect(deltaY > 0.0 && deltaZ > 0.0 && Absolute(result[0]) < 1e-8 &&
           Absolute(deltaY * normal[2] - deltaZ * normal[1]) < 1e-8,
           "edge-edge impulse follows minimum edge axis, not a box face");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestContactOverflow(void)
{
    static VoxelRigidBody bodies[10];
    VoxelRigidBodyDescription description = Description();
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        Expect(VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description),
               "overflow body");
    }
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.gravity[2] = 0.0;
    VoxelCollisionSource collision = {NULL, Floor, NULL};
    Expect(!VoxelRigidBodyStep(bodies, 10u, &collision, &settings, solverScratch,
                               (uint32_t)sizeof(solverScratch)),
           "contact capacity overflow is not silently treated as a successful step");
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
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

static uint64_t HashDouble(uint64_t hash, double value)
{
    union { double scalar; uint64_t bits; } representation = {value};
    return HashWord(hash, representation.bits);
}

static uint64_t Replay(bool reversed, bool hostileFp)
{
    perturbFp = hostileFp;
    static VoxelRigidBody bodies[8];
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t id = 0u; id < 8u; ++id)
    {
        uint32_t slot = reversed ? 7u - id : id;
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)(id % 4u) * 1.25;
        description.position[1] = 0.25;
        uint32_t layer = id / 4u;
        description.position[2] = 2.0 + (double)layer * 1.3;
        description.friction = 0.5;
        description.restitution = 0.1;
        if (perturbFp) LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyInitialize(&bodies[slot], (uint64_t)id + 1u, &description),
               "replay body");
        Expect(VoxelPhysicsThreadIsConfigured(), "initialization restores FP environment");
        const double spin[3] = {(double)(id % 3u) * 0.1, 0.03, -0.01};
        if (perturbFp) LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyAddAngularVelocity(&bodies[slot], spin), "replay spin");
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < 2048u; ++tick)
    {
        if (tick == 1024u)
        {
            const double impact[3] = {2.0, 0.0, 1.0};
            if (perturbFp) LaiueTestSetHostileFpEnvironment();
            Expect(VoxelRigidBodyAddLinearVelocity(&bodies[reversed ? 5u : 2u], impact),
                   "replay fixed-tick input");
        }
        Step(bodies, 8u, &settings);
        for (uint32_t id = 0u; id < 8u; ++id)
        {
            const VoxelRigidBody *body = &bodies[reversed ? 7u - id : id];
            hash = HashWord(hash, body->stableId);
            hash = HashWord(hash, body->active ? 1u : 0u);
            hash = HashWord(hash, body->sleeping ? 1u : 0u);
            hash = HashWord(hash, body->sleepCounter);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                hash = HashCoordinate(hash, &body->position[axis]);
                hash = HashCoordinate(hash, &body->linearVelocity[axis]);
                hash = HashCoordinate(hash, &body->angularVelocity[axis]);
                hash = HashDouble(hash, body->halfExtent[axis]);
                hash = HashDouble(hash, body->inverseInertia[axis]);
            }
            for (uint32_t axis = 0u; axis < 4u; ++axis)
            {
                hash = HashDouble(hash, body->orientation[axis]);
            }
            hash = HashDouble(hash, body->inverseMass);
            hash = HashDouble(hash, body->restitution);
            hash = HashDouble(hash, body->friction);
        }
    }
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
    return hash;
}

static void WriteHash(uint64_t hash)
{
    const char digits[] = "0123456789abcdef";
    char text[19] = {'0', 'x'};
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[2u + index] = digits[(hash >> (60u - index * 4u)) & 15u];
    }
    LaiueTestRuntimeWrite("rigid-replay-hash: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
}

LAIUE_TEST_ENTRY(RigidSolverTestEntryPoint)
{
    TestRestitution();
    TestCoulombFriction();
    TestEdgeNormal();
    TestContactOverflow();
    uint64_t hash = Replay(false, false);
    Expect(Replay(false, false) == hash, "same initial state and ticks repeat bit for bit");
    Expect(Replay(true, false) == hash, "body array permutation does not affect replay");
    Expect(Replay(true, true) == hash, "hostile FP modes do not affect replay");
    WriteHash(hash);
    // Established after the contact corrections on Windows MSVC/clang-cl
    // and Linux GCC, Debug and Release. New platforms must match before
    // joining this contract; changing it requires reviewing physics changes.
    Expect(hash == UINT64_C(0xfb1c9d5a02972c16), "canonical rigid replay reference");
    LAIUE_TEST_SUCCESS();
}
