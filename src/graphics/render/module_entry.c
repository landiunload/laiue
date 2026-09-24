#include "render/graphics_service.h"
#include "render/content_provider.h"
#include "render/chunk_geometry.h"
#include "graphics/graphics_device_service.h"

#include "content/content_service.h"
#include "mod/module_api.h"
#include "mod/module_service.h"
#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifndef LAIUE_RENDER_PROVIDER_ID
#define LAIUE_RENDER_PROVIDER_ID "laiue.graphics"
#endif
typedef struct LaiueGraphicsModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueContentServiceV1 *content;
    LaiueGraphicsDeviceServiceV1 deviceService;
    LaiueGraphicsDeviceServiceV2 deviceServiceV2;
} LaiueGraphicsModuleState;

typedef struct LaiueGraphicsDeviceState
{
    LaiueGraphicsDeviceV1 device;
    LaiueGraphicsDeviceV2 deviceV2;
    const LaiueModuleHostV1 *host;
    Renderer *renderer;
    uint32_t handleNamespace;
    uint32_t generations[256];
    uint64_t sizes[256];
    uint32_t usageFlags[256];
    LaiueGraphicsTextureDescV1 textures[256];
    LaiueGraphicsSamplerDescV1 samplers[256];
    LaiueGraphicsPipelineDescV1 pipelines[256];
    uint32_t shaderRefCounts[256];
    uint32_t shaderStages[256];
    uint32_t vertexCounts[256];
    LaiueGraphicsHandle meshIndexHandles[256];
    uint32_t meshFirstIndices[256];
    uint32_t meshIndexCounts[256];
    int32_t meshVertexOffsets[256];
    void *storage[256];
    void *backendResources[256];
    RendererMesh *meshes[256];
    uint8_t kinds[256];
    uint8_t live[256];
    uint32_t submittedItems;
    float viewProjection[16];
    bool cameraSet;
    bool frameActive;
} LaiueGraphicsDeviceState;

enum
{
    DEVICE_HANDLE_BUFFER = 1u,
    DEVICE_HANDLE_TEXTURE = 2u,
    DEVICE_HANDLE_SAMPLER = 3u,
    DEVICE_HANDLE_PIPELINE = 4u,
    DEVICE_HANDLE_SHADER = 5u,
};

static volatile uint32_t g_nextHandleNamespace;

static uint32_t DeviceAllocateNamespace(void)
{
    uint32_t value = PlatformAtomicIncrementU32(&g_nextHandleNamespace);
    /* Namespace zero is reserved for an invalid handle. */
    if (value == 0u)
        value = PlatformAtomicIncrementU32(&g_nextHandleNamespace);
    return value;
}

static uint32_t DeviceHandleSlot(LaiueGraphicsHandle handle)
{
    return (uint32_t)(handle & UINT64_C(0xffff));
}

static uint32_t DeviceHandleGeneration(LaiueGraphicsHandle handle)
{
    return (uint32_t)((handle >> 16u) & UINT64_C(0xffff));
}

static uint32_t DeviceHandleNamespace(LaiueGraphicsHandle handle)
{
    return (uint32_t)(handle >> 32u);
}

static LaiueGraphicsDeviceState *DeviceState(LaiueGraphicsDeviceV1 *device)
{
    return device == NULL ? NULL : (LaiueGraphicsDeviceState *)device->context;
}

static uint32_t DeviceAllocateHandle(LaiueGraphicsDeviceState *state,
                                     uint8_t kind, uint64_t size,
                                     LaiueGraphicsHandle *outHandle)
{
    if (state == NULL || outHandle == NULL || kind == 0u)
        return 0u;
    for (uint32_t index = 0u; index < 256u; ++index)
        if (state->live[index] == 0u)
        {
            uint32_t generation = (state->generations[index] + 1u) & UINT32_C(0xffff);
            if (generation == 0u)
                generation = 1u;
            state->generations[index] = generation;
            state->sizes[index] = size;
            state->kinds[index] = kind;
            state->live[index] = 1u;
            *outHandle = ((uint64_t)state->handleNamespace << 32u) |
                         ((uint64_t)generation << 16u) | (uint64_t)(index + 1u);
            return 1u;
        }
    return 0u;
}

static uint32_t DeviceHandleIsLive(const LaiueGraphicsDeviceState *state,
                                   LaiueGraphicsHandle handle, uint8_t expectedKind)
{
    const uint32_t index = DeviceHandleSlot(handle);
    const uint32_t generation = DeviceHandleGeneration(handle);
    return state != NULL && state->handleNamespace != 0u &&
                   DeviceHandleNamespace(handle) == state->handleNamespace &&
                   index != 0u && index <= 256u && generation != 0u &&
                   state->live[index - 1u] != 0u &&
                   (state->generations[index - 1u] & UINT32_C(0xffff)) == generation &&
                   (expectedKind == 0u || state->kinds[index - 1u] == expectedKind)
               ? 1u
               : 0u;
}

static uint32_t DeviceCreateBuffer(LaiueGraphicsDeviceV1 *,
                                   const LaiueGraphicsBufferDescV1 *,
                                   LaiueGraphicsHandle *);
static uint32_t DeviceCreateTexture(LaiueGraphicsDeviceV1 *,
                                    const LaiueGraphicsTextureDescV1 *,
                                    LaiueGraphicsHandle *);
static uint32_t DeviceCreateSampler(LaiueGraphicsDeviceV1 *,
                                    const LaiueGraphicsSamplerDescV1 *,
                                    LaiueGraphicsHandle *);
static uint32_t DeviceCreatePipeline(LaiueGraphicsDeviceV1 *,
                                     const LaiueGraphicsPipelineDescV1 *,
                                     LaiueGraphicsHandle *);
static uint32_t DeviceCreateShader(LaiueGraphicsDeviceV1 *,
                                   const LaiueGraphicsShaderDescV1 *,
                                   LaiueGraphicsHandle *);
static uint32_t DeviceCreateWithContext(void *, void *, int32_t, int32_t, uint32_t,
                                        LaiueGraphicsDeviceV1 **);
static uint32_t DeviceUploadBuffer(LaiueGraphicsDeviceV1 *,
                                   const LaiueGraphicsBufferUploadV1 *);
static uint32_t DeviceUploadTexture(LaiueGraphicsDeviceV1 *,
                                    const LaiueGraphicsTextureUploadV1 *);
static void DeviceDestroyHandle(LaiueGraphicsDeviceV1 *, LaiueGraphicsHandle);
static uint32_t DeviceBeginFrame(LaiueGraphicsDeviceV1 *, uint32_t, uint32_t);
static uint32_t DeviceSubmit(LaiueGraphicsDeviceV1 *,
                             const LaiueGraphicsDrawItemV1 *, uint32_t);
static uint32_t DeviceSubmitUi(LaiueGraphicsDeviceV1 *,
                               const LaiueGraphicsUiQuadV1 *, uint32_t);
static uint32_t DeviceSetUiFontAtlas(LaiueGraphicsDeviceV1 *, const uint8_t *, uint32_t,
                                     uint32_t);
static uint32_t DeviceEndFrame(LaiueGraphicsDeviceV1 *);

