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

// Корневые константы: 16 значений матрицы view-projection + 3 значения
// смещения чанка (устанавливается на каждый draw).
#define ROOT_CONSTANT_COUNT 20
#define ROOT_CONSTANT_CHUNK_ORIGIN_OFFSET 16

// Очередь отложенного освобождения: ресурс уничтожается, только когда
// GPU прошёл кадры, которые могли его читать.
#define DEFERRED_RELEASE_CAPACITY 256

typedef struct DeferredRelease
{
    ID3D12Resource* resource;
    UINT64 safeFenceValue;
} DeferredRelease;

struct RendererMesh
{
    ID3D12Resource*          buffer;
    D3D12_VERTEX_BUFFER_VIEW vertexView;
    D3D12_INDEX_BUFFER_VIEW  indexView;
    uint32_t                 indexCount;
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
    float                      viewProjection[16];
    D3D12_VIEWPORT             viewport;
    D3D12_RECT                 scissorRect;
    int32_t                    windowWidth;
    int32_t                    windowHeight;
    int32_t                    resizeWidth;
    int32_t                    resizeHeight;
    bool                       resizeRequested;

    DeferredRelease            deferredReleases[DEFERRED_RELEASE_CAPACITY];
    uint32_t                   deferredReleaseHead;
    uint32_t                   deferredReleaseCount;
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

// Освобождает ресурсы, кадры которых GPU уже прошёл.
static void DrainDeferredReleases(Renderer* renderer, bool releaseEverything)
{
    UINT64 completedValue = ID3D12Fence_GetCompletedValue(renderer->fence);

    while (renderer->deferredReleaseCount > 0)
    {
        DeferredRelease* entry = &renderer->deferredReleases[renderer->deferredReleaseHead];
        if (!releaseEverything && entry->safeFenceValue > completedValue)
        {
            break;
        }

        ID3D12Resource_Release(entry->resource);
        renderer->deferredReleaseHead = (renderer->deferredReleaseHead + 1) % DEFERRED_RELEASE_CAPACITY;
        renderer->deferredReleaseCount--;
    }
}

static bool CreateRootSignature(Renderer* renderer)
{
    D3D12_ROOT_PARAMETER parameter;
    memset(&parameter, 0, sizeof(parameter));
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.Num32BitValues = ROOT_CONSTANT_COUNT;

    D3D12_ROOT_SIGNATURE_DESC description;
    memset(&description, 0, sizeof(description));
    description.NumParameters = 1;
    description.pParameters = &parameter;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
    D3D12_INPUT_ELEMENT_DESC elements[1];
    memset(elements, 0, sizeof(elements));
    elements[0].SemanticName = "DATA";
    elements[0].Format = DXGI_FORMAT_R32_UINT;
    elements[0].AlignedByteOffset = 0;
    elements[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description;
    memset(&description, 0, sizeof(description));
    description.pRootSignature = renderer->rootSignature;
    description.VS.pShaderBytecode = g_chunk_vs;
    description.VS.BytecodeLength = sizeof(g_chunk_vs);
    description.PS.pShaderBytecode = g_chunk_ps;
    description.PS.BytecodeLength = sizeof(g_chunk_ps);
    description.InputLayout.pInputElementDescs = elements;
    description.InputLayout.NumElements = 1;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
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

    renderer->viewProjection[0] = renderer->viewProjection[5] = 1.0f;
    renderer->viewProjection[10] = renderer->viewProjection[15] = 1.0f;

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

void RendererSetViewProjection(Renderer* renderer, const float matrix[16])
{
    for (int32_t i = 0; i < 16; ++i)
    {
        renderer->viewProjection[i] = matrix[i];
    }
}

RendererMesh* RendererCreateMesh(Renderer* renderer,
    const ChunkVertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount)
{
    if (vertexCount == 0 || indexCount == 0)
    {
        return NULL;
    }

    RendererMesh* mesh = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*mesh));
    if (mesh == NULL)
    {
        return NULL;
    }

    uint64_t vertexBytes = (uint64_t)vertexCount * sizeof(ChunkVertex);
    uint64_t indexBytes = (uint64_t)indexCount * sizeof(uint32_t);

    // Один буфер на меш: [вершины][индексы]. Upload-куча с записью
    // один раз при создании — стриминга каждый кадр больше нет.
    D3D12_HEAP_PROPERTIES heapProperties = { .Type = D3D12_HEAP_TYPE_UPLOAD };

    D3D12_RESOURCE_DESC description;
    memset(&description, 0, sizeof(description));
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = vertexBytes + indexBytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.SampleDesc.Count = 1;

    if (FAILED(ID3D12Device_CreateCommittedResource(renderer->device, &heapProperties, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&mesh->buffer)))
    {
        HeapFree(GetProcessHeap(), 0, mesh);
        return NULL;
    }

    D3D12_RANGE emptyRange = { 0, 0 };
    void* mapped = NULL;
    if (FAILED(ID3D12Resource_Map(mesh->buffer, 0, &emptyRange, &mapped)))
    {
        ID3D12Resource_Release(mesh->buffer);
        HeapFree(GetProcessHeap(), 0, mesh);
        return NULL;
    }

    memcpy(mapped, vertices, vertexBytes);
    memcpy((uint8_t*)mapped + vertexBytes, indices, indexBytes);
    ID3D12Resource_Unmap(mesh->buffer, 0, NULL);

    D3D12_GPU_VIRTUAL_ADDRESS bufferAddress = ID3D12Resource_GetGPUVirtualAddress(mesh->buffer);

    mesh->vertexView.BufferLocation = bufferAddress;
    mesh->vertexView.SizeInBytes = (UINT)vertexBytes;
    mesh->vertexView.StrideInBytes = (UINT)sizeof(ChunkVertex);

    mesh->indexView.BufferLocation = bufferAddress + vertexBytes;
    mesh->indexView.SizeInBytes = (UINT)indexBytes;
    mesh->indexView.Format = DXGI_FORMAT_R32_UINT;

    mesh->indexCount = indexCount;
    return mesh;
}

void RendererDestroyMesh(Renderer* renderer, RendererMesh* mesh)
{
    if (mesh == NULL)
    {
        return;
    }

    // Очередь переполнена — дожидаемся GPU и освобождаем всё накопленное.
    if (renderer->deferredReleaseCount == DEFERRED_RELEASE_CAPACITY)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }

