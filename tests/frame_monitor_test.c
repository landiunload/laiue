#include "frame_monitor/frame_monitor.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Frame monitor failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static uint32_t randomState = 0x2545F491u;

static uint32_t NextRandom(void)
{
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 17u;
    randomState ^= randomState << 5u;
    return randomState;
}

static LaiueFrameMonitor *Create(uint32_t history, uint64_t budget, uint32_t medianPercent)
{
    LaiueFrameMonitorDesc desc = {sizeof(desc), history, budget, medianPercent, 8u};
    LaiueFrameMonitor *monitor = NULL;
    Expect(LaiueFrameMonitorCreate(&desc, &monitor), "create");
    return monitor;
}

static LaiueFrameMonitorSummary Summary(const LaiueFrameMonitor *monitor)
{
    LaiueFrameMonitorSummary summary;
    memset(&summary, 0, sizeof(summary));
    summary.structSize = sizeof(summary);
    Expect(LaiueFrameMonitorGetSummary(monitor, &summary), "summary");
    return summary;
}

/* Эталон перцентиля по ближайшему рангу: сортировка вставками. */
static uint64_t ReferencePercentile(const uint64_t *values, uint32_t count, uint32_t perMille)
{
    static uint64_t sorted[1024];
    memcpy(sorted, values, count * sizeof(uint64_t));
    for (uint32_t index = 1u; index < count; ++index)
    {
        const uint64_t value = sorted[index];
        uint32_t position = index;
        while (position != 0u && sorted[position - 1u] > value)
        {
            sorted[position] = sorted[position - 1u];
            --position;
        }
        sorted[position] = value;
    }
    uint64_t rank = ((uint64_t)count * perMille + 999u) / 1000u;
    return sorted[(rank == 0u ? 1u : rank) - 1u];
}

static void TestPercentiles(void)
{
    LaiueFrameMonitorDesc bad = {sizeof(bad), 8u, 0u, 0u, 8u};
    LaiueFrameMonitor *none = NULL;
    Expect(!LaiueFrameMonitorCreate(&bad, &none) && none == NULL, "tiny history rejected");

    for (uint32_t trial = 0u; trial < 20u; ++trial)
    {
        const uint32_t history = 16u + NextRandom() % 1000u;
        LaiueFrameMonitor *monitor = Create(history, 0u, 0u);
        static uint64_t values[1024];
        const uint32_t frames = history + NextRandom() % 300u;
        for (uint32_t frame = 0u; frame < frames; ++frame)
        {
            /* Много одинаковых значений проверяют выбор при равенствах. */
            const uint64_t duration = (uint64_t)(NextRandom() % 50u) * 100000u + 1u;
            LaiueFrameMonitorSubmitFrame(monitor, duration, NULL, 0u);
            values[frame % history] = duration;
        }
        const LaiueFrameMonitorSummary summary = Summary(monitor);
        Expect(summary.frames == history && summary.totalFrames == frames, "window counts");
        Expect(summary.p50Nanoseconds == ReferencePercentile(values, history, 500u), "p50");
        Expect(summary.p90Nanoseconds == ReferencePercentile(values, history, 900u), "p90");
        Expect(summary.p99Nanoseconds == ReferencePercentile(values, history, 990u), "p99");
        Expect(summary.p999Nanoseconds == ReferencePercentile(values, history, 999u), "p99.9");
        Expect(summary.maximumNanoseconds == ReferencePercentile(values, history, 1000u), "max");
        LaiueFrameMonitorDestroy(monitor);
    }

    LaiueFrameMonitor *steady = Create(60u, 0u, 0u);
    for (uint32_t frame = 0u; frame < 60u; ++frame)
        LaiueFrameMonitorSubmitFrame(steady, 16666667u, NULL, 0u);
    const LaiueFrameMonitorSummary summary = Summary(steady);
    Expect(summary.meanNanoseconds == 16666667u && summary.framesPerSecondMilli == 59999u,
           "steady 60 Hz mean and rate");
    LaiueFrameMonitorDestroy(steady);
}

