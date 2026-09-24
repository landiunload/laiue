#include "voxel_render/voxel_render_service.h"

#include "mesh/mesher_service.h"
#include "mod/module_api.h"
#include "mod/module_service.h"
#include "render/graphics_service.h"
#include "scene/math_service.h"
#include "world/world_service.h"

#include <string.h>

typedef struct LaiueVoxelRenderModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueSceneMathServiceV1 *sceneMath;
    const LaiueWorldServiceV1 *world;
    const LaiueMesherServiceV1 *mesher;
    const LaiueGraphicsServiceV1 *graphics;
    LaiueVoxelRenderServiceV1 service;
} LaiueVoxelRenderModuleState;

static ChunkStreaming* CreateWithContext(void *moduleContext, World *world,
    Renderer *renderer, int32_t viewRadiusChunks)
{
    LaiueVoxelRenderModuleState *state = moduleContext;
    if (state == NULL || state->sceneMath == NULL || state->world == NULL ||
        state->mesher == NULL || state->graphics == NULL)
        return NULL;
    return ChunkStreamingCreateWithServices(world, renderer, viewRadiusChunks,
        state->sceneMath, state->world, state->mesher, state->graphics);
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    if (host->allocate == NULL || host->free == NULL)
        return 0u;
    LaiueVoxelRenderModuleState *state = host->allocate(host->context,
        sizeof(*state));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->service.structSize = sizeof(state->service);
    state->service.abiVersion = LAIUE_VOXEL_RENDER_SERVICE_ABI_VERSION_1;
    state->service.create = ChunkStreamingCreate;
    state->service.destroy = ChunkStreamingDestroy;
    state->service.pause = ChunkStreamingPause;
    state->service.setCenter = ChunkStreamingSetCenter;
    state->service.invalidateBlock = ChunkStreamingInvalidateBlock;
    state->service.pump = ChunkStreamingPump;
    state->service.getStats = ChunkStreamingGetStats;
    state->service.draw = ChunkStreamingDraw;
    state->service.createWithContext = CreateWithContext;
    state->service.context = state;
    ChunkStreamingSetSceneMathService(NULL);
    ChunkStreamingSetWorldService(NULL);
    ChunkStreamingSetMesherService(NULL);
    ChunkStreamingSetGraphicsService(NULL);
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueVoxelRenderModuleState *state = (LaiueVoxelRenderModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    ChunkStreamingSetSceneMathService(NULL);
    ChunkStreamingSetWorldService(NULL);
    ChunkStreamingSetMesherService(NULL);
    ChunkStreamingSetGraphicsService(NULL);
    state->sceneMath = (const LaiueSceneMathServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_SCENE_MATH_SERVICE_NAME,
        LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1, sizeof(LaiueSceneMathServiceV1));
    state->world = (const LaiueWorldServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_WORLD_SERVICE_NAME,
        LAIUE_WORLD_SERVICE_ABI_VERSION_1, sizeof(LaiueWorldServiceV1));
    state->mesher = (const LaiueMesherServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_MESHER_SERVICE_NAME,
        LAIUE_MESHER_SERVICE_ABI_VERSION_1, sizeof(LaiueMesherServiceV1));
    state->graphics = (const LaiueGraphicsServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_GRAPHICS_SERVICE_NAME,
        LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1, sizeof(LaiueGraphicsServiceV1));
    if (state->sceneMath == NULL || state->world == NULL || state->mesher == NULL ||
        state->graphics == NULL)
    {
        ChunkStreamingSetSceneMathService(NULL);
        ChunkStreamingSetWorldService(NULL);
        ChunkStreamingSetMesherService(NULL);
        ChunkStreamingSetGraphicsService(NULL);
        state->sceneMath = NULL;
        state->world = NULL;
        state->mesher = NULL;
        state->graphics = NULL;
        return 0u;
    }
    ChunkStreamingSetSceneMathService(state->sceneMath);
    ChunkStreamingSetWorldService(state->world);
    ChunkStreamingSetMesherService(state->mesher);
    ChunkStreamingSetGraphicsService(state->graphics);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_VOXEL_RENDER_SERVICE_NAME,
        .version = LAIUE_VOXEL_RENDER_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(state->service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        ChunkStreamingSetSceneMathService(NULL);
        ChunkStreamingSetWorldService(NULL);
        ChunkStreamingSetMesherService(NULL);
        ChunkStreamingSetGraphicsService(NULL);
        state->sceneMath = NULL;
        state->world = NULL;
        state->mesher = NULL;
        state->graphics = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueVoxelRenderModuleState *state = (LaiueVoxelRenderModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_VOXEL_RENDER_SERVICE_NAME);
    if (state != NULL)
    {
        ChunkStreamingSetSceneMathService(NULL);
        ChunkStreamingSetWorldService(NULL);
        ChunkStreamingSetMesherService(NULL);
        ChunkStreamingSetGraphicsService(NULL);
        state->sceneMath = NULL;
        state->world = NULL;
        state->mesher = NULL;
        state->graphics = NULL;
    }
}

static void ModuleDestroy(void *context)
{
    LaiueVoxelRenderModuleState *state = context;
    ChunkStreamingSetSceneMathService(NULL);
    ChunkStreamingSetWorldService(NULL);
    ChunkStreamingSetMesherService(NULL);
    ChunkStreamingSetGraphicsService(NULL);
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    state->sceneMath = NULL;
    state->world = NULL;
    state->mesher = NULL;
    state->graphics = NULL;
    if (host != NULL && host->free != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_VOXEL_RENDER_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_SCENE_MATH_SERVICE_NAME, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1},
    {LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1},
    {LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1},
    {LAIUE_GRAPHICS_SERVICE_NAME, LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.voxel_render",
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

const LaiueModuleApiV1 *LaiueVoxelRenderGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
