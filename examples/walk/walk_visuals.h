#pragma once

#include "graphics/graphics_device_v2.h"
#include "mesh/mesher_service.h"
#include "walk_physics.h"
#include "voxel/voxel_api.h"

#include <stdbool.h>
#include <stdint.h>

#define WALK_VISUAL_TEXTURE_COUNT 3u
/* 12 lofts, head, 12 joints, hands, nose and facial features. */
#define WALK_RAGDOLL_VISUAL_VERTEX_COUNT 5664u
#define WALK_VISUAL_CHUNK_RADIUS 1
#define WALK_VISUAL_CHUNK_LEVELS 3u
#define WALK_VISUAL_CHUNK_COUNT                                                                    \
    ((WALK_VISUAL_CHUNK_RADIUS * 2 + 1) * (WALK_VISUAL_CHUNK_RADIUS * 2 + 1) *                     \
     WALK_VISUAL_CHUNK_LEVELS)
#define WALK_VISUAL_CHUNK_DRAW_COUNT (WALK_VISUAL_CHUNK_COUNT * WALK_VISUAL_TEXTURE_COUNT)
#define WALK_VISUAL_FAR_TERRAIN_VERTEX_COUNT 24u

typedef uint8_t (*WalkVisualGetBlockFn)(void *context, int64_t x, int64_t y, int64_t z);

typedef struct WalkVisualChunk
{
    int64_t coordinate[3];
    LaiueGraphicsHandle buffers[WALK_VISUAL_TEXTURE_COUNT];
    uint32_t vertexCounts[WALK_VISUAL_TEXTURE_COUNT];
    bool ready;
} WalkVisualChunk;

typedef struct WalkVisualChunkSet
{
    ChunkMesherScratch *scratch;
    WalkVisualChunk chunks[WALK_VISUAL_CHUNK_COUNT];
    int64_t center[3];
    bool centerValid;
} WalkVisualChunkSet;

typedef struct WalkRagdollVisualScratch
{
    LaiueGraphicsVertexV2 vertices[WALK_RAGDOLL_VISUAL_VERTEX_COUNT];
} WalkRagdollVisualScratch;

/* Platform adapters provide encoded asset bytes allocated with PlatformAllocate.
 * The shared visual layer owns and releases the returned buffer. */
typedef bool (*WalkReadAssetFn)(void *context, const char *relativePath, uint8_t **outBytes,
                                uint32_t *outSize);

bool WalkVisualsCreateTerrain(LaiueGraphicsDeviceV2 *device, WalkReadAssetFn readAsset,
                              void *assetContext,
                              LaiueGraphicsHandle outBuffers[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle outTextures[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle *outSampler);
void WalkVisualsDestroyTerrain(LaiueGraphicsDeviceV2 *device,
                               LaiueGraphicsHandle buffers[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle *sampler);

bool WalkVisualsCreateChunkSet(const LaiueMesherServiceV1 *mesher, WalkVisualChunkSet *outSet);
/* A provider-frame rebase invalidates coordinates, but retains mesher scratch. */
void WalkVisualsInvalidateChunkSet(LaiueGraphicsDeviceV2 *device, WalkVisualChunkSet *set);
void WalkVisualsDestroyChunkSet(LaiueGraphicsDeviceV2 *device, const LaiueMesherServiceV1 *mesher,
                                WalkVisualChunkSet *set);
bool WalkVisualsUpdateChunkSet(LaiueGraphicsDeviceV2 *device, const LaiueMesherServiceV1 *mesher,
                               WalkVisualChunkSet *set, WalkVisualGetBlockFn getBlock,
                               void *blockContext, const int64_t centerBlock[3]);
void WalkVisualsInvalidateBlock(WalkVisualChunkSet *set, LaiueGraphicsDeviceV2 *device,
                                int64_t blockX, int64_t blockY, int64_t blockZ);
uint32_t WalkVisualsBuildChunkDraws(const WalkVisualChunkSet *set,
                                    const int64_t renderOriginBlock[3],
                                    const LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                                    LaiueGraphicsHandle sampler,
                                    LaiueGraphicsDrawItemV2 outDraws[WALK_VISUAL_CHUNK_DRAW_COUNT]);

bool WalkVisualsCreateFarTerrainBuffer(LaiueGraphicsDeviceV2 *device,
                                       LaiueGraphicsHandle *outBuffer);
bool WalkVisualsBuildFarTerrainDraw(LaiueGraphicsHandle buffer, const int64_t renderOriginBlock[3],
                                    const LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                                    LaiueGraphicsHandle sampler, LaiueGraphicsDrawItemV2 *outDraw);
void WalkVisualsDestroyBuffer(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle *buffer);

bool WalkVisualsCreateRagdollBuffer(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle *outBuffer);
bool WalkVisualsUpdateRagdollBuffer(const WalkPhysicsContext *context,
                                    LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle buffer,
                                    const VoxelRagdoll *ragdoll, const double renderOrigin[3],
                                    WalkRagdollVisualScratch *scratch);