static void TestSpikes(void)
{
    LaiueFrameMonitor *monitor = Create(64u, 20000000u, 300u);
    const uint32_t simulation = LaiueFrameMonitorRegisterZone(monitor, "simulation");
    const uint32_t render = LaiueFrameMonitorRegisterZone(monitor, "render");
    const uint32_t ui = LaiueFrameMonitorRegisterZone(monitor, "ui");
    const uint32_t audio = LaiueFrameMonitorRegisterZone(monitor, "audio");
    Expect(LaiueFrameMonitorRegisterZone(monitor, "render") == render, "zone names are unique");
    Expect(simulation == 0u && audio == 3u, "zone ids are dense");
    uint64_t zones[4] = {4000000u, 3000000u, 500000u, 200000u};
    for (uint32_t frame = 0u; frame < 64u; ++frame)
        LaiueFrameMonitorSubmitFrame(monitor, 8000000u, zones, 4u);
    Expect(LaiueFrameMonitorSpikeCount(monitor) == 0u, "steady frames are not spikes");

    /* Вдвое длиннее медианы, но в бюджете и ниже 300 %: не спайк. */
    LaiueFrameMonitorSubmitFrame(monitor, 16000000u, zones, 4u);
    Expect(LaiueFrameMonitorSpikeCount(monitor) == 0u, "within budget and below median factor");
    /* 3,5 медианы — спайк по медиане, хотя бюджет не превышен. */
    zones[1] = 20000000u;
    LaiueFrameMonitorSubmitFrame(monitor, 28000000u, zones, 4u);
    Expect(LaiueFrameMonitorSpikeCount(monitor) == 1u, "median spike recorded");
    LaiueFrameMonitorSpike spike;
    memset(&spike, 0, sizeof(spike));
    spike.structSize = sizeof(spike);
    Expect(LaiueFrameMonitorGetSpike(monitor, 0u, &spike), "spike read");
    Expect(spike.durationNanoseconds == 28000000u && spike.thresholdNanoseconds == 20000000u &&
               spike.frameIndex == 65u,
           "spike values");
    Expect(spike.zone[0] == render && spike.zone[1] == simulation && spike.zone[2] == ui &&
               spike.zoneNanoseconds[0] == 20000000u,
           "heaviest zones first");
    const char *name = LaiueFrameMonitorZoneName(monitor, spike.zone[0]);
    Expect(name != NULL && memcmp(name, "render", 7u) == 0, "zone name");

    for (uint32_t extra = 0u; extra < 10u; ++extra)
        LaiueFrameMonitorSubmitFrame(monitor, 50000000u + extra, zones, 4u);
    Expect(LaiueFrameMonitorSpikeCount(monitor) == 8u, "spike log is bounded");
    Expect(LaiueFrameMonitorGetSpike(monitor, 0u, &spike) && spike.durationNanoseconds == 50000009u,
           "newest spike first");
    LaiueFrameMonitorSummary zoneSummary;
    memset(&zoneSummary, 0, sizeof(zoneSummary));
    zoneSummary.structSize = sizeof(zoneSummary);
    Expect(LaiueFrameMonitorGetZoneSummary(monitor, simulation, &zoneSummary) &&
               zoneSummary.p50Nanoseconds == 4000000u,
           "zone percentiles");
    Expect(!LaiueFrameMonitorGetZoneSummary(monitor, 40u, &zoneSummary), "unknown zone");
    LaiueFrameMonitorReset(monitor);
    Expect(Summary(monitor).frames == 0u && LaiueFrameMonitorSpikeCount(monitor) == 0u, "reset");
    LaiueFrameMonitorDestroy(monitor);
}

static void TestClock(void)
{
    LaiueFrameMonitor *monitor = Create(16u, 0u, 0u);
    const uint32_t outer = LaiueFrameMonitorRegisterZone(monitor, "outer");
    const uint32_t inner = LaiueFrameMonitorRegisterZone(monitor, "inner");
    LaiueFrameMonitorBeginFrame(monitor);
    LaiueFrameMonitorBeginZone(monitor, outer);
    LaiueFrameMonitorBeginZone(monitor, inner);
    PlatformSleepMilliseconds(5u);
    LaiueFrameMonitorEndZone(monitor, inner);
    LaiueFrameMonitorEndZone(monitor, outer);
    LaiueFrameMonitorBeginZone(monitor, inner); /* закроется концом кадра */
    LaiueFrameMonitorEndFrame(monitor);
    LaiueFrameMonitorSummary frame = Summary(monitor);
    LaiueFrameMonitorSummary outerSummary;
    LaiueFrameMonitorSummary innerSummary;
    memset(&outerSummary, 0, sizeof(outerSummary));
    memset(&innerSummary, 0, sizeof(innerSummary));
    outerSummary.structSize = sizeof(outerSummary);
    innerSummary.structSize = sizeof(innerSummary);
    Expect(LaiueFrameMonitorGetZoneSummary(monitor, outer, &outerSummary) &&
               LaiueFrameMonitorGetZoneSummary(monitor, inner, &innerSummary),
           "zone summaries");
    Expect(innerSummary.maximumNanoseconds >= 4000000u, "sleep is measured");
    Expect(outerSummary.maximumNanoseconds >= innerSummary.maximumNanoseconds - 1000000u &&
               frame.maximumNanoseconds >= outerSummary.maximumNanoseconds,
           "nested zones are contained in the frame");
    const uint64_t first = PlatformMonotonicNanoseconds();
    const uint64_t second = PlatformMonotonicNanoseconds();
    Expect(second >= first, "monotonic clock");
    LaiueFrameMonitorDestroy(monitor);
}

LAIUE_TEST_ENTRY(FrameMonitorTestEntryPoint)
{
#if !defined(LAIUE_DIAGNOSTICS)
    Expect(false, "the frame monitor target must define LAIUE_DIAGNOSTICS for its consumers");
#endif
    TestPercentiles();
    TestSpikes();
    TestClock();
    LaiueTestRuntimeWrite("frame monitor: passed\n");
    LAIUE_TEST_SUCCESS();
}
