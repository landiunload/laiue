#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct LaiueTaskPool LaiueTaskPool;

// Each callback owns the half-open range [begin, end). Ranges may execute in
// any order or concurrently; callbacks must synchronize shared writes outside
// their own range. Floating-point setup belongs to the callback, not the pool.
typedef void (*LaiueTaskRangeFunction)(void *jobContext, uint32_t begin, uint32_t end);

typedef struct LaiueTaskExecutor
{
    // Initialize to the complete caller-owned descriptor size before GetExecutor.
    uint32_t structSize;
    void *context;
    // Synchronous: every index in [0, count) completes exactly once before
    // returning. grain is the maximum range length; zero means one. count==0,
    // function==NULL or context==NULL is a no-op. jobContext may be NULL.
    void (*run)(void *context, uint32_t count, uint32_t grain, LaiueTaskRangeFunction function,
                void *jobContext);
} LaiueTaskExecutor;

// threadCount includes the calling thread and must be in [1, 64]. Workers are
// persistent; dispatch does not allocate. NULL reports invalid count or failure
// to create the complete pool, after cleaning up any partially created workers.
LAIUE_TASK_API LaiueTaskPool *LaiueTaskPoolCreate(uint32_t threadCount);

// A pool has one dispatching owner. run is non-reentrant: do not run or destroy
// this pool concurrently, or call either operation from one of its callbacks.
// Destroy joins all workers. Destroy(NULL) is harmless.
LAIUE_TASK_API void LaiueTaskPoolDestroy(LaiueTaskPool *pool);

// outExecutor->structSize must cover this version of LaiueTaskExecutor. Failure
// leaves the descriptor unchanged; success preserves structSize and extension
// bytes. The returned executor borrows pool and expires when pool is destroyed.
LAIUE_TASK_API bool LaiueTaskPoolGetExecutor(LaiueTaskPool *pool, LaiueTaskExecutor *outExecutor);

// Advisory platform count, at least one; creating a pool remains an explicit
// application choice so unrelated systems do not each spawn a hidden pool.
LAIUE_TASK_API uint32_t LaiueTaskLogicalProcessorCount(void);
