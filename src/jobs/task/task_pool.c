#include "task/task_pool.h"

#include "platform/system.h"

#include <stddef.h>

#define TASK_MAX_THREADS 64u
// One atomic claim per range costs more than a short range itself once there
// are thousands of them: the cursor line travels between every participant on
// every claim. A claim therefore takes a batch of ranges, sized as a share of
// the work still unclaimed, so early claims are large and the last ones are
// short. The tail is what the barrier waits for, and it shrinks with the
// remainder. Halving the per-participant share keeps the first claim from
// swallowing a slow participant's whole turn.
#define TASK_CLAIM_DIVISOR 2u

// Bounded spin before parking. A participant that just executed `work` range
// indices may busy-wait that many times `TASK_SPIN_PER_INDEX` relax
// iterations for the next publication before it sleeps. A participant that
// claimed nothing spins zero times, so the total spin of the pool cannot
// exceed this fraction of the useful range work it just performed. The cap
// bounds the waste when the next publication never comes (for example after
// the last physics tick of a rendered frame).
#define TASK_SPIN_PER_INDEX 128u
#define TASK_SPIN_MAX 65536u

static uint64_t SpinBudget(uint32_t work)
{
    uint64_t budget = (uint64_t)work * TASK_SPIN_PER_INDEX;
    return budget > TASK_SPIN_MAX ? TASK_SPIN_MAX : budget;
}

struct LaiueTaskPool
{
    PlatformMutex mutex;
    PlatformConditionVariable startCondition;
    // The barrier has a mutex of its own. Workers finish at almost the same
    // instant, and one shared mutex made every one of them queue behind the
    // others just to report completion, with the dispatcher queued last.
    PlatformMutex doneMutex;
    PlatformConditionVariable doneCondition;
    PlatformThread workers[TASK_MAX_THREADS - 1u];
    // Read by every participant in the bounded spin before it parks. The spin
    // itself uses plain volatile reads: a stale or torn load can cause an extra
    // spin or a park, never a missed publication. The publication is stored
    // release and read acquire on the path that starts work without the mutex,
    // so a participant that observes a new generation also observes the job
    // description written before it. Thirty-two bits are enough: only equality
    // against the last observed value is tested, so a wrap is harmless.
    volatile uint32_t generation;
    bool stopping;
    bool mutexReady;
    bool doneMutexReady;
    bool startConditionReady;
    bool doneConditionReady;
    // Three groups follow, and the padding between them keeps each on its own
    // cache line. Two of them are written by every participant on the hot
    // path, and the third is read by every participant; sharing a line would
    // make each write take the read-only group away from everyone else.
    // The separation is spelled out as padding rather than as _Alignas,
    // because the allocator promises sixteen bytes of alignment and would not
    // honour a declared alignment of sixty-four.
    char descriptionPadding[64];
    // Read-only for the whole dispatch. The job is immutable from publication
    // until all workers finish, and the worker count never changes at all.
    uint32_t workerCount;
    uint32_t count;
    uint32_t grain;
    LaiueTaskRangeFunction function;
    void *jobContext;
    char cursorPadding[64];
    // One atomic per claim, from every participant.
    volatile int64_t nextIndex;
    char barrierPadding[64];
    // Counted down atomically: only the worker that brings it to zero has any
    // reason to take the barrier mutex.
    volatile int64_t remainingWorkers;
};

// Indices one claim takes at the given remainder: a whole number of ranges,
// and never fewer than one.
static uint32_t ClaimSpan(uint32_t remaining, uint32_t grain, uint32_t share)
{
    uint32_t batch = remaining / share / grain;
    return batch != 0u ? batch * grain : grain;
}

