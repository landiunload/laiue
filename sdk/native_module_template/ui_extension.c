#include "mod/module_api.h"

#include <stdint.h>

typedef struct ExampleUiExtensionV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    const char *panelId;
    const char *titleUtf8;
} ExampleUiExtensionV1;

static const ExampleUiExtensionV1 service = {
    .structSize = sizeof(ExampleUiExtensionV1),
    .abiVersion = 1u,
    .panelId = "example.settings",
    .titleUtf8 = "Example settings",
};

static const char *const provides[] = {"example.ui.extension"};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {"laiue.ui", 1u},
};

static uint32_t LAIUE_MODULE_CALL Create(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return 0u;
    *outContext = (void *)host;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL Start(void *opaque)
{
    const LaiueModuleHostV1 *host = (const LaiueModuleHostV1 *)opaque;
    LaiueModuleServiceV1 published = {
        .name = "example.ui.extension",
        .version = 1u,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return host != NULL &&
                   host->publishService(host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void LAIUE_MODULE_CALL Stop(void *opaque)
{
    const LaiueModuleHostV1 *host = (const LaiueModuleHostV1 *)opaque;
    if (host != NULL && host->unpublishService != NULL)
        (void)host->unpublishService(host->context, "example.ui.extension");
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
        .id = "example.ui.extension",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = sizeof(requiresServices) / sizeof(requiresServices[0]),
        .providesServices = provides,
        .providesCount = sizeof(provides) / sizeof(provides[0]),
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
