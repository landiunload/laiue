#include "render/renderer.h"
#include "render/renderer_internal.h"

#include <string.h>

static RendererBackendKind LookupBackend(const Renderer* renderer)
{
    return renderer != NULL ? ((const RendererHeader *)renderer)->backend : RENDERER_BACKEND_AUTO;
}

// === Внешние объявления обеих суффиксных реализаций ===
#if defined(LAIUE_RENDER_HAS_D3D12)
extern Renderer* RendererCreate_D3D12(void* windowHandle, int32_t width, int32_t height);
extern void RendererDestroy_D3D12(Renderer* renderer);
extern bool RendererSetMaterialNames_D3D12(Renderer *renderer, const wchar_t *const *names, uint32_t count);
extern bool RendererPrepareWorldFrom_D3D12(Renderer *renderer, LaiueContentCatalog *catalog);
extern bool RendererPrepareWorld_D3D12(Renderer* renderer);
extern void RendererReleaseWorld_D3D12(Renderer* renderer);
extern bool RendererIsWorldReady_D3D12(const Renderer* renderer);
extern bool RendererBeginFrame_D3D12(Renderer* renderer, const RendererFrameSetup* frame);
extern void RendererBeginScenePass_D3D12(Renderer* renderer, uint32_t passIndex);
extern bool RendererEndFrame_D3D12(Renderer* renderer);
extern void RendererGetStats_D3D12(const Renderer* renderer, RendererStats* outStats);
extern void RendererSetVerticalSync_D3D12(Renderer* renderer, bool enabled);
extern bool RendererIsVerticalSyncEnabled_D3D12(const Renderer* renderer);
extern bool RendererUiSetFontAtlas_D3D12(Renderer* renderer, const uint8_t* alphaPixels, uint32_t width, uint32_t height);
extern bool RendererUiLoadBackground_D3D12(Renderer* renderer, const wchar_t* path, uint32_t* outWidth, uint32_t* outHeight);
extern void RendererUiQueue_D3D12(Renderer* renderer, const RendererUiQuad* quads, uint32_t count);
extern RendererMesh* RendererCreateMesh_D3D12(Renderer* renderer, const ChunkQuad* quads, uint32_t quadCount);
extern void RendererDestroyMesh_D3D12(Renderer* renderer, RendererMesh* mesh);
extern void RendererDrawMesh_D3D12(Renderer* renderer, const RendererMesh* mesh, const float chunkOriginRelative[3]);
extern void RendererDrawMeshInstances_D3D12(Renderer* renderer, const RendererMesh* mesh, const RendererMeshInstance* instances, uint32_t instanceCount);
extern void RendererResize_D3D12(Renderer* renderer, int32_t width, int32_t height);
extern bool RendererReloadTexturePackFrom_D3D12(Renderer *renderer, LaiueContentCatalog *catalog);
extern bool RendererReloadTexturePack_D3D12(Renderer* renderer);
extern RendererContentStatus RendererGetTexturePackLoadStatus_D3D12(const Renderer* renderer);
extern void RendererSetWireframe_D3D12(Renderer* renderer, bool enabled);
extern bool RendererIsWireframe_D3D12(const Renderer* renderer);
extern bool RendererReloadShaderSet_D3D12(Renderer *renderer, const LaiueShaderSet *shaderSet);
extern bool RendererReloadShaderPackFrom_D3D12(Renderer *renderer, LaiueContentCatalog *catalog, ShaderPackLoadStatus *outStatus);
extern bool RendererReloadShaderPack_D3D12(Renderer *renderer, ShaderPackLoadStatus *outStatus);
extern bool RendererReloadShaders_D3D12(Renderer* renderer, const void* chunkVS, uint32_t chunkVSLength, const void* chunkPS, uint32_t chunkPSLength, const void* panoramaVS, uint32_t panoramaVSLength, const void* panoramaPS, uint32_t panoramaPSLength, const void* uiVS, uint32_t uiVSLength, const void* uiPS, uint32_t uiPSLength);
#endif

