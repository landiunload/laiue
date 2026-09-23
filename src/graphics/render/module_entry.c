#include "render/graphics_service.h"
#include "render/content_provider.h"
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
} LaiueGraphicsModuleState;

typedef struct LaiueGraphicsDeviceState
{
    LaiueGraphicsDeviceV1 device;
    Renderer *renderer;
    uint32_t generations[256];
    uint64_t sizes[256];
    void *storage[256];
    uint8_t kinds[256];
    uint8_t live[256];
    uint32_t submittedItems;
    bool frameActive;
} LaiueGraphicsDeviceState;

enum
{
    DEVICE_HANDLE_BUFFER = 1u,
    DEVICE_HANDLE_TEXTURE = 2u,
    DEVICE_HANDLE_SAMPLER = 3u,
    DEVICE_HANDLE_PIPELINE = 4u,
};

static LaiueGraphicsDeviceState *DeviceState(LaiueGraphicsDeviceV1 *device)
{
    return device == NULL ? NULL : (LaiueGraphicsDeviceState *)device->context;
}

static const LaiueGraphicsDeviceState *DeviceStateConst(
    const LaiueGraphicsDeviceV1 *device)
{
    return device == NULL ? NULL :
        (const LaiueGraphicsDeviceState *)device->context;
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
            uint32_t generation = state->generations[index] + 1u;
            if (generation == 0u)
                generation = 1u;
            state->generations[index] = generation;
            state->sizes[index] = size;
            state->kinds[index] = kind;
            state->live[index] = 1u;
            *outHandle = ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
            return 1u;
        }
    return 0u;
}

static uint32_t DeviceHandleIsLive(const LaiueGraphicsDeviceState *state,
                                   LaiueGraphicsHandle handle, uint8_t expectedKind)
{
    const uint32_t index = (uint32_t)handle;
    const uint32_t generation = (uint32_t)(handle >> 32u);
    return state != NULL && index != 0u && index <= 256u && generation != 0u &&
                   state->live[index - 1u] != 0u &&
                   state->generations[index - 1u] == generation &&
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
static uint32_t DeviceUploadBuffer(LaiueGraphicsDeviceV1 *,
                                   const LaiueGraphicsBufferUploadV1 *);
static void DeviceDestroyHandle(LaiueGraphicsDeviceV1 *, LaiueGraphicsHandle);
static uint32_t DeviceBeginFrame(LaiueGraphicsDeviceV1 *, uint32_t, uint32_t);
static uint32_t DeviceSubmit(LaiueGraphicsDeviceV1 *,
                             const LaiueGraphicsDrawItemV1 *, uint32_t);
static uint32_t DeviceEndFrame(LaiueGraphicsDeviceV1 *);

static uint32_t DeviceCreate(void *nativeWindow, int32_t width, int32_t height,
                             uint32_t backend, LaiueGraphicsDeviceV1 **outDevice)
{
    if (outDevice == NULL)
        return 0u;
    *outDevice = NULL;
    LaiueGraphicsDeviceState *state =
        (LaiueGraphicsDeviceState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    Renderer *renderer = RendererCreateWithBackend(
        nativeWindow, width, height, (RendererBackendKind)backend);
    if (renderer == NULL)
    {
        PlatformFree(state);
        return 0u;
    }
    memset(state, 0, sizeof(*state));
    state->renderer = renderer;
    state->device.structSize = sizeof(state->device);
    state->device.abiVersion = LAIUE_GRAPHICS_ABI_VERSION_1;
    state->device.context = state;
    state->device.createBuffer = DeviceCreateBuffer;
    state->device.createTexture = DeviceCreateTexture;
    state->device.createSampler = DeviceCreateSampler;
    state->device.createPipeline = DeviceCreatePipeline;
    state->device.uploadBuffer = DeviceUploadBuffer;
    state->device.destroyHandle = DeviceDestroyHandle;
    state->device.beginFrame = DeviceBeginFrame;
    state->device.submit = DeviceSubmit;
    state->device.endFrame = DeviceEndFrame;
    *outDevice = &state->device;
    return 1u;
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
        PlatformFree(state->storage[index]);
    RendererDestroy(state->renderer);
    PlatformFree(state);
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
    if (state == NULL || description == NULL || outBuffer == NULL ||
        description->structSize < sizeof(*description) || description->sizeBytes == 0u)
        return 0u;
    if (!DeviceAllocateHandle(state, DEVICE_HANDLE_BUFFER, description->sizeBytes,
                              outBuffer))
        return 0u;
    const uint32_t index = (uint32_t)*outBuffer - 1u;
    if (description->sizeBytes > (uint64_t)SIZE_MAX ||
        (state->storage[index] = PlatformAllocate((size_t)description->sizeBytes, true)) == NULL)
    {
        DeviceDestroyHandle(device, *outBuffer);
        *outBuffer = 0u;
        return 0u;
    }
    return 1u;
}

static uint32_t DeviceCreateTexture(LaiueGraphicsDeviceV1 *device,
                                    const LaiueGraphicsTextureDescV1 *description,
                                    LaiueGraphicsHandle *outTexture)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || description == NULL || outTexture == NULL ||
        description->structSize < sizeof(*description) || description->extent.width == 0u ||
        description->extent.height == 0u || description->extent.depth == 0u)
        return 0u;
    return DeviceAllocateHandle(state, DEVICE_HANDLE_TEXTURE, 0u, outTexture);
}

static uint32_t DeviceCreateSampler(LaiueGraphicsDeviceV1 *device,
                                    const LaiueGraphicsSamplerDescV1 *description,
                                    LaiueGraphicsHandle *outSampler)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || description == NULL || outSampler == NULL ||
        description->structSize < sizeof(*description))
        return 0u;
    return DeviceAllocateHandle(state, DEVICE_HANDLE_SAMPLER, 0u, outSampler);
}