// Executes the published job and returns how many range indices this
// participant claimed. The caller turns that into a bounded spin budget, so a
// participant that finds no work also does not busy-wait.
static uint32_t ExecuteRanges(LaiueTaskPool *pool)
{
    // The job description is immutable from publication until the barrier, so
    // it is read once. Otherwise every claim reloads it: an indirect call to
    // an unknown callback forbids keeping the fields in registers, and the
    // atomic cursor shares their cache line and takes it from every other
    // participant on each claim.
    const uint32_t count = pool->count;
    const uint32_t grain = pool->grain;
    const LaiueTaskRangeFunction function = pool->function;
    void *const jobContext = pool->jobContext;
    // The worker count is fixed once the pool exists, so the share needs no
    // publication with the job.
    const uint32_t share = (pool->workerCount + 1u) * TASK_CLAIM_DIVISOR;
    uint32_t span = ClaimSpan(count, grain, share);
    uint32_t work = 0u;
    for (;;)
    {
        int64_t after = PlatformAtomicAddI64(&pool->nextIndex, (int64_t)span);
        int64_t claimed = after - (int64_t)span;
        if (claimed >= (int64_t)count)
        {
            return work;
        }
        uint32_t begin = (uint32_t)claimed;
        // The last batch of the job is short, so the limit is clamped rather
        // than assumed. The sum is formed only once it is known not to pass
        // count, so it cannot wrap.
        uint32_t limit = count - begin < span ? count : begin + span;
        while (begin < limit)
        {
            uint32_t remaining = limit - begin;
            uint32_t length = remaining < grain ? remaining : grain;
            function(jobContext, begin, begin + length);
            begin += length;
            work += length;
        }
        // The cursor value this claim returned already counts every claim made
        // before it, so the remainder is known rather than guessed. The next
        // claim is a share of what is actually left.
        span = after >= (int64_t)count
                   ? grain
                   : ClaimSpan((uint32_t)((int64_t)count - after), grain, share);
    }
}

// Reporting completion without holding the barrier mutex would lose a wake:
// the dispatcher can have read the counter and not yet started waiting. The
// empty critical section closes that window. Either this thread takes the
// mutex first, and the release publishes the decrement to the dispatcher's
// read, or the dispatcher took it first, and reaching the mutex at all proves
// it is already inside the wait that released it.
static void ReportCompletion(LaiueTaskPool *pool)
{
    if (PlatformAtomicAddI64(&pool->remainingWorkers, -1) != 0)
    {
        return;
    }
    PlatformMutexLock(&pool->doneMutex);
    PlatformMutexUnlock(&pool->doneMutex);
    PlatformConditionVariableWakeOne(&pool->doneCondition);
}

