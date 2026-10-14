#include "mod/module_api.h"

typedef struct ExampleService
{
    uint32_t value;
} ExampleService;

typedef struct ExampleContext
{
    const LaiueModuleHostV1 *host;
    ExampleService service;
} ExampleContext;

static const char *const provides[] = {"example.service"};
static ExampleContext context;

static uint32_t LAIUE_MODULE_CALL Create(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return 0u;
    context.host = host;
    context.service.value = 1u;
    *outContext = &context;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL Start(void *opaque)
{
    ExampleContext *example = opaque;
    LaiueModuleServiceV1 service = {
        .name = "example.service",
        .version = 1u,
        .table = &example->service,
        .tableSize = sizeof(example->service),
    };
    return example != NULL &&
                   example->host->publishService(example->host->context, &service) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void LAIUE_MODULE_CALL Stop(void *opaque)
{
    ExampleContext *example = opaque;
    if (example != NULL && example->host != NULL && example->host->unpublishService != NULL)
        (void)example->host->unpublishService(example->host->context, "example.service");
}

static void LAIUE_MODULE_CALL Destroy(void *opaque)
{
    (void)opaque;
}

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.service",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = Create,
    .start = Start,
    .stop = Stop,
    .destroy = Destroy,
};

LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