static uint32_t DeviceV2Create(void *, int32_t, int32_t, uint32_t,
                               LaiueGraphicsDeviceV2 **);
static uint32_t DeviceV2CreateWithContext(void *, void *, int32_t, int32_t, uint32_t,
                                          LaiueGraphicsDeviceV2 **);
static void DeviceV2Destroy(LaiueGraphicsDeviceV2 *);
static uint32_t DeviceV2GetBackend(const LaiueGraphicsDeviceV2 *);
static void DeviceV2Resize(LaiueGraphicsDeviceV2 *, int32_t, int32_t);
static uint32_t DeviceV2CreateBuffer(LaiueGraphicsDeviceV2 *,
                                     const LaiueGraphicsBufferDescV1 *,
                                     LaiueGraphicsHandle *);
static uint32_t DeviceV2CreateTexture(LaiueGraphicsDeviceV2 *,
                                      const LaiueGraphicsTextureDescV1 *,
                                      LaiueGraphicsHandle *);
static uint32_t DeviceV2CreateSampler(LaiueGraphicsDeviceV2 *,
                                      const LaiueGraphicsSamplerDescV1 *,
                                      LaiueGraphicsHandle *);
static uint32_t DeviceV2CreatePipeline(LaiueGraphicsDeviceV2 *,
                                       const LaiueGraphicsPipelineDescV1 *,
                                       LaiueGraphicsHandle *);
static uint32_t DeviceV2CreateShader(LaiueGraphicsDeviceV2 *,
                                     const LaiueGraphicsShaderDescV1 *,
                                     LaiueGraphicsHandle *);
static uint32_t DeviceV2UploadBuffer(LaiueGraphicsDeviceV2 *,
                                     const LaiueGraphicsBufferUploadV1 *);
static uint32_t DeviceV2UploadTexture(LaiueGraphicsDeviceV2 *,
                                      const LaiueGraphicsTextureUploadV1 *);
static void DeviceV2DestroyHandle(LaiueGraphicsDeviceV2 *, LaiueGraphicsHandle);
static uint32_t DeviceV2BeginFrame(LaiueGraphicsDeviceV2 *, uint32_t, uint32_t);
static uint32_t DeviceV2SetCamera(LaiueGraphicsDeviceV2 *,
                                  const LaiueGraphicsCameraV2 *);
static uint32_t DeviceV2Submit(LaiueGraphicsDeviceV2 *,
                               const LaiueGraphicsDrawItemV2 *, uint32_t);
static uint32_t DeviceV2SubmitUi(LaiueGraphicsDeviceV2 *,
                                 const LaiueGraphicsUiQuadV1 *, uint32_t);
static uint32_t DeviceV2SetUiFontAtlas(LaiueGraphicsDeviceV2 *, const uint8_t *,
                                       uint32_t, uint32_t);
static uint32_t DeviceV2EndFrame(LaiueGraphicsDeviceV2 *);

static void *DeviceAllocate(const LaiueGraphicsDeviceState *state, size_t size,
                            bool clear)
{
    if (state != NULL && state->host != NULL && state->host->allocate != NULL)
    {
        void *memory = state->host->allocate(state->host->context, size);
        if (memory != NULL && clear)
            memset(memory, 0, size);
        return memory;
    }
    return PlatformAllocate(size, clear);
}

static void DeviceFree(const LaiueGraphicsDeviceState *state, void *memory)
{
    if (memory == NULL)
        return;
    if (state != NULL && state->host != NULL && state->host->free != NULL)
        state->host->free(state->host->context, memory);
    else
        PlatformFree(memory);
}

static bool DeviceBufferIsGeneric(const LaiueGraphicsDeviceState *state,
                                  uint32_t index)
{
    return state != NULL && index < 256u &&
           (state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX) != 0u &&
           (state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX_PULLING) == 0u;
}

static bool DeviceTextureExtentIsRepresentable(const LaiueGraphicsExtentV1 *extent)
{
    if (extent == NULL || extent->width == 0u || extent->height == 0u ||
        extent->depth == 0u)
        return false;
    const uint64_t area = (uint64_t)extent->width * (uint64_t)extent->height;
    return (uint64_t)extent->depth <= UINT64_MAX / area;
}

static bool DeviceTextureFormatIsSupported(const LaiueGraphicsTextureDescV1 *description)
{
    return description != NULL && description->format <= LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB;
}

static bool DeviceSamplerIsValid(const LaiueGraphicsSamplerDescV1 *description)
{
    return description != NULL &&
           description->minFilter <= LAIUE_GRAPHICS_FILTER_LINEAR &&
           description->magFilter <= LAIUE_GRAPHICS_FILTER_LINEAR &&
           description->addressModeU <= LAIUE_GRAPHICS_ADDRESS_BORDER &&
           description->addressModeV <= LAIUE_GRAPHICS_ADDRESS_BORDER &&
           description->addressModeW <= LAIUE_GRAPHICS_ADDRESS_BORDER;
}

static bool DevicePipelineIsValid(const LaiueGraphicsPipelineDescV1 *description)
{
    return description != NULL && description->topology <= LAIUE_GRAPHICS_TOPOLOGY_POINTS;
}

static bool DevicePipelineHandlesAreLive(const LaiueGraphicsDeviceState *state,
                                         const LaiueGraphicsPipelineDescV1 *description)
{
    if (state == NULL || !DevicePipelineIsValid(description))
        return false;
    if (description->vertexShader != 0u)
    {
        if (!DeviceHandleIsLive(state, description->vertexShader, DEVICE_HANDLE_SHADER) ||
            state->shaderStages[DeviceHandleSlot(description->vertexShader) - 1u] !=
                LAIUE_GRAPHICS_SHADER_STAGE_VERTEX)
            return false;
    }
    if (description->fragmentShader != 0u)
    {
        if (!DeviceHandleIsLive(state, description->fragmentShader, DEVICE_HANDLE_SHADER) ||
            state->shaderStages[DeviceHandleSlot(description->fragmentShader) - 1u] !=
                LAIUE_GRAPHICS_SHADER_STAGE_FRAGMENT)
            return false;
    }
    return true;
}

static bool DeviceOptionalBindingsAreLive(const LaiueGraphicsDeviceState *state,
                                          LaiueGraphicsHandle texture,
                                          LaiueGraphicsHandle sampler)
{
    return state != NULL &&
           (texture == 0u || DeviceHandleIsLive(state, texture, DEVICE_HANDLE_TEXTURE)) &&
           (sampler == 0u || DeviceHandleIsLive(state, sampler, DEVICE_HANDLE_SAMPLER));
}

