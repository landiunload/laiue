#include "animation/animation_service.h"

#include "content/content_service.h"
#include "mod/module_service.h"
#include "platform/system.h"

/* The service keeps one content table for the process, so only one module
 * instance may run at a time; a second host fails its create instead of
 * silently sharing or overwriting the first host's catalog provider. */
static volatile uint32_t g_instanceActive;

typedef struct AnimationModuleState
{
    const LaiueModuleHostV1 *host;
    uint32_t published;
} AnimationModuleState;

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
    AnimationModuleState *state =
        (AnimationModuleState *)host->allocate(host->context, sizeof(*state));
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
    AnimationModuleState *state = (AnimationModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    // Каталог необязателен: без него остаются явные файлы и память, а
    // операции с паками отказывают с понятным статусом.
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)LaiueModuleQueryOptionalService(
            state->host, LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
            sizeof(LaiueContentServiceV1));
    LaiueAnimationSetContentService(content);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_ANIMATION_SERVICE_NAME,
        .version = LAIUE_ANIMATION_SERVICE_ABI_VERSION_1,
        .table = LaiueAnimationGetStaticServiceV1(),
        .tableSize = sizeof(LaiueAnimationServiceV1),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        LaiueAnimationSetContentService(NULL);
        return 0u;
    }
    state->published = 1u;
    return 1u;
}

static void ModuleStop(void *context)
{
    AnimationModuleState *state = (AnimationModuleState *)context;
    if (state == NULL)
        return;
    if (state->published != 0u && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_ANIMATION_SERVICE_NAME);
    state->published = 0u;
    LaiueAnimationSetContentService(NULL);
}

static void ModuleDestroy(void *context)
{
    AnimationModuleState *state = (AnimationModuleState *)context;
    if (state == NULL)
        return;
    LaiueAnimationSetContentService(NULL);
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
    PlatformAtomicStoreU32Release(&g_instanceActive, 0u);
}

static const char *const provides[] = {LAIUE_ANIMATION_SERVICE_NAME};
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
            .id = "laiue.animation",
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

const LaiueModuleApiV1 *LaiueAnimationGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
