#include "frame_monitor/frame_monitor.h"

#include "platform/system.h"

#include <string.h>

#define ZONE_NAME_CAPACITY 32u
#define ZONE_STACK_DEPTH 16u

struct LaiueFrameMonitor
{
    LaiueFrameMonitorDesc desc;
    char zoneNames[LAIUE_FRAME_MONITOR_MAXIMUM_ZONES][ZONE_NAME_CAPACITY];
    uint32_t zoneCount;

    /* Кольцо истории: длительность кадра и время каждой зоны в нём. */
    uint64_t *frameNanoseconds;
    uint64_t *zoneNanoseconds; /* historyFrames × MAXIMUM_ZONES */
    uint32_t head;             /* куда пишется следующий кадр */
    uint32_t filled;
    uint64_t totalFrames;
    uint64_t totalSpikes;

    /* Текущий кадр. */
    uint64_t frameStart;
    bool inFrame;
    uint64_t currentZones[LAIUE_FRAME_MONITOR_MAXIMUM_ZONES];
    uint32_t stackZone[ZONE_STACK_DEPTH];
    uint64_t stackStart[ZONE_STACK_DEPTH];
    uint32_t stackDepth;

    /* Медиана для порога спайков пересчитывается редко. */
    uint64_t medianNanoseconds;
    uint32_t framesSinceMedian;

    LaiueFrameMonitorSpike *spikes;
    uint32_t spikeHead;
    uint32_t spikeFilled;

    uint64_t *scratch; /* historyFrames: копия для выбора перцентилей */
};

bool LaiueFrameMonitorCreate(const LaiueFrameMonitorDesc *desc, LaiueFrameMonitor **outMonitor)
{
    if (outMonitor != NULL)
        *outMonitor = NULL;
    if (desc == NULL || outMonitor == NULL || desc->structSize < sizeof(*desc) ||
        desc->historyFrames < 16u || desc->historyFrames > LAIUE_FRAME_MONITOR_MAXIMUM_HISTORY ||
        desc->spikeCapacity == 0u || desc->spikeCapacity > 4096u)
        return false;
    LaiueFrameMonitor *monitor = PlatformAllocate(sizeof(*monitor), true);
    if (monitor == NULL)
        return false;
    monitor->desc = *desc;
    monitor->desc.structSize = sizeof(monitor->desc);
    const size_t history = desc->historyFrames;
    monitor->frameNanoseconds = PlatformAllocate(history * sizeof(uint64_t), true);
    monitor->zoneNanoseconds =
        PlatformAllocate(history * LAIUE_FRAME_MONITOR_MAXIMUM_ZONES * sizeof(uint64_t), true);
    monitor->scratch = PlatformAllocate(history * sizeof(uint64_t), false);
    monitor->spikes = PlatformAllocate(desc->spikeCapacity * sizeof(LaiueFrameMonitorSpike), true);
    if (monitor->frameNanoseconds == NULL || monitor->zoneNanoseconds == NULL ||
        monitor->scratch == NULL || monitor->spikes == NULL)
    {
        LaiueFrameMonitorDestroy(monitor);
        return false;
    }
    *outMonitor = monitor;
    return true;
}

void LaiueFrameMonitorDestroy(LaiueFrameMonitor *monitor)
{
    if (monitor == NULL)
        return;
    PlatformFree(monitor->frameNanoseconds);
    PlatformFree(monitor->zoneNanoseconds);
    PlatformFree(monitor->scratch);
    PlatformFree(monitor->spikes);
    PlatformFree(monitor);
}

void LaiueFrameMonitorReset(LaiueFrameMonitor *monitor)
{
    if (monitor == NULL)
        return;
    monitor->head = 0u;
    monitor->filled = 0u;
    monitor->totalFrames = 0u;
    monitor->totalSpikes = 0u;
    monitor->inFrame = false;
    monitor->stackDepth = 0u;
    monitor->medianNanoseconds = 0u;
    monitor->framesSinceMedian = 0u;
    monitor->spikeHead = 0u;
    monitor->spikeFilled = 0u;
}

