#include "render/renderer.h"

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "render/generated/chunk_vs.h"
#include "render/generated/chunk_ps.h"

#include <stddef.h>

#define FRAME_COUNT 2

#define RENDERER_MAX_VERTICES (512 * 1024)
#define RENDERER_MAX_INDICES (RENDERER_MAX_VERTICES * 6 / 4)

struct RenderMesh
{
    ID3D12Resource*          vertexBuffer;
    ID3D12Resource*          indexBuffer;
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
    ID3D12DescriptorHeap*      rtvHeap;
    UINT                       rtvDescriptorSize;
    ID3D12Resource*            depthStencilBuffer;
    ID3D12DescriptorHeap*      dsvHeap;
    ID3D12Fence*               fence;
    HANDLE                     fenceEvent;
    UINT64                     fenceValues[FRAME_COUNT];
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

    ID3D12Resource*            vertexBuffer;
    ID3D12Resource*            indexBuffer;
    void*                      vertexMapped;
    void*                      indexMapped;
    D3D12_VERTEX_BUFFER_VIEW   vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW    indexBufferView;
    uint32_t                   vertexCursor;
    uint32_t                   indexCursor;
};

static D3D12_RESOURCE_BARRIER MakeTransitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

static void WaitForGpu(struct Renderer* r)
{
    UINT64 target = r->fenceValues[r->frameIndex];
    ID3D12CommandQueue_Signal(r->commandQueue, r->fence, target);
    ID3D12Fence_SetEventOnCompletion(r->fence, target, r->fenceEvent);
    WaitForSingleObject(r->fenceEvent, INFINITE);
    r->fenceValues[r->frameIndex]++;
}

static void MoveToNextFrame(struct Renderer* r)
{
    UINT64 current = r->fenceValues[r->frameIndex];
    ID3D12CommandQueue_Signal(r->commandQueue, r->fence, current);
    r->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapChain);
    if (ID3D12Fence_GetCompletedValue(r->fence) < r->fenceValues[r->frameIndex])
    {
        ID3D12Fence_SetEventOnCompletion(r->fence, r->fenceValues[r->frameIndex], r->fenceEvent);
        WaitForSingleObject(r->fenceEvent, INFINITE);
    }
    r->fenceValues[r->frameIndex] = current + 1;
}

