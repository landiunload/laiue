#include "render/renderer.h"

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "render/generated/chunk_vs.h"
#include "render/generated/chunk_ps.h"

#include <stddef.h>
#include <string.h>

#define FRAME_COUNT 2

// Корневые константы (раскладка совпадает с cbuffer в chunk.hlsl):
// dword 0..15 — view-projection, 16..18 — смещение чанка относительно
// камеры, 20..22 — низшие 32 бита мировых координат угла чанка.
#define ROOT_CONSTANT_COUNT 23
#define ROOT_CONSTANT_ORIGIN_OFFSET 16
#define ROOT_CONSTANT_CHUNK_BASE_OFFSET 20
#define ROOT_PARAMETER_CONSTANTS 0
#define ROOT_PARAMETER_QUAD_BUFFER 1

// Пул геометрии: меши суб-аллоцируются из больших DEFAULT-буферов —
// без 64-КиБ выравнивания на каждый меш и без чтения через PCIe.
#define POOL_BLOCK_BYTES (4u * 1024u * 1024u)
#define MAX_POOL_BLOCKS 64

// Очереди отложенного освобождения: ресурс или диапазон пула
// уничтожается, только когда GPU прошёл кадры, которые могли его читать.
#define DEFERRED_RELEASE_CAPACITY 256
#define MAX_PENDING_UPLOADS 64

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
    uint32_t blockIndex;
    uint32_t destinationOffset;
    uint32_t sizeBytes;
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
    D3D12_VIEWPORT             viewport;
    D3D12_RECT                 scissorRect;
    int32_t                    windowWidth;
    int32_t                    windowHeight;
    int32_t                    resizeWidth;
    int32_t                    resizeHeight;
    bool                       resizeRequested;

    GeometryPoolBlock          poolBlocks[MAX_POOL_BLOCKS];
    uint32_t                   poolBlockCount;

    PendingUpload              pendingUploads[MAX_PENDING_UPLOADS];
    uint32_t                   pendingUploadCount;

    DeferredResourceRelease    deferredResources[DEFERRED_RELEASE_CAPACITY];
    uint32_t                   deferredResourceHead;
    uint32_t                   deferredResourceCount;

    DeferredRangeRelease       deferredRanges[DEFERRED_RELEASE_CAPACITY];
    uint32_t                   deferredRangeHead;
    uint32_t                   deferredRangeCount;
};

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

static void MoveToNextFrame(Renderer* renderer)
{
    UINT64 currentValue = renderer->fenceValues[renderer->frameIndex];
    ID3D12CommandQueue_Signal(renderer->commandQueue, renderer->fence, currentValue);
    renderer->lastSignaledFenceValue = currentValue;

    renderer->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swapChain);

    if (ID3D12Fence_GetCompletedValue(renderer->fence) < renderer->fenceValues[renderer->frameIndex])
    {
        ID3D12Fence_SetEventOnCompletion(renderer->fence, renderer->fenceValues[renderer->frameIndex], renderer->fenceEvent);
        WaitForSingleObject(renderer->fenceEvent, INFINITE);
    }

    renderer->fenceValues[renderer->frameIndex] = currentValue + 1;
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

static bool CreateRootSignature(Renderer* renderer)
{
    D3D12_ROOT_PARAMETER parameters[2];
    memset(parameters, 0, sizeof(parameters));
    parameters[ROOT_PARAMETER_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ROOT_PARAMETER_CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ROOT_PARAMETER_CONSTANTS].Constants.ShaderRegister = 0;
    parameters[ROOT_PARAMETER_CONSTANTS].Constants.Num32BitValues = ROOT_CONSTANT_COUNT;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[ROOT_PARAMETER_QUAD_BUFFER].Descriptor.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC description;
    memset(&description, 0, sizeof(description));
    description.NumParameters = 2;
    description.pParameters = parameters;

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
    description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(renderer->device, &description,
        &IID_ID3D12PipelineState, (void**)&renderer->pipelineState));
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

    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&renderer->factory)))
    {
        HeapFree(GetProcessHeap(), 0, renderer);
        return NULL;
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

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(renderer->swapChain, i, &IID_ID3D12Resource, (void**)&renderer->renderTargets[i])))
        {
            RendererDestroy(renderer);
            return NULL;
        }
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->renderTargets[i], NULL, renderTargetViewHandle);
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

    if (!CreateRootSignature(renderer)) { RendererDestroy(renderer); return NULL; }
    if (!CreatePipelineState(renderer)) { RendererDestroy(renderer); return NULL; }
    if (!CreateDepthBuffer(renderer, width, height)) { RendererDestroy(renderer); return NULL; }

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

    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        ID3D12Resource_Release(renderer->pendingUploads[i].staging);
    }

    for (uint32_t i = 0; i < renderer->poolBlockCount; ++i)
    {
        if (renderer->poolBlocks[i].buffer != NULL) ID3D12Resource_Release(renderer->poolBlocks[i].buffer);
        if (renderer->poolBlocks[i].freeRanges != NULL) HeapFree(GetProcessHeap(), 0, renderer->poolBlocks[i].freeRanges);
    }

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

    HeapFree(GetProcessHeap(), 0, renderer);
}

