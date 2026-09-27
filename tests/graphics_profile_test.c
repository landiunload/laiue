#include "profile/profile_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static union
{
    uint64_t alignment;
    uint8_t bytes[2048];
} allocation;
static uint32_t allocationCount;
static uint32_t freeCount;
static uint32_t failureCount;
static bool failAllocation;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failureCount;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static void *LAIUE_MODULE_CALL TestAllocate(void *context, uint64_t size)
{
    (void)context;
    if (failAllocation || size > sizeof(allocation.bytes) || allocationCount != 0u)
        return NULL;
    ++allocationCount;
    return allocation.bytes;
}

static void LAIUE_MODULE_CALL TestFree(void *context, void *memory)
{
    (void)context;
    if (memory == allocation.bytes)
        ++freeCount;
}

static RendererGpuTimingV1 MakeTiming(uint64_t frameIndex, uint64_t nanoseconds)
{
    RendererGpuTimingV1 timing = {
        .structSize = sizeof(RendererGpuTimingV1),
        .flags = RENDERER_GPU_TIMING_SUPPORTED | RENDERER_GPU_TIMING_VALID,
        .frameIndex = frameIndex,
        .durationNanoseconds = nanoseconds,
    };
    return timing;
}

LAIUE_TEST_ENTRY(GraphicsProfileTestEntryPoint)
{
    const LaiueGraphicsProfileServiceV1 *service =
        LaiueGraphicsProfileGetStaticServiceV1();
    LaiueModuleHostV1 host;
    memset(&host, 0, sizeof(host));
    host.structSize = sizeof(host);
    host.abiVersion = LAIUE_MODULE_ABI_VERSION_1;
    host.allocate = TestAllocate;
    host.free = TestFree;

    LaiueGraphicsProfileHistory *history = NULL;
    Expect(service != NULL && service->structSize == sizeof(*service),
           "profile service reports its ABI size");
    failAllocation = true;
    Expect(service->historyCreate(&host, &history) == LAIUE_GRAPHICS_PROFILE_OUT_OF_MEMORY &&
               history == NULL,
           "failed host allocation is reported without a partial history");
    failAllocation = false;
    Expect(service->historyCreate(&host, &history) == LAIUE_GRAPHICS_PROFILE_OK &&
               history != NULL && allocationCount == 1u,
           "history uses one host allocation");

    LaiueGraphicsProfileSummaryV1 summary = {
        .structSize = sizeof(summary),
    };
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_EMPTY &&
               summary.structSize == sizeof(summary) && summary.sampleCount == 0u,
           "empty history is explicit");
    summary.structSize--;
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT,
           "short output ABI is rejected");
    summary.structSize = sizeof(summary);

    RendererGpuTimingV1 timing = MakeTiming(0u, 10u);
    timing.flags = 0u;
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_UNSUPPORTED,
           "unsupported backend sample is rejected");
    timing.flags = RENDERER_GPU_TIMING_SUPPORTED;
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE,
           "incomplete sample is rejected");
    timing = MakeTiming(0u, 0u);
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE,
           "zero-duration sample is rejected");
    timing = MakeTiming(0u, 10u);
    timing.structSize--;
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE,
           "short timing ABI is rejected");

    timing.structSize = sizeof(timing);
    for (uint64_t frame = 0u; frame < 4u; ++frame)
    {
        timing = MakeTiming(frame, (frame + 1u) * 10u);
        Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OK,
               "valid frame timing is accepted");
    }
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OUT_OF_ORDER,
           "duplicate frame timing is rejected");
    summary.structSize = sizeof(summary);
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_OK &&
               summary.sampleCount == 4u && summary.latestFrameIndex == 3u &&
               summary.latestNanoseconds == 40u && summary.minimumNanoseconds == 10u &&
               summary.medianNanoseconds == 25u && summary.p95Nanoseconds == 40u &&
               summary.maximumNanoseconds == 40u,
           "median and nearest-rank p95 are correct for an even sample count");

    service->reset(history);
    for (uint64_t frame = 0u; frame < LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY; ++frame)
    {
        timing = MakeTiming(frame, 100u + frame);
        Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OK,
               "sample enters bounded rolling history");
    }
    Expect(allocationCount == 1u,
           "submitting samples performs no additional allocation");
    timing = MakeTiming(LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY, 7u);
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OK,
           "new sample replaces the oldest history entry");
    summary.structSize = sizeof(summary);
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_OK &&
               summary.sampleCount == LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY &&
               summary.latestFrameIndex == LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY &&
               summary.minimumNanoseconds == 7u && summary.maximumNanoseconds == 219u &&
               summary.medianNanoseconds == 159u && summary.p95Nanoseconds == 213u,
           "rolling window evicts the oldest sample and sorts percentiles correctly");

    service->reset(history);
    timing = MakeTiming(0u, UINT64_MAX);
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OK,
           "largest representable duration is accepted");
    summary.structSize = sizeof(summary);
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_OK &&
               summary.minimumNanoseconds == UINT64_MAX &&
               summary.medianNanoseconds == UINT64_MAX &&
               summary.p95Nanoseconds == UINT64_MAX &&
               summary.maximumNanoseconds == UINT64_MAX,
           "single-sample statistics do not overflow");

    service->historyDestroy(history);
    Expect(freeCount == 1u, "history releases its host allocation");
    if (failureCount != 0u)
        LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
