#include "model/model_service.h"

#include "content/content_service.h"
#include "mod/module_service.h"
#include "platform/system.h"

/* The service keeps one content table for the process, so only one module
 * instance may run at a time; a second host fails its create instead of
 * silently sharing or overwriting the first host's catalog provider. */
static volatile uint32_t g_instanceActive;

typedef struct ModelModuleState
{
    const LaiueModuleHostV1 *host;
    uint32_t published;
} ModelModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL || host->allocate == NULL ||
        host->free == NULL)
        return 0u;
    *outContext = NULL;
    uint32_t expected = 0u;
    if (!PlatformAtomicCompareExchangeU32(&g_instanceActive, &expected, 1u))
        return 0u;
    ModelModuleState *state = (ModelModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
    {
        PlatformAtomicStoreU32Release(&g_instanceActive, 0u);
        return 0u;
    }
    state->host = host;
    state->published = 0u;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    ModelModuleState *state = (ModelModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    // Каталог необязателен: без него остаются явные файлы и память, а
    // операции с паками отказывают с понятным статусом.
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)LaiueModuleQueryOptionalService(
            state->host, LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
            sizeof(LaiueContentServiceV1));
    LaiueModelSetContentService(content);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_MODEL_SERVICE_NAME,
        .version = LAIUE_MODEL_SERVICE_ABI_VERSION_1,
        .table = LaiueModelGetStaticServiceV1(),
        .tableSize = sizeof(LaiueModelServiceV1),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        LaiueModelSetContentService(NULL);
        return 0u;
    }
    state->published = 1u;
    return 1u;
}

static void ModuleStop(void *context)
{
    ModelModuleState *state = (ModelModuleState *)context;
    if (state == NULL)
        return;
    if (state->published != 0u && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_MODEL_SERVICE_NAME);
    state->published = 0u;
    LaiueModelSetContentService(NULL);
}

static void ModuleDestroy(void *context)
{
    ModelModuleState *state = (ModelModuleState *)context;
    if (state == NULL)
        return;
    LaiueModelSetContentService(NULL);
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
    PlatformAtomicStoreU32Release(&g_instanceActive, 0u);
}

static const char *const provides[] = {LAIUE_MODEL_SERVICE_NAME};
static const LaiueModuleRequirementV1 optionalServices[] = {
    {LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.model",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
            .optionalServices = optionalServices,
            .optionalCount = sizeof(optionalServices) / sizeof(optionalServices[0]),
            .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueModelGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
