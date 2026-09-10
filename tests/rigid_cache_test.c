#include "physics/rigid_body.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#define REPLAY_BODY_COUNT 8u
#define REPLAY_TICKS 2048u

// Test buffers are static: no-CRT targets must not generate a large stack probe.
static uint8_t stepScratch[262144];
static uint8_t contactStorage[1048576];
static VoxelRigidContactCache contactCache;
static bool floorEnabled = true;
static bool perturbFp;
static uint64_t matchedContacts;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Rigid cache failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryFloor(void *context, int64_t x, int64_t y, int64_t z,
                        VoxelBlockPhysics *block)
{
    (void)context;
    (void)x;
    (void)y;
    block->flags = floorEnabled && z < 0 ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    block->friction = 0.6f;
}

static VoxelCollisionSource Collision(void)
{
    VoxelCollisionSource collision = {NULL, QueryFloor, NULL};
    return collision;
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

static void InitializeCache(void)
{
    uint32_t bytes = VoxelRigidContactCacheBytes(REPLAY_BODY_COUNT);
    Expect(bytes != 0u && bytes < sizeof(contactStorage), "cache storage requirement fits");
    // Deliberately unaligned: the public size includes alignment headroom.
    Expect(VoxelRigidContactCacheInitialize(&contactCache, contactStorage + 1u,
                                            REPLAY_BODY_COUNT, bytes), "cache initialized");
}

static void Step(VoxelRigidBody *bodies, uint32_t count,
                   const VoxelRigidStepSettings *settings)
{
    VoxelCollisionSource collision = Collision();
    if (perturbFp) LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyStepCached(bodies, count, &collision, settings, stepScratch,
                                    (uint32_t)sizeof(stepScratch), &contactCache),
           "cached step succeeded");
    Expect(VoxelPhysicsThreadIsConfigured(), "cached step restores FP environment");
    matchedContacts += contactCache.matchedContactCount;
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
        hash = HashDouble(hash, body->halfExtent[axis]);
        hash = HashDouble(hash, body->inverseInertia[axis]);
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        hash = HashDouble(hash, body->orientation[component]);
    }
    hash = HashDouble(hash, body->inverseMass);
    hash = HashDouble(hash, body->restitution);
    return HashDouble(hash, body->friction);
}

