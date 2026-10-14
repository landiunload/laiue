#include "physics/rigid_body.h"
#include "physics/rigid_broadphase.h"
#include "physics/numeric_provider.h"
#include "task/task_pool.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#define TEST_CAPACITY 96u
#define FIXTURE_COUNT 3u
#define REPLAY_BODY_COUNT 64u
#define REPLAY_TICKS 512u
#define TEST_SCRATCH_BYTES 1048576u
#define TEST_CACHE_BYTES 262144u
#define TEST_BROADPHASE_BYTES 262144u
#define TEST_RANGE_CAPACITY (TEST_CAPACITY * VOXEL_RIGID_CONTACTS_PER_BODY)

typedef struct PhysicsFixture
{
    VoxelRigidBody bodies[TEST_CAPACITY];
    uint8_t scratch[TEST_SCRATCH_BYTES];
    uint8_t cacheStorage[TEST_CACHE_BYTES];
    uint8_t broadphaseStorage[TEST_BROADPHASE_BYTES];
    VoxelRigidBroadphase broadphase;
    VoxelRigidContactCache cache;
    VoxelRigidStepProfile profile;
    uint32_t clockCalls;
} PhysicsFixture;

typedef struct HostileRangeJob
{
    LaiueTaskRangeFunction function;
    void *context;
    uint8_t *rangeStarts;
} HostileRangeJob;

static PhysicsFixture fixtures[FIXTURE_COUNT];
static LaiueTaskExecutor poolExecutor;
static LaiueTaskExecutor hostileExecutor;
static LaiueTaskExecutor reverseExecutor;
static bool insideExecutor;
static bool floorEnabled;
// Дерево широкого отбора включается отдельно: у сеточного и у древесного
// путей узкая фаза разная, и параллельной проверки требуют оба.
static bool indexedBroadphase;
static int64_t worldOrigin[3];
static uint64_t queryHash;
static uint32_t hostileDispatches;
static uint32_t reverseDispatches;
static uint32_t hostileRangeCalls;
static uint32_t reverseRangeCalls;
static uint32_t splitRangeLimit;
static union
{
    VoxelRigidStepOptions options;
    VoxelRigidStepProfile profile;
    uint8_t bytes[TEST_SCRATCH_BYTES];
} aliasBuffer;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Rigid parallel failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
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
            return false;
    }
    for (uint32_t component = 0u; component < 4u; ++component)
    {
        if (DoubleBits(first->orientation[component]) != DoubleBits(second->orientation[component]))
            return false;
    }
    return true;
}

