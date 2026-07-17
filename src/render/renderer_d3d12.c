#include "render/renderer.h"

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "render/generated/chunk_vs.h"
#include "render/generated/chunk_ps.h"
#include "render/generated/panorama_vs.h"
#include "render/generated/panorama_ps.h"
#include "render/generated/ui_vs.h"
#include "render/generated/ui_ps.h"
#include "render/texture_pack_internal.h"
#include "render/ui_image_wic.h"

#include <stddef.h>
#include <string.h>

#define FRAME_COUNT 2

// Общая шейдерная куча SRV: фиксированные слоты. Albedo и нормали
// блоков соседствуют — таблица чанкового прохода накрывает оба (t1, t2).
#define SRV_SLOT_BLOCK_TEXTURES 0
#define SRV_SLOT_BLOCK_NORMALS  1
#define SRV_SLOT_PANORAMA_CUBE  2
#define SRV_SLOT_FONT_ATLAS     3
#define SRV_SLOT_UI_BACKGROUND  4
#define SRV_SLOT_COUNT          5

// Слой UI: размер квада держать в синхроне с shaders/ui.hlsl.
// Число квадов является частью публичного контракта renderer.h.
#define UI_QUAD_BYTES 48

// Резолв панорамы: раскладка корневых констант.
#define RESOLVE_ROOT_CONSTANT_COUNT 3

// Корневые константы (раскладка совпадает с cbuffer в chunk.hlsl,
// float3 выравниваются по границам 16 байт):
// dword 0..15 — view-projection, 16..18 — смещение чанка,
// 20..22 — направление от солнца, 24..26 — цвет солнца, 28..30 — ambient.
#define ROOT_CONSTANT_COUNT 32
#define ROOT_CONSTANT_ORIGIN_OFFSET 16
#define ROOT_CONSTANT_LIGHTING_OFFSET 20
#define ROOT_CONSTANT_LIGHTING_COUNT 12
#define ROOT_PARAMETER_CONSTANTS 0
#define ROOT_PARAMETER_QUAD_BUFFER 1
#define ROOT_PARAMETER_BLOCK_TEXTURES 2
#define ROOT_PARAMETER_INSTANCES 3

#define MAX_TEXTURE_SUBRESOURCES 48

// Пул геометрии: меши суб-аллоцируются из больших DEFAULT-буферов —
// без 64-КиБ выравнивания на каждый меш и без чтения через PCIe.
#define POOL_BLOCK_BYTES (4u * 1024u * 1024u)
#define MAX_POOL_BLOCKS 64

// Очереди отложенного освобождения: ресурс или диапазон пула
// уничтожается, только когда GPU прошёл кадры, которые могли его читать.
#define DEFERRED_RELEASE_CAPACITY 256
#define MAX_PENDING_UPLOADS 64
#define MESH_UPLOAD_BYTES_PER_FRAME (4u * 1024u * 1024u)
#define INSTANCE_BYTES_PER_FRAME (64u * 1024u)

typedef struct FreeRange
{
    uint32_t offset;
    uint32_t size;
} FreeRange;

typedef struct GeometryPoolBlock
{
    ID3D12Resource* buffer;
    D3D12_GPU_VIRTUAL_ADDRESS address;
    uint32_t totalBytes;
    FreeRange* freeRanges;      // отсортированы по offset, соседние слиты
    uint32_t freeRangeCount;
    uint32_t freeRangeCapacity;
    D3D12_RESOURCE_STATES currentState;
    bool touchedByUploads;
} GeometryPoolBlock;

typedef struct PendingUpload
{
    ID3D12Resource* staging;
    uint32_t sourceOffset;
    uint32_t blockIndex;
    uint32_t destinationOffset;
    uint32_t sizeBytes;
    bool ownsStaging;
} PendingUpload;

typedef struct DeferredResourceRelease
{
    ID3D12Resource* resource;
    UINT64 safeFenceValue;
} DeferredResourceRelease;

typedef struct DeferredRangeRelease
{
    uint32_t blockIndex;
    uint32_t offset;
    uint32_t size;
    UINT64 safeFenceValue;
} DeferredRangeRelease;

struct RendererMesh
{
    uint32_t blockIndex;
    uint32_t offsetBytes;
    uint32_t sizeBytes;
    uint32_t quadCount;
};

struct Renderer
{
    IDXGIFactory4*             factory;
    ID3D12Device*              device;
    ID3D12CommandQueue*        commandQueue;
    IDXGISwapChain3*           swapChain;
    ID3D12CommandAllocator*    commandAllocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* commandList;
    ID3D12Resource*            renderTargets[FRAME_COUNT];
    ID3D12DescriptorHeap*      renderTargetViewHeap;
    UINT                       renderTargetViewSize;
    ID3D12Resource*            depthBuffer;
    ID3D12DescriptorHeap*      depthStencilViewHeap;
    ID3D12Fence*               fence;
    HANDLE                     fenceEvent;
    UINT64                     fenceValues[FRAME_COUNT];
    UINT64                     lastSignaledFenceValue;
    UINT                       frameIndex;
    ID3D12RootSignature*       rootSignature;
    ID3D12PipelineState*       pipelineState;
    ID3D12Resource*            blockTexture;
    ID3D12Resource*            blockTextureUpload;
    ID3D12Resource*            blockNormalTexture;
    ID3D12Resource*            blockNormalUpload;
    ID3D12DescriptorHeap*      srvHeap;
    UINT                       srvDescriptorSize;
    bool                       blockTextureUploadPending;

    // Панорама: кубмапа сцены (грани в пространстве вида) + резолв.
    ID3D12Resource*            cubeColor;
    ID3D12Resource*            cubeDepth;
    ID3D12DescriptorHeap*      cubeRtvHeap;      // 6 RTV по слоям
    ID3D12DescriptorHeap*      cubeDsvHeap;
    uint32_t                   cubeResolution;   // 0 — ресурсы не созданы
    ID3D12RootSignature*       resolveRootSignature;
    ID3D12PipelineState*       resolvePipelineState;
    RendererFrameSetup         frame;            // setup текущего кадра

    // Слой UI: vertex pulling из upload-кольца, атлас шрифта.
    ID3D12RootSignature*       uiRootSignature;
    ID3D12PipelineState*       uiPipelineState;
    ID3D12Resource*            uiQuadBuffers[FRAME_COUNT];
    uint8_t*                   uiQuadMapped[FRAME_COUNT];
    uint32_t                   uiQuadCount;
    ID3D12Resource*            fontTexture;
    ID3D12Resource*            fontTextureUpload;
    bool                       fontUploadPending;
    bool                       fontReady;
    ID3D12Resource*            backgroundTexture;
    ID3D12Resource*            backgroundTextureUpload;
    bool                       backgroundUploadPending;
    bool                       backgroundReady;
    D3D12_VIEWPORT             viewport;
    D3D12_RECT                 scissorRect;
    int32_t                    windowWidth;
    int32_t                    windowHeight;
    int32_t                    resizeWidth;
    int32_t                    resizeHeight;
    bool                       resizeRequested;
    bool                       verticalSyncEnabled;
    bool                       tearingSupported;
    bool                       tearingPresentEnabled;
    bool                       wireframeEnabled;
    bool                       worldReady;

    // Загруженный байткод шейдеров (для шейдерпаков).
    void* loadedChunkVS;       uint32_t loadedChunkVSLength;
    void* loadedChunkPS;       uint32_t loadedChunkPSLength;
    void* loadedPanoramaVS;    uint32_t loadedPanoramaVSLength;
    void* loadedPanoramaPS;    uint32_t loadedPanoramaPSLength;
    void* loadedUIVS;          uint32_t loadedUIVSLength;
    void* loadedUIPS;          uint32_t loadedUIPSLength;

    GeometryPoolBlock          poolBlocks[MAX_POOL_BLOCKS];
    uint32_t                   poolBlockCount;

    PendingUpload              pendingUploads[MAX_PENDING_UPLOADS];
    uint32_t                   pendingUploadCount;
    ID3D12Resource*            meshUploadBuffers[FRAME_COUNT];
    uint8_t*                   meshUploadMapped[FRAME_COUNT];
    uint32_t                   meshUploadOffsets[FRAME_COUNT];
    ID3D12Resource*            instanceBuffers[FRAME_COUNT];
    uint8_t*                   instanceMapped[FRAME_COUNT];
    uint32_t                   instanceOffsets[FRAME_COUNT];

    DeferredResourceRelease    deferredResources[DEFERRED_RELEASE_CAPACITY];
    uint32_t                   deferredResourceHead;
    uint32_t                   deferredResourceCount;

    DeferredRangeRelease       deferredRanges[DEFERRED_RELEASE_CAPACITY];
    uint32_t                   deferredRangeHead;
    uint32_t                   deferredRangeCount;

    RendererStats              currentStats;
    RendererStats              lastStats;
    RendererContentStatus     texturePackLoadStatus;
};

static bool RecreateChunkPipelineState(Renderer* renderer);

static D3D12_RESOURCE_BARRIER MakeTransitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
    D3D12_RESOURCE_BARRIER barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

static void WaitForGpu(Renderer* renderer)
{
    UINT64 targetValue = renderer->fenceValues[renderer->frameIndex];
    ID3D12CommandQueue_Signal(renderer->commandQueue, renderer->fence, targetValue);
    renderer->lastSignaledFenceValue = targetValue;
    ID3D12Fence_SetEventOnCompletion(renderer->fence, targetValue, renderer->fenceEvent);
    WaitForSingleObject(renderer->fenceEvent, INFINITE);
    renderer->fenceValues[renderer->frameIndex]++;
}

static bool MoveToNextFrame(Renderer* renderer)
{
    bool waitedForFence = false;
    UINT64 currentValue = renderer->fenceValues[renderer->frameIndex];
    ID3D12CommandQueue_Signal(renderer->commandQueue, renderer->fence, currentValue);
    renderer->lastSignaledFenceValue = currentValue;

    renderer->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swapChain);

    if (ID3D12Fence_GetCompletedValue(renderer->fence) < renderer->fenceValues[renderer->frameIndex])
    {
        ID3D12Fence_SetEventOnCompletion(renderer->fence, renderer->fenceValues[renderer->frameIndex], renderer->fenceEvent);
        WaitForSingleObject(renderer->fenceEvent, INFINITE);
        waitedForFence = true;
    }

    renderer->fenceValues[renderer->frameIndex] = currentValue + 1;
    return waitedForFence;
}

// === Пул геометрии ===

static bool PoolBlockCreate(Renderer* renderer, uint32_t minimumBytes, uint32_t* outBlockIndex)
{
    if (renderer->poolBlockCount == MAX_POOL_BLOCKS)
    {
        return false;
    }

    uint32_t totalBytes = minimumBytes > POOL_BLOCK_BYTES ? minimumBytes : POOL_BLOCK_BYTES;

    GeometryPoolBlock* block = &renderer->poolBlocks[renderer->poolBlockCount];
    memset(block, 0, sizeof(*block));

    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC description;
    memset(&description, 0, sizeof(description));
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = totalBytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.SampleDesc.Count = 1;

    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&block->buffer)))
    {
        return false;
    }

    block->freeRangeCapacity = 16;
    block->freeRanges = HeapAlloc(GetProcessHeap(), 0, block->freeRangeCapacity * sizeof(FreeRange));
    if (block->freeRanges == NULL)
    {
        ID3D12Resource_Release(block->buffer);
        block->buffer = NULL;
        return false;
    }

    block->address = ID3D12Resource_GetGPUVirtualAddress(block->buffer);
    block->totalBytes = totalBytes;
    block->freeRanges[0].offset = 0;
    block->freeRanges[0].size = totalBytes;
    block->freeRangeCount = 1;
    block->currentState = D3D12_RESOURCE_STATE_COMMON;

    *outBlockIndex = renderer->poolBlockCount++;
    return true;
}

