// Ручной benchmark горячего шага rigid-body физики. Не входит в CTest:
// запускается явно, чтобы сравнивать изменения на одной машине.

#include "physics/rigid_body.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 5u
#define STEP_COUNT 8u

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