static bool CreateRootSignature(struct Renderer* r)
{
    D3D12_ROOT_PARAMETER param;
    memset(&param, 0, sizeof(param));
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = 16;

    D3D12_ROOT_SIGNATURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    {
        if (err != NULL) ID3D10Blob_Release(err);
        return false;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(r->device, 0,
        ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
        &IID_ID3D12RootSignature, (void**)&r->rootSignature);
    ID3D10Blob_Release(sig);
    if (err != NULL) ID3D10Blob_Release(err);
    return SUCCEEDED(hr);
}

static bool CreatePipelineState(struct Renderer* r)
{
    D3D12_INPUT_ELEMENT_DESC elements[2];
    memset(elements, 0, sizeof(elements));
    elements[0].SemanticName = "POSITION";
    elements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    elements[0].AlignedByteOffset = 0;
    elements[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    elements[1].SemanticName = "COLOR";
    elements[1].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    elements[1].AlignedByteOffset = 12;
    elements[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.pRootSignature = r->rootSignature;
    desc.VS.pShaderBytecode = g_chunk_vs;
    desc.VS.BytecodeLength = sizeof(g_chunk_vs);
    desc.PS.pShaderBytecode = g_chunk_ps;
    desc.PS.BytecodeLength = sizeof(g_chunk_ps);
    desc.InputLayout.pInputElementDescs = elements;
    desc.InputLayout.NumElements = 2;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleMask = 0xFFFFFFFF;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(r->device, &desc, &IID_ID3D12PipelineState, (void**)&r->pipelineState));
}

static bool CreateDepthBuffer(struct Renderer* r, int32_t width, int32_t height)
{
    if (r->dsvHeap != NULL)
    {
        ID3D12DescriptorHeap_Release(r->dsvHeap);
        r->dsvHeap = NULL;
    }
    if (r->depthStencilBuffer != NULL)
    {
        ID3D12Resource_Release(r->depthStencilBuffer);
        r->depthStencilBuffer = NULL;
    }

    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)width;
    desc.Height = (UINT)height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = { DXGI_FORMAT_D32_FLOAT, { 1.0f, 0 } };

    if (FAILED(ID3D12Device_CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, &IID_ID3D12Resource, (void**)&r->depthStencilBuffer)))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(r->device, &dsvDesc, &IID_ID3D12DescriptorHeap, (void**)&r->dsvHeap)))
        return false;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->dsvHeap, &dsvHandle);
    ID3D12Device_CreateDepthStencilView(r->device, r->depthStencilBuffer, NULL, dsvHandle);
    return true;
}

Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height)
{
    Renderer* r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (r == NULL) return NULL;

    r->windowWidth = width;
    r->windowHeight = height;

    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&r->factory)))
    { HeapFree(GetProcessHeap(), 0, r); return NULL; }

    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&r->device)))
    {
        IDXGIAdapter* warp = NULL;
        if (FAILED(IDXGIFactory4_EnumWarpAdapter(r->factory, &IID_IDXGIAdapter, (void**)&warp)))
        { IDXGIFactory4_Release(r->factory); HeapFree(GetProcessHeap(), 0, r); return NULL; }
        HRESULT hr = D3D12CreateDevice((IUnknown*)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&r->device);
        IDXGIAdapter_Release(warp);
        if (FAILED(hr))
        { IDXGIFactory4_Release(r->factory); HeapFree(GetProcessHeap(), 0, r); return NULL; }
    }

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateCommandQueue(r->device, &qd, &IID_ID3D12CommandQueue, (void**)&r->commandQueue)))
    { RendererDestroy(r); return NULL; }

    DXGI_SWAP_CHAIN_DESC1 scd;
    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = FRAME_COUNT;
    scd.Width = (UINT)width;
    scd.Height = (UINT)height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(r->factory, (IUnknown*)r->commandQueue,
        (HWND)windowHandle, &scd, NULL, NULL, &sc1)))
    { RendererDestroy(r); return NULL; }

    if (FAILED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&r->swapChain)))
    { IDXGISwapChain1_Release(sc1); RendererDestroy(r); return NULL; }
    IDXGISwapChain1_Release(sc1);

    r->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapChain);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAME_COUNT, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(r->device, &rtvHeapDesc, &IID_ID3D12DescriptorHeap, (void**)&r->rtvHeap)))
    { RendererDestroy(r); return NULL; }

    r->rtvDescriptorSize = ID3D12Device_GetDescriptorHandleIncrementSize(r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtvHeap, &rtvHandle);
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(IDXGISwapChain3_GetBuffer(r->swapChain, i, &IID_ID3D12Resource, (void**)&r->renderTargets[i])))
        { RendererDestroy(r); return NULL; }
        ID3D12Device_CreateRenderTargetView(r->device, r->renderTargets[i], NULL, rtvHandle);
        rtvHandle.ptr += r->rtvDescriptorSize;
    }

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(ID3D12Device_CreateCommandAllocator(r->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&r->commandAllocators[i])))
        { RendererDestroy(r); return NULL; }
    }

    if (FAILED(ID3D12Device_CreateCommandList(r->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        r->commandAllocators[0], NULL, &IID_ID3D12GraphicsCommandList, (void**)&r->commandList)))
    { RendererDestroy(r); return NULL; }
    ID3D12GraphicsCommandList_Close(r->commandList);

    if (FAILED(ID3D12Device_CreateFence(r->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&r->fence)))
    { RendererDestroy(r); return NULL; }
    r->fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (r->fenceEvent == NULL)
    { RendererDestroy(r); return NULL; }

    if (!CreateRootSignature(r)) { RendererDestroy(r); return NULL; }
    if (!CreatePipelineState(r)) { RendererDestroy(r); return NULL; }
    if (!CreateDepthBuffer(r, width, height)) { RendererDestroy(r); return NULL; }

    r->viewport.TopLeftX = 0.0f;
    r->viewport.TopLeftY = 0.0f;
    r->viewport.Width = (float)width;
    r->viewport.Height = (float)height;
    r->viewport.MinDepth = 0.0f;
    r->viewport.MaxDepth = 1.0f;
    r->scissorRect.left = 0;
    r->scissorRect.top = 0;
    r->scissorRect.right = width;
    r->scissorRect.bottom = height;

    for (UINT i = 0; i < 16; ++i) r->viewProjection[i] = 0.0f;
    r->viewProjection[0] = r->viewProjection[5] = r->viewProjection[10] = r->viewProjection[15] = 1.0f;

    WaitForGpu(r);

    // Create upload heaps for per-frame vertex/index streaming.
    D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC vbDesc;
    memset(&vbDesc, 0, sizeof(vbDesc));
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = RENDERER_MAX_VERTICES * sizeof(ChunkVertex);
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vbDesc.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateCommittedResource(r->device, &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&r->vertexBuffer)))
    { RendererDestroy(r); return NULL; }

    D3D12_RANGE empty = { 0, 0 };
    ID3D12Resource_Map(r->vertexBuffer, 0, &empty, &r->vertexMapped);

    D3D12_RESOURCE_DESC ibDesc;
    memcpy(&ibDesc, &vbDesc, sizeof(ibDesc));
    ibDesc.Width = RENDERER_MAX_INDICES * sizeof(uint32_t);
    if (FAILED(ID3D12Device_CreateCommittedResource(r->device, &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&r->indexBuffer)))
    { RendererDestroy(r); return NULL; }

    ID3D12Resource_Map(r->indexBuffer, 0, &empty, &r->indexMapped);

    r->vertexBufferView.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->vertexBuffer);
    r->vertexBufferView.SizeInBytes = RENDERER_MAX_VERTICES * (UINT)sizeof(ChunkVertex);
    r->vertexBufferView.StrideInBytes = (UINT)sizeof(ChunkVertex);

    r->indexBufferView.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(r->indexBuffer);
    r->indexBufferView.SizeInBytes = RENDERER_MAX_INDICES * (UINT)sizeof(uint32_t);
    r->indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return r;
}