static bool PoolAllocate(Renderer* renderer, uint32_t sizeBytes, uint32_t* outBlockIndex, uint32_t* outOffset)
{
    for (uint32_t blockIndex = 0; blockIndex < renderer->poolBlockCount; ++blockIndex)
    {
        GeometryPoolBlock* block = &renderer->poolBlocks[blockIndex];
        for (uint32_t i = 0; i < block->freeRangeCount; ++i)
        {
            FreeRange* range = &block->freeRanges[i];
            if (range->size < sizeBytes)
            {
                continue;
            }

            *outBlockIndex = blockIndex;
            *outOffset = range->offset;
            range->offset += sizeBytes;
            range->size -= sizeBytes;
            if (range->size == 0)
            {
                block->freeRanges[i] = block->freeRanges[--block->freeRangeCount];
                // Список должен оставаться отсортированным для коалесценции —
                // простая пересортировка вставкой (список короткий).
                for (uint32_t j = 1; j < block->freeRangeCount; ++j)
                {
                    FreeRange moved = block->freeRanges[j];
                    uint32_t position = j;
                    while (position > 0 && block->freeRanges[position - 1].offset > moved.offset)
                    {
                        block->freeRanges[position] = block->freeRanges[position - 1];
                        position--;
                    }
                    block->freeRanges[position] = moved;
                }
            }
            return true;
        }
    }

    uint32_t newBlockIndex;
    if (!PoolBlockCreate(renderer, sizeBytes, &newBlockIndex))
    {
        return false;
    }

    GeometryPoolBlock* block = &renderer->poolBlocks[newBlockIndex];
    *outBlockIndex = newBlockIndex;
    *outOffset = 0;
    block->freeRanges[0].offset = sizeBytes;
    block->freeRanges[0].size = block->totalBytes - sizeBytes;
    block->freeRangeCount = block->freeRanges[0].size == 0 ? 0 : 1;
    return true;
}

// Возвращает диапазон в список свободных с коалесценцией соседей.
static void PoolFree(GeometryPoolBlock* block, uint32_t offset, uint32_t size)
{
    uint32_t position = 0;
    while (position < block->freeRangeCount && block->freeRanges[position].offset < offset)
    {
        position++;
    }

    bool mergesWithPrevious = position > 0 &&
        block->freeRanges[position - 1].offset + block->freeRanges[position - 1].size == offset;
    bool mergesWithNext = position < block->freeRangeCount &&
        offset + size == block->freeRanges[position].offset;

    if (mergesWithPrevious && mergesWithNext)
    {
        block->freeRanges[position - 1].size += size + block->freeRanges[position].size;
        for (uint32_t i = position; i + 1 < block->freeRangeCount; ++i)
        {
            block->freeRanges[i] = block->freeRanges[i + 1];
        }
        block->freeRangeCount--;
        return;
    }
    if (mergesWithPrevious)
    {
        block->freeRanges[position - 1].size += size;
        return;
    }
    if (mergesWithNext)
    {
        block->freeRanges[position].offset = offset;
        block->freeRanges[position].size += size;
        return;
    }

    if (block->freeRangeCount == block->freeRangeCapacity)
    {
        uint32_t newCapacity = block->freeRangeCapacity * 2;
        FreeRange* newRanges = HeapReAlloc(GetProcessHeap(), 0, block->freeRanges, newCapacity * sizeof(FreeRange));
        if (newRanges == NULL)
        {
            return; // диапазон потерян до конца жизни пула — только при OOM
        }
        block->freeRanges = newRanges;
        block->freeRangeCapacity = newCapacity;
    }

    for (uint32_t i = block->freeRangeCount; i > position; --i)
    {
        block->freeRanges[i] = block->freeRanges[i - 1];
    }
    block->freeRanges[position].offset = offset;
    block->freeRanges[position].size = size;
    block->freeRangeCount++;
}

// Освобождает ресурсы и диапазоны, кадры которых GPU уже прошёл.
static void DrainDeferredReleases(Renderer* renderer, bool releaseEverything)
{
    UINT64 completedValue = ID3D12Fence_GetCompletedValue(renderer->fence);

    while (renderer->deferredResourceCount > 0)
    {
        DeferredResourceRelease* entry = &renderer->deferredResources[renderer->deferredResourceHead];
        if (!releaseEverything && entry->safeFenceValue > completedValue)
        {
            break;
        }
        ID3D12Resource_Release(entry->resource);
        renderer->deferredResourceHead = (renderer->deferredResourceHead + 1) % DEFERRED_RELEASE_CAPACITY;
        renderer->deferredResourceCount--;
    }

    while (renderer->deferredRangeCount > 0)
    {
        DeferredRangeRelease* entry = &renderer->deferredRanges[renderer->deferredRangeHead];
        if (!releaseEverything && entry->safeFenceValue > completedValue)
        {
            break;
        }
        PoolFree(&renderer->poolBlocks[entry->blockIndex], entry->offset, entry->size);
        renderer->deferredRangeHead = (renderer->deferredRangeHead + 1) % DEFERRED_RELEASE_CAPACITY;
        renderer->deferredRangeCount--;
    }
}

static void DeferResourceRelease(Renderer* renderer, ID3D12Resource* resource)
{
    if (renderer->deferredResourceCount == DEFERRED_RELEASE_CAPACITY)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }

    uint32_t slot = (renderer->deferredResourceHead + renderer->deferredResourceCount) % DEFERRED_RELEASE_CAPACITY;
    renderer->deferredResources[slot].resource = resource;
    renderer->deferredResources[slot].safeFenceValue = renderer->lastSignaledFenceValue + 1;
    renderer->deferredResourceCount++;
}

static D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(Renderer* renderer, uint32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->srvHeap, &handle);
    handle.ptr += (SIZE_T)slot * renderer->srvDescriptorSize;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(Renderer* renderer, uint32_t slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(renderer->srvHeap, &handle);
    handle.ptr += (UINT64)slot * renderer->srvDescriptorSize;
    return handle;
}

static bool CreateRootSignature(Renderer* renderer)
{
    D3D12_ROOT_PARAMETER parameters[4];
    memset(parameters, 0, sizeof(parameters));
    parameters[ROOT_PARAMETER_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ROOT_PARAMETER_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ROOT_PARAMETER_CONSTANTS].Constants.ShaderRegister = 0;
    parameters[ROOT_PARAMETER_CONSTANTS].Constants.Num32BitValues = ROOT_CONSTANT_COUNT;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].Descriptor.ShaderRegister = 0;
    parameters[ROOT_PARAMETER_INSTANCES].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[ROOT_PARAMETER_INSTANCES].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[ROOT_PARAMETER_INSTANCES].Descriptor.ShaderRegister = 3;

    // t1 — albedo, t2 — нормали: соседние слоты одной таблицы.
    D3D12_DESCRIPTOR_RANGE textureRange;
    memset(&textureRange, 0, sizeof(textureRange));
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 2;
    textureRange.BaseShaderRegister = 1;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[ROOT_PARAMETER_BLOCK_TEXTURES].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ROOT_PARAMETER_BLOCK_TEXTURES].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[ROOT_PARAMETER_BLOCK_TEXTURES].DescriptorTable.NumDescriptorRanges = 1;
    parameters[ROOT_PARAMETER_BLOCK_TEXTURES].DescriptorTable.pDescriptorRanges = &textureRange;

    D3D12_STATIC_SAMPLER_DESC sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description;
    memset(&description, 0, sizeof(description));
    description.NumParameters = 4;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;

    ID3DBlob* signatureBlob = NULL;
    ID3DBlob* errorBlob = NULL;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob)))
    {
        if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
        return false;
    }

    HRESULT result = ID3D12Device_CreateRootSignature(renderer->device, 0,
        ID3D10Blob_GetBufferPointer(signatureBlob), ID3D10Blob_GetBufferSize(signatureBlob),
        &IID_ID3D12RootSignature, (void**)&renderer->rootSignature);

    ID3D10Blob_Release(signatureBlob);
    if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
    return SUCCEEDED(result);
}

static bool CreatePipelineState(Renderer* renderer)
{
    // Vertex pulling: вершинных буферов нет, input layout пуст,
    // геометрия читается шейдером из ByteAddressBuffer по SV_VertexID.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->rootSignature;
    description.VS.pShaderBytecode = g_chunk_vs;
    description.VS.BytecodeLength = sizeof(g_chunk_vs);
    description.PS.pShaderBytecode = g_chunk_ps;
    description.PS.BytecodeLength = sizeof(g_chunk_ps);
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    // Перестановка Y/Z в геометрии инвертирует winding, но перевод Z-up мира
    // в D3D view-space (Y-up, Z-forward) инвертирует его ещё раз. Эти две
    // перестановки компенсируют друг друга, поэтому сохраняем исходное правило:
    // внешние стороны квада имеют clockwise winding в render-target space.
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->pipelineState));
}

// Общий путь создания texture array блоков (albedo или нормали):
// DEFAULT-текстура + upload-буфер с подресурсами, SRV в заданный слот.
typedef bool (*PackSubresourceGetter)(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource);

static bool CreateBlockArrayTexture(Renderer* renderer,
    const TexturePackData* pack, PackSubresourceGetter getSubresource,
    uint32_t srvSlot, DXGI_FORMAT format,
    ID3D12Resource** outTexture, ID3D12Resource** outUpload)
{
    UINT subresourceCount = (UINT)pack->layerCount * pack->mipCount;
    if (subresourceCount == 0 || subresourceCount > MAX_TEXTURE_SUBRESOURCES)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDescription;
    memset(&textureDescription, 0, sizeof(textureDescription));
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = pack->width;
    textureDescription.Height = pack->height;
    textureDescription.DepthOrArraySize = (UINT16)pack->layerCount;
    textureDescription.MipLevels = (UINT16)pack->mipCount;
    textureDescription.Format = format;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &defaultHeap,
        D3D12_HEAP_FLAG_NONE, &textureDescription, D3D12_RESOURCE_STATE_COPY_DEST,
        NULL, &IID_ID3D12Resource, (void**)outTexture)))
    {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[MAX_TEXTURE_SUBRESOURCES];
    UINT64 uploadBytes = 0;
    ID3D12Device_GetCopyableFootprints(renderer->device, &textureDescription,
        0, subresourceCount, 0, layouts, NULL, NULL, &uploadBytes);

    D3D12_RESOURCE_DESC uploadDescription;
    memset(&uploadDescription, 0, sizeof(uploadDescription));
    uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDescription.Width = uploadBytes;
    uploadDescription.Height = 1;
    uploadDescription.DepthOrArraySize = 1;
    uploadDescription.MipLevels = 1;
    uploadDescription.SampleDesc.Count = 1;
    uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap = { .Type = D3D12_HEAP_TYPE_UPLOAD };
    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &uploadHeap,
        D3D12_HEAP_FLAG_NONE, &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL, &IID_ID3D12Resource, (void**)outUpload)))
    {
        return false;
    }

    D3D12_RANGE emptyRange = { 0, 0 };
    unsigned char* mapped = NULL;
    if (FAILED(ID3D12Resource_Map(*outUpload, 0, &emptyRange, (void**)&mapped)))
    {
        return false;
    }

    for (uint32_t layer = 0; layer < pack->layerCount; ++layer)
    {
        for (uint32_t mip = 0; mip < pack->mipCount; ++mip)
        {
            UINT index = layer * pack->mipCount + mip;
            TexturePackSubresource subresource;
            if (!getSubresource(pack, layer, mip, &subresource))
            {
                ID3D12Resource_Unmap(*outUpload, 0, NULL);
                return false;
            }
            unsigned char* destination = mapped + layouts[index].Offset;
            for (uint32_t row = 0; row < subresource.height; ++row)
            {
                memcpy(destination + (size_t)row * layouts[index].Footprint.RowPitch,
                    subresource.pixels + (size_t)row * subresource.rowBytes,
                    subresource.rowBytes);
            }
        }
    }
    ID3D12Resource_Unmap(*outUpload, 0, NULL);

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription;
    memset(&viewDescription, 0, sizeof(viewDescription));
    viewDescription.Format = textureDescription.Format;
    viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    viewDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDescription.Texture2DArray.MipLevels = pack->mipCount;
    viewDescription.Texture2DArray.ArraySize = pack->layerCount;

    ID3D12Device_CreateShaderResourceView(renderer->device, *outTexture,
        &viewDescription, SrvCpuHandle(renderer, srvSlot));
    return true;
}

