#include "render/graphics_service.h"

#include "content/content_service.h"
#include "mod/module_api.h"

typedef struct LaiueGraphicsModuleState
{
    const LaiueModuleHostV1 *host;
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
        host->unpublishService == NULL)
        return 0u;
    moduleState.host = host;
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_GRAPHICS_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
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
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
}

static const char *const provides[] = {LAIUE_GRAPHICS_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
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

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
