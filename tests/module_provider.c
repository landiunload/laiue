#include "mod/module_api.h"

#include <stddef.h>

typedef struct ProviderState
{
    uint32_t starts;
    uint32_t stops;
    const LaiueModuleHostV1 *host;
} ProviderState;

static ProviderState state;
static const char *const provides[] = {"example.counter"};
static LaiueModuleServiceV1 service = {0};

static uint32_t LAIUE_MODULE_CALL ProviderCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return 0u;
    state.starts = 0u;
    state.stops = 0u;
    state.host = host;
    *outContext = &state;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL ProviderStart(void *context)
{
    ProviderState *provider = context;
    if (provider == NULL)
        return 0u;
    service.name = "example.counter";
    service.version = 1u;
    service.table = provider;
    service.tableSize = sizeof(*provider);
    ++provider->starts;
    return provider->host->publishService(provider->host->context, &service) == LAIUE_MODULE_OK ? 1u : 0u;
}

static void LAIUE_MODULE_CALL ProviderStop(void *context)
{
    ProviderState *provider = context;
    if (provider != NULL)
    {
        if (provider->host != NULL && provider->host->unpublishService != NULL)
            (void)provider->host->unpublishService(provider->host->context, "example.counter");
        ++provider->stops;
    }
}

static void LAIUE_MODULE_CALL ProviderDestroy(void *context)
{
    (void)context;
}

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(api),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.provider",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ProviderCreate,
    .start = ProviderStart,
    .stop = ProviderStop,
    .destroy = ProviderDestroy,
};

LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
