#pragma once

#include <stdbool.h>
#include <stdint.h>

// Snapshot preparation can be comparatively expensive. The scheduler keeps
// retries on the fixed 60 Hz server timeline, coalesces duplicate requests,
// and backs off while the previous QUIC auxiliary stream is still draining.
#define SERVER_SNAPSHOT_RETRY_MIN_TICKS 2U
#define SERVER_SNAPSHOT_RETRY_MAX_TICKS 64U

typedef struct ServerSnapshotScheduler
{
    uint32_t nextAttemptTick;
    uint32_t retryDelayTicks;
    bool pending;
    bool attemptActive;
    bool transportInFlight;
} ServerSnapshotScheduler;

// One shared budget per DedicatedServer. It prevents several peers from
// preparing large snapshots in separate ~1 ms outer-loop iterations that all
// belong to the same authoritative 60 Hz tick.
typedef struct ServerSnapshotWorkBudget
{
    uint32_t tick;
    bool consumed;
} ServerSnapshotWorkBudget;

void ServerSnapshotSchedulerInitialize(ServerSnapshotScheduler *scheduler, uint32_t currentTick);
void ServerSnapshotSchedulerRequest(ServerSnapshotScheduler *scheduler);

// transportCanBegin must come from NetworkServerCanBeginSnapshot. Returning
// true reserves exactly one preparation attempt until Finish is called.
bool ServerSnapshotSchedulerTryBegin(ServerSnapshotScheduler *scheduler, uint32_t currentTick,
                                     bool transportCanBegin);
void ServerSnapshotSchedulerFinish(ServerSnapshotScheduler *scheduler, uint32_t currentTick,
                                   bool snapshotSent);

bool ServerSnapshotSchedulerHasPending(const ServerSnapshotScheduler *scheduler);
bool ServerSnapshotSchedulerTransportInFlight(const ServerSnapshotScheduler *scheduler);

void ServerSnapshotWorkBudgetInitialize(ServerSnapshotWorkBudget *budget, uint32_t currentTick);
bool ServerSnapshotWorkBudgetAvailable(ServerSnapshotWorkBudget *budget, uint32_t currentTick);
void ServerSnapshotWorkBudgetConsume(ServerSnapshotWorkBudget *budget);
