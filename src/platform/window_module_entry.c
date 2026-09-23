#include "platform/window_service.h"

#include "mod/module_api.h"
#include "platform/system.h"

typedef struct LaiueWindowModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueWindowModuleState;

static const LaiueWindowServiceV1 service = {
    .structSize = sizeof(LaiueWindowServiceV1),
    .abiVersion = LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
    .create = WindowCreate,
    .destroy = WindowDestroy,
    .getNativeHandle = WindowGetNativeHandle,
    .setRawInputCallback = WindowSetRawInputCallback,
    .getClientSize = WindowGetClientSize,
    .consumeResize = WindowConsumeResize,
    .consumeFocusLoss = WindowConsumeFocusLoss,
    .runLoop = WindowRunLoop,
    .setMouseLook = WindowSetMouseLook,
    .isMouseLookEnabled = WindowIsMouseLookEnabled,
    .getCursorClientPosition = WindowGetCursorClientPosition,
    .requestClose = WindowRequestClose,
    .consumeMouseWheelSteps = WindowConsumeMouseWheelSteps,
    .setFullscreen = WindowSetFullscreen,
    .isFullscreen = WindowIsFullscreen,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL)
        return 0u;
    *outContext = NULL;
    LaiueWindowModuleState *state =
        (LaiueWindowModuleState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueWindowModuleState *state = (LaiueWindowModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_WINDOW_SERVICE_NAME,
        .version = LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
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
    LaiueWindowModuleState *state = (LaiueWindowModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_WINDOW_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueWindowModuleState *state = (LaiueWindowModuleState *)context;
    if (state != NULL)
    {
        state->host = NULL;
        PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_WINDOW_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.window",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueWindowGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