static void HostileRange(void *context, uint32_t begin, uint32_t end)
{
    const HostileRangeJob *job = context;
    // Every range has a distinct begin index. Publish independent slots and
    // count them on the coordinator after the executor's completion barrier.
    job->rangeStarts[begin] = 1u;
    LaiueTestSetHostileFpEnvironment();
    job->function(job->context, begin, end);
    Expect(VoxelPhysicsThreadIsConfigured(), "physics range restores worker FP environment");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void HostileRun(void *context, uint32_t count, uint32_t grain,
                       LaiueTaskRangeFunction function, void *jobContext)
{
    const LaiueTaskExecutor *executor = context;
    Expect(count <= TEST_RANGE_CAPACITY, "instrumented range storage sufficient");
    uint8_t rangeStarts[TEST_RANGE_CAPACITY];
    for (uint32_t index = 0u; index < count; ++index)
        rangeStarts[index] = 0u;
    HostileRangeJob job = {function, jobContext, rangeStarts};
    ++hostileDispatches;
    insideExecutor = true;
    uint32_t rangeSize = grain == 0u ? 1u : grain;
    if (rangeSize > 8u)
        rangeSize = 8u;
    if (splitRangeLimit != 0u && rangeSize > splitRangeLimit)
        rangeSize = splitRangeLimit;
    executor->run(executor->context, count, rangeSize, HostileRange, &job);
    insideExecutor = false;
    for (uint32_t index = 0u; index < count; ++index)
        hostileRangeCalls += rangeStarts[index];
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ReverseRun(void *context, uint32_t count, uint32_t grain,
                       LaiueTaskRangeFunction function, void *jobContext)
{
    (void)context;
    ++reverseDispatches;
    uint32_t rangeSize = grain == 0u ? 1u : grain;
    if (splitRangeLimit != 0u && rangeSize > splitRangeLimit)
        rangeSize = splitRangeLimit;
    insideExecutor = true;
    for (uint32_t end = count; end != 0u;)
    {
        uint32_t length = end < rangeSize ? end : rangeSize;
        uint32_t begin = end - length;
        LaiueTestSetHostileFpEnvironment();
        function(jobContext, begin, end);
        ++reverseRangeCalls;
        Expect(VoxelPhysicsThreadIsConfigured(), "reverse range restores FP environment");
        end = begin;
    }
    insideExecutor = false;
}

static double FakeClock(void *context)
{
    Expect(!insideExecutor, "profile clock remains outside executor callbacks");
    PhysicsFixture *fixture = context;
    ++fixture->clockCalls;
    return (double)fixture->clockCalls / 1024.0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryFloor(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *block)
{
    (void)context;
    Expect(!insideExecutor, "world callbacks remain on coordinator");
    queryHash = HashWord(HashWord(HashWord(queryHash, (uint64_t)x), (uint64_t)y), (uint64_t)z);
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
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        description.halfExtent[axis] = 0.5;
    description.mass = 1.0;
    description.position[2] = 2.0;
    description.friction = 0.5;
    return description;
}

static void ReleaseFixtures(void)
{
    for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
    {
        for (uint32_t slot = 0u; slot < TEST_CAPACITY; ++slot)
        {
            VoxelRigidBodyRelease(&fixtures[fixture].bodies[slot]);
        }
    }
}

static void ResetFixtures(void)
{
    ReleaseFixtures();
    floorEnabled = true;
    indexedBroadphase = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        worldOrigin[axis] = 0;
    uint32_t cacheBytes = VoxelRigidContactCacheBytes(TEST_CAPACITY);
    Expect(cacheBytes != 0u && cacheBytes <= TEST_CACHE_BYTES, "cache storage sufficient");
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(TEST_CAPACITY);
    Expect(scratchBytes != 0u && scratchBytes <= TEST_SCRATCH_BYTES, "scratch storage sufficient");
    for (uint32_t index = 0u; index < FIXTURE_COUNT; ++index)
    {
        PhysicsFixture *fixture = &fixtures[index];
        Expect(VoxelRigidContactCacheInitialize(&fixture->cache, fixture->cacheStorage,
                                                TEST_CAPACITY, TEST_CACHE_BYTES),
               "cache initialized");
        uint32_t indexBytes = VoxelRigidBroadphaseBytes(TEST_CAPACITY);
        Expect(indexBytes != 0u && indexBytes <= TEST_BROADPHASE_BYTES,
               "broadphase storage sufficient");
        Expect(VoxelRigidBroadphaseInitialize(&fixture->broadphase, fixture->broadphaseStorage,
                                              TEST_CAPACITY, TEST_BROADPHASE_BYTES),
               "broadphase initialized");
        fixture->profile = (VoxelRigidStepProfile){.structSize = sizeof(VoxelRigidStepProfile)};
        fixture->clockCalls = 0u;
    }
}

static void InitializeBody(uint32_t slot, const VoxelRigidBodyDescription *description)
{
    for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
    {
        LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyInitialize(&fixtures[fixture].bodies[slot], (uint64_t)slot + 1u,
                                        description),
               "body initialized");
    }
}

static VoxelRigidBody *FindBody(PhysicsFixture *fixture, uint32_t count, uint64_t id)
{
    for (uint32_t slot = 0u; slot < count; ++slot)
    {
        if (fixture->bodies[slot].stableId == id)
            return &fixture->bodies[slot];
    }
    Expect(false, "stable body id exists");
    return NULL;
}

static void AddVelocity(uint32_t count, uint64_t id, const double velocity[3])
{
    for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
    {
        LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyAddLinearVelocity(FindBody(&fixtures[fixture], count, id), velocity),
               "external velocity applied");
    }
}

static VoxelRigidStepOptions Options(PhysicsFixture *fixture, VoxelRigidSolverOrder order,
                                     const LaiueTaskExecutor *executor)
{
    VoxelRigidStepOptions options = {0};
    options.structSize = sizeof(options);
    options.contactCache = &fixture->cache;
    options.broadphase = indexedBroadphase ? &fixture->broadphase : NULL;
    options.executor = executor;
    options.solverOrder = order;
    options.profile = &fixture->profile;
    options.clockSeconds = FakeClock;
    options.clockContext = fixture;
    return options;
}

static void StepAll(uint32_t count, const VoxelRigidStepSettings *settings,
                    VoxelRigidSolverOrder order)
{
    VoxelCollisionSource collision = Collision();
    uint64_t expectedQueries = 0u;
    VoxelRigidStepStats expectedStats = {0};
    for (uint32_t index = 0u; index < FIXTURE_COUNT; ++index)
    {
        PhysicsFixture *fixture = &fixtures[index];
        const LaiueTaskExecutor *executor =
            index == 0u ? NULL : (index == 1u ? &hostileExecutor : &reverseExecutor);
        VoxelRigidStepOptions options = Options(fixture, order, executor);
        queryHash = UINT64_C(14695981039346656037);
        LaiueTestSetHostileFpEnvironment();
        Expect(VoxelRigidBodyStepEx(fixture->bodies, count, &collision, settings, fixture->scratch,
                                    TEST_SCRATCH_BYTES, &options),
               "step completed");
        Expect(VoxelPhysicsThreadIsConfigured(), "coordinator FP environment normalized");
        VoxelRigidStepStats stats;
        Expect(VoxelRigidBodyReadStepStats(fixture->scratch, count, TEST_SCRATCH_BYTES, &stats),
               "step counters available");
        if (index == 0u)
            expectedStats = stats;
        else
        {
            Expect(stats.activeBodyCount == expectedStats.activeBodyCount &&
                       stats.awakeBodyCount == expectedStats.awakeBodyCount &&
                       stats.candidatePairCount == expectedStats.candidatePairCount &&
                       stats.contactCount == expectedStats.contactCount,
                   "active, awake, candidate and contact counters independent of executor");
        }
        if (index == 0u)
            expectedQueries = queryHash;
        else
            Expect(queryHash == expectedQueries, "world callback order independent of executor");
        Expect(fixture->profile.structSize == sizeof(fixture->profile), "profile size preserved");
        for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
        {
            Expect(fixture->profile.seconds[stage] >= 0.0 && fixture->profile.seconds[stage] < 1.0,
                   "fake-clock stage duration finite");
        }
    }
    for (uint32_t index = 1u; index < FIXTURE_COUNT; ++index)
    {
        Expect(fixtures[index].cache.contactCount == fixtures[0].cache.contactCount &&
                   fixtures[index].cache.matchedContactCount ==
                       fixtures[0].cache.matchedContactCount,
               "contact cache counts independent of executor");
        for (uint32_t slot = 0u; slot < count; ++slot)
        {
            const VoxelRigidBody *expected = &fixtures[0].bodies[slot];
            Expect(SameBody(expected, FindBody(&fixtures[index], count, expected->stableId)),
                   "all body fields and coordinate limbs agree every tick");
        }
    }
}

static void ResetCaches(void)
{
    for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
    {
        VoxelRigidContactCacheReset(&fixtures[fixture].cache);
    }
}

static uint64_t Replay(VoxelRigidSolverOrder order)
{
    ResetFixtures();
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t slot = 0u; slot < REPLAY_BODY_COUNT; ++slot)
    {
        uint32_t cluster = slot / 8u;
        uint32_t local = slot % 8u;
        uint32_t layer = local / 4u;
        VoxelRigidBodyDescription description = Description();
        description.position[0] = (double)cluster * 12.0 + (double)(local % 4u) * 1.25;
        description.position[1] = 0.25;
        description.position[2] = 2.0 + (double)layer * 1.3;
        description.restitution = 0.1;
        InitializeBody(slot, &description);
        const double spin[3] = {(double)(local % 3u) * 0.1, 0.03, -0.01};
        for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
        {
            Expect(VoxelRigidBodyAddAngularVelocity(&fixtures[fixture].bodies[slot], spin),
                   "replay spin applied");
        }
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t tick = 0u; tick < REPLAY_TICKS; ++tick)
    {
        if (tick == 128u)
        {
            // Array storage and execution range order are separate adversaries.
            for (uint32_t slot = 0u; slot < REPLAY_BODY_COUNT / 2u; ++slot)
            {
                uint32_t other = REPLAY_BODY_COUNT - 1u - slot;
                VoxelRigidBody temporary = fixtures[2].bodies[slot];
                fixtures[2].bodies[slot] = fixtures[2].bodies[other];
                fixtures[2].bodies[other] = temporary;
            }
        }
        if (tick == 256u)
        {
            const int64_t shift[3] = {17, -9, 3};
            for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
            {
                for (uint32_t slot = 0u; slot < REPLAY_BODY_COUNT; ++slot)
                {
                    Expect(VoxelRigidBodyTranslateBlocks(&fixtures[fixture].bodies[slot], shift),
                           "common floating-origin rebase applied");
                }
            }
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                worldOrigin[axis] += shift[axis];
        }
        if (tick == 384u)
        {
            const double impact[3] = {2.0, 0.0, 1.0};
            AddVelocity(REPLAY_BODY_COUNT, 3u, impact);
        }
        StepAll(REPLAY_BODY_COUNT, &settings, order);
        for (uint32_t slot = 0u; slot < REPLAY_BODY_COUNT; ++slot)
        {
            hash = HashBody(hash, &fixtures[0].bodies[slot]);
        }
    }
    return hash;
}

