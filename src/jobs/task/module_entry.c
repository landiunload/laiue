#include "task/task_service.h"

#include "platform/system.h"

typedef struct LaiueTaskModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueTaskModuleState;

static uint32_t GetExecutor(
    LaiueTaskPool *pool, LaiueTaskExecutor *outExecutor)
{
    return LaiueTaskPoolGetExecutor(pool, outExecutor) ? 1u : 0u;
}

static const LaiueTaskServiceV1 service = {
    .structSize = sizeof(LaiueTaskServiceV1),
    .abiVersion = LAIUE_TASK_SERVICE_ABI_VERSION_1,
    .createPool = LaiueTaskPoolCreate,
    .destroyPool = LaiueTaskPoolDestroy,
    .getExecutor = GetExecutor,
    .logicalProcessorCount = LaiueTaskLogicalProcessorCount,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL ||
        host->free == NULL)
        return 0u;
    *outContext = NULL;
    LaiueTaskModuleState *state =
        (LaiueTaskModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueTaskModuleState *state = (LaiueTaskModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_TASK_SERVICE_NAME,
        .version = LAIUE_TASK_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    LaiueTaskModuleState *state = (LaiueTaskModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_TASK_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueTaskModuleState *state = (LaiueTaskModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {LAIUE_TASK_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.task",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueTaskGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
