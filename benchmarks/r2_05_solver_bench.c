// Узкий harness ROUND 2 агента 05-solver: измеряет шаг rigid-body на нагрузках
// "active" (dense) и "parallel" (executor + цветной решатель + кэш импульсов +
// индекс broadphase). Числа baseline/candidate сравнивает внешний A/B-скрипт,
// подменяя только laiue_physics.dll; поэтому harness печатает ещё и хеш
// конечного состояния, чтобы расхождение физики было видно сразу.
//
// Не входит в CTest и в ALL: запускается явно. Параметры — через переменные
// окружения (no-CRT entry point не получает argv):
//   LAIUE_R2_05_SCENARIO = dense | ground | stack  (по умолчанию dense)
//   LAIUE_R2_05_BODIES   = число тел               (по умолчанию 2048)
//   LAIUE_R2_05_PARALLEL = 0 | 1                    (по умолчанию 0)
//   LAIUE_R2_05_THREADS  = число worker-потоков пула (по умолчанию 4)
//   LAIUE_R2_05_ITERS    = итераций решателя        (по умолчанию 8)
//   LAIUE_R2_05_SAMPLES  = число проб               (по умолчанию 7)
//   LAIUE_R2_05_STEPS    = шагов в пробе            (по умолчанию 8)

#include "physics/numeric_provider.h"
#include "physics/rigid_body.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define R2_05_DEFAULT_BODIES 2048u
#define R2_05_DEFAULT_THREADS 4u
#define R2_05_DEFAULT_ITERS 8u
#define R2_05_DEFAULT_SAMPLES 7u
#define R2_05_DEFAULT_STEPS 8u
#define R2_05_WARMUP_STEPS 8u
#define R2_05_DENSE_SEED 0x6a09e667f3bcc909ull
#define R2_05_STACK_HEIGHT 8u

static volatile uint64_t r2Sink;

typedef struct R2World
{
    bool solidGround;
} R2World;

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
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - index - 1u];
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

static void QueryBlock(void *context, int64_t x, int64_t y, int64_t z,
                       VoxelBlockPhysics *outPhysics)
{
    R2World *world = (R2World *)context;
    (void)x;
    (void)y;
    outPhysics->flags = world->solidGround && z < 0 ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outPhysics->friction = 0.6f;
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state;
}

static uint32_t EnvironmentUnsigned(const char *name, uint32_t fallback)
{
    char text[24] = {0};
    if (PlatformGetEnvironmentUtf8(name, text, sizeof(text)) == 0u)
        return fallback;
    uint32_t value = 0u;
    uint32_t index = 0u;
    while (text[index] >= '0' && text[index] <= '9')
    {
        value = value * 10u + (uint32_t)(text[index] - '0');
        ++index;
    }
    return index == 0u ? fallback : value;
}

static bool EnvironmentEquals(const char *name, const char *expected)
{
    char text[24] = {0};
    if (PlatformGetEnvironmentUtf8(name, text, sizeof(text)) == 0u)
        return false;
    uint32_t index = 0u;
    while (text[index] != '\0' && expected[index] != '\0')
    {
        if (text[index] != expected[index])
            return false;
        ++index;
    }
    return text[index] == '\0' && expected[index] == '\0';
}

// dense/active: колонны с намеренным вертикальным перекрытием 0.01 и случайной
// начальной горизонтальной скоростью, как в physics_benchmark.
static bool InitializeDense(VoxelRigidBody *bodies, uint32_t count)
{
    uint64_t randomState = R2_05_DENSE_SEED;
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
            return false;
        uint64_t random = NextRandom(&randomState);
        double velocity[3] = {(double)((int32_t)(random & 15u) - 8) / 512.0,
                              (double)((int32_t)((random >> 8u) & 15u) - 8) / 512.0, 0.0};
        if (!VoxelRigidBodyAddLinearVelocity(&bodies[index], velocity))
            return false;
    }
    return true;
}