static bool CreateBlockTexture(Renderer* renderer)
{
    TexturePackData pack;
    TexturePackLoadStatus loadStatus = TexturePackLoadActive(&pack);
    switch (loadStatus)
    {
        case TEXTURE_PACK_LOAD_OK:
            renderer->texturePackLoadStatus = RENDERER_CONTENT_OK; break;
        case TEXTURE_PACK_LOAD_NO_ACTIVE_PACK:
            renderer->texturePackLoadStatus = RENDERER_CONTENT_NO_ACTIVE; break;
        case TEXTURE_PACK_LOAD_INVALID:
            renderer->texturePackLoadStatus = RENDERER_CONTENT_INVALID; break;
        default:
            renderer->texturePackLoadStatus = RENDERER_CONTENT_IO_ERROR; break;
    }
    if (pack.pixels == NULL)
    {
        return false;
    }

    // Albedo хранится в sRGB (декод при выборке), нормали — линейные.
    bool succeeded = CreateBlockArrayTexture(renderer, &pack,
        TexturePackGetSubresource, SRV_SLOT_BLOCK_TEXTURES,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        &renderer->blockTexture, &renderer->blockTextureUpload);

    if (succeeded && pack.normalPixels != NULL)
    {
        succeeded = CreateBlockArrayTexture(renderer, &pack,
            TexturePackGetNormalSubresource, SRV_SLOT_BLOCK_NORMALS,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &renderer->blockNormalTexture, &renderer->blockNormalUpload);
    }
    else if (succeeded)
    {
        // Пак без нормалей (LTP1): плоская нормаль 1x1 на каждый слой,
        // AO = 1 — освещение вырождается в чистый ламберт по граням.
        static const uint8_t flatNormalPixels[TEXTURE_PACK_LAYER_COUNT * 4] = {
            128, 128, 255, 255,
            128, 128, 255, 255,
            128, 128, 255, 255,
        };
        TexturePackData flat = {
            .width = 1,
            .height = 1,
            .layerCount = TEXTURE_PACK_LAYER_COUNT,
            .mipCount = 1,
            .pixels = flatNormalPixels,
            .pixelBytes = sizeof(flatNormalPixels),
        };
        succeeded = CreateBlockArrayTexture(renderer, &flat,
            TexturePackGetSubresource, SRV_SLOT_BLOCK_NORMALS,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &renderer->blockNormalTexture, &renderer->blockNormalUpload);
    }

    renderer->blockTextureUploadPending = succeeded;
    if (!succeeded)
        renderer->texturePackLoadStatus = RENDERER_CONTENT_GPU_ERROR;
    TexturePackRelease(&pack);
    return succeeded;
}

// === Панорама и UI: сигнатуры, конвейеры, ресурсы ===

static bool CreateResolveRootSignature(Renderer* renderer)
{
    D3D12_ROOT_PARAMETER parameters[2];
    memset(parameters, 0, sizeof(parameters));
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = RESOLVE_ROOT_CONSTANT_COUNT;

    D3D12_DESCRIPTOR_RANGE cubeRange;
    memset(&cubeRange, 0, sizeof(cubeRange));
    cubeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cubeRange.NumDescriptors = 1;
    cubeRange.BaseShaderRegister = 0;
    cubeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &cubeRange;

    D3D12_STATIC_SAMPLER_DESC sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description;
    memset(&description, 0, sizeof(description));
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;

    ID3DBlob* signatureBlob = NULL;
    ID3DBlob* errorBlob = NULL;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
        &signatureBlob, &errorBlob)))
    {
        if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
        return false;
    }

    HRESULT result = ID3D12Device_CreateRootSignature(renderer->device, 0,
        ID3D10Blob_GetBufferPointer(signatureBlob), ID3D10Blob_GetBufferSize(signatureBlob),
        &IID_ID3D12RootSignature, (void**)&renderer->resolveRootSignature);

    ID3D10Blob_Release(signatureBlob);
    if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
    return SUCCEEDED(result);
}

static bool CreateResolvePipelineState(Renderer* renderer)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->resolveRootSignature;
    description.VS.pShaderBytecode = g_panorama_vs;
    description.VS.BytecodeLength = sizeof(g_panorama_vs);
    description.PS.pShaderBytecode = g_panorama_ps;
    description.PS.BytecodeLength = sizeof(g_panorama_ps);
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->resolvePipelineState));
}

static bool CreateUiRootSignature(Renderer* renderer)
{
    D3D12_ROOT_PARAMETER parameters[3];
    memset(parameters, 0, sizeof(parameters));
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 2;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].Descriptor.ShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE fontRange;
    memset(&fontRange, 0, sizeof(fontRange));
    fontRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fontRange.NumDescriptors = 2;
    fontRange.BaseShaderRegister = 1;
    fontRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &fontRange;

    D3D12_STATIC_SAMPLER_DESC sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description;
    memset(&description, 0, sizeof(description));
    description.NumParameters = 3;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;

    ID3DBlob* signatureBlob = NULL;
    ID3DBlob* errorBlob = NULL;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
        &signatureBlob, &errorBlob)))
    {
        if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
        return false;
    }

    HRESULT result = ID3D12Device_CreateRootSignature(renderer->device, 0,
        ID3D10Blob_GetBufferPointer(signatureBlob), ID3D10Blob_GetBufferSize(signatureBlob),
        &IID_ID3D12RootSignature, (void**)&renderer->uiRootSignature);

    ID3D10Blob_Release(signatureBlob);
    if (errorBlob != NULL) ID3D10Blob_Release(errorBlob);
    return SUCCEEDED(result);
}

static bool CreateUiPipelineState(Renderer* renderer)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->uiRootSignature;
    description.VS.pShaderBytecode = g_ui_vs;
    description.VS.BytecodeLength = sizeof(g_ui_vs);
    description.PS.pShaderBytecode = g_ui_ps;
    description.PS.BytecodeLength = sizeof(g_ui_ps);
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC* blend = &description.BlendState.RenderTarget[0];
    blend->BlendEnable = TRUE;
    blend->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend->BlendOp = D3D12_BLEND_OP_ADD;
    blend->SrcBlendAlpha = D3D12_BLEND_ONE;
    blend->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->uiPipelineState));
}

static bool CreateUiQuadBuffers(Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC description;
    memset(&description, 0, sizeof(description));
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = (UINT64)RENDERER_UI_MAX_QUADS * UI_QUAD_BYTES;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.SampleDesc.Count = 1;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties,
            D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void**)&renderer->uiQuadBuffers[i])))
        {
            return false;
        }

        D3D12_RANGE emptyRange = { 0, 0 };
        if (FAILED(ID3D12Resource_Map(renderer->uiQuadBuffers[i], 0, &emptyRange,
            (void**)&renderer->uiQuadMapped[i])))
        {
            return false;
        }
    }
    return true;
}

static bool CreateMeshUploadBuffers(Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC description;
    memset(&description, 0, sizeof(description));
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = MESH_UPLOAD_BYTES_PER_FRAME;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.SampleDesc.Count = 1;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device,
            &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&renderer->meshUploadBuffers[i])))
        {
            return false;
        }
        D3D12_RANGE emptyRange = { 0, 0 };
        if (FAILED(ID3D12Resource_Map(renderer->meshUploadBuffers[i], 0,
            &emptyRange, (void**)&renderer->meshUploadMapped[i])))
        {
            return false;
        }

        description.Width = INSTANCE_BYTES_PER_FRAME;
        if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device,
            &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&renderer->instanceBuffers[i])))
        {
            return false;
        }
        if (FAILED(ID3D12Resource_Map(renderer->instanceBuffers[i], 0,
            &emptyRange, (void**)&renderer->instanceMapped[i])))
        {
            return false;
        }
        description.Width = MESH_UPLOAD_BYTES_PER_FRAME;
    }
    return true;
}

// Создаёт (или пересоздаёт под новое разрешение) кубмапу сцены и её
// глубину. Пересоздание редкое — при смене разрешения грани от FOV,
// поэтому честно дожидается GPU перед заменой дескрипторов.
static bool EnsureCubeResources(Renderer* renderer, uint32_t resolution)
{
    if (renderer->cubeResolution == resolution && renderer->cubeColor != NULL)
    {
        return true;
    }

    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);

    if (renderer->cubeColor != NULL)
    {
        ID3D12Resource_Release(renderer->cubeColor);
        renderer->cubeColor = NULL;
    }
    if (renderer->cubeDepth != NULL)
    {
        ID3D12Resource_Release(renderer->cubeDepth);
        renderer->cubeDepth = NULL;
    }
    renderer->cubeResolution = 0;

    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC colorDescription;
    memset(&colorDescription, 0, sizeof(colorDescription));
    colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDescription.Width = resolution;
    colorDescription.Height = resolution;
    colorDescription.DepthOrArraySize = 6;
    colorDescription.MipLevels = 1;
    colorDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    colorDescription.SampleDesc.Count = 1;
    colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE colorClear = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .Color = { 0.4f, 0.6f, 0.9f, 1.0f },
    };

    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties,
        D3D12_HEAP_FLAG_NONE, &colorDescription, D3D12_RESOURCE_STATE_RENDER_TARGET,
        &colorClear, &IID_ID3D12Resource, (void**)&renderer->cubeColor)))
    {
        return false;
    }

    D3D12_RESOURCE_DESC depthDescription;
    memset(&depthDescription, 0, sizeof(depthDescription));
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = resolution;
    depthDescription.Height = resolution;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .DepthStencil = { .Depth = 1.0f, .Stencil = 0 },
    };

    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties,
        D3D12_HEAP_FLAG_NONE, &depthDescription, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthClear, &IID_ID3D12Resource, (void**)&renderer->cubeDepth)))
    {
        ID3D12Resource_Release(renderer->cubeColor);
        renderer->cubeColor = NULL;
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->cubeRtvHeap, &rtvHandle);
    for (UINT face = 0; face < 6; ++face)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDescription;
        memset(&rtvDescription, 0, sizeof(rtvDescription));
        rtvDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rtvDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDescription.Texture2DArray.FirstArraySlice = face;
        rtvDescription.Texture2DArray.ArraySize = 1;
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->cubeColor,
            &rtvDescription, rtvHandle);
        rtvHandle.ptr += renderer->renderTargetViewSize;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->cubeDsvHeap, &dsvHandle);
    ID3D12Device_CreateDepthStencilView(renderer->device, renderer->cubeDepth, NULL, dsvHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription;
    memset(&srvDescription, 0, sizeof(srvDescription));
    srvDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.TextureCube.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(renderer->device, renderer->cubeColor,
        &srvDescription, SrvCpuHandle(renderer, SRV_SLOT_PANORAMA_CUBE));

    renderer->cubeResolution = resolution;
    return true;
}