static uint32_t DeviceBuildGenericMesh(LaiueGraphicsDeviceState *state,
                                       uint32_t vertexIndex,
                                       LaiueGraphicsHandle indexBuffer,
                                       uint32_t firstIndex, uint32_t indexCount,
                                       int32_t vertexOffset)
{
    if (state == NULL || vertexIndex >= 256u ||
        !DeviceBufferIsGeneric(state, vertexIndex) || state->storage[vertexIndex] == NULL ||
        state->sizes[vertexIndex] % sizeof(LaiueGraphicsVertexV2) != 0u)
        return 0u;
    const uint32_t vertexCount = (uint32_t)(state->sizes[vertexIndex] /
                                            sizeof(LaiueGraphicsVertexV2));
    if (vertexCount == 0u)
        return 0u;
    if (indexBuffer == 0u)
    {
        if (state->meshes[vertexIndex] != NULL)
            RendererDestroyMesh(state->renderer, state->meshes[vertexIndex]);
        state->meshes[vertexIndex] = RendererCreateGenericMesh(
            state->renderer, (const RendererGenericVertex *)state->storage[vertexIndex],
            vertexCount);
        state->vertexCounts[vertexIndex] = state->meshes[vertexIndex] != NULL
                                                ? vertexCount
                                                : 0u;
        state->meshIndexHandles[vertexIndex] = 0u;
        state->meshFirstIndices[vertexIndex] = 0u;
        state->meshIndexCounts[vertexIndex] = 0u;
        state->meshVertexOffsets[vertexIndex] = 0;
        return state->meshes[vertexIndex] != NULL ? 1u : 0u;
    }
    if (!DeviceHandleIsLive(state, indexBuffer, DEVICE_HANDLE_BUFFER))
        return 0u;
    const uint32_t indexSlot = DeviceHandleSlot(indexBuffer) - 1u;
    if ((state->usageFlags[indexSlot] & LAIUE_GRAPHICS_BUFFER_USAGE_INDEX) == 0u ||
        state->storage[indexSlot] == NULL || state->sizes[indexSlot] % sizeof(uint32_t) != 0u ||
        firstIndex > state->sizes[indexSlot] / sizeof(uint32_t) ||
        indexCount > state->sizes[indexSlot] / sizeof(uint32_t) - firstIndex ||
        indexCount == 0u || (indexCount % 3u) != 0u)
        return 0u;
    if (state->meshes[vertexIndex] != NULL &&
        state->meshIndexHandles[vertexIndex] == indexBuffer &&
        state->meshFirstIndices[vertexIndex] == firstIndex &&
        state->meshIndexCounts[vertexIndex] == indexCount &&
        state->meshVertexOffsets[vertexIndex] == vertexOffset)
        return 1u;
    if (indexCount > UINT32_MAX / sizeof(LaiueGraphicsVertexV2))
        return 0u;
    LaiueGraphicsVertexV2 *expanded = (LaiueGraphicsVertexV2 *)DeviceAllocate(
        state, (size_t)indexCount * sizeof(*expanded), false);
    if (expanded == NULL)
        return 0u;
    const uint32_t *indices = (const uint32_t *)state->storage[indexSlot];
    const LaiueGraphicsVertexV2 *vertices =
        (const LaiueGraphicsVertexV2 *)state->storage[vertexIndex];
    bool valid = true;
    for (uint32_t item = 0u; item < indexCount; ++item)
    {
        const uint32_t raw = indices[firstIndex + item];
        const int64_t resolved = (int64_t)raw + (int64_t)vertexOffset;
        if (resolved < 0 || resolved >= (int64_t)vertexCount)
        {
            valid = false;
            break;
        }
        expanded[item] = vertices[(uint32_t)resolved];
    }
    if (valid)
    {
        if (state->meshes[vertexIndex] != NULL)
            RendererDestroyMesh(state->renderer, state->meshes[vertexIndex]);
        state->meshes[vertexIndex] = RendererCreateGenericMesh(
            state->renderer, (const RendererGenericVertex *)expanded, indexCount);
    }
    DeviceFree(state, expanded);
    if (!valid || state->meshes[vertexIndex] == NULL)
        return 0u;
    state->vertexCounts[vertexIndex] = indexCount;
    state->meshIndexHandles[vertexIndex] = indexBuffer;
    state->meshFirstIndices[vertexIndex] = firstIndex;
    state->meshIndexCounts[vertexIndex] = indexCount;
    state->meshVertexOffsets[vertexIndex] = vertexOffset;
    return 1u;
}

static uint32_t DeviceCreateInternal(const LaiueModuleHostV1 *host,
                                     void *nativeWindow, int32_t width, int32_t height,
                                     uint32_t backend, LaiueGraphicsDeviceV1 **outDevice)
{
    if (outDevice == NULL)
        return 0u;
    *outDevice = NULL;
    LaiueGraphicsDeviceState *state =
        (LaiueGraphicsDeviceState *)(host != NULL && host->allocate != NULL
                                         ? host->allocate(host->context, sizeof(*state))
                                         : PlatformAllocate(sizeof(*state), true));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->handleNamespace = DeviceAllocateNamespace();
    if (state->handleNamespace == 0u)
    {
        DeviceFree(state, state);
        return 0u;
    }
    Renderer *renderer = RendererCreateWithBackend(
        nativeWindow, width, height, (RendererBackendKind)backend);
    if (renderer == NULL)
    {
        DeviceFree(state, state);
        return 0u;
    }
    /* Device submissions use the same backend-owned mesh/pipeline path as
     * voxel_render.  Preparing it once here makes the device contract real:
     * a successful draw reaches D3D12/Vulkan command recording. */
    if (!RendererPrepareWorld(renderer))
    {
        RendererDestroy(renderer);
        DeviceFree(state, state);
        return 0u;
    }
    state->renderer = renderer;
    state->device.structSize = sizeof(state->device);
    state->device.abiVersion = LAIUE_GRAPHICS_ABI_VERSION_1;
    state->device.context = state;
    state->device.createBuffer = DeviceCreateBuffer;
    state->device.createTexture = DeviceCreateTexture;
    state->device.createSampler = DeviceCreateSampler;
    state->device.createPipeline = DeviceCreatePipeline;
    state->device.uploadBuffer = DeviceUploadBuffer;
    state->device.uploadTexture = DeviceUploadTexture;
    state->device.destroyHandle = DeviceDestroyHandle;
    state->device.beginFrame = DeviceBeginFrame;
    state->device.submit = DeviceSubmit;
    state->device.endFrame = DeviceEndFrame;
    state->device.createShader = DeviceCreateShader;
    state->device.submitUi = DeviceSubmitUi;
    state->device.setUiFontAtlas = DeviceSetUiFontAtlas;
    state->deviceV2.structSize = sizeof(state->deviceV2);
    state->deviceV2.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION;
    state->deviceV2.context = state;
    state->deviceV2.createBuffer = DeviceV2CreateBuffer;
    state->deviceV2.createTexture = DeviceV2CreateTexture;
    state->deviceV2.createSampler = DeviceV2CreateSampler;
    state->deviceV2.createPipeline = DeviceV2CreatePipeline;
    state->deviceV2.uploadBuffer = DeviceV2UploadBuffer;
    state->deviceV2.uploadTexture = DeviceV2UploadTexture;
    state->deviceV2.destroyHandle = DeviceV2DestroyHandle;
    state->deviceV2.beginFrame = DeviceV2BeginFrame;
    state->deviceV2.submit = DeviceV2Submit;
    state->deviceV2.endFrame = DeviceV2EndFrame;
    state->deviceV2.createShader = DeviceV2CreateShader;
    state->deviceV2.submitUi = DeviceV2SubmitUi;
    state->deviceV2.setUiFontAtlas = DeviceV2SetUiFontAtlas;
    state->deviceV2.setCamera = DeviceV2SetCamera;
    *outDevice = &state->device;
    return 1u;
}

