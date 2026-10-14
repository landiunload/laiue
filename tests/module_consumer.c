#include "mod/module_api.h"

#include <stddef.h>

typedef struct CounterState
{
    uint32_t starts;
    uint32_t stops;
} CounterState;

static const LaiueModuleRequirementV1 requires[] = {{"example.counter", 1u}};

static uint32_t LAIUE_MODULE_CALL ConsumerCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->queryService == NULL)
        return 0u;
    uint32_t version = 0u;
    uint32_t size = 0u;
    CounterState *counter = (CounterState *)host->queryService(
        host->context, "example.counter", 1u, sizeof(CounterState), &version, &size);
    if (counter == NULL || version < 1u || size < sizeof(*counter))
        return 0u;
    *outContext = counter;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL ConsumerStart(void *context)
{
    CounterState *counter = context;
    if (counter == NULL)
        return 0u;
    ++counter->starts;
    return 1u;
}

static void LAIUE_MODULE_CALL ConsumerStop(void *context)
{
    CounterState *counter = context;
    if (counter != NULL)
        ++counter->stops;
}

static void LAIUE_MODULE_CALL ConsumerDestroy(void *context)
{
    (void)context;
}

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(api),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.consumer",
        .version = "1.0.0",
        .requiresServices = requires,
        .requiresCount = 1u,
    },
    .create = ConsumerCreate,
    .start = ConsumerStart,
    .stop = ConsumerStop,
    .destroy = ConsumerDestroy,
};

LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
