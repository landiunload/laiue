#include "physics/rigid_body.h"
#include "physics/rigid_broadphase.h"
#include "physics/numeric_provider.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#define TEST_CAPACITY 32u
#define REPLAY_BODY_COUNT 8u
#define REPLAY_TICKS 2048u

static VoxelRigidBody legacyBodies[TEST_CAPACITY];
static VoxelRigidBody indexedBodies[TEST_CAPACITY];
static uint8_t legacyScratch[1048576];
static uint8_t indexedScratch[1048576];
static uint8_t legacyContactStorage[1048576];
static uint8_t indexedContactStorage[1048576];
static uint8_t indexStorage[1048576];
static VoxelRigidContactCache legacyContacts;
static VoxelRigidContactCache indexedContacts;
static VoxelRigidBroadphase spatialIndex;
static uint32_t indexBytes;
static uint64_t queryHash;
static int64_t worldOrigin[3];
static bool floorEnabled;
static bool perturbFp;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Rigid broadphase failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
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

static uint64_t DoubleBits(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return representation.bits;
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

static bool SameCoordinate(const InfiniteCoord *first, const InfiniteCoord *second)
{
    if (first->sign != second->sign || first->limbCount != second->limbCount)
        return false;
    for (uint32_t limb = 0u; limb < first->limbCount; ++limb)
    {
        if (first->limbs[limb] != second->limbs[limb])
            return false;
    }
    return true;
}

static bool SameBody(const VoxelRigidBody *first, const VoxelRigidBody *second)
{
    if (first->stableId != second->stableId || first->active != second->active ||
        first->sleeping != second->sleeping || first->sleepCounter != second->sleepCounter ||
        DoubleBits(first->inverseMass) != DoubleBits(second->inverseMass) ||
        DoubleBits(first->restitution) != DoubleBits(second->restitution) ||
        DoubleBits(first->friction) != DoubleBits(second->friction))
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!SameCoordinate(&first->position[axis], &second->position[axis]) ||
            !SameCoordinate(&first->linearVelocity[axis], &second->linearVelocity[axis]) ||
            !SameCoordinate(&first->angularVelocity[axis], &second->angularVelocity[axis]) ||
            DoubleBits(first->halfExtent[axis]) != DoubleBits(second->halfExtent[axis]) ||
            DoubleBits(first->inverseInertia[axis]) != DoubleBits(second->inverseInertia[axis]))
        {
            return false;
        }
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        if (DoubleBits(first->orientation[component]) != DoubleBits(second->orientation[component]))
            return false;
    }
    return true;
}

// Callback order is part of the compatibility check, not just body transforms.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryFloor(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *block)
{
    (void)context;
    queryHash = HashWord(queryHash, (uint64_t)x);
    queryHash = HashWord(queryHash, (uint64_t)y);
    queryHash = HashWord(queryHash, (uint64_t)z);
    block->flags =
        floorEnabled && z + worldOrigin[2] < 0 ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
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

static void ReleaseBodies(void)
{
    for (uint32_t index = 0u; index < TEST_CAPACITY; ++index)
    {
        VoxelRigidBodyRelease(&legacyBodies[index]);
        VoxelRigidBodyRelease(&indexedBodies[index]);
    }
}

static void InitializeWorld(void)
{
    ReleaseBodies();
    floorEnabled = true;
    perturbFp = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        worldOrigin[axis] = 0;
    uint32_t contactBytes = VoxelRigidContactCacheBytes(TEST_CAPACITY);
    Expect(contactBytes > 0u && contactBytes <= sizeof(legacyContactStorage),
           "contact storage fits");
    Expect(VoxelRigidContactCacheInitialize(&legacyContacts, legacyContactStorage, TEST_CAPACITY,
                                            contactBytes),
           "legacy cache initialized");
    Expect(VoxelRigidContactCacheInitialize(&indexedContacts, indexedContactStorage, TEST_CAPACITY,
                                            contactBytes),
           "indexed cache initialized");
    indexBytes = VoxelRigidBroadphaseBytes(TEST_CAPACITY);
    Expect(indexBytes > 0u && indexBytes + 2u <= sizeof(indexStorage), "index storage fits");
    indexStorage[0] = 0xa5u;
    indexStorage[indexBytes + 1u] = 0x5au;
    Expect(
        VoxelRigidBroadphaseInitialize(&spatialIndex, indexStorage + 1u, TEST_CAPACITY, indexBytes),
        "unaligned index initialized");
}

static void InitializeBody(uint32_t slot, uint64_t id, const VoxelRigidBodyDescription *description)
{
    VoxelRigidBodyRelease(&legacyBodies[slot]);
    VoxelRigidBodyRelease(&indexedBodies[slot]);
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyInitialize(&legacyBodies[slot], id, description),
           "legacy body initialized");
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyInitialize(&indexedBodies[slot], id, description),
           "indexed body initialized");
}

