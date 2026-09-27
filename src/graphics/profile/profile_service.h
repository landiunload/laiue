#pragma once

/* Portable, backend-independent aggregation of completed renderer timings. */

#include "render/renderer.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_GRAPHICS_PROFILE_SERVICE_NAME "laiue.graphics.profile"
#define LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1 1u
#define LAIUE_GRAPHICS_PROFILE_HISTORY_CAPACITY 120u

typedef struct LaiueGraphicsProfileHistory LaiueGraphicsProfileHistory;

typedef uint32_t LaiueGraphicsProfileStatus;
#define LAIUE_GRAPHICS_PROFILE_OK UINT32_C(0)
#define LAIUE_GRAPHICS_PROFILE_INVALID_ARGUMENT UINT32_C(1)
#define LAIUE_GRAPHICS_PROFILE_UNSUPPORTED UINT32_C(2)
#define LAIUE_GRAPHICS_PROFILE_INVALID_SAMPLE UINT32_C(3)
#define LAIUE_GRAPHICS_PROFILE_OUT_OF_ORDER UINT32_C(4)
#define LAIUE_GRAPHICS_PROFILE_EMPTY UINT32_C(5)
#define LAIUE_GRAPHICS_PROFILE_OUT_OF_MEMORY UINT32_C(6)

typedef struct LaiueGraphicsProfileSummaryV1
{
    uint32_t structSize;
    uint32_t sampleCount;
    uint64_t latestFrameIndex;
    uint64_t latestNanoseconds;
    uint64_t minimumNanoseconds;
    uint64_t medianNanoseconds; /* Even counts average the two middle values, rounded down. */
    uint64_t p95Nanoseconds;    /* Nearest-rank percentile. */
    uint64_t maximumNanoseconds;
} LaiueGraphicsProfileSummaryV1;

/* Each history is caller-synchronized. submitTiming accepts only supported,
 * valid, nonzero completed timings with strictly increasing accepted frame
 * indices. Summary reads use bounded stack scratch and do not allocate.
 * Destroy every history before unloading its provider; its allocator callbacks
 * are owned by that provider's host. */
typedef struct LaiueGraphicsProfileServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueGraphicsProfileStatus (*historyCreate)(void *serviceContext,
                                                LaiueGraphicsProfileHistory **outHistory);
    void (*historyDestroy)(LaiueGraphicsProfileHistory *history);
    LaiueGraphicsProfileStatus (*submitTiming)(LaiueGraphicsProfileHistory *history,
                                               const RendererGpuTimingV1 *timing);
    LaiueGraphicsProfileStatus (*getSummary)(const LaiueGraphicsProfileHistory *history,
                                             LaiueGraphicsProfileSummaryV1 *outSummary);
    void (*reset)(LaiueGraphicsProfileHistory *history);
    /* Opaque per-module context passed to historyCreate. */
    void *context;
} LaiueGraphicsProfileServiceV1;

/* The static service is a template with context == NULL. Static users pass a
 * LaiueModuleHostV1 pointer to historyCreate; the runtime module sets context
 * to its host and consumers then pass service->context. */
const LaiueGraphicsProfileServiceV1 *LaiueGraphicsProfileGetStaticServiceV1(void);
const LaiueModuleApiV1 *LaiueGraphicsProfileGetStaticModuleApiV1(void);
