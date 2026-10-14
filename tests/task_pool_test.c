#include "task/task_pool.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stddef.h>

// Достаточно широкий набор, чтобы пул раздавал работу пачками: при коротком
// наборе пачка вырождается в один диапазон, и раздача пачками не проверялась
// бы вовсе.
#define TEST_INDEX_COUNT 8192u

typedef struct VisitState
{
    volatile uint32_t visits[TEST_INDEX_COUNT];
    uint32_t results[TEST_INDEX_COUNT];
    volatile uint32_t invocations;
    uint32_t count;
    uint32_t grain;
    // grain после ограничений пула: ноль означает единицу, а grain больше
    // count укорачивается до count.
    uint32_t effectiveGrain;
    uint32_t epoch;
} VisitState;

static VisitState visitState;
static volatile uint32_t nullContextVisits;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Task pool failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void Visit(void *context, uint32_t begin, uint32_t end)
{
    VisitState *state = context;
    Expect(begin < end && end <= state->count, "valid half-open range");
    Expect(end - begin <= state->grain, "range bounded by grain");
    PlatformAtomicIncrementU32(&state->invocations);
    for (uint32_t index = begin; index < end; ++index)
    {
        Expect(PlatformAtomicIncrementU32(&state->visits[index]) == 1u,
               "each index is dispatched exactly once");
        state->results[index] = index ^ state->epoch;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void VisitNullContext(void *context, uint32_t begin, uint32_t end)
{
    Expect(context == NULL, "NULL job context is passed through");
    Expect(begin < end, "NULL-context callback range");
    for (uint32_t index = begin; index < end; ++index)
    {
        PlatformAtomicIncrementU32(&nullContextVisits);
    }
}

typedef struct LargeRangeState
{
    volatile uint32_t visits[2];
} LargeRangeState;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void VisitLargeRange(void *context, uint32_t begin, uint32_t end)
{
    LargeRangeState *state = context;
    bool first = begin == 0u && end == UINT32_MAX - 1u;
    bool second = begin == UINT32_MAX - 1u && end == UINT32_MAX;
    Expect(first || second, "UINT32_MAX range arithmetic");
    PlatformAtomicIncrementU32(&state->visits[first ? 0u : 1u]);
}

typedef struct ConcurrentGate
{
    PlatformMutex mutex;
    PlatformConditionVariable condition;
    uint32_t arrived;
    uint32_t completed;
} ConcurrentGate;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void MeetAtGate(void *context, uint32_t begin, uint32_t end)
{
    ConcurrentGate *gate = context;
    Expect(begin < 4u && end == begin + 1u, "concurrent gate range");
    PlatformMutexLock(&gate->mutex);
    ++gate->arrived;
    if (gate->arrived == 4u)
    {
        PlatformConditionVariableWakeAll(&gate->condition);
    }
    while (gate->arrived < 4u)
    {
        PlatformConditionVariableWait(&gate->condition, &gate->mutex);
    }
    ++gate->completed;
    PlatformMutexUnlock(&gate->mutex);
}

static void TestConcurrentDispatch(const LaiueTaskExecutor *executor)
{
    ConcurrentGate gate = {0};
    Expect(PlatformMutexInitialize(&gate.mutex), "concurrent gate mutex");
    Expect(PlatformConditionVariableInitialize(&gate.condition), "concurrent gate condition");
    // Blocking each claimed range until all four arrive proves the three
    // persistent workers and caller can really participate concurrently.
    // Four ranges are far too few for the pool to hand more than one of them
    // to a single claim, so every participant has to take one. A batching
    // rule that ever gave one participant two of these four would hang here
    // rather than fail, so it is spelled out.
    executor->run(executor->context, 4u, 1u, MeetAtGate, &gate);
    Expect(gate.arrived == 4u && gate.completed == 4u, "all four callbacks complete together");
    PlatformConditionVariableDestroy(&gate.condition);
    PlatformMutexDestroy(&gate.mutex);
}

static void TestDispatch(uint32_t threadCount)
{
    LaiueTaskPool *pool = LaiueTaskPoolCreate(threadCount);
    Expect(pool != NULL, "pool creation");
    LaiueTaskExecutor executor = {.structSize = (uint32_t)sizeof(executor)};
    Expect(LaiueTaskPoolGetExecutor(pool, &executor), "executor acquisition");
    Expect(executor.context == pool && executor.run != NULL, "executor fields");
    if (threadCount == 4u)
    {
        TestConcurrentDispatch(&executor);
    }
    const uint32_t counts[] = {0u, 1u, 2u, 3u, 31u, 257u, 1021u, TEST_INDEX_COUNT};
    const uint32_t grains[] = {0u, 1u, 2u, 3u, 17u, 257u, UINT32_MAX};
    for (uint32_t pass = 0u; pass < 64u; ++pass)
    {
        for (uint32_t index = 0u; index < TEST_INDEX_COUNT; ++index)
        {
            visitState.visits[index] = 0u;
            visitState.results[index] = 0u;
        }
        visitState.count = counts[pass % 8u];
        uint32_t grain = grains[(pass / 8u + pass) % 7u];
        visitState.grain = grain == 0u ? 1u : grain;
        visitState.effectiveGrain = visitState.grain > visitState.count && visitState.count != 0u
                                        ? visitState.count
                                        : visitState.grain;
        visitState.invocations = 0u;
        visitState.epoch = pass + 1u;
        executor.run(executor.context, visitState.count, grain, Visit, &visitState);
        for (uint32_t index = 0u; index < TEST_INDEX_COUNT; ++index)
        {
            bool visited = index < visitState.count;
            Expect(visitState.visits[index] == (visited ? 1u : 0u),
                   "all and only requested indices complete");
            Expect(visitState.results[index] == (visited ? (index ^ visitState.epoch) : 0u),
                   "callback writes visible before synchronous return");
        }
        // Сколько бы индексов пул ни забирал одним захватом, callback обязан
        // получить ровно столько вызовов, сколько диапазонов длиной не больше
        // grain укладывается в count. Проверка ловит и слишком длинный
        // диапазон, и потерянный или задвоенный неполный хвост.
        uint32_t expectedRanges =
            visitState.count == 0u ? 0u : (visitState.count - 1u) / visitState.effectiveGrain + 1u;
        Expect(visitState.invocations == expectedRanges,
               "callback is invoked once per grain-sized range");
    }
    nullContextVisits = 0u;
    executor.run(executor.context, 31u, 2u, VisitNullContext, NULL);
    Expect(nullContextVisits == 31u, "NULL callback context accepted");
    executor.run(NULL, 31u, 2u, VisitNullContext, NULL);
    executor.run(executor.context, 31u, 2u, NULL, NULL);
    executor.run(executor.context, 0u, 2u, VisitNullContext, NULL);
    Expect(nullContextVisits == 31u, "invalid and empty dispatches are no-ops");
    LargeRangeState large = {0};
    executor.run(executor.context, UINT32_MAX, UINT32_MAX - 1u, VisitLargeRange, &large);
    Expect(large.visits[0] == 1u && large.visits[1] == 1u, "large ranges complete exactly once");
    LaiueTaskPoolDestroy(pool);
}

static void TestDescriptor(void)
{
    LaiueTaskPool *pool = LaiueTaskPoolCreate(1u);
    Expect(pool != NULL, "descriptor test pool");
    LaiueTaskExecutor executor = {0};
    executor.context = &executor;
    Expect(!LaiueTaskPoolGetExecutor(pool, &executor), "zero descriptor size rejected");
    Expect(executor.context == &executor && executor.run == NULL && executor.structSize == 0u,
           "invalid descriptor unchanged");
    executor.structSize = (uint32_t)sizeof(executor) - 1u;
    Expect(!LaiueTaskPoolGetExecutor(pool, &executor), "short descriptor rejected");
    executor.structSize = (uint32_t)sizeof(executor);
    Expect(!LaiueTaskPoolGetExecutor(NULL, &executor), "NULL pool rejected");
    Expect(!LaiueTaskPoolGetExecutor(pool, NULL), "NULL descriptor rejected");
    struct ExtendedExecutor
    {
        LaiueTaskExecutor executor;
        uint64_t extension;
    } extended = {{0}, UINT64_C(0x0123456789abcdef)};
    extended.executor.structSize = (uint32_t)sizeof(extended);
    Expect(LaiueTaskPoolGetExecutor(pool, &extended.executor), "extended descriptor accepted");
    Expect(extended.executor.structSize == sizeof(extended) &&
               extended.extension == UINT64_C(0x0123456789abcdef),
           "descriptor capacity and extension preserved");
    LaiueTaskPoolDestroy(pool);
}

LAIUE_TEST_ENTRY(TaskPoolTestEntryPoint)
{
    Expect(LaiueTaskPoolCreate(0u) == NULL, "zero thread count rejected");
    Expect(LaiueTaskPoolCreate(65u) == NULL, "excessive thread count rejected");
    Expect(LaiueTaskPoolCreate(UINT32_MAX) == NULL, "overflow thread count rejected");
    Expect(LaiueTaskLogicalProcessorCount() > 0u, "processor count is positive");
    LaiueTaskPoolDestroy(NULL);
    TestDescriptor();
    TestDispatch(1u);
    for (uint32_t repeat = 0u; repeat < 12u; ++repeat)
    {
        TestDispatch(4u);
    }
    // Восемь участников проходят тот же набор: барьер завершения устроен так,
    // что мьютекс берёт только последний закончивший, и на четырёх участниках
    // ошибка в этом счётчике могла бы не проявиться.
    for (uint32_t repeat = 0u; repeat < 4u; ++repeat)
    {
        TestDispatch(8u);
    }
    LaiueTestRuntimeWrite("task-pool-exact-once: passed\n");
    LAIUE_TEST_SUCCESS();
}
