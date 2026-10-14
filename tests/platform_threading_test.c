// Контракт потоков платформенного слоя. Проверяется именно он, а не его
// использование в сцене: ошибка в мьютексе или в атомарном счётчике
// проявилась бы там как редкое расхождение статистики, а здесь —
// детерминированным расхождением суммы.

#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define WORKER_COUNT 8u
#define INCREMENTS_PER_WORKER 20000

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Platform threading check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

typedef struct SharedState
{
    PlatformMutex mutex;
    PlatformConditionVariable startSignal;
    bool started;

    // Один и тот же счёт ведётся дважды: под мьютексом и атомарно.
    // Совпадение итогов означает, что оба механизма действительно
    // упорядочивают доступ, а не выглядят работающими.
    int64_t guardedCounter;
    volatile int64_t atomicCounter;
    volatile uint32_t finishedWorkers;
} SharedState;

static uint32_t WorkerEntry(void *context)
{
    SharedState *state = (SharedState *)context;

    // Все потоки стартуют разом: так конкуренция за счётчики реальна,
    // а не размазана по времени создания потоков.
    PlatformMutexLock(&state->mutex);
    while (!state->started)
    {
        PlatformConditionVariableWait(&state->startSignal, &state->mutex);
    }
    PlatformMutexUnlock(&state->mutex);

    for (int32_t index = 0; index < INCREMENTS_PER_WORKER; ++index)
    {
        PlatformMutexLock(&state->mutex);
        state->guardedCounter++;
        PlatformMutexUnlock(&state->mutex);
        PlatformAtomicIncrementI64(&state->atomicCounter);
    }

    PlatformAtomicIncrementU32(&state->finishedWorkers);
    return 0u;
}

LAIUE_TEST_ENTRY(PlatformThreadingTestEntryPoint)
{
    Expect(PlatformLogicalProcessorCount() >= 1u,
           "the platform must report at least one logical processor");

    SharedState state;
    for (uint32_t index = 0; index < sizeof(state); ++index) ((uint8_t *)&state)[index] = 0u;
    Expect(PlatformMutexInitialize(&state.mutex), "mutex could not be initialised");
    Expect(PlatformConditionVariableInitialize(&state.startSignal),
           "condition variable could not be initialised");

    PlatformThread workers[WORKER_COUNT];
    uint32_t started = 0u;
    for (uint32_t index = 0; index < WORKER_COUNT; ++index)
    {
        if (PlatformThreadStart(&workers[index], WorkerEntry, &state)) ++started;
    }
    Expect(started == WORKER_COUNT, "every worker thread must start");

    PlatformMutexLock(&state.mutex);
    state.started = true;
    PlatformConditionVariableWakeAll(&state.startSignal);
    PlatformMutexUnlock(&state.mutex);

    for (uint32_t index = 0; index < WORKER_COUNT; ++index)
    {
        PlatformThreadJoin(&workers[index]);
    }

    const int64_t expected = (int64_t)WORKER_COUNT * INCREMENTS_PER_WORKER;
    Expect(state.guardedCounter == expected,
           "the mutex must serialise every increment without loss");
    Expect(PlatformAtomicLoadI64(&state.atomicCounter) == expected,
           "the atomic counter must not lose an increment");
    Expect(PlatformAtomicLoadU32Acquire(&state.finishedWorkers) == WORKER_COUNT,
           "every worker must report completion before the join returns");

    // Повторное присоединение уже завершённого потока безопасно: описатель
    // обнуляется, и вызов становится пустым.
    PlatformThreadJoin(&workers[0]);

    Expect(PlatformAtomicAddI64(&state.atomicCounter, -expected) == 0,
           "an atomic add must return the value it produced");

    PlatformConditionVariableDestroy(&state.startSignal);
    PlatformMutexDestroy(&state.mutex);

    LaiueTestRuntimeWrite("Platform threading checks passed\n");
    LAIUE_TEST_SUCCESS();
}
