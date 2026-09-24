#pragma once

/* Backend-neutral graphics service. It deliberately contains no chunk,
 * voxel, scene, or pack format. Voxel rendering is an adapter that consumes
 * this table and may be replaced without changing the graphics contract. */

#include <stdint.h>

#define LAIUE_GRAPHICS_ABI_VERSION_1 1u

/* Shared frame-boundary layout for backend-neutral UI draw lists.  The UI
 * provider produces these records; graphics backends only consume them and
 * do not need to know about widgets, scenes, or voxel geometry. */
#define LAIUE_GRAPHICS_UI_MAX_QUADS 2048u
#define LAIUE_GRAPHICS_UI_QUAD_TEXT 1u
#define LAIUE_GRAPHICS_UI_QUAD_IMAGE 2u

typedef struct LaiueGraphicsUiQuadV1
{
    float rect[4];
    float uv[4];
    uint32_t colorRGBA;
    float cornerRadius;
    uint32_t flags;
    uint32_t reserved;
} LaiueGraphicsUiQuadV1;

typedef uint64_t LaiueGraphicsHandle;

typedef struct LaiueGraphicsExtentV1
{
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} LaiueGraphicsExtentV1;

typedef struct LaiueGraphicsBufferDescV1
{
    uint32_t structSize;
    uint32_t usageFlags;
    uint64_t sizeBytes;
} LaiueGraphicsBufferDescV1;

typedef struct LaiueGraphicsTextureDescV1
{
    uint32_t structSize;
    uint32_t format;
    LaiueGraphicsExtentV1 extent;
    uint32_t mipLevels;
    uint32_t usageFlags;
} LaiueGraphicsTextureDescV1;

typedef struct LaiueGraphicsSamplerDescV1
{
    uint32_t structSize;
    uint32_t minFilter;
    uint32_t magFilter;
    uint32_t addressModeU;
    uint32_t addressModeV;
    uint32_t addressModeW;
} LaiueGraphicsSamplerDescV1;

typedef struct LaiueGraphicsShaderDescV1
{
    uint32_t structSize;
    uint32_t stage;
    const void *code;
    uint64_t codeSizeBytes;
} LaiueGraphicsShaderDescV1;

typedef struct LaiueGraphicsPipelineDescV1
{
    uint32_t structSize;
    uint32_t topology;
    uint32_t vertexStride;
    LaiueGraphicsHandle vertexShader;
    LaiueGraphicsHandle fragmentShader;
} LaiueGraphicsPipelineDescV1;

typedef struct LaiueGraphicsBufferUploadV1
{
    uint32_t structSize;
    LaiueGraphicsHandle buffer;
    uint64_t offsetBytes;
    const void *data;
    uint64_t sizeBytes;
} LaiueGraphicsBufferUploadV1;

typedef struct LaiueGraphicsDrawItemV1
{
    LaiueGraphicsHandle pipeline;
    LaiueGraphicsHandle vertexBuffer;
    LaiueGraphicsHandle indexBuffer;
    uint32_t indexCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
} LaiueGraphicsDrawItemV1;

typedef struct LaiueGraphicsDeviceV1 LaiueGraphicsDeviceV1;
typedef uint32_t (*LaiueGraphicsCreateBufferFn)(LaiueGraphicsDeviceV1 *,
                                                const LaiueGraphicsBufferDescV1 *,
                                                LaiueGraphicsHandle *outBuffer);
typedef uint32_t (*LaiueGraphicsCreateTextureFn)(LaiueGraphicsDeviceV1 *,
                                                 const LaiueGraphicsTextureDescV1 *,
                                                 LaiueGraphicsHandle *outTexture);
typedef uint32_t (*LaiueGraphicsCreateSamplerFn)(LaiueGraphicsDeviceV1 *,
                                                 const LaiueGraphicsSamplerDescV1 *,
                                                 LaiueGraphicsHandle *outSampler);
typedef uint32_t (*LaiueGraphicsCreatePipelineFn)(LaiueGraphicsDeviceV1 *,
                                                  const LaiueGraphicsPipelineDescV1 *,
                                                  LaiueGraphicsHandle *outPipeline);
typedef uint32_t (*LaiueGraphicsCreateShaderFn)(LaiueGraphicsDeviceV1 *,
                                                const LaiueGraphicsShaderDescV1 *,
                                                LaiueGraphicsHandle *outShader);
typedef uint32_t (*LaiueGraphicsUploadBufferFn)(LaiueGraphicsDeviceV1 *,
                                                const LaiueGraphicsBufferUploadV1 *upload);
typedef void (*LaiueGraphicsDestroyHandleFn)(LaiueGraphicsDeviceV1 *, LaiueGraphicsHandle);
typedef uint32_t (*LaiueGraphicsBeginFrameFn)(LaiueGraphicsDeviceV1 *, uint32_t width,
                                              uint32_t height);
typedef uint32_t (*LaiueGraphicsSubmitFn)(LaiueGraphicsDeviceV1 *,
                                          const LaiueGraphicsDrawItemV1 *items,
                                          uint32_t itemCount);
/* Queue backend-neutral UI quads for the current frame.  The record layout
 * is part of this contract, so UI and graphics remain separately replaceable
 * providers.  The operation is an optional tail extension: consumers must
 * gate it by structSize before calling. */
typedef uint32_t (*LaiueGraphicsSubmitUiFn)(LaiueGraphicsDeviceV1 *,
                                            const LaiueGraphicsUiQuadV1 *quads,
                                            uint32_t quadCount);
/* Upload the alpha font atlas used by text quads.  This remains optional so
 * a client that only emits coloured rectangles can run against an older
 * device provider. */
typedef uint32_t (*LaiueGraphicsSetUiFontAtlasFn)(LaiueGraphicsDeviceV1 *,
                                                  const uint8_t *alphaPixels,
                                                  uint32_t width, uint32_t height);
typedef uint32_t (*LaiueGraphicsEndFrameFn)(LaiueGraphicsDeviceV1 *);

struct LaiueGraphicsDeviceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void *context;
    LaiueGraphicsCreateBufferFn createBuffer;
    LaiueGraphicsCreateTextureFn createTexture;
    LaiueGraphicsCreateSamplerFn createSampler;
    LaiueGraphicsCreatePipelineFn createPipeline;
    LaiueGraphicsUploadBufferFn uploadBuffer;
    LaiueGraphicsDestroyHandleFn destroyHandle;
    LaiueGraphicsBeginFrameFn beginFrame;
    LaiueGraphicsSubmitFn submit;
    LaiueGraphicsEndFrameFn endFrame;
    /* Tail extension: older consumers still see their original fields and
     * gate optional operations by structSize. */
    LaiueGraphicsCreateShaderFn createShader;
    uintptr_t reserved[4];
    /* Appended after the original reserved tail so its offsets remain stable
     * for consumers built against the first revision. */
    LaiueGraphicsSubmitUiFn submitUi;
    LaiueGraphicsSetUiFontAtlasFn setUiFontAtlas;
};
