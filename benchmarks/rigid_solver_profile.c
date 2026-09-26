// Узкий harness профиля шага rigid-body: печатает время каждой стадии шага
// через публичный VoxelRigidBodyStepProfile. Не входит в CTest. Нужен, чтобы
// выбрать гипотезу оптимизации по измерению, а не по догадке, и чтобы потом
// повторить тот же замер на candidate.

#include "physics/numeric_provider.h"
#include "physics/rigid_body.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define PROFILE_SAMPLE_COUNT 5u
#define PROFILE_STEP_COUNT 8u
#define PROFILE_DENSE_SEED 0x6a09e667f3bcc909ull

static volatile uint64_t profileSink;

typedef struct ProfileWorld
{
    bool solidGround;
} ProfileWorld;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
        digits[length++] = '0';
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
    if (fraction < 100u)
        WriteText("0");
    if (fraction < 10u)
        WriteText("0");
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

static void QueryBlock(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *outPhysics)
{
    ProfileWorld *world = (ProfileWorld *)context;
    (void)x;
    (void)y;
    (void)z;
    bool solid = world->solidGround && z < 0;
    outPhysics->flags = solid ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outPhysics->friction = 0.6f;
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state;
}

static bool InitializeDenseBodies(VoxelRigidBody *bodies, uint32_t count)
{
    uint64_t randomState = PROFILE_DENSE_SEED;
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyDescription description = {0};
        description.position[0] = (double)(index % 16u);
        description.position[1] = (double)((index / 16u) % 16u);
        uint32_t layer = index / 256u;
        description.position[2] = 0.44 + (double)layer * 0.89;
        description.halfExtent[0] = 0.45;
        description.halfExtent[1] = 0.45;
        description.halfExtent[2] = 0.45;
        description.mass = 1.0;
        description.restitution = 0.0;
        description.friction = 0.6;
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
        {
            for (uint32_t release = 0u; release < index; ++release)
                VoxelRigidBodyRelease(&bodies[release]);
            return false;
        }
        uint64_t random = NextRandom(&randomState);
        double velocity[3] = {(double)((int32_t)(random & 15u) - 8) / 512.0,
                              (double)((int32_t)((random >> 8u) & 15u) - 8) / 512.0, 0.0};
        if (!VoxelRigidBodyAddLinearVelocity(&bodies[index], velocity))
        {
            for (uint32_t release = 0u; release <= index; ++release)
                VoxelRigidBodyRelease(&bodies[release]);
            return false;
        }
    }
    return true;
}