uint32_t LaiueFrameMonitorRegisterZone(LaiueFrameMonitor *monitor, const char *name)
{
    if (monitor == NULL || name == NULL || name[0] == '\0')
        return LAIUE_FRAME_MONITOR_INVALID_ZONE;
    char copy[ZONE_NAME_CAPACITY] = {0};
    uint32_t length = 0u;
    while (name[length] != '\0' && length + 1u < ZONE_NAME_CAPACITY)
    {
        copy[length] = name[length];
        ++length;
    }
    for (uint32_t zone = 0u; zone < monitor->zoneCount; ++zone)
        if (memcmp(monitor->zoneNames[zone], copy, ZONE_NAME_CAPACITY) == 0)
            return zone;
    if (monitor->zoneCount == LAIUE_FRAME_MONITOR_MAXIMUM_ZONES)
        return LAIUE_FRAME_MONITOR_INVALID_ZONE;
    memcpy(monitor->zoneNames[monitor->zoneCount], copy, ZONE_NAME_CAPACITY);
    return monitor->zoneCount++;
}

const char *LaiueFrameMonitorZoneName(const LaiueFrameMonitor *monitor, uint32_t zone)
{
    return monitor == NULL || zone >= monitor->zoneCount ? NULL : monitor->zoneNames[zone];
}

/* k-й по величине из values[0..count) выбором Хоара; порядок массива
 * портится, поэтому работа идёт по копии. */
static uint64_t Select(uint64_t *values, uint32_t count, uint32_t rank)
{
    uint32_t low = 0u;
    uint32_t high = count - 1u;
    while (low < high)
    {
        const uint64_t pivot = values[low + (high - low) / 2u];
        uint32_t left = low;
        uint32_t right = high;
        while (left <= right)
        {
            while (values[left] < pivot)
                ++left;
            while (values[right] > pivot)
                --right;
            if (left <= right)
            {
                const uint64_t swap = values[left];
                values[left] = values[right];
                values[right] = swap;
                ++left;
                if (right == 0u)
                    break;
                --right;
            }
        }
        if (rank <= right)
            high = right;
        else if (rank >= left)
            low = left;
        else
            return values[rank];
    }
    return values[rank];
}

/* Перцентиль по ближайшему рангу: наименьшее значение, не меньше которого
 * percent/1000 доли выборки. */
static uint64_t Percentile(uint64_t *values, uint32_t count, uint32_t perMille)
{
    uint64_t rank = ((uint64_t)count * perMille + 999u) / 1000u;
    if (rank == 0u)
        rank = 1u;
    return Select(values, count, (uint32_t)(rank - 1u));
}

static void RecordSpike(LaiueFrameMonitor *monitor, uint64_t duration, uint64_t threshold,
                        const uint64_t *zones)
{
    LaiueFrameMonitorSpike *spike = &monitor->spikes[monitor->spikeHead];
    memset(spike, 0, sizeof(*spike));
    spike->structSize = sizeof(*spike);
    spike->frameIndex = monitor->totalFrames;
    spike->durationNanoseconds = duration;
    spike->thresholdNanoseconds = threshold;
    for (uint32_t slot = 0u; slot < LAIUE_FRAME_MONITOR_SPIKE_ZONES; ++slot)
        spike->zone[slot] = LAIUE_FRAME_MONITOR_INVALID_ZONE;
    for (uint32_t zone = 0u; zone < monitor->zoneCount; ++zone)
    {
        uint64_t value = zones[zone];
        uint32_t id = zone;
        for (uint32_t slot = 0u; slot < LAIUE_FRAME_MONITOR_SPIKE_ZONES && value != 0u; ++slot)
            if (spike->zone[slot] == LAIUE_FRAME_MONITOR_INVALID_ZONE ||
                value > spike->zoneNanoseconds[slot])
            {
                const uint64_t movedValue = spike->zoneNanoseconds[slot];
                const uint32_t movedId = spike->zone[slot];
                spike->zoneNanoseconds[slot] = value;
                spike->zone[slot] = id;
                value = movedId == LAIUE_FRAME_MONITOR_INVALID_ZONE ? 0u : movedValue;
                id = movedId;
            }
    }
    monitor->spikeHead = (monitor->spikeHead + 1u) % monitor->desc.spikeCapacity;
    if (monitor->spikeFilled < monitor->desc.spikeCapacity)
        ++monitor->spikeFilled;
    ++monitor->totalSpikes;
}

