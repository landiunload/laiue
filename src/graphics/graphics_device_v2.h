#pragma once

/* Version 2 is a deliberately separate service name.  It keeps the V1
 * facade source-compatible while exposing a complete frame/device contract
 * with camera-relative draw origins.  All handles remain owned by the
 * provider instance and carry a device namespace, generation, and slot so a
 * handle from another device cannot accidentally validate. */

#include "graphics/graphics_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2 "laiue.graphics.device.v2"
#define LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2 2u
#define LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION 2u

typedef struct LaiueGraphicsDeviceV2 LaiueGraphicsDeviceV2;

typedef struct LaiueGraphicsDrawItemV2
{
    uint32_t structSize;
    uint32_t flags;
    LaiueGraphicsHandle pipeline;
    LaiueGraphicsHandle vertexBuffer;
    LaiueGraphicsHandle indexBuffer;
    uint32_t indexCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    float originRelative[3];
    float scale;
    /* Optional tail: old V2 clients may omit these fields.  When present,
     * the handles are validated against the same device namespace as the
     * pipeline and vertex/index buffers. */
    LaiueGraphicsHandle texture;
    LaiueGraphicsHandle sampler;
} LaiueGraphicsDrawItemV2;

#define LAIUE_GRAPHICS_DRAW_ITEM_V2_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueGraphicsDrawItemV2, texture))
#define LAIUE_GRAPHICS_DRAW_ITEM_V2_RESOURCE_SIZE \
    ((uint32_t)sizeof(LaiueGraphicsDrawItemV2))

/* Camera state is supplied in render-local coordinates. Providers never
 * receive the application's absolute origin, which keeps float precision
 * stable even when the simulation is many cells away from zero. */
typedef struct LaiueGraphicsCameraV2
{
    uint32_t structSize;
    uint32_t flags;
    float viewProjection[16];
} LaiueGraphicsCameraV2;

typedef uint32_t (*LaiueGraphicsV2CreateBufferFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsBufferDescV1 *,
    LaiueGraphicsHandle *outBuffer);
typedef uint32_t (*LaiueGraphicsV2CreateTextureFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsTextureDescV1 *,
    LaiueGraphicsHandle *outTexture);
typedef uint32_t (*LaiueGraphicsV2CreateSamplerFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsSamplerDescV1 *,
    LaiueGraphicsHandle *outSampler);
typedef uint32_t (*LaiueGraphicsV2CreatePipelineFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsPipelineDescV1 *,
    LaiueGraphicsHandle *outPipeline);
typedef uint32_t (*LaiueGraphicsV2CreateShaderFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsShaderDescV1 *,
    LaiueGraphicsHandle *outShader);
typedef uint32_t (*LaiueGraphicsV2UploadBufferFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsBufferUploadV1 *upload);
typedef void (*LaiueGraphicsV2DestroyHandleFn)(
    LaiueGraphicsDeviceV2 *, LaiueGraphicsHandle handle);
typedef uint32_t (*LaiueGraphicsV2BeginFrameFn)(
    LaiueGraphicsDeviceV2 *, uint32_t width, uint32_t height);
typedef uint32_t (*LaiueGraphicsV2SetCameraFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsCameraV2 *camera);
typedef uint32_t (*LaiueGraphicsV2SubmitFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsDrawItemV2 *, uint32_t itemCount);
typedef uint32_t (*LaiueGraphicsV2SubmitUiFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsUiQuadV1 *, uint32_t quadCount);
typedef uint32_t (*LaiueGraphicsV2SetUiFontAtlasFn)(
    LaiueGraphicsDeviceV2 *, const uint8_t *, uint32_t width, uint32_t height);
typedef uint32_t (*LaiueGraphicsV2UploadTextureFn)(
    LaiueGraphicsDeviceV2 *, const LaiueGraphicsTextureUploadV1 *upload);
typedef uint32_t (*LaiueGraphicsV2EndFrameFn)(LaiueGraphicsDeviceV2 *);

struct LaiueGraphicsDeviceV2
{
    uint32_t structSize;
    uint32_t abiVersion;
    void *context;
    LaiueGraphicsV2CreateBufferFn createBuffer;
    LaiueGraphicsV2CreateTextureFn createTexture;
    LaiueGraphicsV2CreateSamplerFn createSampler;
    LaiueGraphicsV2CreatePipelineFn createPipeline;
    LaiueGraphicsV2UploadBufferFn uploadBuffer;
    LaiueGraphicsV2DestroyHandleFn destroyHandle;
    LaiueGraphicsV2BeginFrameFn beginFrame;
    LaiueGraphicsV2SubmitFn submit;
    LaiueGraphicsV2EndFrameFn endFrame;
    LaiueGraphicsV2CreateShaderFn createShader;
    LaiueGraphicsV2SubmitUiFn submitUi;
    LaiueGraphicsV2SetUiFontAtlasFn setUiFontAtlas;
    /* Optional tail: 2D-only providers may omit camera control. */
    LaiueGraphicsV2SetCameraFn setCamera;
    LaiueGraphicsV2UploadTextureFn uploadTexture;
};

typedef uint32_t (*LaiueGraphicsDeviceV2CreateFn)(
    void *nativeWindow, int32_t width, int32_t height, uint32_t backend,
    LaiueGraphicsDeviceV2 **outDevice);
typedef uint32_t (*LaiueGraphicsDeviceV2CreateWithContextFn)(
    void *moduleContext, void *nativeWindow, int32_t width, int32_t height,
    uint32_t backend, LaiueGraphicsDeviceV2 **outDevice);
typedef void (*LaiueGraphicsDeviceV2DestroyFn)(LaiueGraphicsDeviceV2 *device);
typedef uint32_t (*LaiueGraphicsDeviceV2GetBackendFn)(
    const LaiueGraphicsDeviceV2 *device);
typedef void (*LaiueGraphicsDeviceV2ResizeFn)(
    LaiueGraphicsDeviceV2 *device, int32_t width, int32_t height);

typedef struct LaiueGraphicsDeviceServiceV2
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueGraphicsDeviceV2CreateFn createDevice;
    LaiueGraphicsDeviceV2DestroyFn destroyDevice;
    LaiueGraphicsDeviceV2GetBackendFn getBackend;
    LaiueGraphicsDeviceV2ResizeFn resize;
    uintptr_t reserved[8];
    LaiueGraphicsDeviceV2CreateWithContextFn createDeviceWithContext;
    void *context;
} LaiueGraphicsDeviceServiceV2;

#define LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueGraphicsDeviceServiceV2, createDeviceWithContext))
#define LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueGraphicsDeviceServiceV2, context) + sizeof(void *)))