static uint32_t DeviceCreate(void *nativeWindow, int32_t width, int32_t height,
                             uint32_t backend, LaiueGraphicsDeviceV1 **outDevice)
{
    return DeviceCreateInternal(NULL, nativeWindow, width, height, backend, outDevice);
}

static uint32_t DeviceCreateWithContext(void *moduleContext, void *nativeWindow,
                                        int32_t width, int32_t height, uint32_t backend,
                                        LaiueGraphicsDeviceV1 **outDevice)
{
    const LaiueGraphicsModuleState *module =
        (const LaiueGraphicsModuleState *)moduleContext;
    if (module == NULL || module->host == NULL || module->host->allocate == NULL ||
        module->host->free == NULL)
        return 0u;
    return DeviceCreateInternal(module->host, nativeWindow, width, height, backend,
                                outDevice);
}

static void DeviceDestroy(LaiueGraphicsDeviceV1 *device)
{
    if (device == NULL)
        return;
    LaiueGraphicsDeviceState *state =
        (LaiueGraphicsDeviceState *)device->context;
    if (state == NULL)
        return;
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        if (state->meshes[index] != NULL)
            RendererDestroyMesh(state->renderer, state->meshes[index]);
        if (state->backendResources[index] != NULL &&
            state->kinds[index] == DEVICE_HANDLE_TEXTURE)
            RendererDestroyTexture(state->renderer,
                                   (RendererTexture *)state->backendResources[index]);
        else if (state->backendResources[index] != NULL &&
                 state->kinds[index] == DEVICE_HANDLE_SAMPLER)
            RendererDestroySampler(state->renderer,
                                   (RendererSampler *)state->backendResources[index]);
        state->backendResources[index] = NULL;
        DeviceFree(state, state->storage[index]);
    }
    RendererDestroy(state->renderer);
    DeviceFree(state, state);
}

static uint32_t DeviceGetBackend(const LaiueGraphicsDeviceV1 *device)
{
    if (device == NULL || device->context == NULL)
        return LAIUE_GRAPHICS_BACKEND_AUTO;
    const LaiueGraphicsDeviceState *state =
        (const LaiueGraphicsDeviceState *)device->context;
    return (uint32_t)RendererGetBackend(state->renderer);
}

static void DeviceResize(LaiueGraphicsDeviceV1 *device, int32_t width, int32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL)
        return;
    RendererResize(state->renderer, width, height);
}

static uint32_t DeviceCreateBuffer(LaiueGraphicsDeviceV1 *device,
                                   const LaiueGraphicsBufferDescV1 *description,
                                   LaiueGraphicsHandle *outBuffer)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (outBuffer == NULL)
        return 0u;
    *outBuffer = 0u;
    if (state == NULL || description == NULL ||
        description->structSize < sizeof(*description) || description->sizeBytes == 0u)
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_BUFFER, description->sizeBytes,
                              outBuffer))
        return 0u;
    const uint32_t index = DeviceHandleSlot(*outBuffer) - 1u;
    state->usageFlags[index] = description->usageFlags;
    if (description->sizeBytes > (uint64_t)SIZE_MAX ||
        (state->storage[index] = DeviceAllocate(state, (size_t)description->sizeBytes, true)) == NULL)
    {
        DeviceDestroyHandle(device, *outBuffer);
        return 0u;
    }
    return 1u;
}

static uint32_t DeviceCreateTexture(LaiueGraphicsDeviceV1 *device,
                                    const LaiueGraphicsTextureDescV1 *description,
                                    LaiueGraphicsHandle *outTexture)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (outTexture == NULL)
        return 0u;
    *outTexture = 0u;
    if (state == NULL || description == NULL ||
        description->structSize < sizeof(*description) || description->mipLevels != 1u ||
        description->extent.depth != 1u || !DeviceTextureFormatIsSupported(description) ||
        !DeviceTextureExtentIsRepresentable(&description->extent))
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_TEXTURE, 0u, outTexture))
        return 0u;
    const uint32_t index = DeviceHandleSlot(*outTexture) - 1u;
    RendererTexture *backendTexture = RendererCreateTexture(
        state->renderer, description->extent.width, description->extent.height,
        description->mipLevels, description->format);
    if (backendTexture == NULL)
    {
        DeviceDestroyHandle(device, *outTexture);
        *outTexture = 0u;
        return 0u;
    }
    state->textures[index] = *description;
    state->textures[index].structSize = sizeof(state->textures[index]);
    state->backendResources[index] = backendTexture;
    return 1u;
}

static uint32_t DeviceCreateSampler(LaiueGraphicsDeviceV1 *device,
                                    const LaiueGraphicsSamplerDescV1 *description,
                                    LaiueGraphicsHandle *outSampler)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (outSampler == NULL)
        return 0u;
    *outSampler = 0u;
    if (state == NULL || description == NULL ||
        description->structSize < sizeof(*description) || !DeviceSamplerIsValid(description))
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_SAMPLER, 0u, outSampler))
        return 0u;
    const uint32_t index = DeviceHandleSlot(*outSampler) - 1u;
    RendererSampler *backendSampler = RendererCreateSampler(
        state->renderer, description->minFilter, description->magFilter,
        description->addressModeU, description->addressModeV, description->addressModeW);
    if (backendSampler == NULL)
    {
        DeviceDestroyHandle(device, *outSampler);
        *outSampler = 0u;
        return 0u;
    }
    state->samplers[index] = *description;
    state->samplers[index].structSize = sizeof(state->samplers[index]);
    state->backendResources[index] = backendSampler;
    return 1u;
}

static uint32_t DeviceCreatePipeline(LaiueGraphicsDeviceV1 *device,
                                     const LaiueGraphicsPipelineDescV1 *description,
                                     LaiueGraphicsHandle *outPipeline)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (outPipeline == NULL)
        return 0u;
    *outPipeline = 0u;
    if (state == NULL || description == NULL ||
        description->structSize < sizeof(*description) || !DevicePipelineIsValid(description))
        return 0u;
    if (!DevicePipelineHandlesAreLive(state, description))
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_PIPELINE, 0u, outPipeline))
        return 0u;
    const uint32_t index = DeviceHandleSlot(*outPipeline) - 1u;
    state->pipelines[index] = *description;
    state->pipelines[index].structSize = sizeof(state->pipelines[index]);
    if (description->vertexShader != 0u)
        ++state->shaderRefCounts[DeviceHandleSlot(description->vertexShader) - 1u];
    if (description->fragmentShader != 0u)
        ++state->shaderRefCounts[DeviceHandleSlot(description->fragmentShader) - 1u];
    return 1u;
}

