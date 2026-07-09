#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Renderer Renderer;

typedef struct ChunkVertex
{
    float   position[3];
    uint8_t color[4];
} ChunkVertex;

LAIUE_RENDER_API Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height);
LAIUE_RENDER_API void      RendererDestroy(Renderer* renderer);

LAIUE_RENDER_API void RendererBeginFrame(Renderer* renderer);
LAIUE_RENDER_API void RendererEndFrame(Renderer* renderer);

LAIUE_RENDER_API void RendererSetViewProjection(Renderer* renderer, const float matrix[16]);

// Upload and draw mesh data in a single call (per-frame streaming).
// Mesh data is copied to the GPU upload heap each frame.
LAIUE_RENDER_API void RendererDrawMesh(Renderer* renderer,
    const ChunkVertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount);

LAIUE_RENDER_API void RendererResize(Renderer* renderer, int32_t width, int32_t height);

// World forward declaration — BuildChunkMesh lives in a separate .c file.
struct World;

// Build a chunk mesh from world data. Allocates vertex/index arrays with HeapAlloc.
// The caller must HeapFree the returned arrays separately.
// Returns vertexCount/indexCount == 0 if the chunk is empty.
LAIUE_RENDER_API bool BuildChunkMesh(struct World* world, int64_t cx, int64_t cy, int64_t cz,
    ChunkVertex** outVertices, uint32_t* outVertexCount,
    uint32_t** outIndices, uint32_t* outIndexCount);