static void CommitFrame(LaiueFrameMonitor *monitor, uint64_t duration, const uint64_t *zones)
{
    uint64_t threshold = UINT64_MAX;
    if (monitor->desc.budgetNanoseconds != 0u)
        threshold = monitor->desc.budgetNanoseconds;
    if (monitor->desc.spikeMedianPercent != 0u && monitor->medianNanoseconds != 0u)
    {
        const uint64_t relative =
            monitor->medianNanoseconds * monitor->desc.spikeMedianPercent / 100u;
        if (relative < threshold)
            threshold = relative;
    }
    if (duration > threshold)
        RecordSpike(monitor, duration, threshold, zones);

    monitor->frameNanoseconds[monitor->head] = duration;
    memcpy(&monitor->zoneNanoseconds[(size_t)monitor->head * LAIUE_FRAME_MONITOR_MAXIMUM_ZONES],
           zones, LAIUE_FRAME_MONITOR_MAXIMUM_ZONES * sizeof(uint64_t));
    monitor->head = (monitor->head + 1u) % monitor->desc.historyFrames;
    if (monitor->filled < monitor->desc.historyFrames)
        ++monitor->filled;
    ++monitor->totalFrames;

    /* Медиана раз в четверть окна: выбор стоит O(n), а кадр — O(1). */
    if (++monitor->framesSinceMedian >= monitor->desc.historyFrames / 4u ||
        monitor->medianNanoseconds == 0u)
    {
        memcpy(monitor->scratch, monitor->frameNanoseconds, monitor->filled * sizeof(uint64_t));
        monitor->medianNanoseconds = Percentile(monitor->scratch, monitor->filled, 500u);
        monitor->framesSinceMedian = 0u;
    }
}

void LaiueFrameMonitorBeginFrame(LaiueFrameMonitor *monitor)
{
    if (monitor == NULL)
        return;
    memset(monitor->currentZones, 0, sizeof(monitor->currentZones));
    monitor->stackDepth = 0u;
    monitor->inFrame = true;
    monitor->frameStart = PlatformMonotonicNanoseconds();
}

void LaiueFrameMonitorEndFrame(LaiueFrameMonitor *monitor)
{
    if (monitor == NULL || !monitor->inFrame)
        return;
    const uint64_t now = PlatformMonotonicNanoseconds();
    /* Незакрытые зоны закрываются концом кадра. */
    while (monitor->stackDepth != 0u)
    {
        --monitor->stackDepth;
        monitor->currentZones[monitor->stackZone[monitor->stackDepth]] +=
            now - monitor->stackStart[monitor->stackDepth];
    }
    monitor->inFrame = false;
    CommitFrame(monitor, now - monitor->frameStart, monitor->currentZones);
}

void LaiueFrameMonitorBeginZone(LaiueFrameMonitor *monitor, uint32_t zone)
{
    if (monitor == NULL || !monitor->inFrame || zone >= monitor->zoneCount ||
        monitor->stackDepth == ZONE_STACK_DEPTH)
        return;
    monitor->stackZone[monitor->stackDepth] = zone;
    monitor->stackStart[monitor->stackDepth] = PlatformMonotonicNanoseconds();
    ++monitor->stackDepth;
}