static bool CreateDepthBuffer(Renderer* renderer, int32_t width, int32_t height)
{
    if (renderer->depthStencilViewHeap != NULL)
    {
        ID3D12DescriptorHeap_Release(renderer->depthStencilViewHeap);
        renderer->depthStencilViewHeap = NULL;
    }
    if (renderer->depthBuffer != NULL)
    {
        ID3D12Resource_Release(renderer->depthBuffer);
        renderer->depthBuffer = NULL;
    }

    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_DEFAULT };

    D3D12_RESOURCE_DESC description;
    memset(&description, 0, sizeof(description));
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = (UINT64)width;
    description.Height = (UINT)height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .DepthStencil = { .Depth = 1.0f, .Stencil = 0 },
    };

    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        &IID_ID3D12Resource, (void**)&renderer->depthBuffer)))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
    };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(renderer->device, &heapDescription,
        &IID_ID3D12DescriptorHeap, (void**)&renderer->depthStencilViewHeap)))
    {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->depthStencilViewHeap, &depthStencilViewHandle);
    ID3D12Device_CreateDepthStencilView(renderer->device, renderer->depthBuffer, NULL, depthStencilViewHandle);
    return true;
}

Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height)
{
    Renderer* renderer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*renderer));
    if (renderer == NULL)
    {
        return NULL;
    }

    renderer->windowWidth = width;
    renderer->windowHeight = height;
    renderer->verticalSyncEnabled = true;

    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&renderer->factory)))
    {
        HeapFree(GetProcessHeap(), 0, renderer);
        return NULL;
    }

    IDXGIFactory5* factory5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(
            renderer->factory, &IID_IDXGIFactory5, (void**)&factory5)))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(factory5,
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing, (UINT)sizeof(allowTearing))))
        {
            renderer->tearingSupported = allowTearing != FALSE;
            renderer->tearingPresentEnabled = renderer->tearingSupported;
        }
        IDXGIFactory5_Release(factory5);
    }

    // Аппаратный адаптер, при неудаче — программный WARP.
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&renderer->device)))
    {
        IDXGIAdapter* warpAdapter = NULL;
        if (FAILED(IDXGIFactory4_EnumWarpAdapter(renderer->factory, &IID_IDXGIAdapter, (void**)&warpAdapter)))
        {
            RendererDestroy(renderer);
            return NULL;
        }
        HRESULT result = D3D12CreateDevice((IUnknown*)warpAdapter, D3D_FEATURE_LEVEL_11_0,
            &IID_ID3D12Device, (void**)&renderer->device);
        IDXGIAdapter_Release(warpAdapter);
        if (FAILED(result))
        {
            RendererDestroy(renderer);
            return NULL;
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(ID3D12Device_CreateCommandQueue(renderer->device, &queueDescription,
        &IID_ID3D12CommandQueue, (void**)&renderer->commandQueue)))
    {
        RendererDestroy(renderer);
        return NULL;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription;
    memset(&swapChainDescription, 0, sizeof(swapChainDescription));
    swapChainDescription.BufferCount = FRAME_COUNT;
    swapChainDescription.Width = (UINT)width;
    swapChainDescription.Height = (UINT)height;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.Flags = renderer->tearingSupported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    IDXGISwapChain1* swapChain1 = NULL;
    if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(renderer->factory, (IUnknown*)renderer->commandQueue,
        (HWND)windowHandle, &swapChainDescription, NULL, NULL, &swapChain1)))
    {
        RendererDestroy(renderer);
        return NULL;
    }

    HRESULT queryResult = IDXGISwapChain1_QueryInterface(swapChain1, &IID_IDXGISwapChain3, (void**)&renderer->swapChain);
    IDXGISwapChain1_Release(swapChain1);
    if (FAILED(queryResult))
    {
        RendererDestroy(renderer);
        return NULL;
    }

    renderer->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swapChain);

    D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = FRAME_COUNT,
    };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(renderer->device, &renderTargetHeapDescription,
        &IID_ID3D12DescriptorHeap, (void**)&renderer->renderTargetViewHeap)))
    {
        RendererDestroy(renderer);
        return NULL;
    }

    renderer->renderTargetViewSize = ID3D12Device_GetDescriptorHandleIncrementSize(renderer->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Swapchain остаётся UNORM (требование flip-модели), но RTV — sRGB:
    // шейдеры работают в линейном свете, кодирование выполняет вывод.
    D3D12_RENDER_TARGET_VIEW_DESC backBufferViewDescription;
    memset(&backBufferViewDescription, 0, sizeof(backBufferViewDescription));
    backBufferViewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    backBufferViewDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(renderer->swapChain, i, &IID_ID3D12Resource, (void**)&renderer->renderTargets[i])))
        {
            RendererDestroy(renderer);
            return NULL;
        }
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->renderTargets[i],
            &backBufferViewDescription, renderTargetViewHandle);
        renderTargetViewHandle.ptr += renderer->renderTargetViewSize;
    }

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(ID3D12Device_CreateCommandAllocator(renderer->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&renderer->commandAllocators[i])))
        {
            RendererDestroy(renderer);
            return NULL;
        }
    }

    if (FAILED(ID3D12Device_CreateCommandList(renderer->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        renderer->commandAllocators[0], NULL, &IID_ID3D12GraphicsCommandList, (void**)&renderer->commandList)))
    {
        RendererDestroy(renderer);
        return NULL;
    }
    ID3D12GraphicsCommandList_Close(renderer->commandList);

    if (FAILED(ID3D12Device_CreateFence(renderer->device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&renderer->fence)))
    {
        RendererDestroy(renderer);
        return NULL;
    }

    renderer->fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (renderer->fenceEvent == NULL)
    {
        RendererDestroy(renderer);
        return NULL;
    }

    // Общая шейдерная куча SRV: блоки, кубмапа панорамы, атлас шрифта.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDescription = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = SRV_SLOT_COUNT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(renderer->device, &srvHeapDescription,
        &IID_ID3D12DescriptorHeap, (void**)&renderer->srvHeap)))
    {
        RendererDestroy(renderer);
        return NULL;
    }
    renderer->srvDescriptorSize = ID3D12Device_GetDescriptorHandleIncrementSize(
        renderer->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateUiRootSignature(renderer)) { RendererDestroy(renderer); return NULL; }
    if (!CreateUiPipelineState(renderer)) { RendererDestroy(renderer); return NULL; }
    if (!CreateUiQuadBuffers(renderer)) { RendererDestroy(renderer); return NULL; }

    renderer->viewport.TopLeftX = 0.0f;
    renderer->viewport.TopLeftY = 0.0f;
    renderer->viewport.Width = (float)width;
    renderer->viewport.Height = (float)height;
    renderer->viewport.MinDepth = 0.0f;
    renderer->viewport.MaxDepth = 1.0f;
    renderer->scissorRect.left = 0;
    renderer->scissorRect.top = 0;
    renderer->scissorRect.right = width;
    renderer->scissorRect.bottom = height;

    WaitForGpu(renderer);

    return renderer;
}

void RendererReleaseWorld(Renderer* renderer)
{
    if (renderer == NULL) return;
    if (renderer->commandQueue != NULL && renderer->fence != NULL
        && renderer->fenceEvent != NULL)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }

    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        if (renderer->pendingUploads[i].ownsStaging
            && renderer->pendingUploads[i].staging != NULL)
            ID3D12Resource_Release(renderer->pendingUploads[i].staging);
    }
    renderer->pendingUploadCount = 0;
    for (uint32_t i = 0; i < renderer->poolBlockCount; ++i)
    {
        if (renderer->poolBlocks[i].buffer != NULL)
            ID3D12Resource_Release(renderer->poolBlocks[i].buffer);
        if (renderer->poolBlocks[i].freeRanges != NULL)
            HeapFree(GetProcessHeap(), 0,
                renderer->poolBlocks[i].freeRanges);
    }
    memset(renderer->poolBlocks, 0, sizeof(renderer->poolBlocks));
    renderer->poolBlockCount = 0;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (renderer->meshUploadBuffers[i] != NULL)
        {
            if (renderer->meshUploadMapped[i] != NULL)
                ID3D12Resource_Unmap(renderer->meshUploadBuffers[i], 0, NULL);
            ID3D12Resource_Release(renderer->meshUploadBuffers[i]);
        }
        if (renderer->instanceBuffers[i] != NULL)
        {
            if (renderer->instanceMapped[i] != NULL)
                ID3D12Resource_Unmap(renderer->instanceBuffers[i], 0, NULL);
            ID3D12Resource_Release(renderer->instanceBuffers[i]);
        }
        renderer->meshUploadBuffers[i] = NULL;
        renderer->meshUploadMapped[i] = NULL;
        renderer->meshUploadOffsets[i] = 0;
        renderer->instanceBuffers[i] = NULL;
        renderer->instanceMapped[i] = NULL;
        renderer->instanceOffsets[i] = 0;
    }

    if (renderer->blockTextureUpload != NULL)
        ID3D12Resource_Release(renderer->blockTextureUpload);
    if (renderer->blockTexture != NULL)
        ID3D12Resource_Release(renderer->blockTexture);
    if (renderer->blockNormalUpload != NULL)
        ID3D12Resource_Release(renderer->blockNormalUpload);
    if (renderer->blockNormalTexture != NULL)
        ID3D12Resource_Release(renderer->blockNormalTexture);
    renderer->blockTextureUpload = NULL;
    renderer->blockTexture = NULL;
    renderer->blockNormalUpload = NULL;
    renderer->blockNormalTexture = NULL;
    renderer->blockTextureUploadPending = false;

    if (renderer->cubeColor != NULL) ID3D12Resource_Release(renderer->cubeColor);
    if (renderer->cubeDepth != NULL) ID3D12Resource_Release(renderer->cubeDepth);
    if (renderer->cubeRtvHeap != NULL) ID3D12DescriptorHeap_Release(renderer->cubeRtvHeap);
    if (renderer->cubeDsvHeap != NULL) ID3D12DescriptorHeap_Release(renderer->cubeDsvHeap);
    renderer->cubeColor = NULL;
    renderer->cubeDepth = NULL;
    renderer->cubeRtvHeap = NULL;
    renderer->cubeDsvHeap = NULL;
    renderer->cubeResolution = 0;
    if (renderer->resolvePipelineState != NULL)
        ID3D12PipelineState_Release(renderer->resolvePipelineState);
    if (renderer->resolveRootSignature != NULL)
        ID3D12RootSignature_Release(renderer->resolveRootSignature);
    renderer->resolvePipelineState = NULL;
    renderer->resolveRootSignature = NULL;
    if (renderer->pipelineState != NULL)
        ID3D12PipelineState_Release(renderer->pipelineState);
    if (renderer->rootSignature != NULL)
        ID3D12RootSignature_Release(renderer->rootSignature);
    renderer->pipelineState = NULL;
    renderer->rootSignature = NULL;
    if (renderer->depthBuffer != NULL)
        ID3D12Resource_Release(renderer->depthBuffer);
    if (renderer->depthStencilViewHeap != NULL)
        ID3D12DescriptorHeap_Release(renderer->depthStencilViewHeap);
    renderer->depthBuffer = NULL;
    renderer->depthStencilViewHeap = NULL;
    renderer->worldReady = false;
}

bool RendererPrepareWorld(Renderer* renderer)
{
    if (renderer == NULL) return false;
    if (renderer->worldReady) return true;

    D3D12_DESCRIPTOR_HEAP_DESC cubeRtvHeapDescription = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = 6,
    };
    D3D12_DESCRIPTOR_HEAP_DESC cubeDsvHeapDescription = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
    };
    bool succeeded = SUCCEEDED(ID3D12Device_CreateDescriptorHeap(
        renderer->device, &cubeRtvHeapDescription,
        &IID_ID3D12DescriptorHeap, (void**)&renderer->cubeRtvHeap))
        && SUCCEEDED(ID3D12Device_CreateDescriptorHeap(renderer->device,
            &cubeDsvHeapDescription, &IID_ID3D12DescriptorHeap,
            (void**)&renderer->cubeDsvHeap))
        && CreateRootSignature(renderer)
        && CreatePipelineState(renderer)
        && CreateResolveRootSignature(renderer)
        && CreateResolvePipelineState(renderer)
        && CreateMeshUploadBuffers(renderer)
        && CreateBlockTexture(renderer)
        && CreateDepthBuffer(renderer, renderer->windowWidth,
            renderer->windowHeight);
    if (succeeded && renderer->wireframeEnabled)
        succeeded = RecreateChunkPipelineState(renderer);
    if (!succeeded)
    {
        RendererReleaseWorld(renderer);
        return false;
    }
    renderer->worldReady = true;
    return true;
}

