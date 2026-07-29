#include "core/remote_player_renderer.h"

#include "render/chunk_geometry.h"
#include "world/block_properties.h"

#include <stddef.h>
#include <string.h>

#define REMOTE_PLAYER_CUBOID_COUNT 6u
#define REMOTE_PLAYER_QUAD_COUNT (REMOTE_PLAYER_CUBOID_COUNT * 6u)
#define REMOTE_PLAYER_MODEL_SCALE 0.1f
#define REMOTE_PLAYER_MODEL_HALF_WIDTH 0.3
#define REMOTE_PLAYER_MODEL_HALF_DEPTH 0.3

static void AppendCuboid(ChunkQuad quads[REMOTE_PLAYER_QUAD_COUNT], uint32_t *count,
                         uint32_t startX, uint32_t startY, uint32_t startZ, uint32_t extentX,
                         uint32_t extentY, uint32_t extentZ, BlockType block)
{
    for (uint32_t face = 0; face < 6u; ++face)
    {
        quads[(*count)++] =
            PackChunkQuad(startX, startY, startZ, face, block, extentX, extentY, extentZ);
    }
}

static RendererMesh *CreateRemotePlayerMesh(Renderer *renderer)
{
    ChunkQuad quads[REMOTE_PLAYER_QUAD_COUNT];
    uint32_t count = 0u;

    // A compact block-style avatar built from one shared mesh. Keeping all
    // peers in one instanced draw makes presentation cost bounded.
    AppendCuboid(quads, &count, 1u, 1u, 0u, 2u, 4u, 6u, BLOCK_EARTH);
    AppendCuboid(quads, &count, 3u, 1u, 0u, 2u, 4u, 6u, BLOCK_EARTH);
    AppendCuboid(quads, &count, 1u, 1u, 6u, 4u, 4u, 7u, BLOCK_GRASS);
    AppendCuboid(quads, &count, 0u, 1u, 6u, 1u, 4u, 7u, BLOCK_EARTH);
    AppendCuboid(quads, &count, 5u, 1u, 6u, 1u, 4u, 7u, BLOCK_EARTH);
    AppendCuboid(quads, &count, 0u, 0u, 13u, 6u, 6u, 5u, BLOCK_EARTH);

    return RendererCreateMesh(renderer, quads, count);
}

bool RemotePlayerRendererEnsure(RemotePlayerRenderer *remotePlayers, Renderer *renderer)
{
    if (remotePlayers == NULL || renderer == NULL)
    {
        return false;
    }
    if (remotePlayers->mesh != NULL)
    {
        return true;
    }
    remotePlayers->mesh = CreateRemotePlayerMesh(renderer);
    return remotePlayers->mesh != NULL;
}

void RemotePlayerRendererShutdown(RemotePlayerRenderer *remotePlayers, Renderer *renderer)
{
    if (remotePlayers == NULL)
    {
        return;
    }
    if (renderer != NULL)
    {
        RendererDestroyMesh(renderer, remotePlayers->mesh);
    }
    memset(remotePlayers, 0, sizeof(*remotePlayers));
}

void RemotePlayerRendererDraw(RemotePlayerRenderer *remotePlayers, Renderer *renderer,
                              const RemotePlayerRenderPose *poses, uint32_t poseCount,
                              const int64_t cameraBlockPosition[3])
{
    if (remotePlayers == NULL || remotePlayers->mesh == NULL || renderer == NULL || poses == NULL ||
        cameraBlockPosition == NULL)
    {
        return;
    }
    if (poseCount > REMOTE_PLAYER_RENDER_CAPACITY)
    {
        poseCount = REMOTE_PLAYER_RENDER_CAPACITY;
    }
    for (uint32_t i = 0u; i < poseCount; ++i)
    {
        RendererMeshInstance *instance = &remotePlayers->instances[i];
        instance->originRelative[0] =
            (float)(poses[i].feetPosition[0] - (double)cameraBlockPosition[0] -
                    REMOTE_PLAYER_MODEL_HALF_WIDTH);
        instance->originRelative[1] =
            (float)(poses[i].feetPosition[1] - (double)cameraBlockPosition[1] -
                    REMOTE_PLAYER_MODEL_HALF_DEPTH);
        instance->originRelative[2] =
            (float)(poses[i].feetPosition[2] - (double)cameraBlockPosition[2]);
        instance->scale = REMOTE_PLAYER_MODEL_SCALE;
    }
    if (poseCount != 0u)
    {
        RendererDrawMeshInstances(renderer, remotePlayers->mesh, remotePlayers->instances,
                                  poseCount);
    }
}
