#pragma once

/* Standalone jobs provider contract. The pool remains opaque and all work is
 * submitted in batches through a borrowed executor table. */

#include "api.h"
#include "mod/module_api.h"
#include "task/task_pool.h"

#include <stdint.h>

#define LAIUE_TASK_SERVICE_ABI_VERSION_1 1u
#define LAIUE_TASK_SERVICE_NAME "laiue.jobs"

typedef LaiueTaskPool *(*LaiueTaskCreatePoolFn)(uint32_t threadCount);
typedef void (*LaiueTaskDestroyPoolFn)(LaiueTaskPool *pool);
typedef uint32_t (*LaiueTaskGetExecutorFn)(
    LaiueTaskPool *pool, LaiueTaskExecutor *outExecutor);
typedef uint32_t (*LaiueTaskLogicalProcessorCountFn)(void);

typedef struct LaiueTaskServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueTaskCreatePoolFn createPool;
    LaiueTaskDestroyPoolFn destroyPool;
    LaiueTaskGetExecutorFn getExecutor;
    LaiueTaskLogicalProcessorCountFn logicalProcessorCount;
    uintptr_t reserved[8];
} LaiueTaskServiceV1;

LAIUE_TASK_API const LaiueModuleApiV1 *LaiueTaskGetStaticModuleApiV1(void);
