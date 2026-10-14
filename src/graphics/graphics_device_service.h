#pragma once

/* Runtime service for the backend-neutral graphics contract. The device is
 * opaque to consumers; backend implementations own its state and may map
 * these operations to D3D12, Vulkan, or another platform provider. */

#include "graphics/graphics_api.h"
#include "graphics/graphics_device_v2.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_GRAPHICS_DEVICE_SERVICE_NAME "laiue.graphics.device"
#define LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1 1u

enum
{
    LAIUE_GRAPHICS_BACKEND_AUTO = 0u,
    LAIUE_GRAPHICS_BACKEND_D3D12 = 1u,
    LAIUE_GRAPHICS_BACKEND_VULKAN = 2u,
};

typedef uint32_t (*LaiueGraphicsDeviceCreateFn)(
    void *nativeWindow, int32_t width, int32_t height, uint32_t backend,
    LaiueGraphicsDeviceV1 **outDevice);
/* Context-aware creation binds device-owned allocations to the provider
 * instance.  The original createDevice entry point remains for older hosts
 * that do not have an owning module context. */
typedef uint32_t (*LaiueGraphicsDeviceCreateWithContextFn)(
    void *moduleContext, void *nativeWindow, int32_t width, int32_t height,
    uint32_t backend, LaiueGraphicsDeviceV1 **outDevice);
typedef void (*LaiueGraphicsDeviceDestroyFn)(LaiueGraphicsDeviceV1 *device);
typedef uint32_t (*LaiueGraphicsDeviceGetBackendFn)(
    const LaiueGraphicsDeviceV1 *device);
typedef void (*LaiueGraphicsDeviceResizeFn)(
    LaiueGraphicsDeviceV1 *device, int32_t width, int32_t height);

typedef struct LaiueGraphicsDeviceServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueGraphicsDeviceCreateFn createDevice;
    LaiueGraphicsDeviceDestroyFn destroyDevice;
    LaiueGraphicsDeviceGetBackendFn getBackend;
    LaiueGraphicsDeviceResizeFn resize;
    uintptr_t reserved[8];
    LaiueGraphicsDeviceCreateWithContextFn createDeviceWithContext;
    void *context;
} LaiueGraphicsDeviceServiceV1;

/* Consumers that only need the original device lifecycle must request this
 * prefix, not sizeof(LaiueGraphicsDeviceServiceV1).  That keeps a provider
 * built before the context-aware tail usable with a newer host. */
#define LAIUE_GRAPHICS_DEVICE_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueGraphicsDeviceServiceV1, createDeviceWithContext))
#define LAIUE_GRAPHICS_DEVICE_SERVICE_V1_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueGraphicsDeviceServiceV1, context) + sizeof(void *)))
