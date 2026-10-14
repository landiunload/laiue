#include "profile/profile_service.h"

#include <stddef.h>
#include <string.h>

struct LaiueGraphicsProfileHistory
{
    LaiueModuleFreeFn freeFn;
    void *freeContext;
    uint64_t timings[LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY];
    uint64_t latestFrameIndex;
    uint64_t latestNanoseconds;
    uint32_t count;
    uint32_t next;
    uint32_t hasFrameIndex;
};

static LaiueGraphicsProfileStatus HistoryCreate(void *serviceContext,
                                                LaiueGraphicsProfileHistory **outHistory)
{
    const LaiueModuleHostV1 *host = (const LaiueModuleHostV1 *)serviceContext;
    if (outHistory == NULL)
        return LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT;
    *outHistory = NULL;
    if (host == NULL || host->allocate == NULL || host->free == NULL)
        return LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT;

    LaiueGraphicsProfileHistory *history = (LaiueGraphicsProfileHistory *)
        host->allocate(host->context, sizeof(*history));
    if (history == NULL)
        return LAIUE_GRAPHICS_PROFILE_OUT_OF_MEMORY;
    memset(history, 0, sizeof(*history));
    history->freeFn = host->free;
    history->freeContext = host->context;
    *outHistory = history;
    return LAIUE_GRAPHICS_PROFILE_OK;
}

static void HistoryDestroy(LaiueGraphicsProfileHistory *history)
{
    if (history != NULL && history->freeFn != NULL)
        history->freeFn(history->freeContext, history);
}

static LaiueGraphicsProfileStatus SubmitTiming(LaiueGraphicsProfileHistory *history,
                                               const RendererGpuTimingV1 *timing)
{
    if (history == NULL || timing == NULL)
        return LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT;
    if (timing->structSize < sizeof(*timing))
        return LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE;
    if ((timing->flags & ~(RENDERER_GPU_TIMING_SUPPORTED |
                           RENDERER_GPU_TIMING_VALID)) != 0u)
        return LAIUE_GRAPHICS_PROFILE_UNSUPPORTED;
    if ((timing->flags & RENDERER_GPU_TIMING_SUPPORTED) == 0u)
        return LAIUE_GRAPHICS_PROFILE_UNSUPPORTED;
    if ((timing->flags & RENDERER_GPU_TIMING_VALID) == 0u)
        return LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE;
    if (timing->durationNanoseconds == 0u)
        return LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE;
    if (history->hasFrameIndex != 0u && timing->frameIndex <= history->latestFrameIndex)
        return LAIUE_GRAPHICS_PROFILE_OUT_OF_ORDER;

    history->timings[history->next] = timing->durationNanoseconds;
    history->next = (history->next + 1u) % LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY;
    if (history->count < LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY)
        ++history->count;
    history->latestFrameIndex = timing->frameIndex;
    history->latestNanoseconds = timing->durationNanoseconds;
    history->hasFrameIndex = 1u;
    return LAIUE_GRAPHICS_PROFILE_OK;
}

static void SortAscending(uint64_t *values, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        const uint64_t value = values[index];
        uint32_t insertion = index;
        while (insertion != 0u && values[insertion - 1u] > value)
        {
            values[insertion] = values[insertion - 1u];
            --insertion;
        }
        values[insertion] = value;
    }
}

static LaiueGraphicsProfileStatus GetSummary(const LaiueGraphicsProfileHistory *history,
                                             LaiueGraphicsProfileSummaryV1 *outSummary)
{
    if (history == NULL || outSummary == NULL)
        return LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT;
    if (outSummary->structSize < sizeof(*outSummary))
        return LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT;

    const uint32_t count = history->count;
    if (count == 0u)
    {
        memset(outSummary, 0, sizeof(*outSummary));
        outSummary->structSize = sizeof(*outSummary);
        return LAIUE_GRAPHICS_PROFILE_EMPTY;
    }

    /* Bounded stack scratch: summary reads never allocate or mutate history. */
    uint64_t sorted[LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY];
    for (uint32_t index = 0u; index < count; ++index)
        sorted[index] = history->timings[index];
    SortAscending(sorted, count);

    const uint32_t p95Rank = (count * 95u + 99u) / 100u; /* nearest-rank */
    outSummary->structSize = sizeof(*outSummary);
    outSummary->sampleCount = count;
    outSummary->latestFrameIndex = history->latestFrameIndex;
    outSummary->latestNanoseconds = history->latestNanoseconds;
    outSummary->minimumNanoseconds = sorted[0];
    if ((count & 1u) != 0u)
    {
        outSummary->medianNanoseconds = sorted[count / 2u];
    }
    else
    {
        const uint64_t lower = sorted[count / 2u - 1u];
        const uint64_t upper = sorted[count / 2u];
        outSummary->medianNanoseconds = lower + (upper - lower) / 2u;
    }
    outSummary->p95Nanoseconds = sorted[p95Rank - 1u];
    outSummary->maximumNanoseconds = sorted[count - 1u];
    return LAIUE_GRAPHICS_PROFILE_OK;
}

static void ResetHistory(LaiueGraphicsProfileHistory *history)
{
    if (history == NULL)
        return;
    memset(history->timings, 0, sizeof(history->timings));
    history->latestFrameIndex = 0u;
    history->latestNanoseconds = 0u;
    history->count = 0u;
    history->next = 0u;
    history->hasFrameIndex = 0u;
}

const LaiueGraphicsProfileServiceV1 *LaiueGraphicsProfileGetStaticServiceV1(void)
{
    static const LaiueGraphicsProfileServiceV1 service = {
        .structSize = sizeof(LaiueGraphicsProfileServiceV1),
        .abiVersion = LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1,
        .historyCreate = HistoryCreate,
        .historyDestroy = HistoryDestroy,
        .submitTiming = SubmitTiming,
        .getSummary = GetSummary,
        .reset = ResetHistory,
        .context = NULL,
    };
    return &service;
}
