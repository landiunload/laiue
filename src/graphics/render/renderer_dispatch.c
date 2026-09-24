#include "render/renderer.h"
#include "platform/system.h"

#include <string.h>

// Renderer* — непрозрачный указатель; полная раскладка struct Renderer
// приватна каждому бэкенду и здесь недостижима (как и раньше — это не
// новое ограничение, приложение тоже никогда не видела настоящую
// структуру). Единственное, что нужно диспетчеру, — помнить, каким
// бэкендом создан конкретный указатель, чтобы звать нужный набор
// суффиксных функций. Создание/уничтожение рендера — редкая операция
// (не на кадр), поэтому линейный реестр под мьютексом ничего не стоит.
#define RENDERER_REGISTRY_CAPACITY 8u

typedef struct RendererRegistryEntry
{
    const Renderer* handle;
    RendererBackendKind backend;
} RendererRegistryEntry;

static RendererRegistryEntry g_rendererRegistry[RENDERER_REGISTRY_CAPACITY];
static PlatformMutex g_rendererRegistryLock;
static volatile uint32_t g_rendererRegistryLockState; // 0=не готов,1=готовится,2=готов

// Быстрый путь без мьютекса для типичного случая одного активного рендера:
// LookupBackend вызывается на каждый публичный вызов (в том числе на
// тысячи DrawMesh за кадр), а захват мьютекса там стоит дороже самой
// отрисовки. Реестр под мьютексом остаётся источником истины; здесь лишь
// кэш одной записи. Writer (Register/Unregister, редкий) пишет handle
// простым присваиванием и лишь потом публикует backend release-записью.
// Reader читает backend acquire-загрузкой, и только увидев ненулевой
// backend, читает handle: acquire гарантирует видимость записи handle.
// Ноль означает «кэш пуст, иди в реестр под мьютексом».
static const Renderer* g_rendererFastHandle;
static volatile uint32_t g_rendererFastKind; // 0=пусто, иначе backend+1

static void PublishFastRenderer(const Renderer* renderer, RendererBackendKind backend)
{
    g_rendererFastHandle = renderer;
    PlatformAtomicStoreU32Release(&g_rendererFastKind,
                                  renderer != NULL ? (uint32_t)backend + 1u : 0u);
}

static void EnsureRegistryLockReady(void)
{
    if (PlatformAtomicLoadU32Acquire(&g_rendererRegistryLockState) == 2u) return;
    uint32_t expected = 0u;
    if (PlatformAtomicCompareExchangeU32(&g_rendererRegistryLockState, &expected, 1u))
    {
        (void)PlatformMutexInitialize(&g_rendererRegistryLock);
        PlatformAtomicStoreU32Release(&g_rendererRegistryLockState, 2u);
        return;
    }
    while (PlatformAtomicLoadU32Acquire(&g_rendererRegistryLockState) != 2u)
    {
        PlatformSleepMilliseconds(0u);
    }
}

static void RegisterRenderer(Renderer* renderer, RendererBackendKind backend)
{
    EnsureRegistryLockReady();
    PlatformMutexLock(&g_rendererRegistryLock);
    for (uint32_t i = 0; i < RENDERER_REGISTRY_CAPACITY; ++i)
    {
        if (g_rendererRegistry[i].handle == NULL)
        {
            g_rendererRegistry[i].handle = renderer;
            g_rendererRegistry[i].backend = backend;
            break;
        }
    }
    if (g_rendererFastHandle == NULL)
    {
        PublishFastRenderer(renderer, backend);
    }
    PlatformMutexUnlock(&g_rendererRegistryLock);
}