static void AddVelocity(uint32_t slot, const double velocity[3])
{
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyAddLinearVelocity(&legacyBodies[slot], velocity), "legacy velocity");
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyAddLinearVelocity(&indexedBodies[slot], velocity), "indexed velocity");
}

static void AddSpin(uint32_t slot, const double spin[3])
{
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyAddAngularVelocity(&legacyBodies[slot], spin), "legacy spin");
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyAddAngularVelocity(&indexedBodies[slot], spin), "indexed spin");
}

static void CheckGuards(void)
{
    Expect(indexStorage[0] == 0xa5u && indexStorage[indexBytes + 1u] == 0x5au,
           "index storage guard bytes preserved");
}

static void StepBoth(uint32_t count, const VoxelRigidStepSettings *settings, bool cached)
{
    VoxelCollisionSource collision = Collision();
    queryHash = UINT64_C(14695981039346656037);
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    bool legacyResult =
        cached ? VoxelRigidBodyStepCached(legacyBodies, count, &collision, settings, legacyScratch,
                                          (uint32_t)sizeof(legacyScratch), &legacyContacts)
               : VoxelRigidBodyStep(legacyBodies, count, &collision, settings, legacyScratch,
                                    (uint32_t)sizeof(legacyScratch));
    Expect(legacyResult, "legacy comparison step succeeded");
    uint64_t legacyQueries = queryHash;
    queryHash = UINT64_C(14695981039346656037);
    if (perturbFp)
        LaiueTestSetHostileFpEnvironment();
    Expect(VoxelRigidBodyStepIndexed(indexedBodies, count, &collision, settings, indexedScratch,
                                     (uint32_t)sizeof(indexedScratch),
                                     cached ? &indexedContacts : NULL, &spatialIndex),
           "indexed comparison step succeeded");
    Expect(VoxelPhysicsThreadIsConfigured(), "indexed step restores FP environment");
    Expect(queryHash == legacyQueries, "world callback order preserved");
    for (uint32_t index = 0u; index < count; ++index)
    {
        Expect(SameBody(&legacyBodies[index], &indexedBodies[index]),
               "all body fields and limbs match legacy every tick");
    }
    if (cached)
    {
        Expect(legacyContacts.contactCount == indexedContacts.contactCount &&
                   legacyContacts.matchedContactCount == indexedContacts.matchedContactCount,
               "warm-start contact counts match legacy");
    }
    CheckGuards();
}

static void Advance(uint32_t count, const VoxelRigidStepSettings *settings, uint32_t steps)
{
    for (uint32_t tick = 0u; tick < steps; ++tick)
        StepBoth(count, settings, true);
}

static void ResetContacts(void)
{
    VoxelRigidContactCacheReset(&legacyContacts);
    VoxelRigidContactCacheReset(&indexedContacts);
}

static void RebaseBodies(uint32_t count, const int64_t shift[3])
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        Expect(VoxelRigidBodyTranslateBlocks(&legacyBodies[index], shift), "legacy rebase");
        Expect(VoxelRigidBodyTranslateBlocks(&indexedBodies[index], shift), "indexed rebase");
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        worldOrigin[axis] += shift[axis];
}