#if defined(LAIUE_RENDER_HAS_VULKAN)
extern Renderer* RendererCreate_Vulkan(void* windowHandle, int32_t width, int32_t height);
extern void RendererDestroy_Vulkan(Renderer* renderer);
extern bool RendererSetMaterialNames_Vulkan(Renderer *renderer, const wchar_t *const *names, uint32_t count);
extern bool RendererPrepareWorldFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog);
extern bool RendererPrepareWorld_Vulkan(Renderer* renderer);
extern void RendererReleaseWorld_Vulkan(Renderer* renderer);
extern bool RendererIsWorldReady_Vulkan(const Renderer* renderer);
extern bool RendererBeginFrame_Vulkan(Renderer* renderer, const RendererFrameSetup* frame);
extern void RendererBeginScenePass_Vulkan(Renderer* renderer, uint32_t passIndex);
extern bool RendererEndFrame_Vulkan(Renderer* renderer);
extern void RendererGetStats_Vulkan(const Renderer* renderer, RendererStats* outStats);
extern void RendererSetVerticalSync_Vulkan(Renderer* renderer, bool enabled);
extern bool RendererIsVerticalSyncEnabled_Vulkan(const Renderer* renderer);
extern bool RendererUiSetFontAtlas_Vulkan(Renderer* renderer, const uint8_t* alphaPixels, uint32_t width, uint32_t height);
extern bool RendererUiLoadBackground_Vulkan(Renderer* renderer, const wchar_t* path, uint32_t* outWidth, uint32_t* outHeight);
extern void RendererUiQueue_Vulkan(Renderer* renderer, const RendererUiQuad* quads, uint32_t count);
extern RendererMesh* RendererCreateMesh_Vulkan(Renderer* renderer, const ChunkQuad* quads, uint32_t quadCount);
extern void RendererDestroyMesh_Vulkan(Renderer* renderer, RendererMesh* mesh);
extern void RendererDrawMesh_Vulkan(Renderer* renderer, const RendererMesh* mesh, const float chunkOriginRelative[3]);
extern void RendererDrawMeshInstances_Vulkan(Renderer* renderer, const RendererMesh* mesh, const RendererMeshInstance* instances, uint32_t instanceCount);
extern void RendererResize_Vulkan(Renderer* renderer, int32_t width, int32_t height);
extern bool RendererReloadTexturePackFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog);
extern bool RendererReloadTexturePack_Vulkan(Renderer* renderer);
extern RendererContentStatus RendererGetTexturePackLoadStatus_Vulkan(const Renderer* renderer);
extern void RendererSetWireframe_Vulkan(Renderer* renderer, bool enabled);
extern bool RendererIsWireframe_Vulkan(const Renderer* renderer);
extern bool RendererReloadShaderSet_Vulkan(Renderer *renderer, const LaiueShaderSet *shaderSet);
extern bool RendererReloadShaderPackFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog, ShaderPackLoadStatus *outStatus);
extern bool RendererReloadShaderPack_Vulkan(Renderer *renderer, ShaderPackLoadStatus *outStatus);
extern bool RendererReloadShaders_Vulkan(Renderer* renderer, const void* chunkVS, uint32_t chunkVSLength, const void* chunkPS, uint32_t chunkPSLength, const void* panoramaVS, uint32_t panoramaVSLength, const void* panoramaPS, uint32_t panoramaPSLength, const void* uiVS, uint32_t uiVSLength, const void* uiPS, uint32_t uiPSLength);
#endif

bool RendererBackendIsAvailable(RendererBackendKind backend)
{
    switch (backend)
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12: return true;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN: return true;
#endif
    default: return false;
    }
}

Renderer* RendererCreateWithBackend(void* windowHandle, int32_t width, int32_t height,
                                     RendererBackendKind backend)
{
    if (backend == RENDERER_BACKEND_AUTO)
    {
#if defined(LAIUE_RENDER_DEFAULT_BACKEND_D3D12)
        backend = RENDERER_BACKEND_D3D12;
#elif defined(LAIUE_RENDER_DEFAULT_BACKEND_VULKAN)
        backend = RENDERER_BACKEND_VULKAN;
#else
        return NULL;
#endif
    }
    Renderer* renderer = NULL;
    switch (backend)
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        renderer = RendererCreate_D3D12(windowHandle, width, height);
        break;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        renderer = RendererCreate_Vulkan(windowHandle, width, height);
        break;
#endif
    default:
        return NULL;
    }
    return renderer;
}

