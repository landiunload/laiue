#include "mesh_world_render/mesh_world_render_service.h"

#include "mod/module_service.h"

/* Every renderer is an explicit object, so the module keeps no process state
 * and any number of hosts may load it side by side. */
typedef struct MeshWorldRenderModuleState
{
    const LaiueModuleHostV1 *host;
    uint32_t published;
} MeshWorldRenderModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    MeshWorldRenderModuleState *state =
        (MeshWorldRenderModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    state->published = 0u;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    MeshWorldRenderModuleState *state = (MeshWorldRenderModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_MESH_WORLD_RENDER_SERVICE_NAME,
        .version = LAIUE_MESH_WORLD_RENDER_SERVICE_ABI_VERSION_1,
        .table = LaiueMeshWorldRenderGetStaticServiceV1(),
        .tableSize = sizeof(LaiueMeshWorldRenderServiceV1),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
        return 0u;
    state->published = 1u;
    return 1u;
}

static void ModuleStop(void *context)
{
    MeshWorldRenderModuleState *state = (MeshWorldRenderModuleState *)context;
    if (state == NULL)
        return;
    if (state->published != 0u && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                            LAIUE_MESH_WORLD_RENDER_SERVICE_NAME);
    state->published = 0u;
}

static void ModuleDestroy(void *context)
{
    MeshWorldRenderModuleState *state = (MeshWorldRenderModuleState *)context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_MESH_WORLD_RENDER_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.mesh_world_render",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueMeshWorldRenderGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