bool RendererIsWorldReady(const Renderer* renderer)
{
    return renderer != NULL && renderer->worldReady;
}

void RendererDestroy(Renderer* renderer)
{
    if (renderer == NULL)
    {
        return;
    }

    if (renderer->swapChain != NULL)
    {
        IDXGISwapChain3_SetFullscreenState(renderer->swapChain, FALSE, NULL);
    }

    if (renderer->commandQueue != NULL && renderer->fence != NULL && renderer->fenceEvent != NULL)
    {
        WaitForGpu(renderer);
    }

    if (renderer->fence != NULL)
    {
        DrainDeferredReleases(renderer, true);
    }

    RendererReleaseWorld(renderer);

    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        if (renderer->pendingUploads[i].ownsStaging)
            ID3D12Resource_Release(renderer->pendingUploads[i].staging);
    }

    for (uint32_t i = 0; i < renderer->poolBlockCount; ++i)
    {
        if (renderer->poolBlocks[i].buffer != NULL) ID3D12Resource_Release(renderer->poolBlocks[i].buffer);
        if (renderer->poolBlocks[i].freeRanges != NULL) HeapFree(GetProcessHeap(), 0, renderer->poolBlocks[i].freeRanges);
    }

    if (renderer->blockTextureUpload != NULL) ID3D12Resource_Release(renderer->blockTextureUpload);
    if (renderer->blockTexture != NULL) ID3D12Resource_Release(renderer->blockTexture);
    if (renderer->blockNormalUpload != NULL) ID3D12Resource_Release(renderer->blockNormalUpload);
    if (renderer->blockNormalTexture != NULL) ID3D12Resource_Release(renderer->blockNormalTexture);
    if (renderer->srvHeap != NULL) ID3D12DescriptorHeap_Release(renderer->srvHeap);

    if (renderer->cubeColor != NULL) ID3D12Resource_Release(renderer->cubeColor);
    if (renderer->cubeDepth != NULL) ID3D12Resource_Release(renderer->cubeDepth);
    if (renderer->cubeRtvHeap != NULL) ID3D12DescriptorHeap_Release(renderer->cubeRtvHeap);
    if (renderer->cubeDsvHeap != NULL) ID3D12DescriptorHeap_Release(renderer->cubeDsvHeap);
    if (renderer->resolvePipelineState != NULL) ID3D12PipelineState_Release(renderer->resolvePipelineState);
    if (renderer->resolveRootSignature != NULL) ID3D12RootSignature_Release(renderer->resolveRootSignature);

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (renderer->uiQuadBuffers[i] != NULL)
        {
            ID3D12Resource_Release(renderer->uiQuadBuffers[i]);
        }
        if (renderer->meshUploadBuffers[i] != NULL)
        {
            if (renderer->meshUploadMapped[i] != NULL)
                ID3D12Resource_Unmap(renderer->meshUploadBuffers[i], 0, NULL);
            ID3D12Resource_Release(renderer->meshUploadBuffers[i]);
        }
        if (renderer->instanceBuffers[i] != NULL)
        {
            if (renderer->instanceMapped[i] != NULL)
                ID3D12Resource_Unmap(renderer->instanceBuffers[i], 0, NULL);
            ID3D12Resource_Release(renderer->instanceBuffers[i]);
        }
    }
    if (renderer->fontTextureUpload != NULL) ID3D12Resource_Release(renderer->fontTextureUpload);
    if (renderer->fontTexture != NULL) ID3D12Resource_Release(renderer->fontTexture);
    if (renderer->backgroundTextureUpload != NULL)
        ID3D12Resource_Release(renderer->backgroundTextureUpload);
    if (renderer->backgroundTexture != NULL)
        ID3D12Resource_Release(renderer->backgroundTexture);
    if (renderer->uiPipelineState != NULL) ID3D12PipelineState_Release(renderer->uiPipelineState);
    if (renderer->uiRootSignature != NULL) ID3D12RootSignature_Release(renderer->uiRootSignature);

    if (renderer->fenceEvent != NULL)
    {
        CloseHandle(renderer->fenceEvent);
    }
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (renderer->renderTargets[i] != NULL) ID3D12Resource_Release(renderer->renderTargets[i]);
        if (renderer->commandAllocators[i] != NULL) ID3D12CommandAllocator_Release(renderer->commandAllocators[i]);
    }

    if (renderer->pipelineState != NULL) ID3D12PipelineState_Release(renderer->pipelineState);
    if (renderer->rootSignature != NULL) ID3D12RootSignature_Release(renderer->rootSignature);
    if (renderer->depthBuffer != NULL) ID3D12Resource_Release(renderer->depthBuffer);
    if (renderer->depthStencilViewHeap != NULL) ID3D12DescriptorHeap_Release(renderer->depthStencilViewHeap);
    if (renderer->renderTargetViewHeap != NULL) ID3D12DescriptorHeap_Release(renderer->renderTargetViewHeap);
    if (renderer->commandList != NULL) ID3D12GraphicsCommandList_Release(renderer->commandList);
    if (renderer->fence != NULL) ID3D12Fence_Release(renderer->fence);
    if (renderer->swapChain != NULL) IDXGISwapChain3_Release(renderer->swapChain);
    if (renderer->commandQueue != NULL) ID3D12CommandQueue_Release(renderer->commandQueue);
    if (renderer->device != NULL) ID3D12Device_Release(renderer->device);
    if (renderer->factory != NULL) IDXGIFactory4_Release(renderer->factory);

    // Освобождаем загруженный байткод шейдеров (если не встроенный).
    if (renderer->loadedChunkVS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedChunkVS);
    if (renderer->loadedChunkPS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedChunkPS);
    if (renderer->loadedPanoramaVS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedPanoramaVS);
    if (renderer->loadedPanoramaPS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedPanoramaPS);
    if (renderer->loadedUIVS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedUIVS);
    if (renderer->loadedUIPS != NULL) HeapFree(GetProcessHeap(), 0, renderer->loadedUIPS);

    HeapFree(GetProcessHeap(), 0, renderer);
}

RendererMesh* RendererCreateMesh(Renderer* renderer, const ChunkQuad* quads, uint32_t quadCount)
{
    if (renderer == NULL || !renderer->worldReady || quads == NULL
        || quadCount == 0
        || quadCount > UINT32_MAX / (uint32_t)sizeof(ChunkQuad)
        || renderer->pendingUploadCount == MAX_PENDING_UPLOADS)
    {
        return NULL;
    }

    uint32_t sizeBytes = quadCount * (uint32_t)sizeof(ChunkQuad);

    uint32_t blockIndex;
    uint32_t offsetBytes;
    if (!PoolAllocate(renderer, sizeBytes, &blockIndex, &offsetBytes))
    {
        return NULL;
    }

    ID3D12Resource* staging = NULL;
    uint32_t sourceOffset = (renderer->meshUploadOffsets[renderer->frameIndex] + 15u) & ~15u;
    bool ownsStaging = sourceOffset > MESH_UPLOAD_BYTES_PER_FRAME
        || sizeBytes > MESH_UPLOAD_BYTES_PER_FRAME - sourceOffset;
    if (!ownsStaging)
    {
        staging = renderer->meshUploadBuffers[renderer->frameIndex];
        memcpy(renderer->meshUploadMapped[renderer->frameIndex] + sourceOffset,
            quads, sizeBytes);
        renderer->meshUploadOffsets[renderer->frameIndex] = sourceOffset + sizeBytes;
    }
    else
    {
        D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC description;
        memset(&description, 0, sizeof(description));
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = sizeBytes;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device,
            &heapProperties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&staging)))
        {
            PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, sizeBytes);
            return NULL;
        }
        D3D12_RANGE emptyRange = { 0, 0 };
        void* mapped = NULL;
        if (FAILED(ID3D12Resource_Map(staging, 0, &emptyRange, &mapped)))
        {
            ID3D12Resource_Release(staging);
            PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, sizeBytes);
            return NULL;
        }
        memcpy(mapped, quads, sizeBytes);
        ID3D12Resource_Unmap(staging, 0, NULL);
        sourceOffset = 0;
    }

    RendererMesh* mesh = HeapAlloc(GetProcessHeap(), 0, sizeof(*mesh));
    if (mesh == NULL)
    {
        if (ownsStaging) ID3D12Resource_Release(staging);
        else renderer->meshUploadOffsets[renderer->frameIndex] = sourceOffset;
        PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, sizeBytes);
        return NULL;
    }

    mesh->blockIndex = blockIndex;
    mesh->offsetBytes = offsetBytes;
    mesh->sizeBytes = sizeBytes;
    mesh->quadCount = quadCount;

    PendingUpload* upload = &renderer->pendingUploads[renderer->pendingUploadCount++];
    upload->staging = staging;
    upload->sourceOffset = sourceOffset;
    upload->blockIndex = blockIndex;
    upload->destinationOffset = offsetBytes;
    upload->sizeBytes = sizeBytes;
    upload->ownsStaging = ownsStaging;

    return mesh;
}

void RendererDestroyMesh(Renderer* renderer, RendererMesh* mesh)
{
    if (mesh == NULL)
    {
        return;
    }

    if (renderer->deferredRangeCount == DEFERRED_RELEASE_CAPACITY)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }

    uint32_t slot = (renderer->deferredRangeHead + renderer->deferredRangeCount) % DEFERRED_RELEASE_CAPACITY;
    renderer->deferredRanges[slot].blockIndex = mesh->blockIndex;
    renderer->deferredRanges[slot].offset = mesh->offsetBytes;
    renderer->deferredRanges[slot].size = mesh->sizeBytes;
    renderer->deferredRanges[slot].safeFenceValue = renderer->lastSignaledFenceValue + 1;
    renderer->deferredRangeCount++;

    HeapFree(GetProcessHeap(), 0, mesh);
}

void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh,
    const float chunkOriginRelative[3])
{
    GeometryPoolBlock* block = &renderer->poolBlocks[mesh->blockIndex];

    float transform[4] = {
        chunkOriginRelative[0], chunkOriginRelative[1],
        chunkOriginRelative[2], 1.0f,
    };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, 4, transform, ROOT_CONSTANT_ORIGIN_OFFSET);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(renderer->commandList,
        ROOT_PARAMETER_QUAD_BUFFER, block->address + mesh->offsetBytes);
    ID3D12GraphicsCommandList_DrawInstanced(renderer->commandList, mesh->quadCount * 6, 1, 0, 0);
    renderer->currentStats.drawCalls++;
    renderer->currentStats.drawnQuads += mesh->quadCount;
}

void RendererDrawMeshInstances(Renderer* renderer, const RendererMesh* mesh,
    const RendererMeshInstance* instances, uint32_t instanceCount)
{
    if (renderer == NULL || mesh == NULL || instances == NULL
        || instanceCount == 0
        || instanceCount > INSTANCE_BYTES_PER_FRAME
            / (uint32_t)sizeof(RendererMeshInstance))
        return;

    uint32_t bytes = instanceCount
        * (uint32_t)sizeof(RendererMeshInstance);
    uint32_t offset = (renderer->instanceOffsets[renderer->frameIndex] + 15U)
        & ~15U;
    if (offset > INSTANCE_BYTES_PER_FRAME
        || bytes > INSTANCE_BYTES_PER_FRAME - offset)
        return;
    memcpy(renderer->instanceMapped[renderer->frameIndex] + offset,
        instances, bytes);
    renderer->instanceOffsets[renderer->frameIndex] = offset + bytes;

    GeometryPoolBlock* block = &renderer->poolBlocks[mesh->blockIndex];
    float transform[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
        renderer->commandList, ROOT_PARAMETER_CONSTANTS, 4, transform,
        ROOT_CONSTANT_ORIGIN_OFFSET);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(
        renderer->commandList, ROOT_PARAMETER_QUAD_BUFFER,
        block->address + mesh->offsetBytes);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(
        renderer->commandList, ROOT_PARAMETER_INSTANCES,
        ID3D12Resource_GetGPUVirtualAddress(
            renderer->instanceBuffers[renderer->frameIndex]) + offset);
    ID3D12GraphicsCommandList_DrawInstanced(renderer->commandList,
        mesh->quadCount * 6, instanceCount, 0, 0);
    renderer->currentStats.drawCalls++;
    renderer->currentStats.drawnQuads +=
        (uint64_t)mesh->quadCount * instanceCount;
}