static uint32_t DeviceCreateShader(LaiueGraphicsDeviceV1 *device,
                                   const LaiueGraphicsShaderDescV1 *description,
                                   LaiueGraphicsHandle *outShader)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (outShader == NULL)
        return 0u;
    *outShader = 0u;
    if (state == NULL || description == NULL ||
        description->structSize < sizeof(*description) ||
        description->stage == 0u || description->stage > LAIUE_GRAPHICS_SHADER_STAGE_COMPUTE ||
        description->code == NULL ||
        description->codeSizeBytes == 0u || description->codeSizeBytes > (uint64_t)SIZE_MAX)
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_SHADER, description->codeSizeBytes,
                              outShader))
        return 0u;
    const uint32_t index = DeviceHandleSlot(*outShader) - 1u;
    state->storage[index] = DeviceAllocate(state, (size_t)description->codeSizeBytes, false);
    if (state->storage[index] == NULL)
    {
        DeviceDestroyHandle(device, *outShader);
        return 0u;
    }
    memcpy(state->storage[index], description->code, (size_t)description->codeSizeBytes);
    state->shaderStages[index] = description->stage;
    return 1u;
}

static uint32_t DeviceUploadBuffer(LaiueGraphicsDeviceV1 *device,
                                   const LaiueGraphicsBufferUploadV1 *upload)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || upload == NULL || upload->structSize < sizeof(*upload) ||
        !DeviceHandleIsLive(state, upload->buffer, DEVICE_HANDLE_BUFFER) ||
        upload->data == NULL || upload->sizeBytes == 0u ||
        upload->sizeBytes > (uint64_t)SIZE_MAX ||
        upload->offsetBytes > UINT64_MAX - upload->sizeBytes)
        return 0u;
    const uint32_t index = DeviceHandleSlot(upload->buffer) - 1u;
    if (upload->offsetBytes > state->sizes[index] ||
        upload->sizeBytes > state->sizes[index] - upload->offsetBytes)
        return 0u;
    if (DeviceBufferIsGeneric(state, index) && state->meshes[index] != NULL)
    {
        RendererDestroyMesh(state->renderer, state->meshes[index]);
        state->meshes[index] = NULL;
        state->vertexCounts[index] = 0u;
        state->meshIndexHandles[index] = 0u;
    }
    if ((state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_INDEX) != 0u)
        for (uint32_t vertex = 0u; vertex < 256u; ++vertex)
            if (state->meshIndexHandles[vertex] == upload->buffer &&
                state->meshes[vertex] != NULL)
            {
                RendererDestroyMesh(state->renderer, state->meshes[vertex]);
                state->meshes[vertex] = NULL;
                state->vertexCounts[vertex] = 0u;
                state->meshIndexHandles[vertex] = 0u;
            }
    memcpy((uint8_t *)state->storage[index] + (size_t)upload->offsetBytes,
           upload->data, (size_t)upload->sizeBytes);
    if ((state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX_PULLING) != 0u &&
        upload->offsetBytes == 0u && upload->sizeBytes == state->sizes[index] &&
        upload->sizeBytes >= sizeof(ChunkQuad) &&
        upload->sizeBytes / sizeof(ChunkQuad) <= UINT32_MAX)
    {
        RendererMesh *mesh = RendererCreateMesh(
            state->renderer, (const ChunkQuad *)state->storage[index],
            (uint32_t)(upload->sizeBytes / sizeof(ChunkQuad)));
        if (mesh == NULL)
            return 0u;
        if (state->meshes[index] != NULL)
            RendererDestroyMesh(state->renderer, state->meshes[index]);
        state->meshes[index] = mesh;
    }
    else if (DeviceBufferIsGeneric(state, index) &&
             upload->offsetBytes == 0u && upload->sizeBytes == state->sizes[index] &&
             upload->sizeBytes % sizeof(LaiueGraphicsVertexV2) == 0u &&
             upload->sizeBytes / sizeof(LaiueGraphicsVertexV2) <= UINT32_MAX)
    {
        if (!DeviceBuildGenericMesh(state, index, 0u, 0u, 0u, 0))
            return 0u;
    }
    return 1u;
}

static uint32_t DeviceUploadTexture(LaiueGraphicsDeviceV1 *device,
                                    const LaiueGraphicsTextureUploadV1 *upload)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || upload == NULL || upload->structSize < sizeof(*upload) ||
        !DeviceHandleIsLive(state, upload->texture, DEVICE_HANDLE_TEXTURE) ||
        upload->data == NULL || upload->sizeBytes > (uint64_t)SIZE_MAX)
        return 0u;

    const uint32_t index = DeviceHandleSlot(upload->texture) - 1u;
    const LaiueGraphicsTextureDescV1 *description = &state->textures[index];
    const uint64_t tightRowPitch = (uint64_t)description->extent.width * 4u;
    const uint64_t rowPitch = upload->rowPitchBytes == 0u
                                  ? tightRowPitch
                                  : (uint64_t)upload->rowPitchBytes;
    if (rowPitch != tightRowPitch ||
        (uint64_t)description->extent.height > UINT64_MAX / rowPitch ||
        upload->sizeBytes != rowPitch * description->extent.height ||
        state->backendResources[index] == NULL)
        return 0u;
    return RendererUploadTexture(
               state->renderer, (RendererTexture *)state->backendResources[index],
               upload->data, upload->sizeBytes, (uint32_t)rowPitch)
               ? 1u
               : 0u;
}

static void DeviceDestroyHandle(LaiueGraphicsDeviceV1 *device,
                                LaiueGraphicsHandle handle)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || !DeviceHandleIsLive(state, handle, 0u))
        return;
    const uint32_t index = DeviceHandleSlot(handle) - 1u;
    const uint8_t kind = state->kinds[index];
    /* A shader remains owned by the device while any pipeline references it.
     * The void destroy ABI cannot report an error, so a premature release is
     * a safe no-op instead of turning a live pipeline into a dangling handle. */
    if (kind == DEVICE_HANDLE_SHADER && state->shaderRefCounts[index] != 0u)
        return;
    if (kind == DEVICE_HANDLE_PIPELINE)
    {
        const LaiueGraphicsPipelineDescV1 *pipeline = &state->pipelines[index];
        if (pipeline->vertexShader != 0u &&
            DeviceHandleIsLive(state, pipeline->vertexShader, DEVICE_HANDLE_SHADER))
        {
            const uint32_t shaderIndex = DeviceHandleSlot(pipeline->vertexShader) - 1u;
            if (state->shaderRefCounts[shaderIndex] != 0u)
                --state->shaderRefCounts[shaderIndex];
        }
        if (pipeline->fragmentShader != 0u &&
            DeviceHandleIsLive(state, pipeline->fragmentShader, DEVICE_HANDLE_SHADER))
        {
            const uint32_t shaderIndex = DeviceHandleSlot(pipeline->fragmentShader) - 1u;
            if (state->shaderRefCounts[shaderIndex] != 0u)
                --state->shaderRefCounts[shaderIndex];
        }
    }
    if ((state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_INDEX) != 0u)
        for (uint32_t vertex = 0u; vertex < 256u; ++vertex)
            if (state->meshIndexHandles[vertex] == handle &&
                state->meshes[vertex] != NULL)
            {
                RendererDestroyMesh(state->renderer, state->meshes[vertex]);
                state->meshes[vertex] = NULL;
                state->vertexCounts[vertex] = 0u;
                state->meshIndexHandles[vertex] = 0u;
            }
    if (state->meshes[index] != NULL)
        RendererDestroyMesh(state->renderer, state->meshes[index]);
    state->meshes[index] = NULL;
    if (kind == DEVICE_HANDLE_TEXTURE && state->backendResources[index] != NULL)
        RendererDestroyTexture(state->renderer,
                               (RendererTexture *)state->backendResources[index]);
    else if (kind == DEVICE_HANDLE_SAMPLER && state->backendResources[index] != NULL)
        RendererDestroySampler(state->renderer,
                               (RendererSampler *)state->backendResources[index]);
    state->backendResources[index] = NULL;
    DeviceFree(state, state->storage[index]);
    state->storage[index] = NULL;
    state->live[index] = 0u;
    state->sizes[index] = 0u;
    state->usageFlags[index] = 0u;
    memset(&state->textures[index], 0, sizeof(state->textures[index]));
    memset(&state->samplers[index], 0, sizeof(state->samplers[index]));
    memset(&state->pipelines[index], 0, sizeof(state->pipelines[index]));
    if (kind != DEVICE_HANDLE_SHADER)
        state->shaderRefCounts[index] = 0u;
    state->shaderStages[index] = 0u;
    state->vertexCounts[index] = 0u;
    state->meshIndexHandles[index] = 0u;
    state->meshFirstIndices[index] = 0u;
    state->meshIndexCounts[index] = 0u;
    state->meshVertexOffsets[index] = 0;
    state->kinds[index] = 0u;
}

