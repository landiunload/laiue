// Ручной benchmark горячего шага rigid-body физики. Не входит в CTest:
// запускается явно, чтобы сравнивать изменения на одной машине.

#include "physics/rigid_body.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 5u
#define STEP_COUNT 8u
#define DENSE_SEED 0x6a09e667f3bcc909ull

static volatile uint64_t benchmarkSink;
static uint64_t benchmarkRandomState = 0x9e3779b97f4a7c15ull;

typedef struct BenchmarkWorld
{
    bool solidGround;
} BenchmarkWorld;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void WriteMilliseconds(double value)
{
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && samples[insertion - 1u] > value)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

static void QueryBlock(void *context, int64_t x, int64_t y, int64_t z,
                       VoxelBlockPhysics *outPhysics)
{
    BenchmarkWorld *world = (BenchmarkWorld *)context;
    (void)x;
    (void)y;
    outPhysics->flags = world->solidGround && z < 0
                            ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID
                            : 0u;
    outPhysics->friction = 0.6f;
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state;
}

static bool InitializeBodies(VoxelRigidBody *bodies, uint32_t count, bool resting)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyDescription description = {0};
        uint64_t random = NextRandom(&benchmarkRandomState);
        description.position[0] = (double)(index % 512u) * 2.0;
        description.position[1] = (double)((index / 512u) % 512u) * 2.0;
        description.position[2] = resting ? 0.49 : 16.0 + (double)(random & 15u) * 0.25;
        description.halfExtent[0] = 0.45;
        description.halfExtent[1] = 0.45;
        description.halfExtent[2] = 0.45;
        description.mass = 1.0;
        description.restitution = 0.0;
        description.friction = 0.6;
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
        {
            for (uint32_t release = 0u; release < index; ++release)
            {
                VoxelRigidBodyRelease(&bodies[release]);
            }
            return false;
        }
    }
    return true;
}

static void ReleaseBodies(VoxelRigidBody *bodies, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
}

static bool InitializeDenseBodies(VoxelRigidBody *bodies, uint32_t count)
{
    uint64_t randomState = DENSE_SEED;
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyDescription description = {0};
        // Separate columns avoid artificial edge/corner overlap with 26 neighbors.
        // The vertical 0.01 overlap deliberately exercises the contact solver.
        description.position[0] = (double)(index % 16u);
        description.position[1] = (double)((index / 16u) % 16u);
        description.position[2] = 0.44 + (double)(index / 256u) * 0.89;
        description.halfExtent[0] = 0.45;
        description.halfExtent[1] = 0.45;
        description.halfExtent[2] = 0.45;
        description.mass = 1.0;
        description.restitution = 0.0;
        description.friction = 0.6;
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
        {
            ReleaseBodies(bodies, index + 1u);
            return false;
        }

        uint64_t random = NextRandom(&randomState);
        double velocity[3] = {
            (double)((int32_t)(random & 15u) - 8) / 512.0,
            (double)((int32_t)((random >> 8u) & 15u) - 8) / 512.0,
            0.0,
        };
        if (!VoxelRigidBodyAddLinearVelocity(&bodies[index], velocity))
        {
            ReleaseBodies(bodies, index + 1u);
            return false;
        }
    }
    return true;
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

static uint64_t HashDouble(uint64_t hash, double value)
{
    union { double scalar; uint64_t bits; } representation = {value};
    return HashWord(hash, representation.bits);
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

static uint64_t HashBodies(const VoxelRigidBody *bodies, uint32_t bodyCount)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t index = 0u; index < bodyCount; ++index)
    {
        const VoxelRigidBody *body = &bodies[index];
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
        hash = HashDouble(hash, body->friction);
    }
    return hash;
}

static bool DenseStep(VoxelRigidBody *bodies, uint32_t bodyCount,
                       const VoxelCollisionSource *collision,
                       const VoxelRigidStepSettings *settings,
                       void *scratch, uint32_t scratchBytes,
                       VoxelRigidContactCache *cache, VoxelRigidBroadphase *broadphase)
{
    if (broadphase != NULL)
    {
        return VoxelRigidBodyStepIndexed(bodies, bodyCount, collision, settings, scratch,
                                         scratchBytes, cache, broadphase);
    }
    return cache == NULL
               ? VoxelRigidBodyStep(bodies, bodyCount, collision, settings, scratch, scratchBytes)
               : VoxelRigidBodyStepCached(bodies, bodyCount, collision, settings, scratch,
                                          scratchBytes, cache);
}