static void UnregisterRenderer(const Renderer* renderer)
{
    EnsureRegistryLockReady();
    PlatformMutexLock(&g_rendererRegistryLock);
    for (uint32_t i = 0; i < RENDERER_REGISTRY_CAPACITY; ++i)
    {
        if (g_rendererRegistry[i].handle == renderer)
        {
            g_rendererRegistry[i].handle = NULL;
            break;
        }
    }
    if (g_rendererFastHandle == renderer)
    {
        // Гасим кэш release-записью нуля и поднимаем любого оставшегося.
        PlatformAtomicStoreU32Release(&g_rendererFastKind, 0u);
        g_rendererFastHandle = NULL;
        for (uint32_t i = 0; i < RENDERER_REGISTRY_CAPACITY; ++i)
        {
            if (g_rendererRegistry[i].handle != NULL)
            {
                PublishFastRenderer(g_rendererRegistry[i].handle,
                                    g_rendererRegistry[i].backend);
                break;
            }
        }
    }
    PlatformMutexUnlock(&g_rendererRegistryLock);
}

static RendererBackendKind LookupBackend(const Renderer* renderer)
{
    if (renderer == NULL) return RENDERER_BACKEND_AUTO;

    // Горячий путь: один активный рендер — ни мьютекса, ни линейного скана.
    uint32_t fastKind = PlatformAtomicLoadU32Acquire(&g_rendererFastKind);
    if (fastKind != 0u && g_rendererFastHandle == renderer)
    {
        return (RendererBackendKind)(fastKind - 1u);
    }

    EnsureRegistryLockReady();
    PlatformMutexLock(&g_rendererRegistryLock);
    RendererBackendKind result = RENDERER_BACKEND_AUTO;
    for (uint32_t i = 0; i < RENDERER_REGISTRY_CAPACITY; ++i)
    {
        if (g_rendererRegistry[i].handle == renderer)
        {
            result = g_rendererRegistry[i].backend;
            break;
        }
    }
    PlatformMutexUnlock(&g_rendererRegistryLock);
    return result;
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
extern RendererMesh* RendererCreateGenericMesh_D3D12(Renderer *renderer,
                                                       const RendererGenericVertex *vertices,
                                                       uint32_t vertexCount);
extern RendererTexture *RendererCreateTexture_D3D12(Renderer *renderer,
                                                     uint32_t width, uint32_t height,
                                                     uint32_t mipLevels, uint32_t format);
extern void RendererDestroyTexture_D3D12(Renderer *renderer, RendererTexture *texture);
extern bool RendererUploadTexture_D3D12(Renderer *renderer, RendererTexture *texture,
                                        const void *data, uint64_t sizeBytes,
                                        uint32_t rowPitchBytes);
extern RendererSampler *RendererCreateSampler_D3D12(Renderer *renderer,
                                                     uint32_t minFilter, uint32_t magFilter,
                                                     uint32_t addressModeU, uint32_t addressModeV,
                                                     uint32_t addressModeW);
extern void RendererDestroySampler_D3D12(Renderer *renderer, RendererSampler *sampler);
extern void RendererDestroyMesh_D3D12(Renderer* renderer, RendererMesh* mesh);
extern void RendererDrawMesh_D3D12(Renderer* renderer, const RendererMesh* mesh, const float chunkOriginRelative[3]);
extern void RendererDrawGenericMesh_D3D12(Renderer *renderer, const RendererMesh *mesh,
                                          const float originRelative[3], float scale,
                                          uint32_t firstVertex, uint32_t vertexCount);
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
extern RendererMesh* RendererCreateGenericMesh_Vulkan(Renderer *renderer,
                                                       const RendererGenericVertex *vertices,
                                                       uint32_t vertexCount);
extern RendererTexture *RendererCreateTexture_Vulkan(Renderer *renderer,
                                                      uint32_t width, uint32_t height,
                                                      uint32_t mipLevels, uint32_t format);
extern void RendererDestroyTexture_Vulkan(Renderer *renderer, RendererTexture *texture);
extern bool RendererUploadTexture_Vulkan(Renderer *renderer, RendererTexture *texture,
                                         const void *data, uint64_t sizeBytes,
                                         uint32_t rowPitchBytes);
extern RendererSampler *RendererCreateSampler_Vulkan(Renderer *renderer,
                                                      uint32_t minFilter, uint32_t magFilter,
                                                      uint32_t addressModeU, uint32_t addressModeV,
                                                      uint32_t addressModeW);
extern void RendererDestroySampler_Vulkan(Renderer *renderer, RendererSampler *sampler);
extern void RendererDestroyMesh_Vulkan(Renderer* renderer, RendererMesh* mesh);
extern void RendererDrawMesh_Vulkan(Renderer* renderer, const RendererMesh* mesh, const float chunkOriginRelative[3]);
extern void RendererDrawGenericMesh_Vulkan(Renderer *renderer, const RendererMesh *mesh,
                                           const float originRelative[3], float scale,
                                           uint32_t firstVertex, uint32_t vertexCount);
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
    if (renderer != NULL) RegisterRenderer(renderer, backend);
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
        UnregisterRenderer(renderer);
        RendererDestroy_D3D12(renderer);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        UnregisterRenderer(renderer);
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

RendererMesh *RendererCreateGenericMesh(Renderer *renderer,
                                         const RendererGenericVertex *vertices,
                                         uint32_t vertexCount)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererCreateGenericMesh_D3D12(renderer, vertices, vertexCount);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererCreateGenericMesh_Vulkan(renderer, vertices, vertexCount);
#endif
    default: break;
    }
    return NULL;
}