static uint32_t DeviceBeginFrame(LaiueGraphicsDeviceV1 *device, uint32_t width,
                                 uint32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || state->frameActive || width == 0u || height == 0u)
        return 0u;
    RendererFrameSetup frame;
    memset(&frame, 0, sizeof(frame));
    frame.passCount = 1u;
    frame.passes[0].rectMaxX = width;
    frame.passes[0].rectMaxY = height;
    if (state->cameraSet)
        memcpy(frame.passes[0].viewProjection, state->viewProjection,
               sizeof(state->viewProjection));
    else
    {
        /* Stable camera-relative orthographic fallback for V1 clients. */
        frame.passes[0].viewProjection[0] = 0.03f;
        frame.passes[0].viewProjection[5] = 0.03f;
        frame.passes[0].viewProjection[10] = 0.03f;
        frame.passes[0].viewProjection[15] = 1.0f;
        frame.passes[0].viewProjection[12] = -1.0f;
        frame.passes[0].viewProjection[13] = -1.0f;
    }
    frame.skyColor[0] = 0.035f;
    frame.skyColor[1] = 0.055f;
    frame.skyColor[2] = 0.09f;
    frame.ambientColor[0] = 0.18f;
    frame.ambientColor[1] = 0.18f;
    frame.ambientColor[2] = 0.18f;
    frame.sunDirection[1] = -1.0f;
    frame.sunColor[0] = 1.0f;
    frame.sunColor[1] = 1.0f;
    frame.sunColor[2] = 1.0f;
    frame.gamma = 1.0f;
    if (!RendererBeginFrame(state->renderer, &frame))
        return 0u;
    state->frameActive = true;
    state->submittedItems = 0u;
    RendererBeginScenePass(state->renderer, 0u);
    return 1u;
}

static bool DeviceValidateDraw(const LaiueGraphicsDeviceState *state,
                               LaiueGraphicsHandle pipeline,
                               LaiueGraphicsHandle vertexBuffer,
                               LaiueGraphicsHandle indexBuffer,
                               LaiueGraphicsHandle texture,
                               LaiueGraphicsHandle sampler)
{
    return state != NULL &&
           (pipeline == 0u ||
            (DeviceHandleIsLive(state, pipeline, DEVICE_HANDLE_PIPELINE) &&
             DevicePipelineHandlesAreLive(
                 state, &state->pipelines[DeviceHandleSlot(pipeline) - 1u]))) &&
           (vertexBuffer == 0u || DeviceHandleIsLive(state, vertexBuffer, DEVICE_HANDLE_BUFFER)) &&
           (indexBuffer == 0u || DeviceHandleIsLive(state, indexBuffer, DEVICE_HANDLE_BUFFER)) &&
           DeviceOptionalBindingsAreLive(state, texture, sampler);
}

static bool DeviceDrawMesh(const LaiueGraphicsDeviceState *state,
                           LaiueGraphicsHandle vertexBuffer,
                           const float origin[3], float scale,
                           uint32_t firstVertex, uint32_t vertexCount)
{
    if (state == NULL || vertexBuffer == 0u)
        return true;
    const uint32_t index = DeviceHandleSlot(vertexBuffer) - 1u;
    if (state->meshes[index] == NULL)
        return true;
    if (DeviceBufferIsGeneric(state, index))
    {
        const uint32_t available = state->vertexCounts[index];
        if (firstVertex > available ||
            (vertexCount != UINT32_MAX &&
             vertexCount > available - firstVertex))
            return false;
        RendererDrawGenericMeshRange(state->renderer, state->meshes[index], origin,
                                     scale == 0.0f ? 1.0f : scale, firstVertex,
                                     vertexCount);
        return true;
    }
    if ((state->usageFlags[index] & LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX_PULLING) == 0u)
        return true;
    if (scale == 0.0f || scale == 1.0f)
    {
        const float zeroOrigin[3] = {0.0f, 0.0f, 0.0f};
        RendererDrawMesh(state->renderer, state->meshes[index],
                         origin != NULL ? origin : zeroOrigin);
    }
    else
    {
        RendererMeshInstance instance;
        memset(&instance, 0, sizeof(instance));
        instance.originRelative[0] = origin != NULL ? origin[0] : 0.0f;
        instance.originRelative[1] = origin != NULL ? origin[1] : 0.0f;
        instance.originRelative[2] = origin != NULL ? origin[2] : 0.0f;
        instance.scale = scale;
        instance.rotation[3] = 1.0f;
        RendererDrawMeshInstances(state->renderer, state->meshes[index], &instance, 1u);
    }
    return true;
}

static uint32_t DeviceSubmit(LaiueGraphicsDeviceV1 *device,
                             const LaiueGraphicsDrawItemV1 *items, uint32_t itemCount)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || !state->frameActive || (itemCount != 0u && items == NULL))
        return 0u;
    for (uint32_t index = 0u; index < itemCount; ++index)
    {
        const LaiueGraphicsDrawItemV1 *item = &items[index];
        if (!DeviceValidateDraw(state, item->pipeline, item->vertexBuffer,
                                item->indexBuffer, 0u, 0u))
            return 0u;
        if (item->indexBuffer != 0u && item->vertexBuffer != 0u &&
            DeviceBufferIsGeneric(state, DeviceHandleSlot(item->vertexBuffer) - 1u) &&
            !DeviceBuildGenericMesh(state, DeviceHandleSlot(item->vertexBuffer) - 1u,
                                    item->indexBuffer, item->firstIndex,
                                    item->indexCount, item->vertexOffset))
            return 0u;
        if (!DeviceDrawMesh(state, item->vertexBuffer, NULL, 1.0f, 0u,
                            item->indexBuffer != 0u ? item->indexCount : UINT32_MAX))
            return 0u;
    }
    state->submittedItems += itemCount;
    return 1u;
}