static void RecordTextureArrayUpload(Renderer* renderer,
    ID3D12Resource* texture, ID3D12Resource** upload)
{
    D3D12_RESOURCE_DESC description;
    ID3D12Resource_GetDesc(texture, &description);
    UINT subresourceCount = (UINT)description.DepthOrArraySize * description.MipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[MAX_TEXTURE_SUBRESOURCES];
    ID3D12Device_GetCopyableFootprints(renderer->device, &description,
        0, subresourceCount, 0, layouts, NULL, NULL, NULL);

    for (UINT index = 0; index < subresourceCount; ++index)
    {
        D3D12_TEXTURE_COPY_LOCATION destination;
        memset(&destination, 0, sizeof(destination));
        destination.pResource = texture;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = index;

        D3D12_TEXTURE_COPY_LOCATION source;
        memset(&source, 0, sizeof(source));
        source.pResource = *upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layouts[index];

        ID3D12GraphicsCommandList_CopyTextureRegion(renderer->commandList,
            &destination, 0, 0, 0, &source, NULL);
    }

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(texture,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    DeferResourceRelease(renderer, *upload);
    *upload = NULL;
}

static void RecordBlockTextureUpload(Renderer* renderer)
{
    if (!renderer->blockTextureUploadPending)
    {
        return;
    }

    RecordTextureArrayUpload(renderer,
        renderer->blockTexture, &renderer->blockTextureUpload);
    RecordTextureArrayUpload(renderer,
        renderer->blockNormalTexture, &renderer->blockNormalUpload);
    renderer->blockTextureUploadPending = false;
}

// Записывает отложенные загрузки мешей в командный список кадра.
static void RecordPendingUploads(Renderer* renderer)
{
    if (renderer->pendingUploadCount == 0)
    {
        return;
    }

    // Блоки с загрузками — в состояние приёмника копирования.
    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        GeometryPoolBlock* block = &renderer->poolBlocks[renderer->pendingUploads[i].blockIndex];
        if (!block->touchedByUploads)
        {
            block->touchedByUploads = true;
            D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(block->buffer,
                block->currentState, D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);
            block->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }

    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        PendingUpload* upload = &renderer->pendingUploads[i];
        GeometryPoolBlock* block = &renderer->poolBlocks[upload->blockIndex];
        ID3D12GraphicsCommandList_CopyBufferRegion(renderer->commandList,
            block->buffer, upload->destinationOffset, upload->staging,
            upload->sourceOffset, upload->sizeBytes);
        if (upload->ownsStaging) DeferResourceRelease(renderer, upload->staging);
        renderer->currentStats.uploadedBytes += upload->sizeBytes;
    }

    // Обратно в состояние чтения вершинным шейдером.
    for (uint32_t i = 0; i < renderer->poolBlockCount; ++i)
    {
        GeometryPoolBlock* block = &renderer->poolBlocks[i];
        if (block->touchedByUploads)
        {
            block->touchedByUploads = false;
            D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(block->buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);
            block->currentState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
    }

    renderer->pendingUploadCount = 0;
}

static bool ApplyPendingResize(Renderer* renderer)
{
    int32_t width = renderer->resizeWidth;
    int32_t height = renderer->resizeHeight;
    if (width <= 0 || height <= 0)
    {
        renderer->resizeRequested = false;
        return true;
    }

    WaitForGpu(renderer);

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (renderer->renderTargets[i] != NULL)
        {
            ID3D12Resource_Release(renderer->renderTargets[i]);
            renderer->renderTargets[i] = NULL;
        }
    }

    UINT swapChainFlags = renderer->tearingSupported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(IDXGISwapChain3_ResizeBuffers(renderer->swapChain, FRAME_COUNT, (UINT)width, (UINT)height,
        DXGI_FORMAT_R8G8B8A8_UNORM, swapChainFlags)))
    {
        return false; // resizeRequested остаётся — повтор в следующем кадре
    }

    // ResizeBuffers сбрасывает индекс back-буфера: чтобы подписи фенса
    // остались монотонными, все слоты выравниваются по текущему значению
    // (иначе ~каждый второй ресайз зависал бы навсегда).
    UINT64 currentFenceValue = renderer->fenceValues[renderer->frameIndex];
    renderer->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swapChain);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        renderer->fenceValues[i] = currentFenceValue;
    }

    D3D12_RENDER_TARGET_VIEW_DESC backBufferViewDescription;
    memset(&backBufferViewDescription, 0, sizeof(backBufferViewDescription));
    backBufferViewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    backBufferViewDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(renderer->swapChain, i, &IID_ID3D12Resource, (void**)&renderer->renderTargets[i])))
        {
            return false;
        }
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->renderTargets[i],
            &backBufferViewDescription, renderTargetViewHandle);
        renderTargetViewHandle.ptr += renderer->renderTargetViewSize;
    }

    if (renderer->worldReady
        && !CreateDepthBuffer(renderer, width, height))
    {
        return false;
    }

    renderer->windowWidth = width;
    renderer->windowHeight = height;
    renderer->viewport.Width = (float)width;
    renderer->viewport.Height = (float)height;
    renderer->scissorRect.right = width;
    renderer->scissorRect.bottom = height;
    renderer->resizeRequested = false;
    return true;
}

// Загрузка атласа шрифта: записана в командный список ближайшего кадра.
static void RecordFontAtlasUpload(Renderer* renderer)
{
    if (!renderer->fontUploadPending)
    {
        return;
    }

    D3D12_RESOURCE_DESC description;
    ID3D12Resource_GetDesc(renderer->fontTexture, &description);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    ID3D12Device_GetCopyableFootprints(renderer->device, &description,
        0, 1, 0, &layout, NULL, NULL, NULL);

    D3D12_TEXTURE_COPY_LOCATION destination;
    memset(&destination, 0, sizeof(destination));
    destination.pResource = renderer->fontTexture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION source;
    memset(&source, 0, sizeof(source));
    source.pResource = renderer->fontTextureUpload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = layout;

    ID3D12GraphicsCommandList_CopyTextureRegion(renderer->commandList,
        &destination, 0, 0, 0, &source, NULL);

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(renderer->fontTexture,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    DeferResourceRelease(renderer, renderer->fontTextureUpload);
    renderer->fontTextureUpload = NULL;
    renderer->fontUploadPending = false;
    renderer->fontReady = true;
}

static void RecordBackgroundUpload(Renderer* renderer)
{
    if (!renderer->backgroundUploadPending) return;

    D3D12_RESOURCE_DESC description;
    ID3D12Resource_GetDesc(renderer->backgroundTexture, &description);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    ID3D12Device_GetCopyableFootprints(renderer->device, &description,
        0, 1, 0, &layout, NULL, NULL, NULL);

    D3D12_TEXTURE_COPY_LOCATION destination;
    memset(&destination, 0, sizeof(destination));
    destination.pResource = renderer->backgroundTexture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source;
    memset(&source, 0, sizeof(source));
    source.pResource = renderer->backgroundTextureUpload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = layout;
    ID3D12GraphicsCommandList_CopyTextureRegion(renderer->commandList,
        &destination, 0, 0, 0, &source, NULL);

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(
        renderer->backgroundTexture, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1,
        &barrier);
    DeferResourceRelease(renderer, renderer->backgroundTextureUpload);
    renderer->backgroundTextureUpload = NULL;
    renderer->backgroundUploadPending = false;
    renderer->backgroundReady = true;
}

bool RendererBeginFrame(Renderer* renderer, const RendererFrameSetup* frame)
{
    if (renderer == NULL || frame == NULL
        || frame->passCount > RENDERER_MAX_SCENE_PASSES
        || (frame->passCount != 0 && !renderer->worldReady))
    {
        return false;
    }

    // Отложенный resize: применяется, когда GPU дошёл до этого кадра.
    // При неудаче кадр пропускается, попытка повторится в следующем.
    if (renderer->resizeRequested && !ApplyPendingResize(renderer))
    {
        return false;
    }
    if (renderer->renderTargets[renderer->frameIndex] == NULL
        || (frame->passCount != 0 && renderer->depthBuffer == NULL))
    {
        return false;
    }

    renderer->frame = *frame;
    renderer->uiQuadCount = 0;
    memset(&renderer->currentStats, 0, sizeof(renderer->currentStats));
    renderer->currentStats.scenePasses = frame->passCount;

    if (renderer->frame.panorama
        && !EnsureCubeResources(renderer, renderer->frame.faceResolution))
    {
        return false;
    }

    DrainDeferredReleases(renderer, false);

    ID3D12CommandAllocator_Reset(renderer->commandAllocators[renderer->frameIndex]);
    ID3D12GraphicsCommandList_Reset(renderer->commandList,
        renderer->commandAllocators[renderer->frameIndex],
        renderer->worldReady ? renderer->pipelineState : NULL);

    if (renderer->worldReady) RecordBlockTextureUpload(renderer);
    RecordFontAtlasUpload(renderer);
    RecordBackgroundUpload(renderer);
    RecordPendingUploads(renderer);

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(renderer->renderTargets[renderer->frameIndex],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    // Общее состояние всех проходов сцены; цель, очистка и viewProjection
    // назначаются в RendererBeginScenePass.
    ID3D12DescriptorHeap* descriptorHeaps[1] = { renderer->srvHeap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(renderer->commandList, 1, descriptorHeaps);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer->commandList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (frame->passCount == 0)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            renderer->renderTargetViewHeap, &renderTargetViewHandle);
        renderTargetViewHandle.ptr += (SIZE_T)renderer->frameIndex
            * renderer->renderTargetViewSize;
        ID3D12GraphicsCommandList_OMSetRenderTargets(renderer->commandList,
            1, &renderTargetViewHandle, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(renderer->commandList,
            1, &renderer->viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(renderer->commandList,
            1, &renderer->scissorRect);
        const float clearColor[4] = {
            frame->skyColor[0], frame->skyColor[1], frame->skyColor[2], 1.0f,
        };
        ID3D12GraphicsCommandList_ClearRenderTargetView(
            renderer->commandList, renderTargetViewHandle, clearColor, 0,
            NULL);
        return true;
    }

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer->commandList,
        renderer->rootSignature);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer->commandList,
        ROOT_PARAMETER_BLOCK_TEXTURES, SrvGpuHandle(renderer, SRV_SLOT_BLOCK_TEXTURES));

    // Свет кадра: float3 в cbuffer выровнены по 16 байт, между ними pad.
    float gammaInverse = frame->gamma > 0.01f ? 1.0f / frame->gamma : 1.0f;
    float lighting[ROOT_CONSTANT_LIGHTING_COUNT] = {
        frame->sunDirection[0], frame->sunDirection[1], frame->sunDirection[2], 0.0f,
        frame->sunColor[0], frame->sunColor[1], frame->sunColor[2], 0.0f,
        frame->ambientColor[0], frame->ambientColor[1], frame->ambientColor[2],
        gammaInverse,
    };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, ROOT_CONSTANT_LIGHTING_COUNT, lighting,
        ROOT_CONSTANT_LIGHTING_OFFSET);

    return true;
}