static void TestInvalidCapacityAndIds(void)
{
    VoxelRigidContactCache invalid = {0};
    uint32_t bytes = VoxelRigidContactCacheBytes(1u);
    Expect(bytes > 0u, "one-body cache has storage");
    Expect(VoxelRigidContactCacheBytes(0u) == 0u &&
           VoxelRigidContactCacheBytes(UINT32_MAX) == 0u, "invalid capacity rejected");
    Expect(!VoxelRigidContactCacheInitialize(NULL, contactStorage, 1u, bytes),
           "null cache rejected");
    Expect(!VoxelRigidContactCacheInitialize(&invalid, NULL, 1u, bytes),
           "null storage rejected");
    Expect(!VoxelRigidContactCacheInitialize(&invalid, contactStorage, 1u, bytes - 1u),
           "undersized cache storage rejected");
    Expect(!VoxelRigidContactCacheInitialize(&invalid, contactStorage, 0u, bytes),
           "zero body capacity rejected");
    Expect(!VoxelRigidContactCacheInitialize(&invalid, &invalid, 1u, bytes),
           "storage cannot overwrite its descriptor");
    VoxelRigidContactCacheReset(NULL);
    VoxelRigidContactCacheReset(&invalid);

    VoxelRigidBody bodies[2];
    VoxelRigidBodyDescription description = Description();
    Expect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "first validation body");
    description.position[0] = 2.0;
    Expect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description), "second validation body");
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelCollisionSource collision = Collision();
    uint64_t before = HashBody(HashBody(0u, &bodies[0]), &bodies[1]);
    Expect(VoxelRigidContactCacheInitialize(&invalid, contactStorage, 1u, bytes),
           "small cache initialized");
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &invalid),
           "cache body capacity enforced");
    Expect(before == HashBody(HashBody(0u, &bodies[0]), &bodies[1]),
           "cache capacity failure leaves bodies unchanged");
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), NULL),
           "cached API requires initialized context");
    InitializeCache();
    VoxelRigidContactCache overlapping = contactCache;
    overlapping.storage = stepScratch;
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &overlapping),
           "cache cannot overlap step scratch");
    overlapping.storage = bodies;
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &overlapping),
           "cache cannot overlap bodies");
    overlapping.storage = &overlapping;
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &overlapping),
           "step rejects storage overlapping its descriptor");
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, &contactCache,
                                     (uint32_t)sizeof(stepScratch), &contactCache),
           "descriptor cannot overlap step scratch");
    Expect(before == HashBody(HashBody(0u, &bodies[0]), &bodies[1]),
           "cache alias failures leave bodies unchanged");
    bodies[1].stableId = 0u;
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &contactCache),
           "cached step rejects zero stable id");
    bodies[1].stableId = 1u;
    Expect(!VoxelRigidBodyStepCached(bodies, 2u, &collision, &settings, stepScratch,
                                     (uint32_t)sizeof(stepScratch), &contactCache),
           "cached step rejects duplicate stable ids");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestSleepingImpactAndRemovedSupport(void)
{
    floorEnabled = true;
    perturbFp = false;
    VoxelRigidContactCacheReset(&contactCache);
    VoxelRigidBody bodies[2];
    VoxelRigidBodyDescription description = Description();
    description.position[2] = 0.49;
    Expect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "sleeping target created");
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t tick = 0u; tick < 512u; ++tick) Step(bodies, 1u, &settings);
    Expect(bodies[0].sleeping, "cached resting target sleeps");
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&bodies[0], position), "sleeping target position");
    description.position[0] = position[0] - 0.99;
    description.position[1] = position[1];
    description.position[2] = position[2];
    Expect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description), "incoming body created");
    const double incoming[3] = {10.0, 0.0, 0.0};
    Expect(VoxelRigidBodyAddLinearVelocity(&bodies[1], incoming), "incoming velocity");
    settings.penetrationCorrection = 0.0;
    VoxelRigidContactCacheReset(&contactCache);
    Step(bodies, 2u, &settings);
    double targetVelocity[3];
    double incomingVelocity[3];
    Expect(VoxelRigidBodyLinearVelocity(&bodies[0], targetVelocity) &&
           VoxelRigidBodyLinearVelocity(&bodies[1], incomingVelocity), "impact velocities");
    Expect(!bodies[0].sleeping && targetVelocity[0] > 1.0 && incomingVelocity[0] < 9.0,
           "cached impact wakes finite-mass target");
    Expect(Absolute(targetVelocity[0] + incomingVelocity[0] - 10.0) < 1e-6,
           "sleeping impact conserves horizontal momentum on frictionless floor");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);

    VoxelRigidContactCacheReset(&contactCache);
    description = Description();
    description.position[2] = 0.49;
    description.friction = 0.6;
    Expect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "support-removal body");
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t tick = 0u; tick < 512u; ++tick) Step(bodies, 1u, &settings);
    Expect(bodies[0].sleeping, "body sleeps before support removal");
    floorEnabled = false;
    VoxelRigidContactCacheReset(&contactCache);
    VoxelRigidBodyWake(&bodies[0]);
    for (uint32_t tick = 0u; tick < 128u; ++tick) Step(bodies, 1u, &settings);
    Expect(VoxelRigidBodyLocalPosition(&bodies[0], position), "unsupported body position");
    Expect(!bodies[0].sleeping && position[2] < -1.0,
           "clear cache and explicit wake remove stale support");
    VoxelRigidBodyRelease(&bodies[0]);
    floorEnabled = true;
}