static void TestIncrementalChanges(void)
{
    InitializeWorld();
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    const uint64_t ids[5] = {50u, 10u, 30u, 20u, 60u};
    for (uint32_t slot = 0u; slot < 3u; ++slot)
    {
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)slot * 0.9 - 1.25;
        description.position[1] = 0.25;
        description.position[2] = 2.0 + (double)slot * 1.2;
        description.friction = 0.5;
        InitializeBody(slot, ids[slot], &description);
    }
    Advance(3u, &settings, 16u);
    for (uint32_t slot = 3u; slot < 5u; ++slot)
    {
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)(slot - 3u) * 1.25;
        description.position[2] = 5.0;
        description.friction = 0.5;
        InitializeBody(slot, ids[slot], &description);
    }
    Advance(5u, &settings, 16u);
    legacyBodies[1].active = false;
    indexedBodies[1].active = false;
    legacyBodies[3].active = false;
    indexedBodies[3].active = false;
    Advance(5u, &settings, 16u);
    legacyBodies[1].active = true;
    indexedBodies[1].active = true;
    VoxelRigidBodyWake(&legacyBodies[1]);
    VoxelRigidBodyWake(&indexedBodies[1]);
    Advance(5u, &settings, 4u);

    // Move ownership of whole bodies, including bigint allocations. Tree slots
    // must follow current array indices, not historical stableId associations.
    VoxelRigidBody temporary = legacyBodies[0];
    legacyBodies[0] = legacyBodies[4];
    legacyBodies[4] = temporary;
    temporary = indexedBodies[0];
    indexedBodies[0] = indexedBodies[4];
    indexedBodies[4] = temporary;
    Advance(5u, &settings, 16u);
    Advance(2u, &settings, 8u);
    Advance(5u, &settings, 8u);

    const int64_t translation[3] = {-20, 0, 0};
    Expect(VoxelRigidBodyTranslateBlocks(&legacyBodies[2], translation), "legacy teleport");
    Expect(VoxelRigidBodyTranslateBlocks(&indexedBodies[2], translation), "indexed teleport");
    ResetContacts();
    Advance(5u, &settings, 8u);
    const int64_t rebase[3] = {17, -9, 3};
    RebaseBodies(5u, rebase);
    // Common rebasing keeps body-local contact anchors; do not reset warm-start.
    Advance(5u, &settings, 16u);
    VoxelRigidBroadphaseReset(&spatialIndex);
    Advance(5u, &settings, 16u);
    ReleaseBodies();
}

static void TestSleepingImpact(void)
{
    InitializeWorld();
    VoxelRigidBodyDescription description = Description();
    description.position[2] = 0.49;
    InitializeBody(0u, 50u, &description);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    Advance(1u, &settings, 512u);
    Expect(indexedBodies[0].sleeping, "target sleeps before indexed impact");
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&indexedBodies[0], position), "sleeping target position");
    description.position[0] = position[0] - 0.99;
    description.position[1] = position[1];
    description.position[2] = position[2];
    InitializeBody(1u, 10u, &description);
    const double incoming[3] = {10.0, 0.0, 0.0};
    AddVelocity(1u, incoming);
    settings.penetrationCorrection = 0.0;
    ResetContacts();
    StepBoth(2u, &settings, true);
    double targetVelocity[3];
    double incomingVelocity[3];
    Expect(VoxelRigidBodyLinearVelocity(&indexedBodies[0], targetVelocity) &&
               VoxelRigidBodyLinearVelocity(&indexedBodies[1], incomingVelocity),
           "impact velocities");
    Expect(!indexedBodies[0].sleeping && targetVelocity[0] > 1.0 && incomingVelocity[0] < 9.0,
           "indexed query wakes finite-mass target");
    Expect(Absolute(targetVelocity[0] + incomingVelocity[0] - 10.0) < 1e-6,
           "indexed impact conserves horizontal momentum");
    floorEnabled = false;
    ResetContacts();
    VoxelRigidBodyWake(&legacyBodies[0]);
    VoxelRigidBodyWake(&indexedBodies[0]);
    VoxelRigidBodyWake(&legacyBodies[1]);
    VoxelRigidBodyWake(&indexedBodies[1]);
    Advance(2u, &settings, 128u);
    Expect(VoxelRigidBodyLocalPosition(&indexedBodies[0], position) && position[2] < -1.0,
           "indexed bodies fall after support removal and explicit wake");
    ReleaseBodies();
}