RendererBackendKind RendererGetBackend(const Renderer* renderer)
{
    return LookupBackend(renderer);
}

// === Диспетчер: единственное определение каждой публичной функции ===
Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height)
{
    return RendererCreateWithBackend(windowHandle, width, height, RENDERER_BACKEND_AUTO);
}

void RendererDestroy(Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDestroy_D3D12(renderer);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDestroy_Vulkan(renderer);
        return;
#endif
    default: break;
    }
}

bool RendererSetMaterialNames(Renderer *renderer, const wchar_t *const *names, uint32_t count)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererSetMaterialNames_D3D12(renderer, names, count);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererSetMaterialNames_Vulkan(renderer, names, count);
#endif
    default: break;
    }
    return false;
}

bool RendererPrepareWorldFrom(Renderer *renderer, LaiueContentCatalog *catalog)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererPrepareWorldFrom_D3D12(renderer, catalog);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererPrepareWorldFrom_Vulkan(renderer, catalog);
#endif
    default: break;
    }
    return false;
}

bool RendererPrepareWorld(Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererPrepareWorld_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererPrepareWorld_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

void RendererReleaseWorld(Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererReleaseWorld_D3D12(renderer);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererReleaseWorld_Vulkan(renderer);
        return;
#endif
    default: break;
    }
}

bool RendererIsWorldReady(const Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererIsWorldReady_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererIsWorldReady_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

bool RendererBeginFrame(Renderer* renderer, const RendererFrameSetup* frame)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererBeginFrame_D3D12(renderer, frame);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererBeginFrame_Vulkan(renderer, frame);
#endif
    default: break;
    }
    return false;
}

void RendererBeginScenePass(Renderer* renderer, uint32_t passIndex)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererBeginScenePass_D3D12(renderer, passIndex);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererBeginScenePass_Vulkan(renderer, passIndex);
        return;
#endif
    default: break;
    }
}

bool RendererEndFrame(Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererEndFrame_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererEndFrame_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

void RendererGetStats(const Renderer* renderer, RendererStats* outStats)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererGetStats_D3D12(renderer, outStats);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererGetStats_Vulkan(renderer, outStats);
        return;
#endif
    default: break;
    }
}

void RendererSetVerticalSync(Renderer* renderer, bool enabled)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererSetVerticalSync_D3D12(renderer, enabled);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererSetVerticalSync_Vulkan(renderer, enabled);
        return;
#endif
    default: break;
    }
}

bool RendererIsVerticalSyncEnabled(const Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererIsVerticalSyncEnabled_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererIsVerticalSyncEnabled_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

bool RendererUiSetFontAtlas(Renderer* renderer, const uint8_t* alphaPixels, uint32_t width, uint32_t height)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererUiSetFontAtlas_D3D12(renderer, alphaPixels, width, height);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererUiSetFontAtlas_Vulkan(renderer, alphaPixels, width, height);
#endif
    default: break;
    }
    return false;
}

bool RendererUiLoadBackground(Renderer* renderer, const wchar_t* path, uint32_t* outWidth, uint32_t* outHeight)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererUiLoadBackground_D3D12(renderer, path, outWidth, outHeight);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererUiLoadBackground_Vulkan(renderer, path, outWidth, outHeight);
#endif
    default: break;
    }
    return false;
}

void RendererUiQueue(Renderer* renderer, const RendererUiQuad* quads, uint32_t count)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererUiQueue_D3D12(renderer, quads, count);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererUiQueue_Vulkan(renderer, quads, count);
        return;
#endif
    default: break;
    }
}

RendererMesh* RendererCreateMesh(Renderer* renderer, const ChunkQuad* quads, uint32_t quadCount)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererCreateMesh_D3D12(renderer, quads, quadCount);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererCreateMesh_Vulkan(renderer, quads, quadCount);
#endif
    default: break;
    }
    return NULL;
}