    uint32_t slot = (renderer->deferredReleaseHead + renderer->deferredReleaseCount) % DEFERRED_RELEASE_CAPACITY;
    renderer->deferredReleases[slot].resource = mesh->buffer;
    renderer->deferredReleases[slot].safeFenceValue = renderer->lastSignaledFenceValue + 1;
    renderer->deferredReleaseCount++;

    HeapFree(GetProcessHeap(), 0, mesh);
}

void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh, const float chunkOrigin[3])
{
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList,
        0, 3, chunkOrigin, ROOT_CONSTANT_CHUNK_ORIGIN_OFFSET);
    ID3D12GraphicsCommandList_IASetVertexBuffers(renderer->commandList, 0, 1, &mesh->vertexView);
    ID3D12GraphicsCommandList_IASetIndexBuffer(renderer->commandList, &mesh->indexView);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(renderer->commandList, mesh->indexCount, 1, 0, 0, 0);
}

static void ApplyPendingResize(Renderer* renderer)
{
    renderer->resizeRequested = false;

    int32_t width = renderer->resizeWidth;
    int32_t height = renderer->resizeHeight;
    if (width <= 0 || height <= 0)
    {
        return;
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

    IDXGISwapChain3_ResizeBuffers(renderer->swapChain, FRAME_COUNT, (UINT)width, (UINT)height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    renderer->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(renderer->swapChain);

    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(renderer->renderTargetViewHeap, &renderTargetViewHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        IDXGISwapChain3_GetBuffer(renderer->swapChain, i, &IID_ID3D12Resource, (void**)&renderer->renderTargets[i]);
        ID3D12Device_CreateRenderTargetView(renderer->device, renderer->renderTargets[i], NULL, renderTargetViewHandle);
        renderTargetViewHandle.ptr += renderer->renderTargetViewSize;
    }

    CreateDepthBuffer(renderer, width, height);

    renderer->windowWidth = width;
    renderer->windowHeight = height;
    renderer->viewport.Width = (float)width;
    renderer->viewport.Height = (float)height;
    renderer->scissorRect.right = width;
    renderer->scissorRect.bottom = height;
}

void RendererBeginFrame(Renderer* renderer)
{
    // Отложенный resize: применяется, когда GPU дошёл до этого кадра
    // (забор предыдущего кадра проверен в MoveToNextFrame).
    if (renderer->resizeRequested)
    {
        ApplyPendingResize(renderer);
    }

    DrainDeferredReleases(renderer, false);

    ID3D12CommandAllocator_Reset(renderer->commandAllocators[renderer->frameIndex]);
    ID3D12GraphicsCommandList_Reset(renderer->commandList, renderer->commandAllocators[renderer->frameIndex], renderer->pipelineState);

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
    ID3D12GraphicsCommandList_SetPipelineState(renderer->commandList, renderer->pipelineState);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(renderer->commandList, 0, 16, renderer->viewProjection, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(renderer->commandList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