static void TestPredictiveProxyChanges(void)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        InitializeWorld();
        floorEnabled = false;
        VoxelRigidStepSettings settings;
        VoxelRigidStepSettingsDefault(&settings);
        for (uint32_t component = 0u; component < 3u; ++component)
            settings.gravity[component] = 0.0;
        settings.sleepFrames = 0u;

        VoxelRigidBodyDescription moving = Description();
        for (uint32_t component = 0u; component < 3u; ++component)
        {
            moving.position[component] = 0.25;
            moving.halfExtent[component] = 0.125;
        }
        InitializeBody(0u, UINT64_MAX, &moving);
        VoxelRigidBodyDescription target = moving;
        target.position[axis] += 1.0;
        InitializeBody(1u, 7u, &target);
        double velocity[3] = {0.0, 0.0, 0.0};
        velocity[axis] = 8.0;
        AddVelocity(0u, velocity);
        Advance(2u, &settings, 8u);

        // Reverse while the fat proxy still extends in the old direction.
        // Grid and persistent tree must agree on every intermediate state.
        velocity[axis] = -16.0;
        AddVelocity(0u, velocity);
        Advance(2u, &settings, 16u);

        VoxelRigidBodyDescription resized = moving;
        resized.position[axis] = -8.0;
        for (uint32_t component = 0u; component < 3u; ++component)
            resized.halfExtent[component] = 2.0;
        InitializeBody(0u, UINT64_MAX, &resized);
        ResetContacts();
        StepBoth(2u, &settings, true);
        for (uint32_t component = 0u; component < 3u; ++component)
            resized.halfExtent[component] = 0.125;
        InitializeBody(0u, UINT64_MAX, &resized);
        ResetContacts();
        StepBoth(2u, &settings, true);
        Expect(spatialIndex.updatedProxyCount == 1u, "moving proxy tightens after a shape shrinks");

        // Reuse the slot with another large stableId, then collide. Its rank
        // is not its array index and cannot be truncated to a 32-bit ID.
        InitializeBody(0u, UINT64_MAX - 1u, &moving);
        ResetContacts();
        velocity[axis] = 8.0;
        AddVelocity(0u, velocity);
        bool collided = false;
        for (uint32_t tick = 0u; tick < 64u; ++tick)
        {
            StepBoth(2u, &settings, true);
            collided = collided || indexedContacts.contactCount != 0u;
        }
        double targetVelocity[3];
        double movingVelocity[3];
        Expect(VoxelRigidBodyLinearVelocity(&indexedBodies[1], targetVelocity) &&
                   VoxelRigidBodyLinearVelocity(&indexedBodies[0], movingVelocity),
               "read velocities after predictive proxy collision");
        Expect(collided && targetVelocity[axis] > 1.0 && movingVelocity[axis] < 7.0,
               "reused predictive proxy preserves collision response");
        Expect(Absolute(targetVelocity[axis] + movingVelocity[axis] - 8.0) < 1e-6,
               "predictive proxy collision preserves linear momentum");
        ReleaseBodies();
    }
}

