#include "scene/math_service.h"

#include "mod/module_api.h"
#include "platform/system.h"

typedef struct LaiueSceneMathModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueSceneMathModuleState;

static const LaiueSceneMathServiceV1 service = {
    .structSize = sizeof(LaiueSceneMathServiceV1),
    .abiVersion = LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
    .matrix4Multiply = Matrix4Multiply,
    .matrix4ExtractFrustumPlanes = Matrix4ExtractFrustumPlanes,
    .frustumIntersectsBox = FrustumIntersectsBox,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL)
        return 0u;
    *outContext = NULL;
    LaiueSceneMathModuleState *state =
        (LaiueSceneMathModuleState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueSceneMathModuleState *state = (LaiueSceneMathModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_SCENE_MATH_SERVICE_NAME,
        .version = LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
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
    LaiueSceneMathModuleState *state = (LaiueSceneMathModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_SCENE_MATH_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueSceneMathModuleState *state = (LaiueSceneMathModuleState *)context;
    if (state != NULL)
    {
        state->host = NULL;
        PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_SCENE_MATH_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.scene_math",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueSceneMathGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