static uint32_t DeviceSubmitUi(LaiueGraphicsDeviceV1 *device,
                               const LaiueGraphicsUiQuadV1 *quads,
                               uint32_t quadCount)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || !state->frameActive ||
        (quadCount != 0u && quads == NULL) ||
        quadCount > LAIUE_GRAPHICS_UI_MAX_QUADS)
        return 0u;
    if (quadCount != 0u)
        RendererUiQueue(state->renderer, (const RendererUiQuad *)quads, quadCount);
    state->submittedItems += quadCount;
    return 1u;
}

static uint32_t DeviceSetUiFontAtlas(LaiueGraphicsDeviceV1 *device,
                                     const uint8_t *alphaPixels, uint32_t width,
                                     uint32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || state->renderer == NULL || alphaPixels == NULL || width == 0u ||
        height == 0u)
        return 0u;
    return RendererUiSetFontAtlas(state->renderer, alphaPixels, width, height) ? 1u : 0u;
}

static uint32_t DeviceEndFrame(LaiueGraphicsDeviceV1 *device)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || !state->frameActive)
        return 0u;
    if (!RendererEndFrame(state->renderer))
    {
        state->frameActive = false;
        return 0u;
    }
    state->frameActive = false;
    return 1u;
}

static LaiueGraphicsDeviceState *DeviceV2State(LaiueGraphicsDeviceV2 *device)
{
    return device == NULL ? NULL : (LaiueGraphicsDeviceState *)device->context;
}

static const LaiueGraphicsDeviceState *DeviceV2StateConst(
    const LaiueGraphicsDeviceV2 *device)
{
    return device == NULL ? NULL :
        (const LaiueGraphicsDeviceState *)device->context;
}

static uint32_t DeviceV2Create(void *nativeWindow, int32_t width, int32_t height,
                               uint32_t backend, LaiueGraphicsDeviceV2 **outDevice)
{
    if (outDevice == NULL)
        return 0u;
    LaiueGraphicsDeviceV1 *base = NULL;
    if (!DeviceCreate(nativeWindow, width, height, backend, &base))
    {
        *outDevice = NULL;
        return 0u;
    }
    LaiueGraphicsDeviceState *state = DeviceState(base);
    *outDevice = state != NULL ? &state->deviceV2 : NULL;
    return *outDevice != NULL ? 1u : 0u;
}

static uint32_t DeviceV2CreateWithContext(void *moduleContext, void *nativeWindow,
                                          int32_t width, int32_t height, uint32_t backend,
                                          LaiueGraphicsDeviceV2 **outDevice)
{
    if (outDevice == NULL)
        return 0u;
    const LaiueGraphicsModuleState *module =
        (const LaiueGraphicsModuleState *)moduleContext;
    LaiueGraphicsDeviceV1 *base = NULL;
    if (module == NULL || !DeviceCreateWithContext(moduleContext, nativeWindow, width,
                                                   height, backend, &base))
    {
        *outDevice = NULL;
        return 0u;
    }
    LaiueGraphicsDeviceState *state = DeviceState(base);
    *outDevice = state != NULL ? &state->deviceV2 : NULL;
    return *outDevice != NULL ? 1u : 0u;
}

static void DeviceV2Destroy(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    if (state != NULL)
        DeviceDestroy(&state->device);
}

static uint32_t DeviceV2GetBackend(const LaiueGraphicsDeviceV2 *device)
{
    const LaiueGraphicsDeviceState *state = DeviceV2StateConst(device);
    return state == NULL ? LAIUE_GRAPHICS_BACKEND_AUTO : DeviceGetBackend(&state->device);
}

static void DeviceV2Resize(LaiueGraphicsDeviceV2 *device, int32_t width, int32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    if (state != NULL)
        DeviceResize(&state->device, width, height);
}

static uint32_t DeviceV2CreateBuffer(LaiueGraphicsDeviceV2 *device,
                                     const LaiueGraphicsBufferDescV1 *description,
                                     LaiueGraphicsHandle *outBuffer)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceCreateBuffer(&state->device, description, outBuffer);
}

static uint32_t DeviceV2CreateTexture(LaiueGraphicsDeviceV2 *device,
                                      const LaiueGraphicsTextureDescV1 *description,
                                      LaiueGraphicsHandle *outTexture)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceCreateTexture(&state->device, description, outTexture);
}

static uint32_t DeviceV2CreateSampler(LaiueGraphicsDeviceV2 *device,
                                      const LaiueGraphicsSamplerDescV1 *description,
                                      LaiueGraphicsHandle *outSampler)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceCreateSampler(&state->device, description, outSampler);
}

static uint32_t DeviceV2CreatePipeline(LaiueGraphicsDeviceV2 *device,
                                       const LaiueGraphicsPipelineDescV1 *description,
                                       LaiueGraphicsHandle *outPipeline)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceCreatePipeline(&state->device, description, outPipeline);
}

static uint32_t DeviceV2CreateShader(LaiueGraphicsDeviceV2 *device,
                                     const LaiueGraphicsShaderDescV1 *description,
                                     LaiueGraphicsHandle *outShader)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceCreateShader(&state->device, description, outShader);
}

static uint32_t DeviceV2UploadBuffer(LaiueGraphicsDeviceV2 *device,
                                     const LaiueGraphicsBufferUploadV1 *upload)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceUploadBuffer(&state->device, upload);
}

static uint32_t DeviceV2UploadTexture(LaiueGraphicsDeviceV2 *device,
                                      const LaiueGraphicsTextureUploadV1 *upload)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceUploadTexture(&state->device, upload);
}

static void DeviceV2DestroyHandle(LaiueGraphicsDeviceV2 *device,
                                  LaiueGraphicsHandle handle)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    if (state != NULL)
        DeviceDestroyHandle(&state->device, handle);
}

static uint32_t DeviceV2BeginFrame(LaiueGraphicsDeviceV2 *device, uint32_t width,
                                   uint32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceBeginFrame(&state->device, width, height);
}

static uint32_t DeviceV2SetCamera(LaiueGraphicsDeviceV2 *device,
                                   const LaiueGraphicsCameraV2 *camera)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    if (state == NULL || camera == NULL || camera->structSize < sizeof(*camera))
        return 0u;
    memcpy(state->viewProjection, camera->viewProjection,
           sizeof(state->viewProjection));
    state->cameraSet = true;
    return 1u;
}