RendererTexture *RendererCreateTexture(Renderer *renderer, uint32_t width,
                                       uint32_t height, uint32_t mipLevels,
                                       uint32_t format)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererCreateTexture_D3D12(renderer, width, height, mipLevels, format);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererCreateTexture_Vulkan(renderer, width, height, mipLevels, format);
#endif
    default:
        return NULL;
    }
}

void RendererDestroyTexture(Renderer *renderer, RendererTexture *texture)
{
    if (texture == NULL) return;
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDestroyTexture_D3D12(renderer, texture);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDestroyTexture_Vulkan(renderer, texture);
        return;
#endif
    default:
        return;
    }
}

bool RendererUploadTexture(Renderer *renderer, RendererTexture *texture,
                           const void *data, uint64_t sizeBytes,
                           uint32_t rowPitchBytes)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererUploadTexture_D3D12(renderer, texture, data, sizeBytes,
                                           rowPitchBytes);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererUploadTexture_Vulkan(renderer, texture, data, sizeBytes,
                                            rowPitchBytes);
#endif
    default:
        return false;
    }
}

RendererSampler *RendererCreateSampler(Renderer *renderer, uint32_t minFilter,
                                       uint32_t magFilter, uint32_t addressModeU,
                                       uint32_t addressModeV, uint32_t addressModeW)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        return RendererCreateSampler_D3D12(renderer, minFilter, magFilter, addressModeU,
                                           addressModeV, addressModeW);
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        return RendererCreateSampler_Vulkan(renderer, minFilter, magFilter, addressModeU,
                                            addressModeV, addressModeW);
#endif
    default:
        return NULL;
    }
}

void RendererDestroySampler(Renderer *renderer, RendererSampler *sampler)
{
    if (sampler == NULL)
        return;
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDestroySampler_D3D12(renderer, sampler);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDestroySampler_Vulkan(renderer, sampler);
        return;
#endif
    default:
        return;
    }
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

void RendererDrawGenericMesh(Renderer *renderer, const RendererMesh *mesh,
                             const float originRelative[3], float scale)
{
    RendererDrawGenericMeshRange(renderer, mesh, originRelative, scale, 0u,
                                  UINT32_MAX);
}

void RendererDrawGenericMeshRange(Renderer *renderer, const RendererMesh *mesh,
                                  const float originRelative[3], float scale,
                                  uint32_t firstVertex, uint32_t vertexCount)
{
    switch (LookupBackend(renderer))
    {
#if defined(LAIUE_RENDER_HAS_D3D12)
    case RENDERER_BACKEND_D3D12:
        RendererDrawGenericMesh_D3D12(renderer, mesh, originRelative, scale,
                                       firstVertex, vertexCount);
        return;
#endif
#if defined(LAIUE_RENDER_HAS_VULKAN)
    case RENDERER_BACKEND_VULKAN:
        RendererDrawGenericMesh_Vulkan(renderer, mesh, originRelative, scale,
                                        firstVertex, vertexCount);
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