// Плотная сцена для параллельной узкой фазы. indexed выбирает путь: без
// дерева работает сеточный обход, с деревом — запрос к дереву. Пути разные,
// и оба обязаны давать то же состояние, что последовательный шаг.
static void DenseParallelScene(VoxelRigidSolverOrder order, bool indexed)
{
    ResetFixtures();
    indexedBroadphase = indexed;
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepFrames = 0u;
    // Ninety-six active bodies cross the parallel grid narrowphase and
    // integration thresholds. Slight vertical overlap exercises real stacked
    // contacts, while separated columns avoid 26-neighbour artificial overlap.
    for (uint32_t slot = 0u; slot < TEST_CAPACITY; ++slot)
    {
        uint32_t row = (slot / 8u) % 4u;
        uint32_t layer = slot / 32u;
        VoxelRigidBodyDescription description = Description();
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            description.halfExtent[axis] = 0.45;
        description.position[0] = (double)(slot % 8u);
        description.position[1] = (double)row;
        description.position[2] = 0.44 + (double)layer * 0.89;
        description.friction = 0.6;
        InitializeBody(slot, &description);
        const double velocity[3] = {(double)((int32_t)(slot % 7u) - 3) / 512.0,
                                    (double)((int32_t)(slot % 5u) - 2) / 512.0, 0.0};
        AddVelocity(TEST_CAPACITY, (uint64_t)slot + 1u, velocity);
    }
    for (uint32_t slot = 0u; slot < TEST_CAPACITY / 2u; ++slot)
    {
        uint32_t other = TEST_CAPACITY - 1u - slot;
        VoxelRigidBody temporary = fixtures[2].bodies[slot];
        fixtures[2].bodies[slot] = fixtures[2].bodies[other];
        fixtures[2].bodies[other] = temporary;
    }
    for (uint32_t tick = 0u; tick < 128u; ++tick)
    {
        StepAll(TEST_CAPACITY, &settings, order);
        if (tick == 0u)
        {
            VoxelRigidStepStats stats;
            Expect(VoxelRigidBodyReadStepStats(fixtures[0].scratch, TEST_CAPACITY,
                                               TEST_SCRATCH_BYTES, &stats) &&
                       stats.contactCount > TEST_CAPACITY,
                   "dense parallel regression exercises many real contacts");
        }
    }
}