void LaiueFrameMonitorEndZone(LaiueFrameMonitor *monitor, uint32_t zone)
{
    if (monitor == NULL || monitor->stackDepth == 0u ||
        monitor->stackZone[monitor->stackDepth - 1u] != zone)
        return;
    --monitor->stackDepth;
    monitor->currentZones[zone] +=
        PlatformMonotonicNanoseconds() - monitor->stackStart[monitor->stackDepth];
}

void LaiueFrameMonitorSubmitFrame(LaiueFrameMonitor *monitor, uint64_t durationNanoseconds,
                                  const uint64_t *zoneNanoseconds, uint32_t zoneCount)
{
    if (monitor == NULL || monitor->inFrame || (zoneNanoseconds == NULL && zoneCount != 0u))
        return;
    uint64_t zones[LAIUE_FRAME_MONITOR_MAXIMUM_ZONES] = {0};
    for (uint32_t zone = 0u; zone < zoneCount && zone < monitor->zoneCount; ++zone)
        zones[zone] = zoneNanoseconds[zone];
    CommitFrame(monitor, durationNanoseconds, zones);
}

static bool Summarize(const LaiueFrameMonitor *monitor, int64_t zone,
                      LaiueFrameMonitorSummary *outSummary)
{
    if (monitor == NULL || outSummary == NULL || outSummary->structSize < sizeof(*outSummary))
        return false;
    const uint32_t structSize = outSummary->structSize;
    memset(outSummary, 0, sizeof(*outSummary));
    outSummary->structSize = structSize;
    outSummary->totalFrames = monitor->totalFrames;
    outSummary->totalSpikes = monitor->totalSpikes;
    const uint32_t count = monitor->filled;
    outSummary->frames = count;
    if (count == 0u)
        return true;
    uint64_t *values = monitor->scratch;
    uint64_t sum = 0u;
    uint64_t maximum = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint64_t value =
            zone < 0 ? monitor->frameNanoseconds[index]
                     : monitor->zoneNanoseconds[(size_t)index * LAIUE_FRAME_MONITOR_MAXIMUM_ZONES +
                                                (size_t)zone];
        values[index] = value;
        sum += value;
        if (value > maximum)
            maximum = value;
    }
    outSummary->meanNanoseconds = sum / count;
    outSummary->maximumNanoseconds = maximum;
    if (zone < 0 && sum != 0u)
        outSummary->framesPerSecondMilli = (uint64_t)count * UINT64_C(1000000000000) / sum;
    outSummary->p50Nanoseconds = Percentile(values, count, 500u);
    outSummary->p90Nanoseconds = Percentile(values, count, 900u);
    outSummary->p99Nanoseconds = Percentile(values, count, 990u);
    outSummary->p999Nanoseconds = Percentile(values, count, 999u);
    return true;
}

bool LaiueFrameMonitorGetSummary(const LaiueFrameMonitor *monitor,
                                 LaiueFrameMonitorSummary *outSummary)
{
    return Summarize(monitor, -1, outSummary);
}

bool LaiueFrameMonitorGetZoneSummary(const LaiueFrameMonitor *monitor, uint32_t zone,
                                     LaiueFrameMonitorSummary *outSummary)
{
    if (monitor == NULL || zone >= monitor->zoneCount)
        return false;
    return Summarize(monitor, (int64_t)zone, outSummary);
}

uint32_t LaiueFrameMonitorSpikeCount(const LaiueFrameMonitor *monitor)
{
    return monitor == NULL ? 0u : monitor->spikeFilled;
}

bool LaiueFrameMonitorGetSpike(const LaiueFrameMonitor *monitor, uint32_t index,
                               LaiueFrameMonitorSpike *outSpike)
{
    if (monitor == NULL || outSpike == NULL || outSpike->structSize < sizeof(*outSpike) ||
        index >= monitor->spikeFilled)
        return false;
    const uint32_t capacity = monitor->desc.spikeCapacity;
    const uint32_t slot = (monitor->spikeHead + capacity - 1u - index) % capacity;
    const uint32_t structSize = outSpike->structSize;
    *outSpike = monitor->spikes[slot];
    outSpike->structSize = structSize;
    return true;
}
