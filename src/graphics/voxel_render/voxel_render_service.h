#pragma once

#include "voxel_render/chunk_streaming.h"

#include <stdint.h>

#define LAIUE_VOXEL_RENDER_SERVICE_NAME "laiue.voxel_render"
#define LAIUE_VOXEL_RENDER_SERVICE_ABI_VERSION_1 1u

/* The loader-facing table keeps the voxel-to-GPU adapter usable without a
 * direct import of laiue_voxel_render.dll.  All handles remain opaque and
 * ownership stays with the provider that created them. */
typedef struct LaiueVoxelRenderServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    ChunkStreaming *(*create)(World *world, Renderer *renderer,
                              int32_t viewRadiusChunks);
    void (*destroy)(ChunkStreaming *streaming);
    bool (*pause)(ChunkStreaming *streaming);
    void (*setCenter)(ChunkStreaming *streaming,
                      int64_t chunkX, int64_t chunkY, int64_t chunkZ);
    void (*invalidateBlock)(ChunkStreaming *streaming,
                            int64_t blockX, int64_t blockY, int64_t blockZ);
    void (*pump)(ChunkStreaming *streaming);
    void (*getStats)(ChunkStreaming *streaming, ChunkStreamingStats *outStats);
    void (*draw)(ChunkStreaming *streaming, const float viewProjection[16],
                 const int64_t renderOriginBlock[3]);
} LaiueVoxelRenderServiceV1;