static void TestDenseParallelGrid(VoxelRigidSolverOrder order)
{
    DenseParallelScene(order, false);
}

// Девяносто шесть тел переводят древесный путь через порог параллельной узкой
// фазы. Враждебный и обратный исполнители режут диапазоны иначе, чем
// последовательный шаг, поэтому этот тест ловит любую зависимость состава или
// порядка контактов от разбиения работы.
static void TestDenseParallelTree(VoxelRigidSolverOrder order)
{
    DenseParallelScene(order, true);
}

// Одно широкое тело под россыпью мелких. У плиты наименьший stableId, поэтому
// все пары записываются от неё, и её контактов заведомо больше восьми — а это
// ровно тот случай, когда второй проход не может скопировать сохранённую
// геометрию и обязан построить манифольды заново. Без этой сцены путь
// перегенерации остаётся непроверенным.
static void TestWideBodyParallelTree(VoxelRigidSolverOrder order)
{
    ResetFixtures();
    indexedBroadphase = true;
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepFrames = 0u;
    for (uint32_t slot = 0u; slot < TEST_CAPACITY; ++slot)
    {
        VoxelRigidBodyDescription description = Description();
        if (slot == 0u)
        {
            description.halfExtent[0] = 6.0;
            description.halfExtent[1] = 3.0;
            description.halfExtent[2] = 0.45;
            description.mass = 1000.0;
            description.position[0] = 3.5;
            description.position[1] = 1.5;
            description.position[2] = 0.44;
        }
        else
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                description.halfExtent[axis] = 0.45;
            description.position[0] = (double)((slot - 1u) % 8u);
            description.position[1] = (double)(((slot - 1u) / 8u) % 4u);
            description.position[2] = 1.32 + (double)((slot - 1u) / 32u) * 0.89;
        }
        description.friction = 0.6;
        InitializeBody(slot, &description);
    }
    for (uint32_t slot = 1u; slot < TEST_CAPACITY / 2u; ++slot)
    {
        uint32_t other = TEST_CAPACITY - slot;
        VoxelRigidBody temporary = fixtures[2].bodies[slot];
        fixtures[2].bodies[slot] = fixtures[2].bodies[other];
        fixtures[2].bodies[other] = temporary;
    }
    for (uint32_t tick = 0u; tick < 96u; ++tick)
    {
        StepAll(TEST_CAPACITY, &settings, order);
        if (tick == 0u)
        {
            VoxelRigidStepStats stats;
            Expect(VoxelRigidBodyReadStepStats(fixtures[0].scratch, TEST_CAPACITY,
                                               TEST_SCRATCH_BYTES, &stats) &&
                       stats.contactCount > 8u * TEST_CAPACITY / 4u,
                   "wide body parallel scene forces manifold regeneration");
        }
    }
}