void RendererDestroyMesh(Renderer* renderer, RendererMesh* mesh)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDestroyMesh_D3D12(renderer, mesh);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDestroyMesh_Vulkan(renderer, mesh);
        return;
#endif
    default: break;
    }
}

void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh, const float chunkOriginRelative[3])
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDrawMesh_D3D12(renderer, mesh, chunkOriginRelative);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDrawMesh_Vulkan(renderer, mesh, chunkOriginRelative);
        return;
#endif
    default: break;
    }
}

void RendererDrawMeshInstances(Renderer* renderer, const RendererMesh* mesh, const RendererMeshInstance* instances, uint32_t instanceCount)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDrawMeshInstances_D3D12(renderer, mesh, instances, instanceCount);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDrawMeshInstances_Vulkan(renderer, mesh, instances, instanceCount);
        return;
#endif
    default: break;
    }
}

void RendererResize(Renderer* renderer, int32_t width, int32_t height)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererResize_D3D12(renderer, width, height);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererResize_Vulkan(renderer, width, height);
        return;
#endif
    default: break;
    }
}

bool RendererReloadTexturePackFrom(Renderer *renderer, LaiueContentCatalog *catalog)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadTexturePackFrom_D3D12(renderer, catalog);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadTexturePackFrom_Vulkan(renderer, catalog);
#endif
    default: break;
    }
    return false;
}

bool RendererReloadTexturePack(Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadTexturePack_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadTexturePack_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

RendererContentStatus RendererGetTexturePackLoadStatus(const Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererGetTexturePackLoadStatus_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererGetTexturePackLoadStatus_Vulkan(renderer);
#endif
    default: break;
    }
    return RENDERER_CONTENT_NOT_ATTEMPTED;
}

void RendererSetWireframe(Renderer* renderer, bool enabled)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererSetWireframe_D3D12(renderer, enabled);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererSetWireframe_Vulkan(renderer, enabled);
        return;
#endif
    default: break;
    }
}

bool RendererIsWireframe(const Renderer* renderer)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererIsWireframe_D3D12(renderer);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererIsWireframe_Vulkan(renderer);
#endif
    default: break;
    }
    return false;
}

bool RendererReloadShaderSet(Renderer *renderer, const LaiueShaderSet *shaderSet)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadShaderSet_D3D12(renderer, shaderSet);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadShaderSet_Vulkan(renderer, shaderSet);
#endif
    default: break;
    }
    return false;
}

bool RendererReloadShaderPackFrom(Renderer *renderer, LaiueContentCatalog *catalog, ShaderPackLoadStatus *outStatus)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadShaderPackFrom_D3D12(renderer, catalog, outStatus);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadShaderPackFrom_Vulkan(renderer, catalog, outStatus);
#endif
    default: break;
    }
    return false;
}

bool RendererReloadShaderPack(Renderer *renderer, ShaderPackLoadStatus *outStatus)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadShaderPack_D3D12(renderer, outStatus);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadShaderPack_Vulkan(renderer, outStatus);
#endif
    default: break;
    }
    return false;
}

bool RendererReloadShaders(Renderer* renderer, const void* chunkVS, uint32_t chunkVSLength, const void* chunkPS, uint32_t chunkPSLength, const void* panoramaVS, uint32_t panoramaVSLength, const void* panoramaPS, uint32_t panoramaPSLength, const void* uiVS, uint32_t uiVSLength, const void* uiPS, uint32_t uiPSLength)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererReloadShaders_D3D12(renderer, chunkVS, chunkVSLength, chunkPS, chunkPSLength, panoramaVS, panoramaVSLength, panoramaPS, panoramaPSLength, uiVS, uiVSLength, uiPS, uiPSLength);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererReloadShaders_Vulkan(renderer, chunkVS, chunkVSLength, chunkPS, chunkPSLength, panoramaVS, panoramaVSLength, panoramaPS, panoramaPSLength, uiVS, uiVSLength, uiPS, uiPSLength);
#endif
    default: break;
    }
    return false;
}
