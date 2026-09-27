#include "profile/profile_service.h"

#include "mod/module_service.h"

#include <string.h>

typedef struct LaiueGraphicsProfileModuleState
{
    const LaiueModuleHostV1 *host;
    LaiueGraphicsProfileServiceV1 service;
} LaiueGraphicsProfileModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (outContext == NULL)
        return 0u;
    *outContext = NULL;
    if (host == NULL || host->publishService == NULL || host->unpublishService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;

    LaiueGraphicsProfileModuleState *state = (LaiueGraphicsProfileModuleState *)
        host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->service = *LaiueGraphicsProfileGetStaticServiceV1();
    state->service.context = (void *)host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueGraphicsProfileModuleState *state = (LaiueGraphicsProfileModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;

    LaiueModuleServiceV1 published = {
        .name = LAIUE_GRAPHICS_PROFILE_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(LaiueGraphicsProfileServiceV1),
    };
    return state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    LaiueGraphicsProfileModuleState *state = (LaiueGraphicsProfileModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                            LAIUE_GRAPHICS_PROFILE_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueGraphicsProfileModuleState *state = (LaiueGraphicsProfileModuleState *)context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_GRAPHICS_PROFILE_SERVICE_NAME};
static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.graphics.profile",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueGraphicsProfileGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
