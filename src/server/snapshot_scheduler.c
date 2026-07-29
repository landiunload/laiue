#include "server/snapshot_scheduler.h"

#include <stddef.h>

static bool ServerSnapshotTickReached(uint32_t currentTick, uint32_t targetTick)
{
    return currentTick == targetTick || currentTick - targetTick < 0x80000000U;
}

static void ServerSnapshotScheduleRetry(ServerSnapshotScheduler *scheduler, uint32_t currentTick)
{
    scheduler->nextAttemptTick = currentTick + scheduler->retryDelayTicks;
    if (scheduler->retryDelayTicks < SERVER_SNAPSHOT_RETRY_MAX_TICKS)
    {
        uint32_t doubled = scheduler->retryDelayTicks * 2U;
        scheduler->retryDelayTicks =
            doubled > SERVER_SNAPSHOT_RETRY_MAX_TICKS ? SERVER_SNAPSHOT_RETRY_MAX_TICKS : doubled;
    }
}

void ServerSnapshotSchedulerInitialize(ServerSnapshotScheduler *scheduler, uint32_t currentTick)
{
    if (scheduler == NULL)
        return;
    scheduler->nextAttemptTick = currentTick;
    scheduler->retryDelayTicks = SERVER_SNAPSHOT_RETRY_MIN_TICKS;
    scheduler->pending = false;
    scheduler->attemptActive = false;
    scheduler->transportInFlight = false;
}

void ServerSnapshotSchedulerRequest(ServerSnapshotScheduler *scheduler)
{
    if (scheduler == NULL)
        return;
    // Do not move nextAttemptTick: repeated interest changes and duplicate
    // resync requests must not defeat an active transport/error backoff.
    scheduler->pending = true;
}

bool ServerSnapshotSchedulerTryBegin(ServerSnapshotScheduler *scheduler, uint32_t currentTick,
                                     bool transportCanBegin)
{
    if (scheduler == NULL || !scheduler->pending || scheduler->attemptActive ||
        !ServerSnapshotTickReached(currentTick, scheduler->nextAttemptTick))
    {
        return false;
    }
    if (!transportCanBegin)
    {
        scheduler->transportInFlight = true;
        ServerSnapshotScheduleRetry(scheduler, currentTick);
        return false;
    }

    scheduler->transportInFlight = false;
    scheduler->attemptActive = true;
    return true;
}

void ServerSnapshotSchedulerFinish(ServerSnapshotScheduler *scheduler, uint32_t currentTick,
                                   bool snapshotSent)
{
    if (scheduler == NULL || !scheduler->attemptActive)
        return;
    scheduler->attemptActive = false;
    scheduler->transportInFlight = snapshotSent;
    if (snapshotSent)
    {
        scheduler->pending = false;
        scheduler->retryDelayTicks = SERVER_SNAPSHOT_RETRY_MIN_TICKS;
        scheduler->nextAttemptTick = currentTick + SERVER_SNAPSHOT_RETRY_MIN_TICKS;
        return;
    }

    scheduler->pending = true;
    // A failed send may have opened the stream before pressure or another
    // transport error was reported. Treat it as in-flight until the transport
    // explicitly reports that a new snapshot can begin.
    scheduler->transportInFlight = true;
    ServerSnapshotScheduleRetry(scheduler, currentTick);
}

bool ServerSnapshotSchedulerHasPending(const ServerSnapshotScheduler *scheduler)
{
    return scheduler != NULL && scheduler->pending;
}

bool ServerSnapshotSchedulerTransportInFlight(const ServerSnapshotScheduler *scheduler)
{
    return scheduler != NULL && scheduler->transportInFlight;
}

void ServerSnapshotWorkBudgetInitialize(ServerSnapshotWorkBudget *budget, uint32_t currentTick)
{
    if (budget == NULL)
        return;
    budget->tick = currentTick;
    budget->consumed = false;
}

bool ServerSnapshotWorkBudgetAvailable(ServerSnapshotWorkBudget *budget, uint32_t currentTick)
{
    if (budget == NULL)
        return false;
    if (budget->tick != currentTick)
    {
        budget->tick = currentTick;
        budget->consumed = false;
    }
    return !budget->consumed;
}

void ServerSnapshotWorkBudgetConsume(ServerSnapshotWorkBudget *budget)
{
    if (budget != NULL)
        budget->consumed = true;
}