// stack: колонны выше, чем dense, при той же плотности контактов: тела стоят
// друг на друге в столбцах высотой R2_05_STACK_HEIGHT без горизонтальных
// соседей, то есть решаются в основном вертикальные контакты.
static bool InitializeStack(VoxelRigidBody *bodies, uint32_t count)
{
    uint32_t height = R2_05_STACK_HEIGHT;
    uint32_t columns = count / height;
    if (columns == 0u)
        return false;
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyDescription description = {0};
        uint32_t column = index / height;
        uint32_t layer = index % height;
        description.position[0] = (double)(column % 16u);
        description.position[1] = (double)((column / 16u) % 16u);
        description.position[2] = 0.44 + (double)layer * 0.89;
        description.halfExtent[0] = 0.45;
        description.halfExtent[1] = 0.45;
        description.halfExtent[2] = 0.45;
        description.mass = 1.0;
        description.restitution = 0.0;
        description.friction = 0.6;
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
            return false;
    }
    return true;
}

// ground: тело над полом без вертикальных соседей — нагрузка в основном на
// world-контакты и широкий отбор.
static bool InitializeGround(VoxelRigidBody *bodies, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelRigidBodyDescription description = {0};
        description.position[0] = (double)(index % 512u) * 2.0;
        description.position[1] = (double)((index / 512u) % 512u) * 2.0;
        description.position[2] = 0.49;
        description.halfExtent[0] = 0.45;
        description.halfExtent[1] = 0.45;
        description.halfExtent[2] = 0.45;
        description.mass = 1.0;
        description.restitution = 0.0;
        description.friction = 0.6;
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
            return false;
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

static uint64_t HashCoordinate(uint64_t hash, const InfiniteCoord *coordinate)
{
    hash = HashWord(hash, (uint64_t)(int64_t)coordinate->sign);
    hash = HashWord(hash, coordinate->limbCount);
    for (uint32_t limb = 0u; limb < coordinate->limbCount; ++limb)
        hash = HashWord(hash, coordinate->limbs[limb]);
    return hash;
}

static uint64_t HashDouble(uint64_t hash, double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return HashWord(hash, representation.bits);
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
            hash = HashDouble(hash, body->orientation[component]);
        hash = HashDouble(hash, body->inverseMass);
        hash = HashDouble(hash, body->restitution);
        hash = HashDouble(hash, body->friction);
    }
    return hash;
}

static bool StepOnce(VoxelRigidBody *bodies, uint32_t bodyCount, const VoxelCollisionSource *collision,
                     const VoxelRigidStepSettings *settings, void *scratch, uint32_t scratchBytes,
                     VoxelRigidContactCache *cache, VoxelRigidBroadphase *broadphase,
                     const LaiueTaskExecutor *executor)
{
    VoxelRigidStepOptions options = {0};
    options.structSize = sizeof(options);
    options.contactCache = cache;
    options.broadphase = broadphase;
    options.executor = executor;
    options.solverOrder = executor != NULL ? VOXEL_RIGID_SOLVER_COLORED : VOXEL_RIGID_SOLVER_CANONICAL;
    return VoxelRigidBodyStepEx(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                &options);
}