static uint32_t DeviceV2Submit(LaiueGraphicsDeviceV2 *device,
                               const LaiueGraphicsDrawItemV2 *items, uint32_t itemCount)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    if (state == NULL || !state->frameActive || (itemCount != 0u && items == NULL))
        return 0u;
    for (uint32_t index = 0u; index < itemCount; ++index)
    {
        const LaiueGraphicsDrawItemV2 *item = &items[index];
        LaiueGraphicsHandle texture = 0u;
        LaiueGraphicsHandle sampler = 0u;
        if (item->structSize >= LAIUE_GRAPHICS_DRAW_ITEM_V2_RESOURCE_SIZE)
        {
            texture = item->texture;
            sampler = item->sampler;
        }
        if (item->structSize < LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE ||
            !DeviceValidateDraw(state, item->pipeline, item->vertexBuffer,
                                item->indexBuffer, texture, sampler))
            return 0u;
        if (item->vertexBuffer != 0u &&
            DeviceBufferIsGeneric(state, DeviceHandleSlot(item->vertexBuffer) - 1u))
        {
            const uint32_t vertexIndex = DeviceHandleSlot(item->vertexBuffer) - 1u;
            if (item->indexBuffer != 0u)
            {
                if (!DeviceBuildGenericMesh(state, vertexIndex, item->indexBuffer,
                                            item->firstIndex, item->indexCount,
                                            item->vertexOffset))
                    return 0u;
                if (!DeviceDrawMesh(state, item->vertexBuffer, item->originRelative,
                                    item->scale == 0.0f ? 1.0f : item->scale, 0u,
                                    item->indexCount))
                    return 0u;
            }
            else if (!DeviceDrawMesh(state, item->vertexBuffer, item->originRelative,
                                     item->scale == 0.0f ? 1.0f : item->scale, 0u,
                                     item->indexCount == 0u ? UINT32_MAX : item->indexCount))
                return 0u;
        }
        else if (!DeviceDrawMesh(state, item->vertexBuffer, item->originRelative,
                                 item->scale == 0.0f ? 1.0f : item->scale, 0u,
                                 UINT32_MAX))
            return 0u;
    }
    state->submittedItems += itemCount;
    return 1u;
}

static uint32_t DeviceV2SubmitUi(LaiueGraphicsDeviceV2 *device,
                                 const LaiueGraphicsUiQuadV1 *quads, uint32_t quadCount)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceSubmitUi(&state->device, quads, quadCount);
}

static uint32_t DeviceV2SetUiFontAtlas(LaiueGraphicsDeviceV2 *device,
                                       const uint8_t *pixels, uint32_t width,
                                       uint32_t height)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceSetUiFontAtlas(&state->device, pixels, width, height);
}

static uint32_t DeviceV2EndFrame(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsDeviceState *state = DeviceV2State(device);
    return state == NULL ? 0u : DeviceEndFrame(&state->device);
}

static const LaiueGraphicsServiceV1 service = {
    .structSize = sizeof(LaiueGraphicsServiceV1),
    .abiVersion = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
    .backendIsAvailable = RendererBackendIsAvailable,
    .createWithBackend = RendererCreateWithBackend,
    .getBackend = RendererGetBackend,
    .create = RendererCreate,
    .destroy = RendererDestroy,
    .prepareWorldFrom = RendererPrepareWorldFrom,
    .prepareWorld = RendererPrepareWorld,
    .releaseWorld = RendererReleaseWorld,
    .isWorldReady = RendererIsWorldReady,
    .beginFrame = RendererBeginFrame,
    .beginScenePass = RendererBeginScenePass,
    .endFrame = RendererEndFrame,
    .getStats = RendererGetStats,
    .setVerticalSync = RendererSetVerticalSync,
    .isVerticalSyncEnabled = RendererIsVerticalSyncEnabled,
    .uiSetFontAtlas = RendererUiSetFontAtlas,
    .uiLoadBackground = RendererUiLoadBackground,
    .uiQueue = RendererUiQueue,
    .createMesh = RendererCreateMesh,
    .destroyMesh = RendererDestroyMesh,
    .drawMesh = RendererDrawMesh,
    .drawMeshInstances = RendererDrawMeshInstances,
    .resize = RendererResize,
    .getTexturePackLoadStatus = RendererGetTexturePackLoadStatus,
    .setWireframe = RendererSetWireframe,
    .isWireframe = RendererIsWireframe,
};

static const LaiueGraphicsDeviceServiceV1 deviceServiceTemplate = {
    .structSize = sizeof(LaiueGraphicsDeviceServiceV1),
    .abiVersion = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
    .createDevice = DeviceCreate,
    .destroyDevice = DeviceDestroy,
    .getBackend = DeviceGetBackend,
    .resize = DeviceResize,
    .createDeviceWithContext = DeviceCreateWithContext,
};

static const LaiueGraphicsDeviceServiceV2 deviceServiceV2Template = {
    .structSize = sizeof(LaiueGraphicsDeviceServiceV2),
    .abiVersion = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
    .createDevice = DeviceV2Create,
    .destroyDevice = DeviceV2Destroy,
    .getBackend = DeviceV2GetBackend,
    .resize = DeviceV2Resize,
    .createDeviceWithContext = DeviceV2CreateWithContext,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    LaiueGraphicsModuleState *state =
        (LaiueGraphicsModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    state->content = NULL;
    state->deviceService = deviceServiceTemplate;
    state->deviceService.context = state;
    state->deviceServiceV2 = deviceServiceV2Template;
    state->deviceServiceV2.context = state;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    state->content = (const LaiueContentServiceV1 *)LaiueModuleQueryOptionalService(
        state->host, LAIUE_CONTENT_SERVICE_NAME,
        LAIUE_CONTENT_SERVICE_ABI_VERSION_1, sizeof(LaiueContentServiceV1));
    if (!RendererTryAcquireContentService(state, state->content))
    {
        state->content = NULL;
        return 0u;
    }
    LaiueModuleServiceV1 published = {
        .name = LAIUE_GRAPHICS_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->content = NULL;
        RendererReleaseContentService(state);
        return 0u;
    }
    LaiueModuleServiceV1 devicePublished = {
        .name = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
        .table = &state->deviceService,
        .tableSize = sizeof(state->deviceService),
    };
    if (state->host->publishService(state->host->context, &devicePublished) != LAIUE_MODULE_OK)
    {
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
        state->content = NULL;
        RendererReleaseContentService(state);
        return 0u;
    }
    LaiueModuleServiceV1 deviceV2Published = {
        .name = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
        .version = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
        .table = &state->deviceServiceV2,
        .tableSize = sizeof(state->deviceServiceV2),
    };
    if (state->host->publishService(state->host->context, &deviceV2Published) !=
        LAIUE_MODULE_OK)
    {
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_DEVICE_SERVICE_NAME);
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
        state->content = NULL;
        RendererReleaseContentService(state);
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
    {
        (void)state->host->unpublishService(state->host->context,
                                            LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2);
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_DEVICE_SERVICE_NAME);
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
    }
    if (state != NULL)
    {
        state->content = NULL;
        RendererReleaseContentService(state);
    }
}

static void ModuleDestroy(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        /* The content bridge stores the module state as its owner token.
         * Release that token before returning the state to the host allocator;
         * otherwise a failed start (where destroy runs without stop) leaves a
         * dangling owner pointer in the process-wide compatibility bridge. */
        RendererReleaseContentService(state);
        state->host = NULL;
        state->content = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {
    LAIUE_GRAPHICS_SERVICE_NAME,
    LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
    LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
};
static const LaiueModuleRequirementV1 optionalServices[] = {
    {LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = LAIUE_RENDER_PROVIDER_ID,
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = sizeof(provides) / sizeof(provides[0]),
        .optionalServices = optionalServices,
        .optionalCount = sizeof(optionalServices) / sizeof(optionalServices[0]),
        .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void)
{
    return &api;
}

const LaiueGraphicsServiceV1 *LaiueGraphicsGetStaticServiceV1(void)
{
    return &service;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