static void TestCommonOriginTranslation(void)
{
    VoxelRigidContactCacheReset(&contactCache);
    VoxelRigidBody body;
    VoxelRigidBodyDescription description = Description();
    description.position[2] = 0.49;
    description.friction = 0.6;
    Expect(VoxelRigidBodyInitialize(&body, 1u, &description), "rebase body created");
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepFrames = 0u;
    for (uint32_t tick = 0u; tick < 8u; ++tick) Step(&body, 1u, &settings);
    Expect(contactCache.matchedContactCount > 0u, "resting contacts are warm before rebase");
    double before[3];
    Expect(VoxelRigidBodyLocalPosition(&body, before), "position before rebase");
    const int64_t shift[3] = {256, -512, 0};
    // The infinite flat floor is unchanged by this horizontal origin shift.
    Expect(VoxelRigidBodyTranslateBlocks(&body, shift), "common origin translated");
    Step(&body, 1u, &settings);
    Expect(contactCache.matchedContactCount > 0u, "local contact anchors survive rebase");
    double after[3];
    Expect(VoxelRigidBodyLocalPosition(&body, after), "position after rebase");
    Expect(Absolute(after[0] - before[0] + 256.0) < 0.01 &&
           Absolute(after[1] - before[1] - 512.0) < 0.01 &&
           Absolute(after[2] - before[2]) < 0.01, "rebase does not inject a large impulse");
    VoxelRigidBodyRelease(&body);
}

static uint64_t Replay(bool reversed, bool hostileFp)
{
    perturbFp = hostileFp;
    floorEnabled = true;
    // Reset must discard history while retaining the caller's storage.
    VoxelRigidContactCacheReset(&contactCache);
    Expect(contactCache.contactCount == 0u && contactCache.matchedContactCount == 0u,
           "reset discards cached contacts and diagnostics");
    matchedContacts = 0u;
    static VoxelRigidBody bodies[REPLAY_BODY_COUNT];
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t id = 0u; id < REPLAY_BODY_COUNT; ++id)
    {
        uint32_t slot = reversed ? REPLAY_BODY_COUNT - 1u - id : id;
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)(id % 4u) * 1.25;
        description.position[1] = 0.25;
        uint32_t layer = id / 4u;
        description.position[2] = 2.0 + (double)layer * 1.3;
        description.friction = 0.5;
        description.restitution = 0.1;
        if (perturbFp) LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyInitialize(&bodies[slot], (uint64_t)id + 1u, &description),
               "cached replay body");
        Expect(VoxelPhysicsThreadIsConfigured(), "initialization restores FP environment");
        const double spin[3] = {(double)(id % 3u) * 0.1, 0.03, -0.01};
        if (perturbFp) LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyAddAngularVelocity(&bodies[slot], spin), "cached replay spin");
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < REPLAY_TICKS; ++tick)
    {
        if (tick == 1024u)
        {
            const double impact[3] = {2.0, 0.0, 1.0};
            if (perturbFp) LaiueTestSetHostileFpEnvironment();
            Expect(VoxelRigidBodyAddLinearVelocity(&bodies[reversed ? 5u : 2u], impact),
                   "cached replay fixed-tick input");
        }
        Step(bodies, REPLAY_BODY_COUNT, &settings);
        for (uint32_t id = 0u; id < REPLAY_BODY_COUNT; ++id)
        {
            uint32_t slot = reversed ? REPLAY_BODY_COUNT - 1u - id : id;
            hash = HashBody(hash, &bodies[slot]);
        }
    }
    for (uint32_t index = 0u; index < REPLAY_BODY_COUNT; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
    Expect(matchedContacts > 0u, "replay exercises contact warm starting");
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
    LaiueTestRuntimeWrite("rigid-cached-replay-hash: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
}

LAIUE_TEST_ENTRY(RigidCacheTestEntryPoint)
{
    TestInvalidCapacityAndIds();
    TestSleepingImpactAndRemovedSupport();
    TestCommonOriginTranslation();
    uint64_t hash = Replay(false, false);
    Expect(Replay(false, false) == hash, "cache reset and repeated replay match exactly");
    Expect(Replay(true, false) == hash, "cached replay independent of array order");
    Expect(Replay(true, true) == hash, "cached replay restores hostile FP modes");
    WriteHash(hash);
    Expect(hash == UINT64_C(0x58c622ddfd840a45), "cached replay matches verified golden state");
    LAIUE_TEST_SUCCESS();
}
