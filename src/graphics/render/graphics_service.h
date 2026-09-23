#pragma once

#include "render/renderer.h"

#include <stdint.h>

#define LAIUE_GRAPHICS_SERVICE_NAME "laiue.graphics"
#define LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1 1u

/* Backend-neutral renderer contract.  Backend DLLs remain an implementation
 * detail of this provider; callers select a backend through the enum and do
 * not import D3D12/Vulkan symbols themselves. */
typedef struct LaiueGraphicsServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    bool (*backendIsAvailable)(RendererBackendKind backend);
    Renderer *(*createWithBackend)(void *windowHandle, int32_t width,
                                   int32_t height, RendererBackendKind backend);
    RendererBackendKind (*getBackend)(const Renderer *renderer);
    Renderer *(*create)(void *windowHandle, int32_t width, int32_t height);
    void (*destroy)(Renderer *renderer);
    bool (*prepareWorldFrom)(Renderer *renderer, LaiueContentCatalog *catalog);
    bool (*prepareWorld)(Renderer *renderer);
    void (*releaseWorld)(Renderer *renderer);
    bool (*isWorldReady)(const Renderer *renderer);
    bool (*beginFrame)(Renderer *renderer, const RendererFrameSetup *frame);
    void (*beginScenePass)(Renderer *renderer, uint32_t passIndex);
    bool (*endFrame)(Renderer *renderer);
    void (*getStats)(const Renderer *renderer, RendererStats *outStats);
    void (*setVerticalSync)(Renderer *renderer, bool enabled);
    bool (*isVerticalSyncEnabled)(const Renderer *renderer);
    bool (*uiSetFontAtlas)(Renderer *renderer, const uint8_t *alphaPixels,
                           uint32_t width, uint32_t height);
    bool (*uiLoadBackground)(Renderer *renderer, const wchar_t *path,
                             uint32_t *outWidth, uint32_t *outHeight);
    void (*uiQueue)(Renderer *renderer, const RendererUiQuad *quads, uint32_t count);
    RendererMesh *(*createMesh)(Renderer *renderer, const ChunkQuad *quads,
                                uint32_t quadCount);
    void (*destroyMesh)(Renderer *renderer, RendererMesh *mesh);
    void (*drawMesh)(Renderer *renderer, const RendererMesh *mesh,
                     const float chunkOriginRelative[3]);
    void (*drawMeshInstances)(Renderer *renderer, const RendererMesh *mesh,
                              const RendererMeshInstance *instances,
                              uint32_t instanceCount);
    void (*resize)(Renderer *renderer, int32_t width, int32_t height);
    RendererContentStatus (*getTexturePackLoadStatus)(const Renderer *renderer);
    void (*setWireframe)(Renderer *renderer, bool enabled);
    bool (*isWireframe)(const Renderer *renderer);
} LaiueGraphicsServiceV1;