void RendererBeginScenePass(Renderer* renderer, uint32_t passIndex)
{
    if (renderer == NULL || !renderer->worldReady
        || passIndex >= renderer->frame.passCount)
    {
        return;
    }
    const RendererScenePass* pass = &renderer->frame.passes[passIndex];

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilViewHandle;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;

    if (renderer->frame.panorama)
    {
        // Грань кубмапы: рисуется и очищается только задействованный
        // прямоугольник — остальные тексели резолв не читает.
        uint32_t faceIndex = pass->faceIndex < 6 ? pass->faceIndex : 0;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            renderer->cubeRtvHeap, &renderTargetViewHandle);
        renderTargetViewHandle.ptr += (SIZE_T)faceIndex * renderer->renderTargetViewSize;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            renderer->cubeDsvHeap, &depthStencilViewHandle);

        scissor.left = (LONG)pass->rectMinX;
        scissor.top = (LONG)pass->rectMinY;
        scissor.right = (LONG)pass->rectMaxX;
        scissor.bottom = (LONG)pass->rectMaxY;
        viewport.TopLeftX = (float)scissor.left;
        viewport.TopLeftY = (float)scissor.top;
        viewport.Width = (float)(scissor.right - scissor.left);
        viewport.Height = (float)(scissor.bottom - scissor.top);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
    }
    else
    {
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            renderer->renderTargetViewHeap, &renderTargetViewHandle);
        renderTargetViewHandle.ptr += (SIZE_T)renderer->frameIndex * renderer->renderTargetViewSize;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            renderer->depthStencilViewHeap, &depthStencilViewHandle);
        viewport = renderer->viewport;
        scissor = renderer->scissorRect;
    }

    ID3D12GraphicsCommandList_OMSetRenderTargets(renderer->commandList,
        1, &renderTargetViewHandle, FALSE, &depthStencilViewHandle);

    const float clearColor[4] = {
        renderer->frame.skyColor[0],
        renderer->frame.skyColor[1],
        renderer->frame.skyColor[2],
        1.0f,
    };
    ID3D12GraphicsCommandList_ClearRenderTargetView(renderer->commandList,
        renderTargetViewHandle, clearColor, 1, &scissor);
    ID3D12GraphicsCommandList_ClearDepthStencilView(renderer->commandList,
        depthStencilViewHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &scissor);

    ID3D12GraphicsCommandList_RSSetViewports(renderer->commandList, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(renderer->commandList, 1, &scissor);

    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, 16, pass->viewProjection, 0);
}

bool RendererEndFrame(Renderer* renderer)
{
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
        renderer->renderTargetViewHeap, &renderTargetViewHandle);
    renderTargetViewHandle.ptr += (SIZE_T)renderer->frameIndex * renderer->renderTargetViewSize;

    if (renderer->frame.panorama)
    {
        // Резолв: кубмапа -> back-буфер выбранной проекцией.
        D3D12_RESOURCE_BARRIER toShaderResource = MakeTransitionBarrier(renderer->cubeColor,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &toShaderResource);

        ID3D12GraphicsCommandList_OMSetRenderTargets(renderer->commandList,
            1, &renderTargetViewHandle, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(renderer->commandList, 1, &renderer->viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(renderer->commandList, 1, &renderer->scissorRect);

        ID3D12GraphicsCommandList_SetPipelineState(renderer->commandList,
            renderer->resolvePipelineState);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer->commandList,
            renderer->resolveRootSignature);

        float resolveConstants[2] = {
            renderer->frame.fovHalfRadians,
            renderer->frame.resolveVerticalScale,
        };
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
            0, 2, resolveConstants, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(renderer->commandList,
            0, (UINT)renderer->frame.resolveMapping, 2);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer->commandList,
            1, SrvGpuHandle(renderer, SRV_SLOT_PANORAMA_CUBE));

        ID3D12GraphicsCommandList_DrawInstanced(renderer->commandList, 3, 1, 0, 0);
        renderer->currentStats.drawCalls++;

        D3D12_RESOURCE_BARRIER toRenderTarget = MakeTransitionBarrier(renderer->cubeColor,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &toRenderTarget);
    }

    if (renderer->uiQuadCount > 0 && renderer->fontReady)
    {
        // Слой UI: альфа-смешивание поверх кадра, без глубины.
        ID3D12GraphicsCommandList_OMSetRenderTargets(renderer->commandList,
            1, &renderTargetViewHandle, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(renderer->commandList, 1, &renderer->viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(renderer->commandList, 1, &renderer->scissorRect);

        ID3D12GraphicsCommandList_SetPipelineState(renderer->commandList,
            renderer->uiPipelineState);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer->commandList,
            renderer->uiRootSignature);

        float screenSize[2] = {
            (float)renderer->windowWidth,
            (float)renderer->windowHeight,
        };
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
            0, 2, screenSize, 0);
        ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(renderer->commandList,
            1, ID3D12Resource_GetGPUVirtualAddress(
                renderer->uiQuadBuffers[renderer->frameIndex]));
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(renderer->commandList,
            2, SrvGpuHandle(renderer, SRV_SLOT_FONT_ATLAS));

        ID3D12GraphicsCommandList_DrawInstanced(renderer->commandList,
            renderer->uiQuadCount * 6, 1, 0, 0);
        renderer->currentStats.drawCalls++;
    }

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(renderer->renderTargets[renderer->frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    ID3D12GraphicsCommandList_Close(renderer->commandList);

    ID3D12CommandList* commandLists[1] = { (ID3D12CommandList*)renderer->commandList };
    ID3D12CommandQueue_ExecuteCommandLists(renderer->commandQueue, 1, commandLists);

    UINT syncInterval = renderer->verticalSyncEnabled ? 1 : 0;
    UINT presentFlags = 0;
    if (!renderer->verticalSyncEnabled && renderer->tearingPresentEnabled)
    {
        BOOL fullscreen = TRUE;
        if (SUCCEEDED(IDXGISwapChain3_GetFullscreenState(
                renderer->swapChain, &fullscreen, NULL)) && !fullscreen)
        {
            presentFlags = DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    HRESULT presentResult = IDXGISwapChain3_Present(
        renderer->swapChain, syncInterval, presentFlags);
    if (presentResult == DXGI_ERROR_INVALID_CALL
        && presentFlags == DXGI_PRESENT_ALLOW_TEARING)
    {
        renderer->tearingPresentEnabled = false;
        presentResult = IDXGISwapChain3_Present(renderer->swapChain, 0, 0);
    }
    if (FAILED(presentResult))
    {
        return false;
    }

    renderer->lastStats = renderer->currentStats;

    bool waitedForFence = MoveToNextFrame(renderer);
    renderer->meshUploadOffsets[renderer->frameIndex] = 0;
    renderer->instanceOffsets[renderer->frameIndex] = 0;
    if (!renderer->verticalSyncEnabled && !waitedForFence)
    {
        // Без vsync Present не является точкой ожидания. Отдаём остаток
        // кванта рабочим потокам, не вводя задержку или предел FPS.
        SwitchToThread();
    }
    return true;
}

void RendererGetStats(const Renderer* renderer, RendererStats* outStats)
{
    if (renderer == NULL || outStats == NULL) return;

    *outStats = renderer->lastStats;
    uint64_t capacity = 0;
    uint64_t freeBytes = 0;
    for (uint32_t blockIndex = 0;
        blockIndex < renderer->poolBlockCount; ++blockIndex)
    {
        const GeometryPoolBlock* block = &renderer->poolBlocks[blockIndex];
        capacity += block->totalBytes;
        for (uint32_t rangeIndex = 0;
            rangeIndex < block->freeRangeCount; ++rangeIndex)
        {
            freeBytes += block->freeRanges[rangeIndex].size;
        }
    }
    outStats->geometryPoolCapacityBytes = capacity;
    outStats->geometryPoolUsedBytes = capacity - freeBytes;
}

void RendererSetVerticalSync(Renderer* renderer, bool enabled)
{
    renderer->verticalSyncEnabled = enabled;
}

bool RendererIsVerticalSyncEnabled(const Renderer* renderer)
{
    return renderer->verticalSyncEnabled;
}

void RendererResize(Renderer* renderer, int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    renderer->resizeWidth = width;
    renderer->resizeHeight = height;
    renderer->resizeRequested = true;
}

static void ReleaseLoadedShader(void** loadedShader)
{
    if (*loadedShader != NULL)
    {
        HeapFree(GetProcessHeap(), 0, *loadedShader);
        *loadedShader = NULL;
    }
}

static void* CopyShaderBytecode(const void* bytecode, uint32_t length)
{
    if (bytecode == NULL || length == 0)
    {
        return NULL;
    }
    void* copy = HeapAlloc(GetProcessHeap(), 0, length);
    if (copy != NULL)
    {
        memcpy(copy, bytecode, length);
    }
    return copy;
}

static bool RecreateChunkPipelineState(Renderer* renderer)
{
    if (renderer->pipelineState != NULL)
    {
        ID3D12PipelineState_Release(renderer->pipelineState);
        renderer->pipelineState = NULL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->rootSignature;
    description.VS.pShaderBytecode = renderer->loadedChunkVS != NULL
        ? renderer->loadedChunkVS : (const void*)g_chunk_vs;
    description.VS.BytecodeLength = renderer->loadedChunkVS != NULL
        ? renderer->loadedChunkVSLength : sizeof(g_chunk_vs);
    description.PS.pShaderBytecode = renderer->loadedChunkPS != NULL
        ? renderer->loadedChunkPS : (const void*)g_chunk_ps;
    description.PS.BytecodeLength = renderer->loadedChunkPS != NULL
        ? renderer->loadedChunkPSLength : sizeof(g_chunk_ps);
    description.RasterizerState.FillMode = renderer->wireframeEnabled
        ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    description.RasterizerState.FrontCounterClockwise = FALSE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->pipelineState));
}

static bool RecreateResolvePipelineState(Renderer* renderer)
{
    if (renderer->resolvePipelineState != NULL)
    {
        ID3D12PipelineState_Release(renderer->resolvePipelineState);
        renderer->resolvePipelineState = NULL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->resolveRootSignature;
    description.VS.pShaderBytecode = renderer->loadedPanoramaVS != NULL
        ? renderer->loadedPanoramaVS : (const void*)g_panorama_vs;
    description.VS.BytecodeLength = renderer->loadedPanoramaVS != NULL
        ? renderer->loadedPanoramaVSLength : sizeof(g_panorama_vs);
    description.PS.pShaderBytecode = renderer->loadedPanoramaPS != NULL
        ? renderer->loadedPanoramaPS : (const void*)g_panorama_ps;
    description.PS.BytecodeLength = renderer->loadedPanoramaPS != NULL
        ? renderer->loadedPanoramaPSLength : sizeof(g_panorama_ps);
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->resolvePipelineState));
}

static bool RecreateUiPipelineState(Renderer* renderer)
{
    if (renderer->uiPipelineState != NULL)
    {
        ID3D12PipelineState_Release(renderer->uiPipelineState);
        renderer->uiPipelineState = NULL;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->uiRootSignature;
    description.VS.pShaderBytecode = renderer->loadedUIVS != NULL
        ? renderer->loadedUIVS : (const void*)g_ui_vs;
    description.VS.BytecodeLength = renderer->loadedUIVS != NULL
        ? renderer->loadedUIVSLength : sizeof(g_ui_vs);
    description.PS.pShaderBytecode = renderer->loadedUIPS != NULL
        ? renderer->loadedUIPS : (const void*)g_ui_ps;
    description.PS.BytecodeLength = renderer->loadedUIPS != NULL
        ? renderer->loadedUIPSLength : sizeof(g_ui_ps);
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;

    D3D12_RENDER_TARGET_BLEND_DESC* blend = &description.BlendState.RenderTarget[0];
    blend->BlendEnable = TRUE;
    blend->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend->BlendOp = D3D12_BLEND_OP_ADD;
    blend->SrcBlendAlpha = D3D12_BLEND_ONE;
    blend->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    description.SampleMask = 0xFFFFFFFF;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->uiPipelineState));
}

