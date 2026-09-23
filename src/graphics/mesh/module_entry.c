#include "mesh/mesher_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"
#include "world/world_service.h"

typedef struct LaiueMesherModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueWorldServiceV1 *world;
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
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.world = NULL;
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueMesherModuleState *state = (LaiueMesherModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    state->world = (const LaiueWorldServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_WORLD_SERVICE_NAME,
        LAIUE_WORLD_SERVICE_ABI_VERSION_1, sizeof(LaiueWorldServiceV1));
    if (state->world == NULL)
        return 0u;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_MESHER_SERVICE_NAME,
        .version = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->world = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueMesherModuleState *state = (LaiueMesherModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_MESHER_SERVICE_NAME);
    if (state != NULL)
        state->world = NULL;
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    moduleState.world = NULL;
}

static const char *const provides[] = {LAIUE_MESHER_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.mesher",
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

const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
