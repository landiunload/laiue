#include "mesh/mesher_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"

typedef struct LaiueMesherModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueMesherModuleState;

static LaiueMesherModuleState moduleState;

static const LaiueMesherServiceV1 service = {
    .structSize = sizeof(LaiueMesherServiceV1),
    .abiVersion = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
    .scratchCreate = ChunkMesherScratchCreate,
    .scratchDestroy = ChunkMesherScratchDestroy,
    .buildChunkMesh = BuildChunkMesh,
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
    LaiueMesherModuleState *state = (LaiueMesherModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_MESHER_SERVICE_NAME,
        .version = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
        return 0u;
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueMesherModuleState *state = (LaiueMesherModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_MESHER_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
}

static const char *const provides[] = {LAIUE_MESHER_SERVICE_NAME};
static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.mesher",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void)
{
    return &api;
}

const LaiueMesherServiceV1 *LaiueMesherGetStaticServiceV1(void)
{
    return &service;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
