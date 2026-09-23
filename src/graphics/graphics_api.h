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
typedef void (*LaiueGraphicsDestroyHandleFn)(LaiueGraphicsDeviceV1 *, LaiueGraphicsHandle);
typedef uint32_t (*LaiueGraphicsBeginFrameFn)(LaiueGraphicsDeviceV1 *, uint32_t width,
                                              uint32_t height);
typedef uint32_t (*LaiueGraphicsSubmitFn)(LaiueGraphicsDeviceV1 *,
                                          const LaiueGraphicsDrawItemV1 *items,
                                          uint32_t itemCount);
typedef uint32_t (*LaiueGraphicsEndFrameFn)(LaiueGraphicsDeviceV1 *);

struct LaiueGraphicsDeviceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void *context;
    LaiueGraphicsCreateBufferFn createBuffer;
    LaiueGraphicsCreateTextureFn createTexture;
    LaiueGraphicsDestroyHandleFn destroyHandle;
    LaiueGraphicsBeginFrameFn beginFrame;
    LaiueGraphicsSubmitFn submit;
    LaiueGraphicsEndFrameFn endFrame;
    uintptr_t reserved[8];
};
