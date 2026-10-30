#include "frame_monitor/frame_monitor_service.h"

static const LaiueFrameMonitorServiceV1 service = {
    .structSize = sizeof(LaiueFrameMonitorServiceV1),
    .abiVersion = LAIUE_FRAME_MONITOR_SERVICE_ABI_VERSION_1,
    .create = LaiueFrameMonitorCreate,
    .destroy = LaiueFrameMonitorDestroy,
    .registerZone = LaiueFrameMonitorRegisterZone,
    .zoneName = LaiueFrameMonitorZoneName,
    .beginFrame = LaiueFrameMonitorBeginFrame,
    .endFrame = LaiueFrameMonitorEndFrame,
    .beginZone = LaiueFrameMonitorBeginZone,
    .endZone = LaiueFrameMonitorEndZone,
    .submitFrame = LaiueFrameMonitorSubmitFrame,
    .getSummary = LaiueFrameMonitorGetSummary,
    .getZoneSummary = LaiueFrameMonitorGetZoneSummary,
    .spikeCount = LaiueFrameMonitorSpikeCount,
    .getSpike = LaiueFrameMonitorGetSpike,
    .reset = LaiueFrameMonitorReset,
};

typedef struct FrameMonitorModuleState
{
    const LaiueModuleHostV1 *host;
} FrameMonitorModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    FrameMonitorModuleState *state = host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    FrameMonitorModuleState *state = context;
    const LaiueModuleServiceV1 published = {
        .name = LAIUE_FRAME_MONITOR_SERVICE_NAME,
        .version = LAIUE_FRAME_MONITOR_SERVICE_ABI_VERSION_1,
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
    FrameMonitorModuleState *state = context;
    if (state != NULL && state->host != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_FRAME_MONITOR_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    FrameMonitorModuleState *state = context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_FRAME_MONITOR_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.frame_monitor",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueFrameMonitorServiceV1 *LaiueFrameMonitorGetStaticServiceV1(void)
{
    return &service;
}

const LaiueModuleApiV1 *LaiueFrameMonitorGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
