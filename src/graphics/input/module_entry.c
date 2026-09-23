#include "input/input_service.h"

#include "mod/module_api.h"

typedef struct LaiueInputModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueInputModuleState;

static LaiueInputModuleState moduleState;

static const LaiueInputServiceV1 service = {
    .structSize = sizeof(LaiueInputServiceV1),
    .abiVersion = LAIUE_INPUT_SERVICE_ABI_VERSION_1,
    .create = InputCreate,
    .destroy = InputDestroy,
    .handleRawInput = InputHandleRawInput,
    .endFrame = InputEndFrame,
    .resetState = InputResetState,
    .isKeyDown = InputIsKeyDown,
    .wasKeyPressed = InputWasKeyPressed,
    .consumeKeyPress = InputConsumeKeyPress,
    .isMouseButtonDown = InputIsMouseButtonDown,
    .wasMouseButtonPressed = InputWasMouseButtonPressed,
    .getMouseDelta = InputGetMouseDelta,
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
    LaiueInputModuleState *state = (LaiueInputModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_INPUT_SERVICE_NAME,
        .version = LAIUE_INPUT_SERVICE_ABI_VERSION_1,
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
    LaiueInputModuleState *state = (LaiueInputModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_INPUT_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
}

static const char *const provides[] = {LAIUE_INPUT_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.input",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueInputGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