static void TestSplitWarmCacheRuns(VoxelRigidSolverOrder order)
{
    ResetFixtures();
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    settings.sleepFrames = 0u;
    // With this fixture's 256 cache buckets, every (lower, world) and
    // (lower, upper) key below lands in bucket zero. Different pair owners
    // traverse the same chain while consuming only their own used flags.
    // Fixed input vectors avoid coupling the test to private cache layout.
    static const uint64_t pairIds[16][2] = {
        {16u, 100017u},   {280u, 100149u},  {469u, 100313u},  {560u, 101107u},
        {875u, 101296u},  {958u, 101340u},  {1120u, 101650u}, {1134u, 101735u},
        {1991u, 102772u}, {2044u, 103190u}, {2280u, 103391u}, {2423u, 103627u},
        {2815u, 103695u}, {3359u, 104386u}, {3945u, 104788u}, {4171u, 104917u}};
    for (uint32_t stack = 0u; stack < 16u; ++stack)
    {
        for (uint32_t layer = 0u; layer < 2u; ++layer)
        {
            uint32_t slot = stack * 2u + layer;
            VoxelRigidBodyDescription description = Description();
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                description.halfExtent[axis] = 0.45;
            description.position[0] = (double)stack * 4.0 + 0.25;
            description.position[1] = 0.25;
            description.position[2] = 0.449 + (double)layer * 0.899;
            InitializeBody(slot, &description);
            // Assign the test identity before a step or cache entry exists.
            for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
                fixtures[fixture].bodies[slot].stableId = pairIds[stack][layer];
        }
    }
    VoxelRigidBodyDescription wide = Description();
    wide.halfExtent[0] = 7.0;
    wide.halfExtent[1] = 7.0;
    wide.position[0] = -20.0;
    wide.position[2] = 0.499;
    InitializeBody(32u, &wide);
    // This box crosses several six-cell world tiles. Their manifolds form
    // one long (body, world) run, not independently matchable submanifolds.
    for (uint32_t slot = 0u; slot < 16u; ++slot)
    {
        uint32_t other = 32u - slot;
        VoxelRigidBody temporary = fixtures[2].bodies[slot];
        fixtures[2].bodies[slot] = fixtures[2].bodies[other];
        fixtures[2].bodies[other] = temporary;
    }
    for (uint32_t tick = 0u; tick < 32u; ++tick)
    {
        // Neither size aligns with four-point manifolds. A run beginning in
        // another range must still consume each old contact at most once.
        splitRangeLimit = (tick & 1u) == 0u ? 1u : 3u;
        StepAll(33u, &settings, order);
        Expect(fixtures[0].cache.contactCount > 64u,
               "split-run fixture crosses parallel warm matching threshold");
        if (tick == 0u)
            Expect(fixtures[0].cache.contactCount >= 160u,
                   "wide world body produces a long multi-tile contact run");
        else
            Expect(fixtures[0].cache.matchedContactCount > 0u,
                   "split-run fixture actually reuses cached contact impulses");
    }
    splitRangeLimit = 0u;
}