bool RendererReloadTexturePack(Renderer* renderer)
{
    if (renderer == NULL)
    {
        return false;
    }

    if (!renderer->worldReady)
    {
        renderer->texturePackLoadStatus = RENDERER_CONTENT_NOT_ATTEMPTED;
        return true;
    }

    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);

    // Уничтожаем старые текстурные ресурсы.
    if (renderer->blockTextureUpload != NULL)
    {
        ID3D12Resource_Release(renderer->blockTextureUpload);
        renderer->blockTextureUpload = NULL;
    }
    if (renderer->blockTexture != NULL)
    {
        ID3D12Resource_Release(renderer->blockTexture);
        renderer->blockTexture = NULL;
    }
    if (renderer->blockNormalUpload != NULL)
    {
        ID3D12Resource_Release(renderer->blockNormalUpload);
        renderer->blockNormalUpload = NULL;
    }
    if (renderer->blockNormalTexture != NULL)
    {
        ID3D12Resource_Release(renderer->blockNormalTexture);
        renderer->blockNormalTexture = NULL;
    }
    renderer->blockTextureUploadPending = false;

    return CreateBlockTexture(renderer);
}

RendererContentStatus RendererGetTexturePackLoadStatus(
    const Renderer* renderer)
{
    return renderer != NULL ? renderer->texturePackLoadStatus
        : RENDERER_CONTENT_NOT_ATTEMPTED;
}

void RendererSetWireframe(Renderer* renderer, bool enabled)
{
    if (renderer == NULL || renderer->wireframeEnabled == enabled)
    {
        return;
    }
    renderer->wireframeEnabled = enabled;

    if (!renderer->worldReady) return;

    WaitForGpu(renderer);
    RecreateChunkPipelineState(renderer);
}

bool RendererIsWireframe(const Renderer* renderer)
{
    return renderer != NULL && renderer->wireframeEnabled;
}

bool RendererReloadShaders(Renderer* renderer,
    const void* chunkVS, uint32_t chunkVSLength,
    const void* chunkPS, uint32_t chunkPSLength,
    const void* panoramaVS, uint32_t panoramaVSLength,
    const void* panoramaPS, uint32_t panoramaPSLength,
    const void* uiVS, uint32_t uiVSLength,
    const void* uiPS, uint32_t uiPSLength)
{
    if (renderer == NULL)
    {
        return false;
    }

    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);

    // Освобождаем старый байткод и сохраняем новый.
    ReleaseLoadedShader(&renderer->loadedChunkVS);
    ReleaseLoadedShader(&renderer->loadedChunkPS);
    ReleaseLoadedShader(&renderer->loadedPanoramaVS);
    ReleaseLoadedShader(&renderer->loadedPanoramaPS);
    ReleaseLoadedShader(&renderer->loadedUIVS);
    ReleaseLoadedShader(&renderer->loadedUIPS);

    renderer->loadedChunkVS = CopyShaderBytecode(chunkVS, chunkVSLength);
    renderer->loadedChunkVSLength = chunkVSLength;
    renderer->loadedChunkPS = CopyShaderBytecode(chunkPS, chunkPSLength);
    renderer->loadedChunkPSLength = chunkPSLength;
    renderer->loadedPanoramaVS = CopyShaderBytecode(panoramaVS, panoramaVSLength);
    renderer->loadedPanoramaVSLength = panoramaVSLength;
    renderer->loadedPanoramaPS = CopyShaderBytecode(panoramaPS, panoramaPSLength);
    renderer->loadedPanoramaPSLength = panoramaPSLength;
    renderer->loadedUIVS = CopyShaderBytecode(uiVS, uiVSLength);
    renderer->loadedUIVSLength = uiVSLength;
    renderer->loadedUIPS = CopyShaderBytecode(uiPS, uiPSLength);
    renderer->loadedUIPSLength = uiPSLength;

    // Пересоздаём все PSO.
    if (renderer->worldReady
        && !RecreateChunkPipelineState(renderer)) return false;
    if (renderer->worldReady
        && !RecreateResolvePipelineState(renderer)) return false;
    if (!RecreateUiPipelineState(renderer)) return false;

    return true;
}

// === Слой интерфейса ===

_Static_assert(sizeof(RendererUiQuad) == UI_QUAD_BYTES,
    "RendererUiQuad is part of the GPU format (shaders/ui.hlsl)");

bool RendererUiSetFontAtlas(Renderer* renderer,
    const uint8_t* alphaPixels, uint32_t width, uint32_t height)
{
    if (alphaPixels == NULL || width == 0 || height == 0)
    {
        return false;
    }

    // Замена атласа — редкое событие (смена масштаба интерфейса):
    // честно дожидаемся GPU перед заменой текстуры и дескриптора.
    // Вызывать вне пары BeginFrame/EndFrame.
    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);

    if (renderer->fontTextureUpload != NULL)
    {
        ID3D12Resource_Release(renderer->fontTextureUpload);
        renderer->fontTextureUpload = NULL;
    }
    if (renderer->fontTexture != NULL)
    {
        ID3D12Resource_Release(renderer->fontTexture);
        renderer->fontTexture = NULL;
    }
    renderer->fontUploadPending = false;
    renderer->fontReady = false;

    D3D12_RESOURCE_DESC textureDescription;
    memset(&textureDescription, 0, sizeof(textureDescription));
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.DepthOrArraySize = 1;
    textureDescription.MipLevels = 1;
    textureDescription.Format = DXGI_FORMAT_R8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &defaultHeap,
        D3D12_HEAP_FLAG_NONE, &textureDescription, D3D12_RESOURCE_STATE_COPY_DEST,
        NULL, &IID_ID3D12Resource, (void**)&renderer->fontTexture)))
    {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    memset(&layout, 0, sizeof(layout));
    UINT64 uploadBytes = 0;
    ID3D12Device_GetCopyableFootprints(renderer->device, &textureDescription,
        0, 1, 0, &layout, NULL, NULL, &uploadBytes);

    D3D12_RESOURCE_DESC uploadDescription;
    memset(&uploadDescription, 0, sizeof(uploadDescription));
    uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDescription.Width = uploadBytes;
    uploadDescription.Height = 1;
    uploadDescription.DepthOrArraySize = 1;
    uploadDescription.MipLevels = 1;
    uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDescription.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES uploadHeap = { .Type = D3D12_HEAP_TYPE_UPLOAD };
    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &uploadHeap,
        D3D12_HEAP_FLAG_NONE, &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL, &IID_ID3D12Resource, (void**)&renderer->fontTextureUpload)))
    {
        ID3D12Resource_Release(renderer->fontTexture);
        renderer->fontTexture = NULL;
        return false;
    }

    D3D12_RANGE emptyRange = { 0, 0 };
    unsigned char* mapped = NULL;
    if (FAILED(ID3D12Resource_Map(renderer->fontTextureUpload, 0,
        &emptyRange, (void**)&mapped)))
    {
        ID3D12Resource_Release(renderer->fontTextureUpload);
        renderer->fontTextureUpload = NULL;
        ID3D12Resource_Release(renderer->fontTexture);
        renderer->fontTexture = NULL;
        return false;
    }

    for (uint32_t row = 0; row < height; ++row)
    {
        memcpy(mapped + layout.Offset + (size_t)row * layout.Footprint.RowPitch,
            alphaPixels + (size_t)row * width, width);
    }
    ID3D12Resource_Unmap(renderer->fontTextureUpload, 0, NULL);

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription;
    memset(&viewDescription, 0, sizeof(viewDescription));
    viewDescription.Format = DXGI_FORMAT_R8_UNORM;
    viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDescription.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(renderer->device, renderer->fontTexture,
        &viewDescription, SrvCpuHandle(renderer, SRV_SLOT_FONT_ATLAS));

    renderer->fontUploadPending = true;
    return true;
}

bool RendererUiLoadBackground(Renderer* renderer, const wchar_t* path,
    uint32_t* outWidth, uint32_t* outHeight)
{
    if (renderer == NULL || path == NULL) return false;

    uint8_t* pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!UiImageLoadRgba(path, &pixels, &width, &height)) return false;

    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);
    if (renderer->backgroundTextureUpload != NULL)
    {
        ID3D12Resource_Release(renderer->backgroundTextureUpload);
        renderer->backgroundTextureUpload = NULL;
    }
    if (renderer->backgroundTexture != NULL)
    {
        ID3D12Resource_Release(renderer->backgroundTexture);
        renderer->backgroundTexture = NULL;
    }
    renderer->backgroundUploadPending = false;
    renderer->backgroundReady = false;

    D3D12_RESOURCE_DESC textureDescription;
    memset(&textureDescription, 0, sizeof(textureDescription));
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.DepthOrArraySize = 1;
    textureDescription.MipLevels = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defaultHeap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    bool succeeded = SUCCEEDED(ID3D12Device_CreateCommittedResource(
        renderer->device, &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &textureDescription, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
        &IID_ID3D12Resource, (void**)&renderer->backgroundTexture));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    memset(&layout, 0, sizeof(layout));
    UINT64 uploadBytes = 0;
    if (succeeded)
    {
        ID3D12Device_GetCopyableFootprints(renderer->device,
            &textureDescription, 0, 1, 0, &layout, NULL, NULL,
            &uploadBytes);
        D3D12_RESOURCE_DESC uploadDescription;
        memset(&uploadDescription, 0, sizeof(uploadDescription));
        uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDescription.Width = uploadBytes;
        uploadDescription.Height = 1;
        uploadDescription.DepthOrArraySize = 1;
        uploadDescription.MipLevels = 1;
        uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDescription.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES uploadHeap = {
            .Type = D3D12_HEAP_TYPE_UPLOAD,
        };
        succeeded = SUCCEEDED(ID3D12Device_CreateCommittedResource(
            renderer->device, &uploadHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDescription, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource,
            (void**)&renderer->backgroundTextureUpload));
    }

    if (succeeded)
    {
        D3D12_RANGE emptyRange = { 0, 0 };
        uint8_t* mapped = NULL;
        succeeded = SUCCEEDED(ID3D12Resource_Map(
            renderer->backgroundTextureUpload, 0, &emptyRange,
            (void**)&mapped));
        if (succeeded)
        {
            uint32_t sourceStride = width * 4U;
            for (uint32_t row = 0; row < height; ++row)
            {
                memcpy(mapped + layout.Offset
                        + (size_t)row * layout.Footprint.RowPitch,
                    pixels + (size_t)row * sourceStride, sourceStride);
            }
            ID3D12Resource_Unmap(renderer->backgroundTextureUpload, 0,
                NULL);
        }
    }
    HeapFree(GetProcessHeap(), 0, pixels);

    if (!succeeded)
    {
        if (renderer->backgroundTextureUpload != NULL)
        {
            ID3D12Resource_Release(renderer->backgroundTextureUpload);
            renderer->backgroundTextureUpload = NULL;
        }
        if (renderer->backgroundTexture != NULL)
        {
            ID3D12Resource_Release(renderer->backgroundTexture);
            renderer->backgroundTexture = NULL;
        }
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription;
    memset(&viewDescription, 0, sizeof(viewDescription));
    viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDescription.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(renderer->device,
        renderer->backgroundTexture, &viewDescription,
        SrvCpuHandle(renderer, SRV_SLOT_UI_BACKGROUND));
    renderer->backgroundUploadPending = true;
    if (outWidth != NULL) *outWidth = width;
    if (outHeight != NULL) *outHeight = height;
    return true;
}

void RendererUiQueue(Renderer* renderer, const RendererUiQuad* quads, uint32_t count)
{
    if (quads == NULL || count == 0)
    {
        return;
    }

    uint32_t space = RENDERER_UI_MAX_QUADS - renderer->uiQuadCount;
    if (count > space)
    {
        count = space;
    }
    if (count == 0)
    {
        return;
    }

    memcpy(renderer->uiQuadMapped[renderer->frameIndex]
            + (size_t)renderer->uiQuadCount * UI_QUAD_BYTES,
        quads, (size_t)count * UI_QUAD_BYTES);
    renderer->uiQuadCount += count;
}