RendererMesh* RendererCreateMesh(Renderer* renderer, const ChunkQuad* quads, uint32_t quadCount)
{
    if (quadCount == 0 || renderer->pendingUploadCount == MAX_PENDING_UPLOADS)
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

    // Временный staging-буфер: живёт до исполнения копирования GPU,
    // освобождается отложенно.
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

    ID3D12Resource* staging = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&staging)))
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

    RendererMesh* mesh = HeapAlloc(GetProcessHeap(), 0, sizeof(*mesh));
    if (mesh == NULL)
    {
        ID3D12Resource_Release(staging);
        PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, sizeBytes);
        return NULL;
    }

    mesh->blockIndex = blockIndex;
    mesh->offsetBytes = offsetBytes;
    mesh->sizeBytes = sizeBytes;
    mesh->quadCount = quadCount;

    PendingUpload* upload = &renderer->pendingUploads[renderer->pendingUploadCount++];
    upload->staging = staging;
    upload->blockIndex = blockIndex;
    upload->destinationOffset = offsetBytes;
    upload->sizeBytes = sizeBytes;

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
    const float chunkOriginRelative[3], const uint32_t chunkBaseLow[3])
{
    GeometryPoolBlock* block = &renderer->poolBlocks[mesh->blockIndex];

    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, 3, chunkOriginRelative, ROOT_CONSTANT_ORIGIN_OFFSET);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, 3, chunkBaseLow, ROOT_CONSTANT_CHUNK_BASE_OFFSET);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(renderer->commandList,
        ROOT_PARAMETER_QUAD_BUFFER, block->address + mesh->offsetBytes);
    ID3D12GraphicsCommandList_DrawInstanced(renderer->commandList, mesh->quadCount * 6, 1, 0, 0);
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
            block->buffer, upload->destinationOffset, upload->staging, 0, upload->sizeBytes);
        DeferResourceRelease(renderer, upload->staging);
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

    if (FAILED(IDXGISwapChain3_ResizeBuffers(renderer->swapChain, FRAME_COUNT, (UINT)width, (UINT)height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0)))
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

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(renderer->swapChain, i, &IID_ID3D12Resource, (void**)&renderer->renderTargets[i])))
        {
            return false;
        }
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->renderTargets[i], NULL, renderTargetViewHandle);
        renderTargetViewHandle.ptr += renderer->renderTargetViewSize;
    }

    if (!CreateDepthBuffer(renderer, width, height))
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

bool RendererBeginFrame(Renderer* renderer, const float viewProjection[16])
{
    // Отложенный resize: применяется, когда GPU дошёл до этого кадра.
    // При неудаче кадр пропускается, попытка повторится в следующем.
    if (renderer->resizeRequested && !ApplyPendingResize(renderer))
    {
        return false;
    }
    if (renderer->renderTargets[renderer->frameIndex] == NULL || renderer->depthBuffer == NULL)
    {
        return false;
    }

    DrainDeferredReleases(renderer, false);

    ID3D12CommandAllocator_Reset(renderer->commandAllocators[renderer->frameIndex]);
    ID3D12GraphicsCommandList_Reset(renderer->commandList, renderer->commandAllocators[renderer->frameIndex], renderer->pipelineState);

    RecordPendingUploads(renderer);

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(renderer->renderTargets[renderer->frameIndex],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    renderTargetViewHandle.ptr += (SIZE_T)renderer->frameIndex * renderer->renderTargetViewSize;

    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->depthStencilViewHeap, &depthStencilViewHandle);

    ID3D12GraphicsCommandList_OMSetRenderTargets(renderer->commandList, 1, &renderTargetViewHandle, FALSE, &depthStencilViewHandle);

    const float clearColor[4] = { 0.4f, 0.6f, 0.9f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(renderer->commandList, renderTargetViewHandle, clearColor, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(renderer->commandList, depthStencilViewHandle,
        D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

    ID3D12GraphicsCommandList_RSSetViewports(renderer->commandList, 1, &renderer->viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(renderer->commandList, 1, &renderer->scissorRect);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(renderer->commandList, renderer->rootSignature);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        ROOT_PARAMETER_CONSTANTS, 16, viewProjection, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer->commandList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return true;
}

void RendererEndFrame(Renderer* renderer)
{
    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(renderer->renderTargets[renderer->frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_ResourceBarrier(renderer->commandList, 1, &barrier);

    ID3D12GraphicsCommandList_Close(renderer->commandList);

    ID3D12CommandList* commandLists[1] = { (ID3D12CommandList*)renderer->commandList };
    ID3D12CommandQueue_ExecuteCommandLists(renderer->commandQueue, 1, commandLists);

    IDXGISwapChain3_Present(renderer->swapChain, 1, 0);
    MoveToNextFrame(renderer);
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
