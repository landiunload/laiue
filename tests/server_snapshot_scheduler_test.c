#include "server/snapshot_scheduler.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t schedulerChecks;

static void SchedulerExpect(bool condition, const char *name)
{
    ++schedulerChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static void TestSuccessfulSnapshotAndBusyBackoff(void)
{
    ServerSnapshotScheduler scheduler;
    ServerSnapshotSchedulerInitialize(&scheduler, 100U);
    SchedulerExpect(!ServerSnapshotSchedulerHasPending(&scheduler), "scheduler starts idle");

    ServerSnapshotSchedulerRequest(&scheduler);
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 100U, true),
                    "first ready snapshot begins immediately");
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 100U, true),
                    "active attempt cannot begin twice");
    ServerSnapshotSchedulerFinish(&scheduler, 100U, true);
    SchedulerExpect(!ServerSnapshotSchedulerHasPending(&scheduler) &&
                        ServerSnapshotSchedulerTransportInFlight(&scheduler),
                    "successful snapshot drains as explicit in-flight work");

    ServerSnapshotSchedulerRequest(&scheduler);
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 101U, false),
                    "minimum retry interval is fixed-tick bounded");
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 102U, false),
                    "busy auxiliary stream does not start preparation");

    // Request flooding must not reset the busy-stream backoff.
    ServerSnapshotSchedulerRequest(&scheduler);
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 103U, true),
                    "duplicate request cannot bypass busy backoff");
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 104U, true),
                    "retired auxiliary stream becomes eligible after backoff");
    ServerSnapshotSchedulerFinish(&scheduler, 104U, true);
    SchedulerExpect(!ServerSnapshotSchedulerHasPending(&scheduler),
                    "successful retry coalesces pending requests");
}

static void TestFailureBackoffAndRecovery(void)
{
    ServerSnapshotScheduler scheduler;
    ServerSnapshotSchedulerInitialize(&scheduler, 0U);
    ServerSnapshotSchedulerRequest(&scheduler);

    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 0U, true),
                    "failure scenario begins");
    ServerSnapshotSchedulerFinish(&scheduler, 0U, false);
    SchedulerExpect(ServerSnapshotSchedulerHasPending(&scheduler) &&
                        ServerSnapshotSchedulerTransportInFlight(&scheduler),
                    "failed send remains pending and conservatively in-flight");
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 1U, true),
                    "failed send waits minimum backoff");
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 2U, true),
                    "failed send retries after minimum backoff");
    ServerSnapshotSchedulerFinish(&scheduler, 2U, false);
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, 5U, true),
                    "repeated failure uses exponential backoff");
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 6U, true),
                    "exponential retry reaches next deadline");
    ServerSnapshotSchedulerFinish(&scheduler, 6U, true);
}

static void TestTickWrap(void)
{
    ServerSnapshotScheduler scheduler;
    ServerSnapshotSchedulerInitialize(&scheduler, UINT32_MAX - 1U);
    ServerSnapshotSchedulerRequest(&scheduler);
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, UINT32_MAX - 1U, true),
                    "pre-wrap attempt begins");
    ServerSnapshotSchedulerFinish(&scheduler, UINT32_MAX - 1U, false);
    SchedulerExpect(!ServerSnapshotSchedulerTryBegin(&scheduler, UINT32_MAX, true),
                    "pre-wrap retry waits");
    SchedulerExpect(ServerSnapshotSchedulerTryBegin(&scheduler, 0U, true),
                    "retry deadline survives uint32 tick wrap");
    ServerSnapshotSchedulerFinish(&scheduler, 0U, true);
}

static void TestGlobalFixedTickBudget(void)
{
    ServerSnapshotWorkBudget budget;
    ServerSnapshotWorkBudgetInitialize(&budget, UINT32_MAX);
    SchedulerExpect(ServerSnapshotWorkBudgetAvailable(&budget, UINT32_MAX),
                    "new fixed tick has snapshot work budget");
    ServerSnapshotWorkBudgetConsume(&budget);
    SchedulerExpect(!ServerSnapshotWorkBudgetAvailable(&budget, UINT32_MAX),
                    "only one expensive snapshot is allowed per fixed tick");
    SchedulerExpect(ServerSnapshotWorkBudgetAvailable(&budget, 0U),
                    "fixed-tick budget resets across uint32 wrap");
}

LAIUE_TEST_ENTRY(ServerSnapshotSchedulerTestEntryPoint)
{
    TestSuccessfulSnapshotAndBusyBackoff();
    TestFailureBackoffAndRecovery();
    TestTickWrap();
    TestGlobalFixedTickBudget();
    SchedulerExpect(schedulerChecks == 21U, "all snapshot scheduler checks executed");
    LaiueTestRuntimeWrite("Server snapshot scheduler: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
