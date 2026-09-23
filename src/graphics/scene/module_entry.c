#include "scene/scene_service.h"

#include "mod/module_api.h"
#include "render/graphics_service.h"
#include "scene/math_service.h"

typedef struct LaiueSceneModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueGraphicsServiceV1 *graphics;
    const LaiueSceneMathServiceV1 *sceneMath;
} LaiueSceneModuleState;

static LaiueSceneModuleState moduleState;

static const LaiueSceneServiceV1 service = {
    .structSize = sizeof(LaiueSceneServiceV1),
    .abiVersion = LAIUE_SCENE_SERVICE_ABI_VERSION_1,
    .cameraInit = CameraInit,
    .cameraUpdate = CameraUpdate,
    .cameraGetForwardVector = CameraGetForwardVector,
    .cameraGetViewMatrix = CameraGetViewMatrix,
    .cameraGetProjectionMatrix = CameraGetProjectionMatrix,
    .panoramaIsActive = PanoramaIsActive,
    .panoramaBuildFrameSetup = PanoramaBuildFrameSetup,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.graphics = NULL;
    moduleState.sceneMath = NULL;
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueSceneModuleState *state = (LaiueSceneModuleState *)context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return 0u;
    uint32_t graphicsVersion = 0u;
    uint32_t graphicsSize = 0u;
    uint32_t sceneMathVersion = 0u;
    uint32_t sceneMathSize = 0u;
    state->graphics = (const LaiueGraphicsServiceV1 *)state->host->queryService(
        state->host->context, LAIUE_GRAPHICS_SERVICE_NAME,
        LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1, sizeof(LaiueGraphicsServiceV1),
        &graphicsVersion, &graphicsSize);
    state->sceneMath = (const LaiueSceneMathServiceV1 *)state->host->queryService(
        state->host->context, LAIUE_SCENE_MATH_SERVICE_NAME,
        LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1, sizeof(LaiueSceneMathServiceV1),
        &sceneMathVersion, &sceneMathSize);
    if (state->graphics == NULL || graphicsVersion < LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1 ||
        graphicsSize < sizeof(*state->graphics) || state->sceneMath == NULL ||
        sceneMathVersion < LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1 ||
        sceneMathSize < sizeof(*state->sceneMath))
    {
        state->graphics = NULL;
        state->sceneMath = NULL;
        return 0u;
    }
    PanoramaSetSceneMathService(state->sceneMath);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_SCENE_SERVICE_NAME,
        .version = LAIUE_SCENE_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        PanoramaSetSceneMathService(NULL);
        state->graphics = NULL;
        state->sceneMath = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueSceneModuleState *state = (LaiueSceneModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_SCENE_SERVICE_NAME);
    if (state != NULL)
    {
        PanoramaSetSceneMathService(NULL);
        state->graphics = NULL;
        state->sceneMath = NULL;
    }
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    PanoramaSetSceneMathService(NULL);
    moduleState.graphics = NULL;
    moduleState.sceneMath = NULL;
}

static const char *const provides[] = {LAIUE_SCENE_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_GRAPHICS_SERVICE_NAME, LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1},
    {LAIUE_SCENE_MATH_SERVICE_NAME, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.scene",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = sizeof(requiresServices) / sizeof(requiresServices[0]),
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueSceneGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