static void TestSleepWake(VoxelRigidSolverOrder order)
{
    ResetFixtures();
    VoxelRigidBodyDescription description = Description();
    description.position[2] = 0.49;
    description.friction = 0.0;
    InitializeBody(0u, &description);
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t tick = 0u; tick < 512u; ++tick)
        StepAll(1u, &settings, order);
    Expect(fixtures[0].bodies[0].sleeping, "target sleeps before impact");
    Expect(VoxelRigidBodyLocalPosition(&fixtures[0].bodies[0], description.position),
           "sleeping body position available");
    description.position[0] -= 0.99;
    InitializeBody(1u, &description);
    const double incoming[3] = {10.0, 0.0, 0.0};
    AddVelocity(2u, 2u, incoming);
    settings.penetrationCorrection = 0.0;
    ResetCaches();
    StepAll(2u, &settings, order);
    double velocity[3];
    Expect(!fixtures[0].bodies[0].sleeping &&
               VoxelRigidBodyLinearVelocity(&fixtures[0].bodies[0], velocity) && velocity[0] > 1.0,
           "impact wakes a finite-mass target");
    floorEnabled = false;
    ResetCaches();
    for (uint32_t fixture = 0u; fixture < FIXTURE_COUNT; ++fixture)
    {
        VoxelRigidBodyWake(&fixtures[fixture].bodies[0]);
        VoxelRigidBodyWake(&fixtures[fixture].bodies[1]);
    }
    for (uint32_t tick = 0u; tick < 128u; ++tick)
        StepAll(2u, &settings, order);
    double position[3];
    Expect(VoxelRigidBodyLocalPosition(&fixtures[0].bodies[0], position) && position[2] < -1.0,
           "explicit wake and support removal allow falling");
}

