#include "render/graphics_service.h"
#include "render/content_provider.h"

#include "content/content_service.h"
#include "mod/module_api.h"
#include "mod/module_service.h"

typedef struct LaiueGraphicsModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueContentServiceV1 *content;
} LaiueGraphicsModuleState;

static LaiueGraphicsModuleState moduleState;

static const LaiueGraphicsServiceV1 service = {
    .structSize = sizeof(LaiueGraphicsServiceV1),
    .abiVersion = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
    .backendIsAvailable = RendererBackendIsAvailable,
    .createWithBackend = RendererCreateWithBackend,
    .getBackend = RendererGetBackend,
    .create = RendererCreate,
    .destroy = RendererDestroy,
    .prepareWorldFrom = RendererPrepareWorldFrom,
    .prepareWorld = RendererPrepareWorld,
    .releaseWorld = RendererReleaseWorld,
    .isWorldReady = RendererIsWorldReady,
    .beginFrame = RendererBeginFrame,
    .beginScenePass = RendererBeginScenePass,
    .endFrame = RendererEndFrame,
    .getStats = RendererGetStats,
    .setVerticalSync = RendererSetVerticalSync,
    .isVerticalSyncEnabled = RendererIsVerticalSyncEnabled,
    .uiSetFontAtlas = RendererUiSetFontAtlas,
    .uiLoadBackground = RendererUiLoadBackground,
    .uiQueue = RendererUiQueue,
    .createMesh = RendererCreateMesh,
    .destroyMesh = RendererDestroyMesh,
    .drawMesh = RendererDrawMesh,
    .drawMeshInstances = RendererDrawMeshInstances,
    .resize = RendererResize,
    .getTexturePackLoadStatus = RendererGetTexturePackLoadStatus,
    .setWireframe = RendererSetWireframe,
    .isWireframe = RendererIsWireframe,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.content = NULL;
    RendererSetContentService(NULL);
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    RendererSetContentService(NULL);
    state->content = (const LaiueContentServiceV1 *)LaiueModuleQueryOptionalService(
        state->host, LAIUE_CONTENT_SERVICE_NAME,
        LAIUE_CONTENT_SERVICE_ABI_VERSION_1, sizeof(LaiueContentServiceV1));
    RendererSetContentService(state->content);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_GRAPHICS_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->content = NULL;
        RendererSetContentService(NULL);
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
    if (state != NULL)
    {
        state->content = NULL;
        RendererSetContentService(NULL);
    }
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    moduleState.content = NULL;
    RendererSetContentService(NULL);
}

static const char *const provides[] = {LAIUE_GRAPHICS_SERVICE_NAME};
static const LaiueModuleRequirementV1 optionalServices[] = {
    {LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.graphics",
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

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void)
{
    return &api;
}

const LaiueGraphicsServiceV1 *LaiueGraphicsGetStaticServiceV1(void)
{
    return &service;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
