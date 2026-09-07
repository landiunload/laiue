#include "task/task_pool.h"

#include "platform/system.h"

#include <stddef.h>

#define TASK_MAX_THREADS 64u

struct LaiueTaskPool
{
    PlatformMutex mutex;
    PlatformConditionVariable startCondition;
    PlatformConditionVariable doneCondition;
    PlatformThread workers[TASK_MAX_THREADS - 1u];
    uint32_t workerCount;
    uint32_t remainingWorkers;
    uint64_t generation;
    bool stopping;
    bool mutexReady;
    bool startConditionReady;
    bool doneConditionReady;
    // The job is immutable from publication until all workers finish. The
    // mutex publishes the job; only this cursor needs per-range atomics.
    volatile int64_t nextIndex;
    uint32_t count;
    uint32_t grain;
    LaiueTaskRangeFunction function;
    void *jobContext;
};

static void ExecuteRanges(LaiueTaskPool *pool)
{
    for (;;)
    {
        int64_t begin =
            PlatformAtomicAddI64(&pool->nextIndex, (int64_t)pool->grain) - (int64_t)pool->grain;
        if (begin >= (int64_t)pool->count)
        {
            return;
        }
        uint32_t remaining = pool->count - (uint32_t)begin;
        uint32_t length = remaining < pool->grain ? remaining : pool->grain;
        pool->function(pool->jobContext, (uint32_t)begin, (uint32_t)begin + length);
    }
}

static uint32_t WorkerEntry(void *context)
{
    LaiueTaskPool *pool = context;
    uint64_t observedGeneration = 0u;
    PlatformMutexLock(&pool->mutex);
    for (;;)
    {
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
        ExecuteRanges(pool);
        PlatformMutexLock(&pool->mutex);
        --pool->remainingWorkers;
        if (pool->remainingWorkers == 0u)
        {
            PlatformConditionVariableWakeOne(&pool->doneCondition);
        }
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
    // ordinary reset is safe. With at most 64 participants and grain<=count,
    // even the final unsuccessful claims stay far below INT64_MAX.
    pool->nextIndex = 0;
    pool->remainingWorkers = pool->workerCount;
    ++pool->generation;
    PlatformConditionVariableWakeAll(&pool->startCondition);
    PlatformMutexUnlock(&pool->mutex);

    ExecuteRanges(pool);

    PlatformMutexLock(&pool->mutex);
    while (pool->remainingWorkers != 0u)
    {
        PlatformConditionVariableWait(&pool->doneCondition, &pool->mutex);
    }
    pool->function = NULL;
    pool->jobContext = NULL;
    PlatformMutexUnlock(&pool->mutex);
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