static uint32_t DeviceCreatePipeline(LaiueGraphicsDeviceV1 *device,
                                     const LaiueGraphicsPipelineDescV1 *description,
                                     LaiueGraphicsHandle *outPipeline)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || description == NULL || outPipeline == NULL ||
        description->structSize < sizeof(*description))
        return 0u;
    if ((description->vertexShader != 0u &&
         !DeviceHandleIsLive(state, description->vertexShader, 0u)) ||
        (description->fragmentShader != 0u &&
         !DeviceHandleIsLive(state, description->fragmentShader, 0u)))
        return 0u;
    return DeviceAllocateHandle(state, DEVICE_HANDLE_PIPELINE, 0u, outPipeline);
}

static uint32_t DeviceUploadBuffer(LaiueGraphicsDeviceV1 *device,
                                   const LaiueGraphicsBufferUploadV1 *upload)
{
    const LaiueGraphicsDeviceState *state = DeviceStateConst(device);
    if (state == NULL || upload == NULL || upload->structSize < sizeof(*upload) ||
        !DeviceHandleIsLive(state, upload->buffer, DEVICE_HANDLE_BUFFER) ||
        upload->data == NULL || upload->sizeBytes == 0u ||
        upload->sizeBytes > (uint64_t)SIZE_MAX ||
        upload->offsetBytes > UINT64_MAX - upload->sizeBytes)
        return 0u;
    const uint32_t index = (uint32_t)upload->buffer - 1u;
    if (upload->offsetBytes > state->sizes[index] ||
        upload->sizeBytes > state->sizes[index] - upload->offsetBytes)
        return 0u;
    memcpy((uint8_t *)state->storage[index] + (size_t)upload->offsetBytes,
           upload->data, (size_t)upload->sizeBytes);
    return 1u;
}

static void DeviceDestroyHandle(LaiueGraphicsDeviceV1 *device,
                                LaiueGraphicsHandle handle)
{
    LaiueGraphicsDeviceState *state = DeviceState(device);
    if (state == NULL || !DeviceHandleIsLive(state, handle, 0u))
        return;
    const uint32_t index = (uint32_t)handle - 1u;
    PlatformFree(state->storage[index]);
    state->storage[index] = NULL;
    state->live[index] = 0u;
    state->sizes[index] = 0u;
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
    frame.passCount = 0u;
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
    return 1u;
}

static uint32_t DeviceSubmit(LaiueGraphicsDeviceV1 *device,
                             const LaiueGraphicsDrawItemV1 *items, uint32_t itemCount)
{
    const LaiueGraphicsDeviceState *state = DeviceStateConst(device);
    if (state == NULL || !state->frameActive || (itemCount != 0u && items == NULL))
        return 0u;
    for (uint32_t index = 0u; index < itemCount; ++index)
    {
        const LaiueGraphicsDrawItemV1 *item = &items[index];
        if ((item->pipeline != 0u &&
             !DeviceHandleIsLive(state, item->pipeline, DEVICE_HANDLE_PIPELINE)) ||
            (item->vertexBuffer != 0u &&
             !DeviceHandleIsLive(state, item->vertexBuffer, DEVICE_HANDLE_BUFFER)) ||
            (item->indexBuffer != 0u &&
             !DeviceHandleIsLive(state, item->indexBuffer, DEVICE_HANDLE_BUFFER)))
            return 0u;
    }
    return 1u;
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

static const LaiueGraphicsDeviceServiceV1 deviceService = {
    .structSize = sizeof(LaiueGraphicsDeviceServiceV1),
    .abiVersion = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
    .createDevice = DeviceCreate,
    .destroyDevice = DeviceDestroy,
    .getBackend = DeviceGetBackend,
    .resize = DeviceResize,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    *outContext = NULL;
    LaiueGraphicsModuleState *state =
        (LaiueGraphicsModuleState *)PlatformAllocate(sizeof(*state), true);
    if (state == NULL)
        return 0u;
    state->host = host;
    state->content = NULL;
    RendererSetContentService(NULL);
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    RendererSetContentService(NULL);
    state->content = (const LaiueContentServiceV1 *)LaiueModuleQueryOptionalService(
        state->host, LAIUE_CONTENT_SERVICE_NAME,
        LAIUE_CONTENT_SERVICE_ABI_VERSION_1, sizeof(LaiueContentServiceV1));
    RendererSetContentService(state->content);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_GRAPHICS_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->content = NULL;
        RendererSetContentService(NULL);
        return 0u;
    }
    LaiueModuleServiceV1 devicePublished = {
        .name = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
        .version = LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
        .table = &deviceService,
        .tableSize = sizeof(deviceService),
    };
    if (state->host->publishService(state->host->context, &devicePublished) != LAIUE_MODULE_OK)
    {
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
        state->content = NULL;
        RendererSetContentService(NULL);
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
                                             LAIUE_GRAPHICS_DEVICE_SERVICE_NAME);
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_GRAPHICS_SERVICE_NAME);
    }
    if (state != NULL)
    {
        state->content = NULL;
        RendererSetContentService(NULL);
    }
}

static void ModuleDestroy(void *context)
{
    LaiueGraphicsModuleState *state = (LaiueGraphicsModuleState *)context;
    if (state != NULL)
    {
        state->host = NULL;
        state->content = NULL;
        PlatformFree(state);
    }
    RendererSetContentService(NULL);
}

static const char *const provides[] = {
    LAIUE_GRAPHICS_SERVICE_NAME,
    LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
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