static uint32_t NextChurnRandom(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static void ExpectSynchronizedProxies(uint32_t count)
{
    uint32_t activeCount = 0u;
    for (uint32_t slot = 0u; slot < count; ++slot)
    {
        if (indexedBodies[slot].active)
            ++activeCount;
    }
    Expect(spatialIndex.indexedBodyCount == count, "index remembers current array extent");
    Expect(spatialIndex.proxyCount == activeCount,
           "index contains exactly the current active collidable bodies");
}

static void TestFullCapacityChurn(void)
{
    InitializeWorld();
    floorEnabled = false;
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        settings.gravity[axis] = 0.0;
    settings.sleepFrames = 0u;
    for (uint32_t slot = 0u; slot < TEST_CAPACITY; ++slot)
    {
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)(slot % 8u) * 4.0;
        uint32_t row = slot / 8u;
        description.position[1] = (double)row * 4.0;
        if (slot == 0u)
        {
            description.position[0] = -256.0;
            description.position[2] = 128.0;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                description.halfExtent[axis] = 128.0;
        }
        InitializeBody(slot, (uint64_t)slot + 1u, &description);
    }
    StepBoth(TEST_CAPACITY, &settings, true);
    ExpectSynchronizedProxies(TEST_CAPACITY);
    Expect(spatialIndex.proxyCount == TEST_CAPACITY, "all leaf and branch pool slots occupied");

    // Shrinking a large shape in place must not retain an enormous fat proxy
    // indefinitely. Nothing else moves, so exactly this proxy is reinserted.
    VoxelRigidBodyDescription small = Description();
    small.position[0] = -256.0;
    small.position[2] = 128.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        small.halfExtent[axis] = 0.25;
    InitializeBody(0u, 1u, &small);
    ResetContacts();
    StepBoth(TEST_CAPACITY, &settings, true);
    ExpectSynchronizedProxies(TEST_CAPACITY);
    Expect(spatialIndex.updatedProxyCount == 1u, "oversized historical fat bounds are tightened");

    // Reduce a saturated tree to a root leaf, then consume the entire free pool
    // again. The removed bodies stay caller-owned and can be included again.
    StepBoth(1u, &settings, true);
    ExpectSynchronizedProxies(1u);
    StepBoth(TEST_CAPACITY, &settings, true);
    ExpectSynchronizedProxies(TEST_CAPACITY);

    uint32_t random = UINT32_C(0x712ca543);
    uint64_t nextId = 1000u;
    uint32_t count = TEST_CAPACITY;
    for (uint32_t tick = 0u; tick < 256u; ++tick)
    {
        // Slot zero remains awake, so every call must synchronize removals even
        // when other slots are inactive or temporarily outside the array extent.
        uint32_t slot = 1u + NextChurnRandom(&random) % (TEST_CAPACITY - 1u);
        switch (tick % 8u)
        {
        case 0u:
            legacyBodies[slot].active = false;
            indexedBodies[slot].active = false;
            break;
        case 1u:
            legacyBodies[slot].active = true;
            indexedBodies[slot].active = true;
            VoxelRigidBodyWake(&legacyBodies[slot]);
            VoxelRigidBodyWake(&indexedBodies[slot]);
            break;
        case 2u:
        {
            uint32_t other = 1u + NextChurnRandom(&random) % (TEST_CAPACITY - 1u);
            VoxelRigidBody temporary = legacyBodies[slot];
            legacyBodies[slot] = legacyBodies[other];
            legacyBodies[other] = temporary;
            temporary = indexedBodies[slot];
            indexedBodies[slot] = indexedBodies[other];
            indexedBodies[other] = temporary;
            break;
        }
        case 3u:
        case 4u:
        {
            VoxelRigidBodyDescription description = Description();
            if (tick % 8u == 3u)
            {
                description.position[0] = (double)(NextChurnRandom(&random) % 16u) * 4.0;
                description.position[1] = (double)(NextChurnRandom(&random) % 16u) * 4.0;
                description.position[2] = 4.0 + (double)(NextChurnRandom(&random) % 4u) * 4.0;
            }
            else
            {
                Expect(VoxelRigidBodyLocalPosition(&indexedBodies[slot], description.position),
                       "shape replacement position available");
            }
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                description.halfExtent[axis] =
                    0.125 + (double)(NextChurnRandom(&random) % 4u) * 0.125;
            }
            InitializeBody(slot, nextId++, &description);
            ResetContacts();
            const double spin[3] = {0.1, -0.05, 0.025};
            AddSpin(slot, spin);
            break;
        }
        case 5u:
        {
            const int64_t translation[3] = {1, -1, 0};
            Expect(VoxelRigidBodyTranslateBlocks(&legacyBodies[slot], translation),
                   "legacy churn translation");
            Expect(VoxelRigidBodyTranslateBlocks(&indexedBodies[slot], translation),
                   "indexed churn translation");
            ResetContacts();
            break;
        }
        case 6u:
            count = 1u + NextChurnRandom(&random) % (TEST_CAPACITY - 1u);
            break;
        default:
            count = TEST_CAPACITY;
            break;
        }
        StepBoth(count, &settings, true);
        ExpectSynchronizedProxies(count);
    }
    ReleaseBodies();
}