static void TestColorOverflow(void)
{
    ResetFixtures();
    floorEnabled = false;
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        settings.gravity[axis] = 0.0;
    settings.sleepFrames = 0u;
    VoxelRigidBodyDescription giant = Description();
    giant.halfExtent[0] = 10.0;
    giant.halfExtent[1] = 10.0;
    giant.position[2] = 0.0;
    giant.mass = 100.0;
    InitializeBody(0u, &giant);
    for (uint32_t slot = 1u; slot <= 81u; ++slot)
    {
        VoxelRigidBodyDescription small = Description();
        uint32_t row = (slot - 1u) / 9u;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            small.halfExtent[axis] = 0.4;
        small.position[0] = (double)((slot - 1u) % 9u) * 2.0 - 8.0;
        small.position[1] = (double)row * 2.0 - 8.0;
        small.position[2] = 0.8;
        InitializeBody(slot, &small);
    }
    StepAll(82u, &settings, VOXEL_RIGID_SOLVER_COLORED);
    VoxelRigidStepStats stats;
    Expect(VoxelRigidBodyReadStepStats(fixtures[0].scratch, 82u, TEST_SCRATCH_BYTES, &stats) &&
               stats.contactCount == 324u,
           "all 81 four-point giant contacts retained");
    Expect(fixtures[0].profile.solverOverflowContacts > 0u,
           "more than 64 shared-body colors use serial overflow");
    for (uint32_t fixture = 1u; fixture < FIXTURE_COUNT; ++fixture)
    {
        Expect(fixtures[fixture].profile.solverOverflowContacts ==
                   fixtures[0].profile.solverOverflowContacts,
               "overflow count independent of executor");
    }
    for (uint32_t tick = 0u; tick < 16u; ++tick)
    {
        StepAll(82u, &settings, VOXEL_RIGID_SOLVER_COLORED);
    }
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static void TestColoredRestitution(void)
{
    const double coefficients[3] = {0.0, 0.5, 1.0};
    for (uint32_t sample = 0u; sample < 3u; ++sample)
    {
        ResetFixtures();
        floorEnabled = false;
        for (uint32_t pair = 0u; pair < 40u; ++pair)
        {
            VoxelRigidBodyDescription description = Description();
            description.position[0] = (double)pair * 4.0;
            description.friction = 0.0;
            description.restitution = coefficients[sample];
            InitializeBody(pair * 2u, &description);
            description.position[0] += 0.99;
            InitializeBody(pair * 2u + 1u, &description);
            const double incoming[3] = {10.0, 0.0, 0.0};
            AddVelocity(80u, (uint64_t)pair * 2u + 1u, incoming);
        }
        VoxelRigidStepSettings settings;
        VoxelRigidStepSettingsDefault(&settings);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            settings.gravity[axis] = 0.0;
        settings.penetrationCorrection = 0.0;
        settings.sleepFrames = 0u;
        StepAll(80u, &settings, VOXEL_RIGID_SOLVER_COLORED);
        for (uint32_t pair = 0u; pair < 40u; ++pair)
        {
            double first[3];
            double second[3];
            uint32_t firstSlot = pair * 2u;
            Expect(VoxelRigidBodyLinearVelocity(&fixtures[0].bodies[firstSlot], first) &&
                       VoxelRigidBodyLinearVelocity(&fixtures[0].bodies[firstSlot + 1u], second),
                   "colored impact velocities available");
            Expect(Absolute(first[0] + second[0] - 10.0) < 1e-7,
                   "colored impact conserves linear momentum");
            Expect(Absolute(second[0] - first[0] - 10.0 * coefficients[sample]) < 0.03,
                   "colored impact restitution matches pre-solve relative speed");
        }
    }
}

static void TestColoredFriction(void)
{
    const double masses[4] = {1.0, 1e-100, 1e100, 1e200};
    for (uint32_t sample = 0u; sample < 4u; ++sample)
    {
        ResetFixtures();
        for (uint32_t slot = 0u; slot < 64u; ++slot)
        {
            uint32_t row = slot / 8u;
            VoxelRigidBodyDescription description = Description();
            description.position[0] = (double)(slot % 8u) * 3.0;
            description.position[1] = (double)row * 3.0;
            description.position[2] = 0.49;
            description.friction = 0.3;
            description.mass = masses[sample];
            InitializeBody(slot, &description);
            const double incoming[3] = {10.0, 10.0, -1.0};
            AddVelocity(64u, (uint64_t)slot + 1u, incoming);
        }
        VoxelRigidStepSettings settings;
        VoxelRigidStepSettingsDefault(&settings);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            settings.gravity[axis] = 0.0;
        settings.penetrationCorrection = 0.0;
        settings.sleepFrames = 0u;
        StepAll(64u, &settings, VOXEL_RIGID_SOLVER_COLORED);
        for (uint32_t slot = 0u; slot < 64u; ++slot)
        {
            double velocity[3];
            Expect(VoxelRigidBodyLinearVelocity(&fixtures[0].bodies[slot], velocity),
                   "colored friction velocity available");
            double deltaX = 10.0 - velocity[0];
            double deltaY = 10.0 - velocity[1];
            double normalDelta = velocity[2] + 1.0;
            double limit = 0.3 * normalDelta;
            Expect(normalDelta > 0.0 && deltaX > 0.0 && deltaY > 0.0,
                   "colored friction opposes sliding");
            Expect(deltaX * deltaX + deltaY * deltaY <= limit * limit + 1e-7,
                   "colored total friction remains inside the Coulomb disk");
        }
    }
}

static void TestOptionsValidation(void)
{
    ResetFixtures();
    VoxelRigidBodyDescription description = Description();
    InitializeBody(0u, &description);
    PhysicsFixture *fixture = &fixtures[0];
    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    VoxelCollisionSource collision = Collision();
    uint64_t before = HashBody(0u, &fixture->bodies[0]);
    VoxelRigidStepOptions options = Options(fixture, VOXEL_RIGID_SOLVER_CANONICAL, NULL);
    --options.structSize;
    Expect(!VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, fixture->scratch,
                                 TEST_SCRATCH_BYTES, &options),
           "short options rejected");
    options.structSize = sizeof(options);
    --fixture->profile.structSize;
    Expect(!VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, fixture->scratch,
                                 TEST_SCRATCH_BYTES, &options),
           "short profile rejected");
    fixture->profile.structSize = sizeof(fixture->profile);
    // Deliberately exercise the public API's invalid-enum validation.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    options.solverOrder = (VoxelRigidSolverOrder)99;
    Expect(!VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, fixture->scratch,
                                 TEST_SCRATCH_BYTES, &options),
           "unknown solver order rejected");
    options.solverOrder = VOXEL_RIGID_SOLVER_CANONICAL;
    aliasBuffer.options = options;
    Expect(!VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, aliasBuffer.bytes,
                                 TEST_SCRATCH_BYTES, &aliasBuffer.options),
           "options and scratch alias rejected");
    aliasBuffer.profile = (VoxelRigidStepProfile){.structSize = sizeof(VoxelRigidStepProfile)};
    options.profile = &aliasBuffer.profile;
    Expect(!VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, aliasBuffer.bytes,
                                 TEST_SCRATCH_BYTES, &options),
           "profile and scratch alias rejected");
    Expect(before == HashBody(0u, &fixture->bodies[0]), "validation failures preserve body state");
    options = Options(fixture, VOXEL_RIGID_SOLVER_CANONICAL, NULL);
    options.clockSeconds = NULL;
    options.clockContext = NULL;
    Expect(VoxelRigidBodyStepEx(fixture->bodies, 1u, &collision, &settings, fixture->scratch,
                                TEST_SCRATCH_BYTES, &options),
           "profile counters work without a clock");
    for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
    {
        Expect(fixture->profile.seconds[stage] == 0.0, "absent clock does not fabricate timings");
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

LAIUE_TEST_ENTRY(RigidParallelTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    LaiueTaskPool *pool = LaiueTaskPoolCreate(4u);
    Expect(pool != NULL, "persistent four-participant pool created");
    poolExecutor.structSize = sizeof(poolExecutor);
    Expect(LaiueTaskPoolGetExecutor(pool, &poolExecutor), "pool executor obtained");
    hostileExecutor.structSize = sizeof(hostileExecutor);
    hostileExecutor.context = &poolExecutor;
    hostileExecutor.run = HostileRun;
    reverseExecutor.structSize = sizeof(reverseExecutor);
    reverseExecutor.context = &reverseDispatches;
    reverseExecutor.run = ReverseRun;
    TestOptionsValidation();
    uint64_t canonicalHash = Replay(VOXEL_RIGID_SOLVER_CANONICAL);
    uint64_t coloredHash = Replay(VOXEL_RIGID_SOLVER_COLORED);
    TestDenseParallelGrid(VOXEL_RIGID_SOLVER_CANONICAL);
    TestDenseParallelGrid(VOXEL_RIGID_SOLVER_COLORED);
    TestDenseParallelTree(VOXEL_RIGID_SOLVER_CANONICAL);
    TestDenseParallelTree(VOXEL_RIGID_SOLVER_COLORED);
    TestWideBodyParallelTree(VOXEL_RIGID_SOLVER_CANONICAL);
    TestWideBodyParallelTree(VOXEL_RIGID_SOLVER_COLORED);
    TestSplitWarmCacheRuns(VOXEL_RIGID_SOLVER_CANONICAL);
    TestSplitWarmCacheRuns(VOXEL_RIGID_SOLVER_COLORED);
    TestSleepWake(VOXEL_RIGID_SOLVER_CANONICAL);
    TestSleepWake(VOXEL_RIGID_SOLVER_COLORED);
    TestColorOverflow();
    TestColoredRestitution();
    TestColoredFriction();
    Expect(hostileDispatches != 0u && reverseDispatches != 0u,
           "both adversarial executors actually dispatched physics callbacks");
    Expect(hostileRangeCalls > hostileDispatches && reverseRangeCalls > reverseDispatches,
           "dispatches actually execute multiple ranges");
    ReleaseFixtures();
    LaiueTaskPoolDestroy(pool);
    WriteHash("rigid-parallel-canonical-replay-hash: ", canonicalHash);
    WriteHash("rigid-parallel-colored-replay-hash: ", coloredHash);
    WriteCount("rigid-parallel-pool-range-calls: ", hostileRangeCalls);
    WriteCount("rigid-parallel-reverse-range-calls: ", reverseRangeCalls);
    LAIUE_TEST_SUCCESS();
}