void RendererDestroy(Renderer* r)
{
    if (r == NULL) return;

    if (r->swapChain != NULL)
        IDXGISwapChain3_SetFullscreenState(r->swapChain, FALSE, NULL);

    if (r->commandQueue != NULL && r->fence != NULL)
        WaitForGpu(r);

    if (r->fenceEvent != NULL) CloseHandle(r->fenceEvent);

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (r->renderTargets[i] != NULL) ID3D12Resource_Release(r->renderTargets[i]);
        if (r->commandAllocators[i] != NULL) ID3D12CommandAllocator_Release(r->commandAllocators[i]);
    }
    if (r->vertexBuffer != NULL)
    {
        ID3D12Resource_Unmap(r->vertexBuffer, 0, NULL);
        ID3D12Resource_Release(r->vertexBuffer);
    }
    if (r->indexBuffer != NULL)
    {
        ID3D12Resource_Unmap(r->indexBuffer, 0, NULL);
        ID3D12Resource_Release(r->indexBuffer);
    }
    if (r->pipelineState != NULL) ID3D12PipelineState_Release(r->pipelineState);
    if (r->rootSignature != NULL) ID3D12RootSignature_Release(r->rootSignature);
    if (r->depthStencilBuffer != NULL) ID3D12Resource_Release(r->depthStencilBuffer);
    if (r->dsvHeap != NULL) ID3D12DescriptorHeap_Release(r->dsvHeap);
    if (r->rtvHeap != NULL) ID3D12DescriptorHeap_Release(r->rtvHeap);
    if (r->commandList != NULL) ID3D12GraphicsCommandList_Release(r->commandList);
    if (r->fence != NULL) ID3D12Fence_Release(r->fence);
    if (r->swapChain != NULL) IDXGISwapChain3_Release(r->swapChain);
    if (r->commandQueue != NULL) ID3D12CommandQueue_Release(r->commandQueue);
    if (r->device != NULL) ID3D12Device_Release(r->device);
    if (r->factory != NULL) IDXGIFactory4_Release(r->factory);

    HeapFree(GetProcessHeap(), 0, r);
}

void RendererSetViewProjection(Renderer* r, const float matrix[16])
{
    for (int i = 0; i < 16; ++i)
        r->viewProjection[i] = matrix[i];
}