static bool RunDenseCase(uint32_t bodyCount, bool cached, uint32_t warmupSteps,
                         bool indexed, uint64_t *outHash)
{
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(bodyCount);
    uint32_t cacheBytes = cached ? VoxelRigidContactCacheBytes(bodyCount) : 0u;
    uint32_t indexBytes = indexed ? VoxelRigidBroadphaseBytes(bodyCount) : 0u;
    if (scratchBytes == 0u || (cached && cacheBytes == 0u) ||
        (indexed && indexBytes == 0u)) return false;

    BenchmarkWorld world = {.solidGround = true};
    VoxelCollisionSource collision = {
        .context = &world,
        .queryBlockPhysics = QueryBlock,
        .queryDynamicColliders = NULL,
    };
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepLinearSpeed = 0.0;
    settings.sleepAngularSpeed = 0.0;

    double samples[SAMPLE_COUNT];
    uint64_t referenceCandidates = 0u;
    uint64_t referenceContacts = 0u;
    uint64_t referenceMatches = 0u;
    uint64_t referenceHash = 0u;
    uint32_t referencePeakContacts = 0u;
    uint64_t referenceUpdates = 0u;
    uint64_t referenceVisits = 0u;
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
    {
        // Reset both bodies and scratch outside the timed region. Every sample
        // measures the same eight ticks, not a progressively settling scene.
        VoxelRigidBody *bodies = PlatformAllocate((size_t)bodyCount * sizeof(*bodies), true);
        void *scratch = PlatformAllocate(scratchBytes, true);
        void *cacheStorage = cached ? PlatformAllocate(cacheBytes, true) : NULL;
        void *indexStorage = indexed ? PlatformAllocate(indexBytes, true) : NULL;
        VoxelRigidContactCache cache = {0};
        VoxelRigidBroadphase broadphase = {0};
        if (bodies == NULL || scratch == NULL ||
            (cached && (cacheStorage == NULL ||
                        !VoxelRigidContactCacheInitialize(&cache, cacheStorage,
                                                           bodyCount, cacheBytes))) ||
            (indexed && (indexStorage == NULL ||
                         !VoxelRigidBroadphaseInitialize(&broadphase, indexStorage,
                                                          bodyCount, indexBytes))))
        {
            if (indexStorage != NULL) PlatformFree(indexStorage);
            if (cacheStorage != NULL) PlatformFree(cacheStorage);
            if (scratch != NULL) PlatformFree(scratch);
            if (bodies != NULL) PlatformFree(bodies);
            return false;
        }
        if (!InitializeDenseBodies(bodies, bodyCount))
        {
            if (indexStorage != NULL) PlatformFree(indexStorage);
            if (cacheStorage != NULL) PlatformFree(cacheStorage);
            PlatformFree(scratch);
            PlatformFree(bodies);
            return false;
        }

        bool succeeded = true;
        uint64_t candidates = 0u;
        uint64_t contacts = 0u;
        uint64_t matches = 0u;
        uint64_t updates = 0u;
        uint64_t visits = 0u;
        uint32_t peakContacts = 0u;
        for (uint32_t warmup = 0u; warmup < warmupSteps; ++warmup)
        {
            if (!DenseStep(bodies, bodyCount, &collision, &settings, scratch,
                            scratchBytes, cached ? &cache : NULL, indexed ? &broadphase : NULL))
            {
                succeeded = false;
                break;
            }
        }
        double start = PlatformMonotonicSeconds();
        for (uint32_t step = 0u; succeeded && step < STEP_COUNT; ++step)
        {
            VoxelRigidStepStats stats;
            if (!DenseStep(bodies, bodyCount, &collision, &settings, scratch,
                            scratchBytes, cached ? &cache : NULL, indexed ? &broadphase : NULL) ||
                !VoxelRigidBodyReadStepStats(scratch, bodyCount, scratchBytes, &stats) ||
                stats.activeBodyCount != bodyCount || stats.awakeBodyCount != bodyCount ||
                stats.contactCount == 0u)
            {
                succeeded = false;
                break;
            }
            candidates += stats.candidatePairCount;
            contacts += stats.contactCount;
            matches += cache.matchedContactCount;
            updates += broadphase.updatedProxyCount;
            visits += broadphase.visitedNodeCount;
            if (stats.contactCount > peakContacts) peakContacts = stats.contactCount;
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0 /
                          (double)STEP_COUNT;
        uint64_t stateHash = HashBodies(bodies, bodyCount);
        benchmarkSink ^= contacts;
        ReleaseBodies(bodies, bodyCount);
        PlatformFree(scratch);
        PlatformFree(bodies);
        if (cacheStorage != NULL) PlatformFree(cacheStorage);
        if (indexStorage != NULL) PlatformFree(indexStorage);
        if (!succeeded) return false;

        if (sample == 0u)
        {
            referenceCandidates = candidates;
            referenceContacts = contacts;
            referenceMatches = matches;
            referenceHash = stateHash;
            referencePeakContacts = peakContacts;
            referenceUpdates = updates;
            referenceVisits = visits;
        }
        else if (candidates != referenceCandidates || contacts != referenceContacts ||
                 matches != referenceMatches || stateHash != referenceHash ||
                 peakContacts != referencePeakContacts || updates != referenceUpdates ||
                 visits != referenceVisits)
        {
            WriteText("rigid.step.dense workload changed between reset samples\n");
            return false;
        }
    }

    WriteText("rigid.step.dense bodies=");
    WriteUnsigned(bodyCount);
    WriteText(" cached=");
    WriteUnsigned(cached ? 1u : 0u);
    WriteText(" warmup_steps=");
    WriteUnsigned(warmupSteps);
    WriteText(" indexed=");
    WriteUnsigned(indexed ? 1u : 0u);
    WriteText(" awake=");
    WriteUnsigned(bodyCount);
    WriteText(" solver_iterations=");
    WriteUnsigned(settings.solverIterations);
    WriteText(" seed=");
    WriteUnsigned(DENSE_SEED);
    WriteText(" median_ms=");
    WriteMilliseconds(Median(samples, SAMPLE_COUNT));
    WriteText(" candidate_pairs_avg=");
    WriteUnsigned(referenceCandidates / STEP_COUNT);
    WriteText(" contacts_avg=");
    WriteUnsigned(referenceContacts / STEP_COUNT);
    WriteText(" warm_contacts_avg=");
    WriteUnsigned(referenceMatches / STEP_COUNT);
    WriteText(" contacts_peak=");
    WriteUnsigned(referencePeakContacts);
    WriteText(" final_state_hash=");
    WriteUnsigned(referenceHash);
    WriteText(" proxy_updates_avg=");
    WriteUnsigned(referenceUpdates / STEP_COUNT);
    WriteText(" tree_visits_avg=");
    WriteUnsigned(referenceVisits / STEP_COUNT);
    WriteText(" samples=5 steps=8 reset_each_sample=1\n");
    *outHash = referenceHash;
    return true;
}

static bool RunDensePair(uint32_t bodyCount, bool cached, uint32_t warmupSteps)
{
    uint64_t gridHash = 0u;
    uint64_t indexHash = 0u;
    if (!RunDenseCase(bodyCount, cached, warmupSteps, false, &gridHash) ||
        !RunDenseCase(bodyCount, cached, warmupSteps, true, &indexHash)) return false;
    if (gridHash != indexHash)
    {
        WriteText("persistent broadphase changed dense physical state\n");
        return false;
    }
    return true;
}

static bool RunCase(uint32_t bodyCount, bool solidGround, bool resting)
{
    VoxelRigidBody *bodies = PlatformAllocate((size_t)bodyCount * sizeof(*bodies), true);
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(bodyCount);
    void *scratch = scratchBytes == 0u ? NULL : PlatformAllocate(scratchBytes, false);
    if (bodies == NULL || scratch == NULL || !InitializeBodies(bodies, bodyCount, resting))
    {
        if (scratch != NULL) PlatformFree(scratch);
        if (bodies != NULL) PlatformFree(bodies);
        return false;
    }

    BenchmarkWorld world = {.solidGround = solidGround};
    VoxelCollisionSource collision = {
        .context = &world,
        .queryBlockPhysics = QueryBlock,
        .queryDynamicColliders = NULL,
    };
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.solverIterations = 4u;

    for (uint32_t warmup = 0u; warmup < (resting ? 40u : 2u); ++warmup)
    {
        if (!VoxelRigidBodyStep(bodies, bodyCount, &collision, &settings,
                                scratch, scratchBytes))
        {
            for (uint32_t index = 0u; index < bodyCount; ++index)
            {
                VoxelRigidBodyRelease(&bodies[index]);
            }
            PlatformFree(scratch);
            PlatformFree(bodies);
            return false;
        }
    }

    if (resting)
    {
        uint32_t sleeping = 0u;
        for (uint32_t index = 0u; index < bodyCount; ++index)
        {
            if (bodies[index].sleeping) ++sleeping;
        }
        WriteText("rigid.sleeping bodies=");
        WriteUnsigned(sleeping);
        WriteText("/");
        WriteUnsigned(bodyCount);
        WriteText("\n");
    }

    double samples[SAMPLE_COUNT];
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t step = 0u; step < STEP_COUNT; ++step)
        {
            if (!VoxelRigidBodyStep(bodies, bodyCount, &collision, &settings,
                                    scratch, scratchBytes))
            {
                LaiueTestRuntimeExit(2);
            }
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0 /
                          (double)STEP_COUNT;
    }

    WriteText(resting ? "rigid.step.resting bodies="
                     : (solidGround ? "rigid.step.ground bodies=" : "rigid.step.empty bodies="));
    WriteUnsigned(bodyCount);
    WriteText(" median_ms=");
    WriteMilliseconds(Median(samples, SAMPLE_COUNT));
    WriteText(" bodies_per_second=");
    double milliseconds = Median(samples, SAMPLE_COUNT);
    WriteUnsigned(milliseconds > 0.0
                      ? (uint64_t)((double)bodyCount / (milliseconds / 1000.0))
                      : 0u);
    WriteText("\n");

    benchmarkSink ^= (uint64_t)bodies[bodyCount - 1u].stableId;
    for (uint32_t index = 0u; index < bodyCount; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
    PlatformFree(scratch);
    PlatformFree(bodies);
    return true;
}

LAIUE_TEST_ENTRY(PhysicsBenchmarkEntryPoint)
{
    WriteText("laiue rigid-body benchmark\n");
    // Cached and uncached modes have the same initial conditions, not necessarily
    // the same later contact workload: warm starting changes finite-iteration
    // approximations. Report contacts and hashes rather than claiming equivalence.
    if (!RunDensePair(512u, false, 0u) || !RunDensePair(512u, true, 0u) ||
        !RunDensePair(2048u, false, 0u) || !RunDensePair(2048u, true, 0u) ||
        !RunDensePair(512u, false, 128u) || !RunDensePair(512u, true, 128u))
    {
        WriteText("dense physics benchmark failed\n");
        LaiueTestRuntimeExit(1);
    }

    char denseOnly[2] = {0};
    if (PlatformGetEnvironmentUtf8("LAIUE_PHYSICS_BENCHMARK_DENSE_ONLY",
                                   denseOnly, sizeof(denseOnly)) == 1u &&
        denseOnly[0] == '1')
    {
        LAIUE_TEST_SUCCESS();
    }

    const uint32_t counts[] = {10000u, 50000u, 100000u};
    for (uint32_t index = 0u; index < sizeof(counts) / sizeof(counts[0]); ++index)
    {
        if (!RunCase(counts[index], false, false) || !RunCase(counts[index], true, false) ||
            !RunCase(counts[index], true, true))
        {
            WriteText("physics benchmark failed\n");
            LaiueTestRuntimeExit(1);
        }
    }
    if (benchmarkSink == UINT64_MAX) WriteText("");
    LAIUE_TEST_SUCCESS();
}