static bool RunScenario(const char *scenario, uint32_t bodyCount, bool parallel, uint32_t threads,
                        uint32_t iterations, uint32_t samples, bool sleepEnabled,
                        uint32_t warmupSteps)
{
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(bodyCount);
    uint32_t cacheBytes = parallel ? VoxelRigidContactCacheBytes(bodyCount) : 0u;
    uint32_t indexBytes = parallel ? VoxelRigidBroadphaseBytes(bodyCount) : 0u;
    if (scratchBytes == 0u || (parallel && (cacheBytes == 0u || indexBytes == 0u)))
        return false;

    R2World world = {.solidGround = !EnvironmentEquals("LAIUE_R2_05_SCENARIO", "empty")};
    VoxelCollisionSource collision = {&world, QueryBlock, NULL};
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.solverIterations = iterations;
    if (!sleepEnabled)
    {
        // Цель "active"/"stack" — измерить решатель, а не ранний выход сна.
        settings.sleepLinearSpeed = 0.0;
        settings.sleepAngularSpeed = 0.0;
    }

    LaiueTaskPool *pool = NULL;
    LaiueTaskExecutor executor = {0};
    if (parallel)
    {
        executor.structSize = sizeof(executor);
        pool = LaiueTaskPoolCreate(threads);
        if (pool == NULL || !LaiueTaskPoolGetExecutor(pool, &executor))
        {
            if (pool != NULL)
                LaiueTaskPoolDestroy(pool);
            return false;
        }
    }

    VoxelRigidBody *bodies = PlatformAllocate((size_t)bodyCount * sizeof(*bodies), true);
    void *scratchMemory = PlatformAllocate(scratchBytes, true);
    void *cacheStorage = parallel ? PlatformAllocate(cacheBytes, true) : NULL;
    void *indexStorage = parallel ? PlatformAllocate(indexBytes, true) : NULL;
    VoxelRigidContactCache cache = {0};
    VoxelRigidBroadphase broadphase = {0};
    bool ok = bodies != NULL && scratchMemory != NULL && (!parallel || cacheStorage != NULL) &&
              (!parallel || indexStorage != NULL);
    if (ok && parallel)
    {
        ok = VoxelRigidContactCacheInitialize(&cache, cacheStorage, bodyCount, cacheBytes) &&
             VoxelRigidBroadphaseInitialize(&broadphase, indexStorage, bodyCount, indexBytes);
    }
    if (ok)
    {
        if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "ground") ||
            EnvironmentEquals("LAIUE_R2_05_SCENARIO", "resting"))
            ok = InitializeGround(bodies, bodyCount);
        else if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "stack"))
            ok = InitializeStack(bodies, bodyCount);
        else
            ok = InitializeDense(bodies, bodyCount);
    }
    if (!ok)
    {
        if (bodies != NULL)
            PlatformFree(bodies);
        if (scratchMemory != NULL)
            PlatformFree(scratchMemory);
        if (cacheStorage != NULL)
            PlatformFree(cacheStorage);
        if (indexStorage != NULL)
            PlatformFree(indexStorage);
        if (pool != NULL)
            LaiueTaskPoolDestroy(pool);
        return false;
    }

    double *sampleTimes = PlatformAllocate((size_t)samples * sizeof(double), true);
    if (sampleTimes == NULL)
        ok = false;
    uint64_t hash = 0u;
    uint64_t contactsTotal = 0u;
    uint32_t referencePeak = 0u;
    for (uint32_t sample = 0u; ok && sample < samples; ++sample)
    {
        // Каждая проба — с тем же стартовым состоянием: тела переинициализируются
        // вне замера, поэтому прогреваемая сцена не «оседает» между пробами.
        for (uint32_t index = 0u; index < bodyCount; ++index)
            VoxelRigidBodyRelease(&bodies[index]);
        if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "ground") ||
            EnvironmentEquals("LAIUE_R2_05_SCENARIO", "resting"))
            ok = InitializeGround(bodies, bodyCount);
        else if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "stack"))
            ok = InitializeStack(bodies, bodyCount);
        else
            ok = InitializeDense(bodies, bodyCount);
        if (!ok)
            break;
        for (uint32_t warmup = 0u; warmup < warmupSteps; ++warmup)
        {
            if (!StepOnce(bodies, bodyCount, &collision, &settings, scratchMemory, scratchBytes,
                          parallel ? &cache : NULL, parallel ? &broadphase : NULL,
                          parallel ? &executor : NULL))
            {
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
        uint64_t contacts = 0u;
        uint32_t peak = 0u;
        double start = PlatformMonotonicSeconds();
        for (uint32_t step = 0u; step < R2_05_DEFAULT_STEPS; ++step)
        {
            VoxelRigidStepStats stats;
            if (!StepOnce(bodies, bodyCount, &collision, &settings, scratchMemory, scratchBytes,
                          parallel ? &cache : NULL, parallel ? &broadphase : NULL,
                          parallel ? &executor : NULL) ||
                !VoxelRigidBodyReadStepStats(scratchMemory, bodyCount, scratchBytes, &stats))
            {
                ok = false;
                break;
            }
            contacts += stats.contactCount;
            if (stats.contactCount > peak)
                peak = stats.contactCount;
        }
        if (!ok)
            break;
        sampleTimes[sample] =
            (PlatformMonotonicSeconds() - start) * 1000.0 / (double)R2_05_DEFAULT_STEPS;
        hash = HashBodies(bodies, bodyCount);
        if (sample == 0u)
        {
            contactsTotal = contacts;
            referencePeak = peak;
        }
        else if (contacts != contactsTotal || peak != referencePeak)
        {
            ok = false;
            break;
        }
    }

    if (ok)
    {
        WriteText("r2_05 scenario=");
        WriteText(scenario);
        WriteText(" parallel=");
        WriteUnsigned(parallel ? 1u : 0u);
        WriteText(" bodies=");
        WriteUnsigned(bodyCount);
        WriteText(" iterations=");
        WriteUnsigned(iterations);
        WriteText(" median_ms=");
        WriteMilliseconds(Median(sampleTimes, samples));
        WriteText(" contacts_avg=");
        WriteUnsigned(contactsTotal / R2_05_DEFAULT_STEPS);
        WriteText(" contacts_peak=");
        WriteUnsigned(referencePeak);
        WriteText(" final_state_hash=");
        WriteUnsigned(hash);
        WriteText(" samples=");
        WriteUnsigned(samples);
        WriteText(" steps=");
        WriteUnsigned(R2_05_DEFAULT_STEPS);
        WriteText("\n");
        r2Sink ^= hash;
    }

    for (uint32_t index = 0u; index < bodyCount; ++index)
        VoxelRigidBodyRelease(&bodies[index]);
    if (sampleTimes != NULL)
        PlatformFree(sampleTimes);
    if (bodies != NULL)
        PlatformFree(bodies);
    if (scratchMemory != NULL)
        PlatformFree(scratchMemory);
    if (cacheStorage != NULL)
        PlatformFree(cacheStorage);
    if (indexStorage != NULL)
        PlatformFree(indexStorage);
    if (pool != NULL)
        LaiueTaskPoolDestroy(pool);
    return ok;
}