static uint32_t WorkerEntry(void *context)
{
    LaiueTaskPool *pool = context;
    uint32_t observedGeneration = 0u;
    uint32_t work = 0u;
    for (;;)
    {
        // Bounded spin before parking. The budget comes from the work this
        // worker just performed: an idle participant parks immediately, a busy
        // one waits for the next publication without a kernel sleep.
        uint64_t spin = 0u;
        uint64_t budget = SpinBudget(work);
        while (spin < budget && pool->generation == observedGeneration)
        {
            PlatformCpuRelax();
            ++spin;
        }
        // The spin is plain volatile: on its own it only decides that a
        // publication is worth taking. When a new generation is visible, the
        // acquire read pairs with the release store in Run and lets this
        // participant start without the mutex. A participant that saw nothing
        // takes the mutex exactly as before, so the parked path pays no extra
        // synchronisation on platforms where the acquire read is a locked
        // operation.
        if (pool->generation != observedGeneration)
        {
            uint32_t generation = PlatformAtomicLoadU32Acquire(&pool->generation);
            if (generation != observedGeneration)
            {
                observedGeneration = generation;
                work = ExecuteRanges(pool);
                ReportCompletion(pool);
                continue;
            }
        }
        PlatformMutexLock(&pool->mutex);
        while (!pool->stopping && observedGeneration == pool->generation)
        {
            PlatformConditionVariableWait(&pool->startCondition, &pool->mutex);
        }
        if (pool->stopping)
        {
            PlatformMutexUnlock(&pool->mutex);
            return 0u;
        }
        observedGeneration = pool->generation;
        PlatformMutexUnlock(&pool->mutex);
        work = ExecuteRanges(pool);
        ReportCompletion(pool);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void Run(void *context, uint32_t count, uint32_t grain, LaiueTaskRangeFunction function,
                void *jobContext)
{
    LaiueTaskPool *pool = context;
    if (pool == NULL || function == NULL || count == 0u)
    {
        return;
    }
    if (grain == 0u)
    {
        grain = 1u;
    }
    if (grain > count)
    {
        grain = count;
    }
    if (pool->workerCount == 0u || count <= grain)
    {
        for (uint32_t begin = 0u; begin < count;)
        {
            uint32_t remaining = count - begin;
            uint32_t length = remaining < grain ? remaining : grain;
            function(jobContext, begin, begin + length);
            begin += length;
        }
        return;
    }

    PlatformMutexLock(&pool->mutex);
    pool->count = count;
    pool->grain = grain;
    pool->function = function;
    pool->jobContext = jobContext;
    // All earlier claimants have joined the previous dispatch barrier, so an
    // ordinary reset is safe. A claim span never exceeds count, and each of
    // the at most 64 participants overshoots by one claim, so the cursor stays
    // far below INT64_MAX.
    pool->nextIndex = 0;
    pool->remainingWorkers = (int64_t)pool->workerCount;
    // Release so a participant that sees the new generation also sees the job
    // description, the cursor and the barrier counter written just above.
    PlatformAtomicStoreU32Release(&pool->generation, pool->generation + 1u);
    PlatformMutexUnlock(&pool->mutex);
    // Waking after the release keeps the woken workers from immediately
    // blocking on a mutex this thread still holds. A worker that was about to
    // sleep rechecks the generation under the mutex, so nothing is lost.
    PlatformConditionVariableWakeAll(&pool->startCondition);

    uint32_t work = ExecuteRanges(pool);

    // Bounded spin before parking the dispatcher: the workers usually finish
    // within the work the caller just performed, and sleeping would cost a
    // kernel transition on every short dispatch.
    uint64_t spin = 0u;
    uint64_t budget = SpinBudget(work);
    while (spin < budget && pool->remainingWorkers != 0)
    {
        PlatformCpuRelax();
        ++spin;
    }

    PlatformMutexLock(&pool->doneMutex);
    while (PlatformAtomicLoadI64(&pool->remainingWorkers) != 0)
    {
        PlatformConditionVariableWait(&pool->doneCondition, &pool->doneMutex);
    }
    PlatformMutexUnlock(&pool->doneMutex);
    // Every worker left ExecuteRanges before decrementing the counter, so no
    // participant can still read the job. Clearing it keeps a finished
    // dispatch from leaving a dangling caller context behind.
    pool->function = NULL;
    pool->jobContext = NULL;
}

LaiueTaskPool *LaiueTaskPoolCreate(uint32_t threadCount)
{
    if (threadCount == 0u || threadCount > TASK_MAX_THREADS)
    {
        return NULL;
    }
    LaiueTaskPool *pool = PlatformAllocate(sizeof(*pool), true);
    if (pool == NULL)
    {
        return NULL;
    }
    pool->mutexReady = PlatformMutexInitialize(&pool->mutex);
    if (!pool->mutexReady)
    {
        LaiueTaskPoolDestroy(pool);
        return NULL;
    }
    pool->doneMutexReady = PlatformMutexInitialize(&pool->doneMutex);
    if (!pool->doneMutexReady)
    {
        LaiueTaskPoolDestroy(pool);
        return NULL;
    }
    pool->startConditionReady = PlatformConditionVariableInitialize(&pool->startCondition);
    if (!pool->startConditionReady)
    {
        LaiueTaskPoolDestroy(pool);
        return NULL;
    }
    pool->doneConditionReady = PlatformConditionVariableInitialize(&pool->doneCondition);
    if (!pool->doneConditionReady)
    {
        LaiueTaskPoolDestroy(pool);
        return NULL;
    }
    while (pool->workerCount < threadCount - 1u)
    {
        if (!PlatformThreadStart(&pool->workers[pool->workerCount], WorkerEntry, pool))
        {
            LaiueTaskPoolDestroy(pool);
            return NULL;
        }
        ++pool->workerCount;
    }
    return pool;
}

void LaiueTaskPoolDestroy(LaiueTaskPool *pool)
{
    if (pool == NULL)
    {
        return;
    }
    if (pool->mutexReady)
    {
        PlatformMutexLock(&pool->mutex);
        pool->stopping = true;
        if (pool->startConditionReady)
        {
            PlatformConditionVariableWakeAll(&pool->startCondition);
        }
        PlatformMutexUnlock(&pool->mutex);
    }
    for (uint32_t index = 0u; index < pool->workerCount; ++index)
    {
        PlatformThreadJoin(&pool->workers[index]);
    }
    if (pool->doneConditionReady)
    {
        PlatformConditionVariableDestroy(&pool->doneCondition);
    }
    if (pool->startConditionReady)
    {
        PlatformConditionVariableDestroy(&pool->startCondition);
    }
    if (pool->mutexReady)
    {
        PlatformMutexDestroy(&pool->mutex);
    }
    if (pool->doneMutexReady)
    {
        PlatformMutexDestroy(&pool->doneMutex);
    }
    PlatformFree(pool);
}

bool LaiueTaskPoolGetExecutor(LaiueTaskPool *pool, LaiueTaskExecutor *outExecutor)
{
    if (pool == NULL || outExecutor == NULL || outExecutor->structSize < sizeof(*outExecutor))
    {
        return false;
    }
    outExecutor->context = pool;
    outExecutor->run = Run;
    return true;
}

uint32_t LaiueTaskLogicalProcessorCount(void)
{
    uint32_t count = PlatformLogicalProcessorCount();
    return count > 0u ? count : 1u;
}
