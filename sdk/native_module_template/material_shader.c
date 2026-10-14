#include "mod/module_api.h"

#include <stdint.h>

typedef struct ExampleMaterialShaderV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    const char *materialName;
    const uint8_t *shaderBytes;
    uint32_t shaderSize;
} ExampleMaterialShaderV1;

/* The sample deliberately uses a tiny namespaced payload. Real modules can
 * publish compiled DXIL/SPIR-V selected by the active graphics provider. */
static const uint8_t shaderBytes[] = {0x4cu, 0x41u, 0x49u, 0x55u, 0x45u};
static const ExampleMaterialShaderV1 service = {
    .structSize = sizeof(ExampleMaterialShaderV1),
    .abiVersion = 1u,
    .materialName = "example.material",
    .shaderBytes = shaderBytes,
    .shaderSize = sizeof(shaderBytes),
};

static const char *const provides[] = {"example.material.shader"};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {"laiue.graphics.device", 1u},
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
        .name = "example.material.shader",
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
        (void)host->unpublishService(host->context, "example.material.shader");
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
        .id = "example.material.shader",
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