static void TestValidation(void)
{
    InitializeWorld();
    Expect(VoxelRigidBroadphaseBytes(0u) == 0u && VoxelRigidBroadphaseBytes(UINT32_MAX) == 0u,
           "invalid index capacity rejected");
    VoxelRigidBroadphase invalid = {0};
    Expect(!VoxelRigidBroadphaseInitialize(NULL, indexStorage + 1u, TEST_CAPACITY, indexBytes),
           "null index rejected");
    Expect(!VoxelRigidBroadphaseInitialize(&invalid, NULL, TEST_CAPACITY, indexBytes),
           "null index storage rejected");
    Expect(!VoxelRigidBroadphaseInitialize(&invalid, indexStorage + 1u, TEST_CAPACITY,
                                           indexBytes - 1u),
           "short index storage rejected");
    VoxelRigidBroadphaseReset(NULL);
    CheckGuards();
    VoxelRigidBodyDescription description = Description();
    InitializeBody(0u, 1u, &description);
    description.position[0] = 2.0;
    InitializeBody(1u, 2u, &description);
    VoxelCollisionSource collision = Collision();
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    uint64_t before = HashBody(HashBody(0u, &indexedBodies[0]), &indexedBodies[1]);
    Expect(!VoxelRigidBodyStepIndexed(indexedBodies, 2u, &collision, &settings, indexedScratch,
                                      (uint32_t)sizeof(indexedScratch), &indexedContacts, NULL),
           "indexed step requires index context");
    Expect(!VoxelRigidBodyStepIndexed(indexedBodies, 2u, &collision, &settings, indexedScratch,
                                      (uint32_t)sizeof(indexedScratch), &indexedContacts, &invalid),
           "uninitialized index rejected");
    uint32_t smallBytes = VoxelRigidBroadphaseBytes(1u);
    Expect(VoxelRigidBroadphaseInitialize(&invalid, indexStorage + 1u, 1u, smallBytes),
           "small index initialized");
    Expect(!VoxelRigidBodyStepIndexed(indexedBodies, 2u, &collision, &settings, indexedScratch,
                                      (uint32_t)sizeof(indexedScratch), &indexedContacts, &invalid),
           "index body capacity enforced");
    Expect(before == HashBody(HashBody(0u, &indexedBodies[0]), &indexedBodies[1]),
           "index validation failure leaves bodies unchanged");
    Expect(VoxelRigidBroadphaseInitialize(&invalid, indexedScratch, TEST_CAPACITY,
                                          (uint32_t)sizeof(indexedScratch)),
           "aliased index initialized");
    Expect(!VoxelRigidBodyStepIndexed(indexedBodies, 2u, &collision, &settings, indexedScratch,
                                      (uint32_t)sizeof(indexedScratch), &indexedContacts, &invalid),
           "index and scratch overlap rejected");
    Expect(before == HashBody(HashBody(0u, &indexedBodies[0]), &indexedBodies[1]),
           "overlap validation does not mutate bodies");
    ReleaseBodies();
}

static uint64_t Replay(bool reversed, bool hostileFp, bool resetIndexEachTick, bool cached)
{
    InitializeWorld();
    perturbFp = hostileFp;
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
        InitializeBody(slot, (uint64_t)id + 1u, &description);
        const double spin[3] = {(double)(id % 3u) * 0.1, 0.03, -0.01};
        AddSpin(slot, spin);
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < REPLAY_TICKS; ++tick)
    {
        if (tick == 1024u)
        {
            const double impact[3] = {2.0, 0.0, 1.0};
            AddVelocity(reversed ? 5u : 2u, impact);
        }
        if (resetIndexEachTick)
            VoxelRigidBroadphaseReset(&spatialIndex);
        StepBoth(REPLAY_BODY_COUNT, &settings, cached);
        for (uint32_t id = 0u; id < REPLAY_BODY_COUNT; ++id)
        {
            uint32_t slot = reversed ? REPLAY_BODY_COUNT - 1u - id : id;
            hash = HashBody(hash, &indexedBodies[slot]);
        }
    }
    ReleaseBodies();
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
    LaiueTestRuntimeWrite("rigid-indexed-cached-replay-hash: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
}

LAIUE_TEST_ENTRY(RigidBroadphaseTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    TestValidation();
    TestIncrementalChanges();
    TestSleepingImpact();
    TestPredictiveProxyChanges();
    TestFullCapacityChurn();
    uint64_t hash = Replay(false, false, false, true);
    WriteHash(hash);
    Expect(hash == UINT64_C(0x58c622ddfd840a45), "indexed cached reference unchanged");
    Expect(Replay(false, false, true, true) == hash, "index reset each tick preserves trajectory");
    Expect(Replay(true, false, false, true) == hash, "reversed array preserves indexed trajectory");
    Expect(Replay(true, true, false, true) == hash, "indexed replay survives hostile FP modes");
    Expect(Replay(false, false, false, false) == UINT64_C(0xda25a4455485aafc),
           "optional cache absent preserves uncached reference");
    LAIUE_TEST_SUCCESS();
}