static double ClockSeconds(void *context)
{
    (void)context;
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    static bool initialized = false;
    if (!initialized)
    {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    return PlatformMonotonicSeconds();
#endif
}

static VoxelRigidSolverOrder profileOrder = VOXEL_RIGID_SOLVER_CANONICAL;

static bool StepOnce(VoxelRigidBody *bodies, uint32_t bodyCount, const VoxelCollisionSource *collision,
                     const VoxelRigidStepSettings *settings, void *scratch, uint32_t scratchBytes,
                     VoxelRigidContactCache *cache, VoxelRigidBroadphase *broadphase,
                     VoxelRigidStepProfile *profile)
{
    VoxelRigidStepOptions options = {0};
    options.structSize = sizeof(options);
    options.contactCache = cache;
    options.broadphase = broadphase;
    options.executor = NULL;
    options.solverOrder = profileOrder;
    options.profile = profile;
    options.clockSeconds = ClockSeconds;
    options.clockContext = NULL;
    return VoxelRigidBodyStepEx(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                &options);
}

static const char *StageName(uint32_t stage)
{
    static const char *names[VOXEL_RIGID_PROFILE_STAGE_COUNT] = {
        "order",  "forces", "bounds",   "broadphase", "wake",     "world",  "body",
        "prepare", "warm",   "schedule", "solve",      "integrate", "sleep", "store"};
    return stage < VOXEL_RIGID_PROFILE_STAGE_COUNT ? names[stage] : "?";
}

static bool RunDenseProfile(uint32_t bodyCount, bool cached, bool indexed, uint32_t solverIterations,
                            VoxelRigidSolverOrder order)
{
    profileOrder = order;
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(bodyCount);
    uint32_t cacheBytes = cached ? VoxelRigidContactCacheBytes(bodyCount) : 0u;
    uint32_t indexBytes = indexed ? VoxelRigidBroadphaseBytes(bodyCount) : 0u;
    if (scratchBytes == 0u || (cached && cacheBytes == 0u) || (indexed && indexBytes == 0u))
        return false;

    ProfileWorld world = {.solidGround = true};
    VoxelCollisionSource collision = {
        .context = &world, .queryBlockPhysics = QueryBlock, .queryDynamicColliders = NULL};
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepLinearSpeed = 0.0;
    settings.sleepAngularSpeed = 0.0;
    settings.solverIterations = solverIterations;

    double samples[PROFILE_SAMPLE_COUNT];
    double stageTotals[VOXEL_RIGID_PROFILE_STAGE_COUNT] = {0.0};
    uint64_t referenceContacts = 0u;
    uint64_t referenceMatches = 0u;
    uint32_t referencePeak = 0u;
    for (uint32_t sample = 0u; sample < PROFILE_SAMPLE_COUNT; ++sample)
    {
        VoxelRigidBody *bodies = PlatformAllocate((size_t)bodyCount * sizeof(*bodies), true);
        void *scratch = PlatformAllocate(scratchBytes, true);
        void *cacheStorage = cached ? PlatformAllocate(cacheBytes, true) : NULL;
        void *indexStorage = indexed ? PlatformAllocate(indexBytes, true) : NULL;
        VoxelRigidContactCache cache = {0};
        VoxelRigidBroadphase broadphase = {0};
        if (bodies == NULL || scratch == NULL ||
            (cached && (cacheStorage == NULL ||
                        !VoxelRigidContactCacheInitialize(&cache, cacheStorage, bodyCount, cacheBytes))) ||
            (indexed && (indexStorage == NULL ||
                         !VoxelRigidBroadphaseInitialize(&broadphase, indexStorage, bodyCount,
                                                         indexBytes))))
        {
            if (indexStorage != NULL)
                PlatformFree(indexStorage);
            if (cacheStorage != NULL)
                PlatformFree(cacheStorage);
            if (scratch != NULL)
                PlatformFree(scratch);
            if (bodies != NULL)
                PlatformFree(bodies);
            return false;
        }
        if (!InitializeDenseBodies(bodies, bodyCount))
        {
            PlatformFree(indexStorage);
            PlatformFree(cacheStorage);
            PlatformFree(scratch);
            PlatformFree(bodies);
            return false;
        }

        bool succeeded = true;
        double stageSum[VOXEL_RIGID_PROFILE_STAGE_COUNT] = {0.0};
        uint64_t contacts = 0u;
        uint64_t matches = 0u;
        uint32_t peak = 0u;
        double start = PlatformMonotonicSeconds();
        for (uint32_t step = 0u; step < PROFILE_STEP_COUNT; ++step)
        {
            VoxelRigidStepProfile profile = {0};
            profile.structSize = sizeof(profile);
            VoxelRigidStepStats stats;
            if (!StepOnce(bodies, bodyCount, &collision, &settings, scratch, scratchBytes,
                          cached ? &cache : NULL, indexed ? &broadphase : NULL, &profile) ||
                !VoxelRigidBodyReadStepStats(scratch, bodyCount, scratchBytes, &stats) ||
                stats.contactCount == 0u)
            {
                succeeded = false;
                break;
            }
            for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
                stageSum[stage] += profile.seconds[stage];
            contacts += stats.contactCount;
            matches += cache.matchedContactCount;
            if (stats.contactCount > peak)
                peak = stats.contactCount;
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0 / (double)PROFILE_STEP_COUNT;
        for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
            stageTotals[stage] += stageSum[stage] / (double)PROFILE_STEP_COUNT;
        if (sample == 0u)
        {
            referenceContacts = contacts;
            referenceMatches = matches;
            referencePeak = peak;
        }
        profileSink ^= contacts;
        for (uint32_t index = 0u; index < bodyCount; ++index)
            VoxelRigidBodyRelease(&bodies[index]);
        if (indexStorage != NULL)
            PlatformFree(indexStorage);
        if (cacheStorage != NULL)
            PlatformFree(cacheStorage);
        PlatformFree(scratch);
        PlatformFree(bodies);
        if (!succeeded)
            return false;
    }

    WriteText("profile.dense bodies=");
    WriteUnsigned(bodyCount);
    WriteText(" cached=");
    WriteUnsigned(cached ? 1u : 0u);
    WriteText(" indexed=");
    WriteUnsigned(indexed ? 1u : 0u);
    WriteText(" order=");
    WriteUnsigned(order == VOXEL_RIGID_SOLVER_COLORED ? 1u : 0u);
    WriteText(" iterations=");
    WriteUnsigned(solverIterations);
    WriteText(" median_ms=");
    WriteMilliseconds(Median(samples, PROFILE_SAMPLE_COUNT));
    WriteText(" contacts_avg=");
    WriteUnsigned(referenceContacts / PROFILE_STEP_COUNT);
    WriteText(" warm_avg=");
    WriteUnsigned(referenceMatches / PROFILE_STEP_COUNT);
    WriteText(" contacts_peak=");
    WriteUnsigned(referencePeak);
    WriteText(" stages_ms=");
    for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
    {
        WriteText(StageName(stage));
        WriteText(":");
        WriteMilliseconds(stageTotals[stage] / (double)PROFILE_SAMPLE_COUNT * 1000.0);
        if (stage + 1u < VOXEL_RIGID_PROFILE_STAGE_COUNT)
            WriteText(",");
    }
    WriteText(" samples=");
    WriteUnsigned(PROFILE_SAMPLE_COUNT);
    WriteText(" steps=");
    WriteUnsigned(PROFILE_STEP_COUNT);
    WriteText("\n");
    return true;
}

LAIUE_TEST_ENTRY(RigidSolverProfileEntryPoint)
{
    const LaiueNumericServiceV1 *numeric = LaiueNumericGetStaticServiceV1();
    if (numeric == NULL)
    {
        WriteText("rigid profile numeric service unavailable\n");
        LaiueTestRuntimeExit(1);
    }
    PhysicsSetNumericService(numeric);
    if (!RunDenseProfile(512u, false, false, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(512u, true, false, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(2048u, false, false, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(2048u, true, false, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(2048u, false, true, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(2048u, true, true, 8u, VOXEL_RIGID_SOLVER_CANONICAL) ||
        !RunDenseProfile(512u, false, false, 8u, VOXEL_RIGID_SOLVER_COLORED) ||
        !RunDenseProfile(2048u, false, false, 8u, VOXEL_RIGID_SOLVER_COLORED))
    {
        WriteText("rigid profile failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (profileSink == UINT64_MAX)
        WriteText("");
    LAIUE_TEST_SUCCESS();
}