LAIUE_TEST_ENTRY(R2SolverBenchmarkEntryPoint)
{
    // laiue_physics obtains arbitrary-precision arithmetic through the numeric
    // service table; a bare process that links both DLLs must inject it the same
    // way tests do, before any body is created.
    const LaiueNumericServiceV1 *numeric = LaiueNumericGetStaticServiceV1();
    if (numeric == NULL)
    {
        WriteText("r2_05 numeric service unavailable\n");
        LaiueTestRuntimeExit(1);
    }
    PhysicsSetNumericService(numeric);
    const char *scenario = "dense";
    if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "stack"))
        scenario = "stack";
    else if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "ground"))
        scenario = "ground";
    else if (EnvironmentEquals("LAIUE_R2_05_SCENARIO", "resting"))
        scenario = "resting";
    uint32_t bodyCount = EnvironmentUnsigned("LAIUE_R2_05_BODIES", R2_05_DEFAULT_BODIES);
    uint32_t threads = EnvironmentUnsigned("LAIUE_R2_05_THREADS", R2_05_DEFAULT_THREADS);
    uint32_t iterations = EnvironmentUnsigned("LAIUE_R2_05_ITERS", R2_05_DEFAULT_ITERS);
    uint32_t samples = EnvironmentUnsigned("LAIUE_R2_05_SAMPLES", R2_05_DEFAULT_SAMPLES);
    bool parallel = EnvironmentUnsigned("LAIUE_R2_05_PARALLEL", 0u) != 0u;
    bool sleepEnabled = EnvironmentUnsigned("LAIUE_R2_05_SLEEP", 0u) != 0u ||
                        EnvironmentEquals("LAIUE_R2_05_SCENARIO", "resting");
    uint32_t warmupSteps = EnvironmentUnsigned("LAIUE_R2_05_WARMUP", R2_05_WARMUP_STEPS);
    if (sleepEnabled && warmupSteps < 64u)
        warmupSteps = 64u;
    if (bodyCount == 0u || samples == 0u || iterations == 0u)
    {
        WriteText("r2_05 invalid parameters\n");
        LaiueTestRuntimeExit(1);
    }
    if (!RunScenario(scenario, bodyCount, parallel, threads, iterations, samples, sleepEnabled,
                     warmupSteps))
    {
        WriteText("r2_05 scenario failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (r2Sink == UINT64_MAX)
        WriteText("");
    LAIUE_TEST_SUCCESS();
}