void RendererDrawMesh(Renderer* r, const ChunkVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    if (r->vertexCursor + vertexCount > RENDERER_MAX_VERTICES) return;
    if (r->indexCursor + indexCount > RENDERER_MAX_INDICES) return;

    uint32_t baseVertex = r->vertexCursor;
    memcpy((uint8_t*)r->vertexMapped + baseVertex * sizeof(ChunkVertex), vertices, vertexCount * sizeof(ChunkVertex));
    
    uint32_t* dstIndices = (uint32_t*)r->indexMapped + r->indexCursor;
    for (uint32_t i = 0; i < indexCount; ++i)
        dstIndices[i] = indices[i] + baseVertex;

    ID3D12GraphicsCommandList_IASetVertexBuffers(r->commandList, 0, 1, &r->vertexBufferView);
    ID3D12GraphicsCommandList_IASetIndexBuffer(r->commandList, &r->indexBufferView);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(r->commandList, indexCount, 1, r->indexCursor, baseVertex, 0);

    r->vertexCursor += vertexCount;
    r->indexCursor += indexCount;
}

void RendererBeginFrame(Renderer* r)
{
    // Deferred resize: process after GPU has naturally caught up
    // (the previous frame's fence was checked in MoveToNextFrame).
    if (r->resizeRequested)
    {
        r->resizeRequested = false;
        int32_t w = r->resizeWidth;
        int32_t h = r->resizeHeight;
        if (w > 0 && h > 0)
        {
            WaitForGpu(r);
            for (UINT i = 0; i < FRAME_COUNT; ++i)
            {
                if (r->renderTargets[i] != NULL)
                {
                    ID3D12Resource_Release(r->renderTargets[i]);
                    r->renderTargets[i] = NULL;
                }
            }
            IDXGISwapChain3_ResizeBuffers(r->swapChain, FRAME_COUNT, (UINT)w, (UINT)h, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
            r->frameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(r->swapChain);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtvHeap, &rtvHandle);
            for (UINT i = 0; i < FRAME_COUNT; ++i)
            {
                IDXGISwapChain3_GetBuffer(r->swapChain, i, &IID_ID3D12Resource, (void**)&r->renderTargets[i]);
                ID3D12Device_CreateRenderTargetView(r->device, r->renderTargets[i], NULL, rtvHandle);
                rtvHandle.ptr += r->rtvDescriptorSize;
            }
            CreateDepthBuffer(r, w, h);
            r->windowWidth = w;
            r->windowHeight = h;
            r->viewport.Width = (float)w;
            r->viewport.Height = (float)h;
            r->scissorRect.left = 0;
            r->scissorRect.top = 0;
            r->scissorRect.right = w;
            r->scissorRect.bottom = h;
        }
    }

    r->vertexCursor = 0;
    r->indexCursor = 0;

    ID3D12CommandAllocator_Reset(r->commandAllocators[r->frameIndex]);
    ID3D12GraphicsCommandList_Reset(r->commandList, r->commandAllocators[r->frameIndex], r->pipelineState);

    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(r->renderTargets[r->frameIndex],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ResourceBarrier(r->commandList, 1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->rtvHeap, &rtvHandle);
    rtvHandle.ptr += (SIZE_T)r->frameIndex * r->rtvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r->dsvHeap, &dsvHandle);

    ID3D12GraphicsCommandList_OMSetRenderTargets(r->commandList, 1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[4] = { 0.4f, 0.6f, 0.9f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(r->commandList, rtvHandle, clearColor, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(r->commandList, dsvHandle,
        D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

    ID3D12GraphicsCommandList_RSSetViewports(r->commandList, 1, &r->viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(r->commandList, 1, &r->scissorRect);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(r->commandList, r->rootSignature);
    ID3D12GraphicsCommandList_SetPipelineState(r->commandList, r->pipelineState);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(r->commandList, 0, 16, r->viewProjection, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(r->commandList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void RendererEndFrame(Renderer* r)
{
    D3D12_RESOURCE_BARRIER barrier = MakeTransitionBarrier(r->renderTargets[r->frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_ResourceBarrier(r->commandList, 1, &barrier);

    ID3D12GraphicsCommandList_Close(r->commandList);

    ID3D12CommandList* lists[1] = { (ID3D12CommandList*)r->commandList };
    ID3D12CommandQueue_ExecuteCommandLists(r->commandQueue, 1, lists);

    IDXGISwapChain3_Present(r->swapChain, 1, 0);
    MoveToNextFrame(r);
}

void RendererResize(Renderer* r, int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0) return;
    r->resizeWidth = width;
    r->resizeHeight = height;
    r->resizeRequested = true;
}
