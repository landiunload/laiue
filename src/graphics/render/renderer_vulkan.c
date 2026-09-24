// Vulkan-бэкенд рендерера: тот же контракт renderer.h, что и у D3D12.
// Без native handle он остаётся deterministic offscreen provider; при
// переданном HWND/X11 handle/ANativeWindow создаёт соответствующую surface,
// swapchain и present, не меняя публичный контракт.
//
// Шейдеры те же самые HLSL, скомпилированные glslang в SPIR-V. Регистры
// HLSL переводятся в один descriptor set сдвигами (cmake/LaiueShader.cmake):
// b0 -> 0, t0..t3 -> 1..4, s0 -> 5.

#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "render/texture_pack_internal.h"
#include "render/content_provider.h"
#include "platform/system.h"

// Оконный вывод выбирается платформенным provider-ом. Рендерер не знает
// Window и принимает только native surface payload: HWND на Windows,
// {Display, Window} на X11 и ANativeWindow на Android.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#if defined(LAIUE_RENDER_HAS_X11)
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#include <vulkan/vulkan.h>

#define LaiueContentCatalogDefault RendererContentDefaultCatalog

#if defined(_WIN32)
#include <vulkan/vulkan_win32.h>
#endif
#if defined(LAIUE_RENDER_HAS_X11)
#include <vulkan/vulkan_xlib.h>
typedef struct LaiueWindowNativeHandleV1
{
    void *display;
    uintptr_t window;
} LaiueWindowNativeHandleV1;
#endif
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#endif

#include <stddef.h>
#include <string.h>

#include "render/generated/vulkan/chunk_vs.h"
#include "render/generated/vulkan/chunk_ps.h"
#include "render/generated/vulkan/panorama_vs.h"
#include "render/generated/vulkan/panorama_ps.h"
#include "render/generated/vulkan/ui_vs.h"
#include "render/generated/vulkan/ui_ps.h"
#include "render/generated/vulkan/generic_vs.h"
#include "render/generated/vulkan/generic_ps.h"

// Публичное имя RendererDestroy теперь живёт в renderer_dispatch.c, а
// путь отката внутри RendererCreate_Vulkan зовёт суффиксную реализацию
// раньше её определения — объявляем её здесь.
void RendererDestroy_Vulkan(Renderer *renderer);

#define FRAME_COUNT 2

// Столько образов swapchain максимум держим наготове. minImageCount с
// этого GPU — 2, максимум — 8; всё, что выше, движок не поддерживает.
#define MAX_SWAPCHAIN_IMAGES 8u

// Раскладка дескрипторов повторяет сдвиги регистров HLSL.
#define BINDING_CONSTANTS 0u
#define BINDING_QUAD_BUFFER 1u
#define BINDING_BLOCK_TEXTURES 2u
#define BINDING_BLOCK_NORMALS 3u
#define BINDING_INSTANCES 4u
#define BINDING_SAMPLER 5u
#define BINDING_PANORAMA_CUBE 1u
#define BINDING_UI_QUADS 1u
#define BINDING_UI_FONT 2u
#define BINDING_UI_BACKGROUND 3u
#define BINDING_COUNT 6u

// Слой UI: размер квада держать в синхроне с shaders/ui.hlsl.
#define UI_QUAD_BYTES 48

// Пул геометрии: меши суб-аллоцируются из больших device-local буферов.
#define POOL_BLOCK_BYTES (4u * 1024u * 1024u)
#define MAX_POOL_BLOCKS 64

#define DEFERRED_RELEASE_CAPACITY 256
#define MAX_PENDING_UPLOADS 64
#define MESH_UPLOAD_BYTES_PER_FRAME (4u * 1024u * 1024u)
// Крупные записи (один большой меш или всплеск чанков, который не влезает
// в основное кольцо) идут в отдельную арену с тем же двойным
// буферированием. Она создаётся лениво, при первом переполнении, и
// переиспользуется каждый кадр, поэтому на кадр не создаётся буфер.
#define LARGE_MESH_UPLOAD_BYTES_PER_FRAME (8u * 1024u * 1024u)
// Инстансы идут в независимых storage-буферах: descriptor set фиксирует
// конкретный буфер до конца командного списка, поэтому растить один
// VkBuffer заменой нельзя без копирования уже записанных данных. Чанки
// создаются лениво и живут до RendererDestroy; курсор слота сбрасывается
// только после ожидания его fence.
#define INSTANCE_CHUNK_BYTES (1u * 1024u * 1024u)
#define INSTANCE_MAX_BYTES_PER_FRAME (64u * 1024u * 1024u)
#define INSTANCE_MAX_CHUNKS_PER_FRAME 64u
// Кольцо констант: D3D12 переписывает корневые константы на каждый
// вызов отрисовки, у Vulkan та же роль у uniform-буфера с динамическим
// смещением, поэтому на кадр нужен свой диапазон.
#define CONSTANT_BYTES_PER_FRAME (4u * 1024u * 1024u)

#define COLOR_FORMAT VK_FORMAT_R8G8B8A8_SRGB
#define CAPTURE_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

// Константы кадра. Раскладка обязана совпадать с cbuffer в chunk.hlsl:
// glslang переносит правила упаковки HLSL в SPIR-V без изменений, поэтому
// смещения 0, 64, 76, 80, 92, 96, 112, 124 общие для обоих бэкендов.
typedef struct ChunkConstants
{
    float viewProjection[16];
    float chunkOriginRelative[3];
    float meshScale;
    float sunDirection[3];
    float materialCount;
    float sunColor[3];
    float reserved;
    float ambientColor[3];
    float gammaInverse;
    // По байту слоя на материал: кадр анимации выбран процессором, и
    // шейдеру остаётся только достать номер.
    uint32_t materialSlices[16];
} ChunkConstants;

_Static_assert(sizeof(ChunkConstants) == 192,
    "ChunkConstants shares its layout with the HLSL cbuffer");
_Static_assert(offsetof(ChunkConstants, materialSlices) == 128,
    "the slice table follows the lighting rows");
_Static_assert(offsetof(ChunkConstants, sunDirection) == 80,
    "HLSL packs float3 chunkOriginRelative and float meshScale into one row");
_Static_assert(offsetof(ChunkConstants, gammaInverse) == 124,
    "gammaInverse occupies the free w component of ambientColor");

typedef struct ResolveConstants
{
    float fovHalfRadians;
    float verticalScale;
    uint32_t mapping;
    uint32_t reserved;
} ResolveConstants;

typedef struct UiConstants
{
    float screenSize[2];
    float reserved[2];
} UiConstants;

typedef struct GpuBuffer
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize sizeBytes;
    uint8_t *mapped;
} GpuBuffer;

typedef struct GpuImage
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;
    uint32_t width;
    uint32_t height;
    uint32_t layerCount;
} GpuImage;

struct RendererTexture
{
    GpuImage image;
};

typedef struct FreeRange
{
    uint32_t offset;
    uint32_t size;
} FreeRange;

typedef struct GeometryPoolBlock
{
    GpuBuffer buffer;
    uint32_t totalBytes;
    FreeRange *freeRanges;      // отсортированы по offset, соседние слиты
    uint32_t freeRangeCount;
    uint32_t freeRangeCapacity;
    // Один набор на кадровый слот и instance-чank. Набор намеренно не
    // переиспользуется для другого VkBuffer: он может быть уже записан в
    // ожидающий командный буфер.
    VkDescriptorSet sets[INSTANCE_MAX_CHUNKS_PER_FRAME][FRAME_COUNT];
} GeometryPoolBlock;

typedef struct PendingUpload
{
    VkBuffer staging;
    VkDeviceMemory stagingMemory;   // не NULL только у собственного буфера
    uint32_t sourceOffset;
    uint32_t blockIndex;
    uint32_t destinationOffset;
    uint32_t sizeBytes;
} PendingUpload;

typedef struct DeferredBufferRelease
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint64_t safeFrameIndex;
} DeferredBufferRelease;

typedef struct DeferredRangeRelease
{
    uint32_t blockIndex;
    uint32_t offset;
    uint32_t size;
    uint64_t safeFrameIndex;
} DeferredRangeRelease;

struct RendererMesh
{
    uint32_t blockIndex;
    uint32_t offsetBytes;
    uint32_t sizeBytes;
    // Выровненный диапазон, реально занятый в блоке пула: его же возвращает
    // PoolFree, иначе хвост выравнивания оставался бы занятым навсегда.
    uint32_t poolSpanBytes;
    uint32_t quadCount;
    uint32_t vertexCount;
    bool generic;
};

typedef struct BlockTextureReplacement
{
    GpuImage albedo;
    GpuImage normals;
    uint32_t layerCount;
} BlockTextureReplacement;

// Итог вывода готового colorTarget в swapchain.
typedef enum PresentOutcome
{
    PRESENT_OUTCOME_NONE = 0,   // поверхность не используется (offscreen)
    PRESENT_OUTCOME_PRESENT,    // образ захвачен и записан — нужен present
    PRESENT_OUTCOME_SKIP,       // swapchain устарел: кадр offscreen, без present
} PresentOutcome;

struct Renderer
{
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    VkDeviceSize uniformAlignment;
    VkDeviceSize storageAlignment;
    VkDevice device;
    /* Vulkan 1.3 commands are loaded from the device rather than imported
     * from libvulkan.  Android's loader intentionally exports only the
     * dispatch entry points, so taking these addresses also keeps the
     * desktop and mobile providers on the same ABI-safe path. */
    PFN_vkCmdBeginRendering cmdBeginRendering;
    PFN_vkCmdEndRendering cmdEndRendering;
    bool useDynamicRendering;
    VkRenderPass colorRenderPass;
    VkRenderPass depthRenderPass;
    VkFramebuffer colorFramebuffers[FRAME_COUNT];
    VkFramebuffer depthFramebuffers[FRAME_COUNT];
    VkFramebuffer cubeFramebuffers[6];
    uint32_t queueFamily;
    VkQueue queue;
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffers[FRAME_COUNT];
    VkFence frameFences[FRAME_COUNT];
    uint32_t frameIndex;
    uint64_t submittedFrames;   // растёт монотонно, задаёт срок отложенных освобождений

    // Цели кадра. Без swapchain «показанный» кадр — это последний
    // отрисованный образ, который остаётся доступным для чтения.
    GpuImage colorTargets[FRAME_COUNT];
    GpuImage depthTarget;
    int32_t windowWidth;
    int32_t windowHeight;
    int32_t resizeWidth;
    int32_t resizeHeight;
    bool resizeRequested;
    bool lastFrameValid;
    uint32_t lastFrameIndex;

    // Панорама: кубмапа сцены и её резолв.
    GpuImage cubeColor;
    VkImageView cubeFaceViews[6];
    GpuImage cubeDepth;
    uint32_t cubeResolution;

    VkDescriptorPool descriptorPool;
    VkDescriptorSetLayout chunkSetLayout;
    VkDescriptorSetLayout resolveSetLayout;
    VkDescriptorSetLayout uiSetLayout;
    VkPipelineLayout chunkPipelineLayout;
    VkPipelineLayout resolvePipelineLayout;
    VkPipelineLayout uiPipelineLayout;
    VkPipeline chunkPipeline;
    VkPipeline genericPipeline;
    VkPipeline resolvePipeline;
    VkPipeline uiPipeline;
    VkDescriptorSet resolveSets[FRAME_COUNT];
    VkDescriptorSet uiSets[FRAME_COUNT];
    VkSampler sampler;

    // Нейтральные заглушки: шейдер статически читает все свои ресурсы,
    // поэтому дескриптор обязан быть валиден даже до загрузки пака.
    GpuImage fallbackArray;
    GpuImage fallbackImage;
    GpuImage fallbackCube;

    GpuImage blockTexture;
    GpuImage blockNormalTexture;
    TexturePackAnimationSet blockAnimation;
    TexturePackMaterialNames materialNames;
    GpuImage fontTexture;
    bool fontReady;

    GpuBuffer constantBuffers[FRAME_COUNT];
    uint32_t constantOffsets[FRAME_COUNT];
    GpuBuffer meshUploadBuffers[FRAME_COUNT];
    uint32_t meshUploadOffsets[FRAME_COUNT];
    GpuBuffer largeMeshUploadBuffers[FRAME_COUNT];
    uint32_t largeMeshUploadOffsets[FRAME_COUNT];
    GpuBuffer instanceBuffers[FRAME_COUNT][INSTANCE_MAX_CHUNKS_PER_FRAME];
    uint32_t instanceChunkCount[FRAME_COUNT];
    uint32_t instanceChunkIndex[FRAME_COUNT];
    uint32_t instanceChunkOffset[FRAME_COUNT];
    uint32_t instancePoolBytes[FRAME_COUNT];
    GpuBuffer uiQuadBuffers[FRAME_COUNT];
    uint32_t uiQuadCount;

    RendererFrameSetup frame;
    ChunkConstants chunkConstants;
    bool renderingActive;
    bool frameRecording;

    GeometryPoolBlock poolBlocks[MAX_POOL_BLOCKS];
    uint32_t poolBlockCount;
    // pool accounting
    // Инкрементальные счётчики вместо обхода всех блоков и свободных
    // диапазонов в RendererGetStats_Vulkan: capacity — сумма размеров
    // блоков, used — сумма выданных мешам выровненных диапазонов. Оба
    // меняются только при создании блока, успешном PoolAllocate, успешном
    // PoolFree и обоих способах освобождения блока.
    uint64_t poolCapacityBytes;
    uint64_t poolUsedBytes;

    PendingUpload pendingUploads[MAX_PENDING_UPLOADS];
    uint32_t pendingUploadCount;

    DeferredBufferRelease deferredBuffers[DEFERRED_RELEASE_CAPACITY];
    uint32_t deferredBufferHead;
    uint32_t deferredBufferCount;

    DeferredRangeRelease deferredRanges[DEFERRED_RELEASE_CAPACITY];
    uint32_t deferredRangeHead;
    uint32_t deferredRangeCount;

    void *loadedShaders[LAIUE_SHADER_SLOT_COUNT];
    uint32_t loadedShaderLengths[LAIUE_SHADER_SLOT_COUNT];

    RendererStats currentStats;
    RendererStats lastStats;
    RendererContentStatus texturePackLoadStatus;
    bool verticalSyncEnabled;
    bool wireframeEnabled;
    bool worldReady;

    // swapchain: вывод уже нарисованного colorTarget в окно. Все поля
    // добавлены одним блоком в конец структуры, чтобы слияние с правками
    // пула геометрии было бесконфликтным.
    bool hasSurface;
    bool swapchainOutOfDate;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    uint32_t swapchainImageCount;
    uint32_t swapchainImageIndex;
    uint32_t swapchainRecreateCount;   // счётчик пересозданий (диагностика)
    VkImage swapchainImages[MAX_SWAPCHAIN_IMAGES];
    VkImageView swapchainViews[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore imageAvailable[FRAME_COUNT];
    VkSemaphore renderFinished[MAX_SWAPCHAIN_IMAGES];
};

// === Мелкие помощники ===

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

// Сколько байт блока занимает выдача sizeBytes с учётом требования
// выравнивания динамического storage-смещения. PoolAllocate и последующее
// освобождение обязаны считать один и тот же диапазон, иначе хвост
// выравнивания остаётся занятым навсегда.
static uint32_t PoolSpanBytes(const Renderer *renderer, uint32_t sizeBytes)
{
    uint32_t alignment = (uint32_t)renderer->storageAlignment;
    if (alignment == 0u) alignment = 16u;
    return AlignUp(sizeBytes, alignment);
}

static bool FindMemoryType(const Renderer *renderer, uint32_t typeBits,
                           VkMemoryPropertyFlags required, uint32_t *outIndex)
{
    for (uint32_t index = 0; index < renderer->memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeBits & (1u << index)) == 0u) continue;
        if ((renderer->memoryProperties.memoryTypes[index].propertyFlags & required) != required)
            continue;
        *outIndex = index;
        return true;
    }
    return false;
}

static void BufferDestroy(Renderer *renderer, GpuBuffer *buffer)
{
    if (buffer->mapped != NULL)
    {
        vkUnmapMemory(renderer->device, buffer->memory);
        buffer->mapped = NULL;
    }
    if (buffer->buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(renderer->device, buffer->buffer, NULL);
    if (buffer->memory != VK_NULL_HANDLE)
        vkFreeMemory(renderer->device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static bool BufferCreate(Renderer *renderer, VkDeviceSize sizeBytes, VkBufferUsageFlags usage,
                         bool hostVisible, GpuBuffer *outBuffer)
{
    memset(outBuffer, 0, sizeof(*outBuffer));
    if (sizeBytes == 0) return false;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeBytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(renderer->device, &bufferInfo, NULL, &outBuffer->buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(renderer->device, outBuffer->buffer, &requirements);

    VkMemoryPropertyFlags properties = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t typeIndex = 0;
    if (!FindMemoryType(renderer, requirements.memoryTypeBits, properties, &typeIndex))
    {
        BufferDestroy(renderer, outBuffer);
        return false;
    }

    VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = typeIndex,
    };
    if (vkAllocateMemory(renderer->device, &allocateInfo, NULL, &outBuffer->memory) != VK_SUCCESS)
    {
        BufferDestroy(renderer, outBuffer);
        return false;
    }
    if (vkBindBufferMemory(renderer->device, outBuffer->buffer, outBuffer->memory, 0) != VK_SUCCESS)
    {
        BufferDestroy(renderer, outBuffer);
        return false;
    }
    if (hostVisible &&
        vkMapMemory(renderer->device, outBuffer->memory, 0, VK_WHOLE_SIZE, 0,
                    (void **)&outBuffer->mapped) != VK_SUCCESS)
    {
        BufferDestroy(renderer, outBuffer);
        return false;
    }
    outBuffer->sizeBytes = sizeBytes;
    return true;
}

static uint32_t InstanceStorageAlignment(const Renderer *renderer)
{
    // firstInstance must express the byte offset as a whole shader element.
    VkDeviceSize alignment = renderer->storageAlignment;
    if (alignment < 16u) alignment = 16u;
    if (alignment < sizeof(RendererMeshInstance)) alignment = sizeof(RendererMeshInstance);
    if (alignment > UINT32_MAX) return 0u;
    return (uint32_t)alignment;
}

static bool AlignInstanceBytes(const Renderer *renderer, uint32_t requestedBytes,
                               uint32_t *outCapacityBytes)
{
    uint32_t alignment = InstanceStorageAlignment(renderer);
    if (alignment == 0u || requestedBytes == 0u ||
        requestedBytes > UINT32_MAX - alignment + 1u)
        return false;
    uint32_t capacityBytes = AlignUp(requestedBytes, alignment);
    if (capacityBytes < requestedBytes) return false;
    *outCapacityBytes = capacityBytes;
    return true;
}

static bool CreateVulkanInstanceChunk(Renderer *renderer, uint32_t frameIndex,
                                      uint32_t chunkIndex, uint32_t requestedBytes)
{
    if (frameIndex >= FRAME_COUNT || chunkIndex >= INSTANCE_MAX_CHUNKS_PER_FRAME ||
        requestedBytes == 0u)
        return false;

    uint32_t capacityBytes = 0u;
    if (!AlignInstanceBytes(renderer, requestedBytes, &capacityBytes)) return false;

    return BufferCreate(renderer, capacityBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                        &renderer->instanceBuffers[frameIndex][chunkIndex]);
}

static void ImageDestroy(Renderer *renderer, GpuImage *image)
{
    if (image->view != VK_NULL_HANDLE)
        vkDestroyImageView(renderer->device, image->view, NULL);
    if (image->image != VK_NULL_HANDLE)
        vkDestroyImage(renderer->device, image->image, NULL);
    if (image->memory != VK_NULL_HANDLE)
        vkFreeMemory(renderer->device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
}

static bool ImageCreate(Renderer *renderer, uint32_t width, uint32_t height, uint32_t layerCount,
                        VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImageViewType viewType, bool cubeCompatible, GpuImage *outImage)
{
    memset(outImage, 0, sizeof(*outImage));

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cubeCompatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1u },
        .mipLevels = 1u,
        .arrayLayers = layerCount,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(renderer->device, &imageInfo, NULL, &outImage->image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(renderer->device, outImage->image, &requirements);
    uint32_t typeIndex = 0;
    if (!FindMemoryType(renderer, requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
    {
        ImageDestroy(renderer, outImage);
        return false;
    }
    VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = typeIndex,
    };
    if (vkAllocateMemory(renderer->device, &allocateInfo, NULL, &outImage->memory) != VK_SUCCESS ||
        vkBindImageMemory(renderer->device, outImage->image, outImage->memory, 0) != VK_SUCCESS)
    {
        ImageDestroy(renderer, outImage);
        return false;
    }

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = outImage->image,
        .viewType = viewType,
        .format = format,
        .subresourceRange = { aspect, 0u, 1u, 0u, layerCount },
    };
    if (vkCreateImageView(renderer->device, &viewInfo, NULL, &outImage->view) != VK_SUCCESS)
    {
        ImageDestroy(renderer, outImage);
        return false;
    }

    outImage->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    outImage->width = width;
    outImage->height = height;
    outImage->layerCount = layerCount;
    return true;
}

/* Vulkan 1.3 dynamic rendering is the fast path, but Android devices still
 * commonly expose only Vulkan 1.2.  Keep the same backend usable there with a
 * small render-pass compatibility path.  The render passes intentionally use
 * LOAD for color/depth; callers clear attachments explicitly when a pass
 * starts, so the same compatible pipeline can be reused by the resolve and UI
 * passes without creating duplicate pipeline layouts. */
static bool CreateRenderPasses(Renderer *renderer)
{
    VkAttachmentDescription color = {
        .format = COLOR_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference colorReference = {
        .attachment = 0u,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription colorSubpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &colorReference,
    };
    VkRenderPassCreateInfo colorInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1u,
        .pAttachments = &color,
        .subpassCount = 1u,
        .pSubpasses = &colorSubpass,
    };
    if (vkCreateRenderPass(renderer->device, &colorInfo, NULL, &renderer->colorRenderPass) !=
        VK_SUCCESS)
        return false;

    VkAttachmentDescription attachments[2] = { color };
    attachments[1] = (VkAttachmentDescription){
        .format = DEPTH_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depthReference = {
        .attachment = 1u,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription depthSubpass = colorSubpass;
    depthSubpass.pDepthStencilAttachment = &depthReference;
    VkRenderPassCreateInfo depthInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2u,
        .pAttachments = attachments,
        .subpassCount = 1u,
        .pSubpasses = &depthSubpass,
    };
    if (vkCreateRenderPass(renderer->device, &depthInfo, NULL, &renderer->depthRenderPass) !=
        VK_SUCCESS)
    {
        vkDestroyRenderPass(renderer->device, renderer->colorRenderPass, NULL);
        renderer->colorRenderPass = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool CreateFrameTargetFramebuffers(Renderer *renderer)
{
    if (renderer->useDynamicRendering) return true;
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame)
    {
        VkImageView colorAttachments[1] = { renderer->colorTargets[frame].view };
        VkFramebufferCreateInfo colorInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderer->colorRenderPass,
            .attachmentCount = 1u,
            .pAttachments = colorAttachments,
            .width = renderer->colorTargets[frame].width,
            .height = renderer->colorTargets[frame].height,
            .layers = 1u,
        };
        if (vkCreateFramebuffer(renderer->device, &colorInfo, NULL,
                                &renderer->colorFramebuffers[frame]) != VK_SUCCESS)
            return false;

        VkImageView depthAttachments[2] = {
            renderer->colorTargets[frame].view, renderer->depthTarget.view,
        };
        VkFramebufferCreateInfo depthInfo = colorInfo;
        depthInfo.renderPass = renderer->depthRenderPass;
        depthInfo.attachmentCount = 2u;
        depthInfo.pAttachments = depthAttachments;
        if (vkCreateFramebuffer(renderer->device, &depthInfo, NULL,
                                &renderer->depthFramebuffers[frame]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static bool CreateCubeFramebuffers(Renderer *renderer)
{
    if (renderer->useDynamicRendering) return true;
    for (uint32_t face = 0u; face < 6u; ++face)
    {
        VkImageView attachments[2] = { renderer->cubeFaceViews[face], renderer->cubeDepth.view };
        VkFramebufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderer->depthRenderPass,
            .attachmentCount = 2u,
            .pAttachments = attachments,
            .width = renderer->cubeColor.width,
            .height = renderer->cubeColor.height,
            .layers = 1u,
        };
        if (vkCreateFramebuffer(renderer->device, &info, NULL,
                                &renderer->cubeFramebuffers[face]) != VK_SUCCESS)
            return false;
    }
    return true;
}

// Переход раскладки образа. Барьер намеренно широкий: кадр движка
// содержит десятки переходов, а не тысячи, и точная маска стадий здесь
// не окупает риск ошибки синхронизации.
static void ImageBarrier(VkCommandBuffer commandBuffer, GpuImage *image, VkImageAspectFlags aspect,
                         VkImageLayout newLayout)
{
    if (image->layout == newLayout) return;

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = image->layout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = { aspect, 0u, 1u, 0u, image->layerCount },
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    image->layout = newLayout;
}

// Одноразовый командный буфер для загрузок вне кадра.
static VkCommandBuffer BeginImmediate(Renderer *renderer)
{
    VkCommandBufferAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = renderer->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(renderer->device, &allocateInfo, &commandBuffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(renderer->device, renderer->commandPool, 1u, &commandBuffer);
        return VK_NULL_HANDLE;
    }
    return commandBuffer;
}

static bool EndImmediate(Renderer *renderer, VkCommandBuffer commandBuffer)
{
    bool ok = vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
    if (ok)
    {
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1u,
            .pCommandBuffers = &commandBuffer,
        };
        ok = vkQueueSubmit(renderer->queue, 1u, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS &&
             vkQueueWaitIdle(renderer->queue) == VK_SUCCESS;
    }
    vkFreeCommandBuffers(renderer->device, renderer->commandPool, 1u, &commandBuffer);
    return ok;
}

// === Пул геометрии ===

static bool PoolBlockCreate(Renderer *renderer, uint32_t minimumBytes, uint32_t *outBlockIndex)
{
    if (renderer->poolBlockCount == MAX_POOL_BLOCKS) return false;

    uint32_t blockBytes = POOL_BLOCK_BYTES;
    while (blockBytes < minimumBytes)
    {
        if (blockBytes > UINT32_MAX / 2u) return false;
        blockBytes *= 2u;
    }

    GeometryPoolBlock *block = &renderer->poolBlocks[renderer->poolBlockCount];
    memset(block, 0, sizeof(*block));
    if (!BufferCreate(renderer, blockBytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      false, &block->buffer))
        return false;

    block->freeRangeCapacity = 16u;
    block->freeRanges = PlatformAllocate(block->freeRangeCapacity * sizeof(FreeRange), false);
    if (block->freeRanges == NULL)
    {
        BufferDestroy(renderer, &block->buffer);
        return false;
    }
    block->freeRanges[0].offset = 0u;
    block->freeRanges[0].size = blockBytes;
    block->freeRangeCount = 1u;
    block->totalBytes = blockBytes;

    renderer->poolCapacityBytes += blockBytes;
    *outBlockIndex = renderer->poolBlockCount++;
    return true;
}

static bool PoolAllocate(Renderer *renderer, uint32_t sizeBytes, uint32_t *outBlockIndex,
                         uint32_t *outOffset)
{
    // Квады читаются как storage buffer с динамическим смещением, поэтому
    // каждая выдача выравнивается по требованию устройства.
    uint32_t alignment = (uint32_t)renderer->storageAlignment;
    if (alignment == 0u) alignment = 16u;
    uint32_t aligned = PoolSpanBytes(renderer, sizeBytes);
    if (aligned < sizeBytes) return false;

    for (uint32_t attempt = 0; attempt < 2u; ++attempt)
    {
        for (uint32_t blockIndex = 0; blockIndex < renderer->poolBlockCount; ++blockIndex)
        {
            GeometryPoolBlock *block = &renderer->poolBlocks[blockIndex];
            for (uint32_t rangeIndex = 0; rangeIndex < block->freeRangeCount; ++rangeIndex)
            {
                FreeRange *range = &block->freeRanges[rangeIndex];
                uint32_t start = AlignUp(range->offset, alignment);
                if (start < range->offset) continue;
                uint32_t padding = start - range->offset;
                if (range->size < padding || range->size - padding < aligned) continue;

                *outBlockIndex = blockIndex;
                *outOffset = start;

                uint32_t tailOffset = start + aligned;
                uint32_t tailSize = range->size - padding - aligned;
                if (padding == 0u)
                {
                    if (tailSize == 0u)
                    {
                        memmove(range, range + 1,
                                (block->freeRangeCount - rangeIndex - 1u) * sizeof(FreeRange));
                        block->freeRangeCount--;
                    }
                    else
                    {
                        range->offset = tailOffset;
                        range->size = tailSize;
                    }
                }
                else
                {
                    range->size = padding;
                    if (tailSize != 0u)
                    {
                        if (block->freeRangeCount == block->freeRangeCapacity)
                        {
                            uint32_t capacity = block->freeRangeCapacity * 2u;
                            FreeRange *grown = PlatformReallocate(
                                block->freeRanges, capacity * sizeof(FreeRange), false);
                            if (grown == NULL)
                            {
                                // Хвост потерять нельзя: возвращаем диапазон целиком.
                                range->size = padding + aligned + tailSize;
                                return false;
                            }
                            block->freeRanges = grown;
                            block->freeRangeCapacity = capacity;
                            range = &block->freeRanges[rangeIndex];
                        }
                        memmove(&block->freeRanges[rangeIndex + 2],
                                &block->freeRanges[rangeIndex + 1],
                                (block->freeRangeCount - rangeIndex - 1u) * sizeof(FreeRange));
                        block->freeRanges[rangeIndex + 1].offset = tailOffset;
                        block->freeRanges[rangeIndex + 1].size = tailSize;
                        block->freeRangeCount++;
                    }
                }
                renderer->poolUsedBytes += aligned;
                return true;
            }
        }

        uint32_t created = 0u;
        if (attempt == 0u && !PoolBlockCreate(renderer, aligned, &created)) return false;
    }
    return false;
}

// Возвращает диапазон в список свободных с коалесценцией соседей.
// false — диапазон потерян из-за OOM при росте списка; вызывающий тогда не
// уменьшает poolUsedBytes, потому что занятость блока не изменилась.
static bool PoolFree(GeometryPoolBlock *block, uint32_t offset, uint32_t size)
{
    uint32_t insertion = 0u;
    while (insertion < block->freeRangeCount && block->freeRanges[insertion].offset < offset)
        ++insertion;

    if (block->freeRangeCount == block->freeRangeCapacity)
    {
        uint32_t capacity = block->freeRangeCapacity * 2u;
        FreeRange *grown =
            PlatformReallocate(block->freeRanges, capacity * sizeof(FreeRange), false);
        if (grown == NULL) return false;   // диапазон останется занятым, но пул не повредится
        block->freeRanges = grown;
        block->freeRangeCapacity = capacity;
    }

    memmove(&block->freeRanges[insertion + 1], &block->freeRanges[insertion],
            (block->freeRangeCount - insertion) * sizeof(FreeRange));
    block->freeRanges[insertion].offset = offset;
    block->freeRanges[insertion].size = size;
    block->freeRangeCount++;

    // Слияние с соседями держит список коротким и без дыр.
    if (insertion + 1u < block->freeRangeCount &&
        block->freeRanges[insertion].offset + block->freeRanges[insertion].size ==
            block->freeRanges[insertion + 1].offset)
    {
        block->freeRanges[insertion].size += block->freeRanges[insertion + 1].size;
        memmove(&block->freeRanges[insertion + 1], &block->freeRanges[insertion + 2],
                (block->freeRangeCount - insertion - 2u) * sizeof(FreeRange));
        block->freeRangeCount--;
    }
    if (insertion > 0u &&
        block->freeRanges[insertion - 1].offset + block->freeRanges[insertion - 1].size ==
            block->freeRanges[insertion].offset)
    {
        block->freeRanges[insertion - 1].size += block->freeRanges[insertion].size;
        memmove(&block->freeRanges[insertion], &block->freeRanges[insertion + 1],
                (block->freeRangeCount - insertion - 1u) * sizeof(FreeRange));
        block->freeRangeCount--;
    }
    return true;
}

// === Отложенные освобождения ===

static void WaitForGpu(Renderer *renderer)
{
    vkDeviceWaitIdle(renderer->device);
}

// Полностью опустевший блок пула: все выданные из него области уже
// вернулись, и в списке свободных лежит ровно весь блок.
static bool PoolBlockIsEmpty(const GeometryPoolBlock *block)
{
    return block->freeRangeCount == 1u && block->freeRanges[0].offset == 0u &&
           block->freeRanges[0].size == block->totalBytes;
}

// Блок ещё нужен незакрытым загрузкам или отложенным освобождениям: его
// индекс обязан остаться валидным до конца дренажа.
static bool PoolBlockIsReferenced(const Renderer *renderer, uint32_t blockIndex)
{
    for (uint32_t i = 0; i < renderer->pendingUploadCount; ++i)
    {
        if (renderer->pendingUploads[i].blockIndex == blockIndex) return true;
    }
    for (uint32_t i = 0; i < renderer->deferredRangeCount; ++i)
    {
        uint32_t index = (renderer->deferredRangeHead + i) % DEFERRED_RELEASE_CAPACITY;
        if (renderer->deferredRanges[index].blockIndex == blockIndex) return true;
    }
    return false;
}

static void FreeBlockDescriptorSets(Renderer *renderer, GeometryPoolBlock *block)
{
    for (uint32_t chunk = 0u; chunk < INSTANCE_MAX_CHUNKS_PER_FRAME; ++chunk)
    {
        for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame)
        {
            VkDescriptorSet *set = &block->sets[chunk][frame];
            if (*set == VK_NULL_HANDLE) continue;
            vkFreeDescriptorSets(renderer->device, renderer->descriptorPool, 1u, set);
            *set = VK_NULL_HANDLE;
        }
    }
}

// Отдаёт системе хвостовые блоки пула, в которых не осталось ни одной
// выданной области. Иначе пул только растёт: после разового всплеска
// геометрии память GPU остаётся занятой до RendererReleaseWorld. Последний
// блок базового размера сохраняется тёплым кэшем, чтобы одиночный меш не
// создавал и не уничтожал ресурс каждый кадр; блок крупнее базового
// освобождается всегда.
//
// Безопасность по GPU: блок становится пустым только после дренажа всех
// отложенных диапазонов, а диапазон дренируется лишь при
// submittedFrames >= safeFrameIndex = момент удаления + FRAME_COUNT. Ядро
// кадра устроено так, что каждый кадровый слот переиспользуется не раньше
// подтверждения своего фенса (RendererBeginFrame ждёт frameFences[slot]),
// поэтому за FRAME_COUNT отправок командный буфер, читавший блок и его
// дескрипторные наборы, гарантированно завершён.
static void PoolReclaimEmptyTail(Renderer *renderer)
{
    while (renderer->poolBlockCount > 0u)
    {
        uint32_t last = renderer->poolBlockCount - 1u;
        GeometryPoolBlock *block = &renderer->poolBlocks[last];
        if (!PoolBlockIsEmpty(block)) break;
        if (PoolBlockIsReferenced(renderer, last)) break;
        if (renderer->poolBlockCount == 1u && block->totalBytes == POOL_BLOCK_BYTES) break;

        // Пул дескрипторов создан с VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        // поэтому наборы блока возвращаются пулу наборов, а не остаются в слоте.
        BufferDestroy(renderer, &block->buffer);
        if (block->freeRanges != NULL) PlatformFree(block->freeRanges);
        FreeBlockDescriptorSets(renderer, block);
        renderer->poolCapacityBytes -= block->totalBytes;
        memset(block, 0, sizeof(*block));
        renderer->poolBlockCount--;
    }
}

static void DrainDeferredReleases(Renderer *renderer, bool releaseEverything)
{
    while (renderer->deferredBufferCount > 0u)
    {
        DeferredBufferRelease *entry = &renderer->deferredBuffers[renderer->deferredBufferHead];
        if (!releaseEverything && entry->safeFrameIndex > renderer->submittedFrames) break;
        if (entry->buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(renderer->device, entry->buffer, NULL);
        if (entry->memory != VK_NULL_HANDLE)
            vkFreeMemory(renderer->device, entry->memory, NULL);
        renderer->deferredBufferHead =
            (renderer->deferredBufferHead + 1u) % DEFERRED_RELEASE_CAPACITY;
        renderer->deferredBufferCount--;
    }
    while (renderer->deferredRangeCount > 0u)
    {
        DeferredRangeRelease *entry = &renderer->deferredRanges[renderer->deferredRangeHead];
        if (!releaseEverything && entry->safeFrameIndex > renderer->submittedFrames) break;
        if (PoolFree(&renderer->poolBlocks[entry->blockIndex], entry->offset, entry->size))
            renderer->poolUsedBytes -= entry->size;
        renderer->deferredRangeHead =
            (renderer->deferredRangeHead + 1u) % DEFERRED_RELEASE_CAPACITY;
        renderer->deferredRangeCount--;
    }

    PoolReclaimEmptyTail(renderer);
}

static void DeferBufferRelease(Renderer *renderer, VkBuffer buffer, VkDeviceMemory memory)
{
    if (renderer->deferredBufferCount == DEFERRED_RELEASE_CAPACITY)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }
    uint32_t slot =
        (renderer->deferredBufferHead + renderer->deferredBufferCount) % DEFERRED_RELEASE_CAPACITY;
    renderer->deferredBuffers[slot].buffer = buffer;
    renderer->deferredBuffers[slot].memory = memory;
    renderer->deferredBuffers[slot].safeFrameIndex = renderer->submittedFrames + FRAME_COUNT;
    renderer->deferredBufferCount++;
}

// === Дескрипторы и конвейеры ===

static bool CreateShaderModule(Renderer *renderer, const void *bytes, uint32_t sizeBytes,
                               VkShaderModule *outModule)
{
    // Пак шейдеров, собранный под D3D12, содержит DXBC. Проверка сигнатуры
    // SPIR-V превращает это в понятный отказ вместо падения драйвера.
    if (bytes == NULL || sizeBytes < 20u || (sizeBytes % 4u) != 0u) return false;
    uint32_t magic = 0u;
    memcpy(&magic, bytes, sizeof(magic));
    if (magic != 0x07230203u) return false;

    VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeBytes,
        .pCode = (const uint32_t *)bytes,
    };
    return vkCreateShaderModule(renderer->device, &moduleInfo, NULL, outModule) == VK_SUCCESS;
}

static const void *SelectShader(const Renderer *renderer, LaiueShaderSlot slot,
                                const void *fallback, uint32_t fallbackBytes,
                                uint32_t *outSizeBytes)
{
    if (slot < LAIUE_SHADER_SLOT_COUNT && renderer->loadedShaders[slot] != NULL)
    {
        *outSizeBytes = renderer->loadedShaderLengths[slot];
        return renderer->loadedShaders[slot];
    }
    *outSizeBytes = fallbackBytes;
    return fallback;
}

typedef struct PipelineRecipe
{
    LaiueShaderSlot vertexSlot;
    LaiueShaderSlot pixelSlot;
    const void *vertexFallback;
    uint32_t vertexFallbackBytes;
    const void *pixelFallback;
    uint32_t pixelFallbackBytes;
    VkPipelineLayout layout;
    bool depthTest;
    bool blend;
    VkCullModeFlags cullMode;
    bool wireframe;
} PipelineRecipe;

static bool CreateGraphicsPipeline(Renderer *renderer, const PipelineRecipe *recipe,
                                   VkPipeline *outPipeline)
{
    uint32_t vertexBytes = 0u;
    uint32_t pixelBytes = 0u;
    const void *vertexCode =
        SelectShader(renderer, recipe->vertexSlot, recipe->vertexFallback,
                     recipe->vertexFallbackBytes, &vertexBytes);
    const void *pixelCode = SelectShader(renderer, recipe->pixelSlot, recipe->pixelFallback,
                                         recipe->pixelFallbackBytes, &pixelBytes);

    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule pixelModule = VK_NULL_HANDLE;
    if (!CreateShaderModule(renderer, vertexCode, vertexBytes, &vertexModule)) return false;
    if (!CreateShaderModule(renderer, pixelCode, pixelBytes, &pixelModule))
    {
        vkDestroyShaderModule(renderer->device, vertexModule, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexModule,
            .pName = "VSMain",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = pixelModule,
            .pName = "PSMain",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1u,
        .scissorCount = 1u,
    };
    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = recipe->wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .cullMode = recipe->cullMode,
        // Отрицательная высота viewport переворачивает Y обратно в
        // соглашение D3D, поэтому лицевой обход остаётся по часовой.
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = recipe->depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = recipe->depthTest ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState blendAttachment = {
        .blendEnable = recipe->blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo colorBlend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1u,
        .pAttachments = &blendAttachment,
    };
    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2u,
        .pDynamicStates = dynamicStates,
    };

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2u,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = recipe->layout,
    };
    VkFormat colorFormat = COLOR_FORMAT;
    VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1u,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = recipe->depthTest ? DEPTH_FORMAT : VK_FORMAT_UNDEFINED,
    };
    if (renderer->useDynamicRendering)
    {
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
    }
    else
    {
        pipelineInfo.renderPass = recipe->depthTest ? renderer->depthRenderPass
                                                    : renderer->colorRenderPass;
        pipelineInfo.subpass = 0u;
    }

    VkResult result = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1u,
                                                &pipelineInfo, NULL, outPipeline);
    vkDestroyShaderModule(renderer->device, vertexModule, NULL);
    vkDestroyShaderModule(renderer->device, pixelModule, NULL);
    return result == VK_SUCCESS;
}

static bool CreateChunkPipeline(Renderer *renderer, VkPipeline *outPipeline)
{
    PipelineRecipe recipe = {
        .vertexSlot = LAIUE_SHADER_CHUNK_VERTEX,
        .pixelSlot = LAIUE_SHADER_CHUNK_PIXEL,
        .vertexFallback = g_chunk_vs,
        .vertexFallbackBytes = (uint32_t)sizeof(g_chunk_vs),
        .pixelFallback = g_chunk_ps,
        .pixelFallbackBytes = (uint32_t)sizeof(g_chunk_ps),
        .layout = renderer->chunkPipelineLayout,
        .depthTest = true,
        .blend = false,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .wireframe = renderer->wireframeEnabled,
    };
    return CreateGraphicsPipeline(renderer, &recipe, outPipeline);
}

static bool CreateGenericPipeline(Renderer *renderer, VkPipeline *outPipeline)
{
    PipelineRecipe recipe = {
        .vertexSlot = LAIUE_SHADER_SLOT_COUNT,
        .pixelSlot = LAIUE_SHADER_SLOT_COUNT,
        .vertexFallback = g_generic_vs,
        .vertexFallbackBytes = (uint32_t)sizeof(g_generic_vs),
        .pixelFallback = g_generic_ps,
        .pixelFallbackBytes = (uint32_t)sizeof(g_generic_ps),
        .layout = renderer->chunkPipelineLayout,
        .depthTest = true,
        .blend = false,
        // Generic geometry has no voxel-specific winding contract.  The
        // public V2 mesh ABI accepts ordinary vertex streams, so both
        // clockwise and counter-clockwise triangles must reach the target;
        // voxel quads keep their specialized back-face culling below.
        .cullMode = VK_CULL_MODE_NONE,
        .wireframe = renderer->wireframeEnabled,
    };
    return CreateGraphicsPipeline(renderer, &recipe, outPipeline);
}

static bool CreateResolvePipeline(Renderer *renderer, VkPipeline *outPipeline)
{
    PipelineRecipe recipe = {
        .vertexSlot = LAIUE_SHADER_PANORAMA_VERTEX,
        .pixelSlot = LAIUE_SHADER_PANORAMA_PIXEL,
        .vertexFallback = g_panorama_vs,
        .vertexFallbackBytes = (uint32_t)sizeof(g_panorama_vs),
        .pixelFallback = g_panorama_ps,
        .pixelFallbackBytes = (uint32_t)sizeof(g_panorama_ps),
        .layout = renderer->resolvePipelineLayout,
        .depthTest = false,
        .blend = false,
        .cullMode = VK_CULL_MODE_NONE,
        .wireframe = false,
    };
    return CreateGraphicsPipeline(renderer, &recipe, outPipeline);
}

static bool CreateUiPipeline(Renderer *renderer, VkPipeline *outPipeline)
{
    PipelineRecipe recipe = {
        .vertexSlot = LAIUE_SHADER_UI_VERTEX,
        .pixelSlot = LAIUE_SHADER_UI_PIXEL,
        .vertexFallback = g_ui_vs,
        .vertexFallbackBytes = (uint32_t)sizeof(g_ui_vs),
        .pixelFallback = g_ui_ps,
        .pixelFallbackBytes = (uint32_t)sizeof(g_ui_ps),
        .layout = renderer->uiPipelineLayout,
        .depthTest = false,
        .blend = true,
        .cullMode = VK_CULL_MODE_NONE,
        .wireframe = false,
    };
    return CreateGraphicsPipeline(renderer, &recipe, outPipeline);
}

static bool CreateDescriptorLayouts(Renderer *renderer)
{
    VkDescriptorSetLayoutBinding chunkBindings[] = {
        { BINDING_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_QUAD_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { BINDING_BLOCK_TEXTURES, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_BLOCK_NORMALS, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_INSTANCES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { BINDING_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutBinding resolveBindings[] = {
        { BINDING_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_PANORAMA_CUBE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutBinding uiBindings[] = {
        { BINDING_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { BINDING_UI_QUADS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1u,
          VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { BINDING_UI_FONT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_FRAGMENT_BIT,
          NULL },
        { BINDING_UI_BACKGROUND, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { BINDING_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };

    struct
    {
        const VkDescriptorSetLayoutBinding *bindings;
        uint32_t count;
        VkDescriptorSetLayout *target;
    } layouts[] = {
        { chunkBindings, 6u, &renderer->chunkSetLayout },
        { resolveBindings, 3u, &renderer->resolveSetLayout },
        { uiBindings, 5u, &renderer->uiSetLayout },
    };
    for (uint32_t index = 0; index < 3u; ++index)
    {
        VkDescriptorSetLayoutCreateInfo layoutInfo = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = layouts[index].count,
            .pBindings = layouts[index].bindings,
        };
        if (vkCreateDescriptorSetLayout(renderer->device, &layoutInfo, NULL,
                                        layouts[index].target) != VK_SUCCESS)
            return false;
    }

    VkDescriptorSetLayout pipelineLayoutTargets[3] = {
        renderer->chunkSetLayout, renderer->resolveSetLayout, renderer->uiSetLayout
    };
    VkPipelineLayout *pipelineLayouts[3] = {
        &renderer->chunkPipelineLayout, &renderer->resolvePipelineLayout,
        &renderer->uiPipelineLayout
    };
    for (uint32_t index = 0; index < 3u; ++index)
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1u,
            .pSetLayouts = &pipelineLayoutTargets[index],
        };
        if (vkCreatePipelineLayout(renderer->device, &pipelineLayoutInfo, NULL,
                                   pipelineLayouts[index]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static void WriteImageDescriptor(Renderer *renderer, VkDescriptorSet set, uint32_t binding,
                                 VkImageView view)
{
    VkDescriptorImageInfo imageInfo = {
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &imageInfo,
    };
    vkUpdateDescriptorSets(renderer->device, 1u, &write, 0u, NULL);
}

static void WriteBufferDescriptor(Renderer *renderer, VkDescriptorSet set, uint32_t binding,
                                  VkDescriptorType type, VkBuffer buffer, VkDeviceSize range)
{
    VkDescriptorBufferInfo bufferInfo = {
        .buffer = buffer,
        .offset = 0u,
        .range = range,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1u,
        .descriptorType = type,
        .pBufferInfo = &bufferInfo,
    };
    vkUpdateDescriptorSets(renderer->device, 1u, &write, 0u, NULL);
}

static void WriteSamplerDescriptor(Renderer *renderer, VkDescriptorSet set)
{
    VkDescriptorImageInfo imageInfo = { .sampler = renderer->sampler };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = BINDING_SAMPLER,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = &imageInfo,
    };
    vkUpdateDescriptorSets(renderer->device, 1u, &write, 0u, NULL);
}

// Текущие текстуры блоков: до загрузки пака шейдер читает нейтральную
// заглушку, иначе дескриптор был бы невалиден.
static VkImageView ActiveBlockAlbedoView(const Renderer *renderer)
{
    return renderer->blockTexture.view != VK_NULL_HANDLE ? renderer->blockTexture.view
                                                         : renderer->fallbackArray.view;
}

static VkImageView ActiveBlockNormalView(const Renderer *renderer)
{
    return renderer->blockNormalTexture.view != VK_NULL_HANDLE
               ? renderer->blockNormalTexture.view
               : renderer->fallbackArray.view;
}

static void RefreshChunkSetTextures(Renderer *renderer)
{
    for (uint32_t blockIndex = 0; blockIndex < renderer->poolBlockCount; ++blockIndex)
    {
        for (uint32_t chunk = 0u; chunk < INSTANCE_MAX_CHUNKS_PER_FRAME; ++chunk)
        {
            for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
            {
                VkDescriptorSet set = renderer->poolBlocks[blockIndex].sets[chunk][frame];
                if (set == VK_NULL_HANDLE) continue;
                WriteImageDescriptor(renderer, set, BINDING_BLOCK_TEXTURES,
                                     ActiveBlockAlbedoView(renderer));
                WriteImageDescriptor(renderer, set, BINDING_BLOCK_NORMALS,
                                     ActiveBlockNormalView(renderer));
            }
        }
    }
}

static void PopulateBlockDescriptorSet(Renderer *renderer, GeometryPoolBlock *block,
                                       uint32_t frameIndex, uint32_t chunkIndex,
                                       VkDescriptorSet set)
{
    WriteBufferDescriptor(renderer, set, BINDING_CONSTANTS,
                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                          renderer->constantBuffers[frameIndex].buffer, sizeof(ChunkConstants));
    WriteBufferDescriptor(renderer, set, BINDING_QUAD_BUFFER,
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, block->buffer.buffer,
                          block->buffer.sizeBytes > 0u ? VK_WHOLE_SIZE : 0u);
    WriteBufferDescriptor(renderer, set, BINDING_INSTANCES,
                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                          renderer->instanceBuffers[frameIndex][chunkIndex].buffer,
                          VK_WHOLE_SIZE);
    WriteImageDescriptor(renderer, set, BINDING_BLOCK_TEXTURES,
                         ActiveBlockAlbedoView(renderer));
    WriteImageDescriptor(renderer, set, BINDING_BLOCK_NORMALS,
                         ActiveBlockNormalView(renderer));
    WriteSamplerDescriptor(renderer, set);
}

static bool EnsureBlockDescriptorSets(Renderer *renderer, uint32_t blockIndex)
{
    GeometryPoolBlock *block = &renderer->poolBlocks[blockIndex];
    if (block->sets[0][0] != VK_NULL_HANDLE) return true;

    VkDescriptorSetLayout layouts[FRAME_COUNT];
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame) layouts[frame] = renderer->chunkSetLayout;
    VkDescriptorSetAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = renderer->descriptorPool,
        .descriptorSetCount = FRAME_COUNT,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(renderer->device, &allocateInfo, block->sets[0]) != VK_SUCCESS)
        return false;

    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        PopulateBlockDescriptorSet(renderer, block, frame, 0u, block->sets[0][frame]);
    }
    return true;
}

static bool EnsureBlockInstanceDescriptorSet(Renderer *renderer, uint32_t blockIndex,
                                              uint32_t frameIndex, uint32_t chunkIndex)
{
    if (blockIndex >= renderer->poolBlockCount || frameIndex >= FRAME_COUNT ||
        chunkIndex >= renderer->instanceChunkCount[frameIndex])
        return false;
    // The base set is also needed by non-instanced draws.  Ensure it before
    // allocating a higher chunk set; otherwise a block whose first call is a
    // >1 MiB instanced draw would leave its ordinary mesh path unbound.
    if (!EnsureBlockDescriptorSets(renderer, blockIndex)) return false;

    GeometryPoolBlock *block = &renderer->poolBlocks[blockIndex];
    VkDescriptorSet *set = &block->sets[chunkIndex][frameIndex];
    if (*set != VK_NULL_HANDLE) return true;

    VkDescriptorSetAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = renderer->descriptorPool,
        .descriptorSetCount = 1u,
        .pSetLayouts = &renderer->chunkSetLayout,
    };
    if (vkAllocateDescriptorSets(renderer->device, &allocateInfo, set) != VK_SUCCESS)
        return false;
    PopulateBlockDescriptorSet(renderer, block, frameIndex, chunkIndex, *set);
    return true;
}

// === Загрузка текстур ===

static bool UploadImagePixels(Renderer *renderer, GpuImage *image, const uint8_t *pixels,
                              uint32_t bytesPerPixel, uint32_t layerCount)
{
    VkDeviceSize layerBytes = (VkDeviceSize)image->width * image->height * bytesPerPixel;
    VkDeviceSize totalBytes = layerBytes * layerCount;
    GpuBuffer staging;
    if (!BufferCreate(renderer, totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &staging))
        return false;
    memcpy(staging.mapped, pixels, (size_t)totalBytes);

    VkCommandBuffer commandBuffer = BeginImmediate(renderer);
    if (commandBuffer == VK_NULL_HANDLE)
    {
        BufferDestroy(renderer, &staging);
        return false;
    }
    ImageBarrier(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {
        .bufferOffset = 0u,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, layerCount },
        .imageExtent = { image->width, image->height, 1u },
    };
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    ImageBarrier(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = EndImmediate(renderer, commandBuffer);
    BufferDestroy(renderer, &staging);
    return ok;
}

// Слои пака лежат отдельными подресурсами; общий staging собирается из
// них построчно, потому что VkBufferImageCopy требует плотной укладки.
static bool CreateBlockArrayTexture(Renderer *renderer, const TexturePackData *pack, bool normals,
                                    GpuImage *outImage)
{
    uint32_t layerCount = pack->sliceCount;
    if (layerCount == 0u) return false;

    // Пак без карт нормалей: на каждый слой кладётся плоская нормаль
    // 1x1 с полной открытостью. Освещение вырождается в чистый ламберт
    // по граням, а не отказывает — иначе пак, собранный без нормалей,
    // просто не загрузился бы.
    bool flat = normals && pack->normalPixels == NULL;
    uint32_t width = flat ? 1u : pack->width;
    uint32_t height = flat ? 1u : pack->height;

    if (!ImageCreate(renderer, width, height, layerCount, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, false, outImage))
        return false;

    VkDeviceSize layerBytes = (VkDeviceSize)width * height * 4u;
    GpuBuffer staging;
    if (!BufferCreate(renderer, layerBytes * layerCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                      &staging))
    {
        ImageDestroy(renderer, outImage);
        return false;
    }

    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        uint8_t *destination = staging.mapped + layerBytes * layer;
        if (flat)
        {
            destination[0] = 128u;
            destination[1] = 128u;
            destination[2] = 255u;
            destination[3] = 255u;
            continue;
        }

        TexturePackSubresource subresource;
        bool ok = normals ? TexturePackGetNormalSubresource(pack, layer, 0u, &subresource)
                          : TexturePackGetSubresource(pack, layer, 0u, &subresource);
        if (!ok || subresource.byteCount != layerBytes)
        {
            BufferDestroy(renderer, &staging);
            ImageDestroy(renderer, outImage);
            return false;
        }
        memcpy(destination, subresource.pixels, (size_t)layerBytes);
    }

    VkCommandBuffer commandBuffer = BeginImmediate(renderer);
    if (commandBuffer == VK_NULL_HANDLE)
    {
        BufferDestroy(renderer, &staging);
        ImageDestroy(renderer, outImage);
        return false;
    }
    ImageBarrier(commandBuffer, outImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, layerCount },
        .imageExtent = { width, height, 1u },
    };
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, outImage->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    ImageBarrier(commandBuffer, outImage, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = EndImmediate(renderer, commandBuffer);
    BufferDestroy(renderer, &staging);
    if (!ok) ImageDestroy(renderer, outImage);
    return ok;
}

static bool CreateFallbackImages(Renderer *renderer)
{
    static const uint8_t whitePixel[4] = { 255u, 255u, 255u, 255u };
    // Нормаль «наружу» с полной освещённостью: (0.5, 0.5, 1) и AO = 1.
    static const uint8_t neutralNormal[4] = { 128u, 128u, 255u, 255u };
    uint8_t cubePixels[6 * 4];
    for (uint32_t face = 0; face < 6u; ++face) memcpy(cubePixels + face * 4u, whitePixel, 4u);

    if (!ImageCreate(renderer, 1u, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, false,
                     &renderer->fallbackArray))
        return false;
    if (!UploadImagePixels(renderer, &renderer->fallbackArray, neutralNormal, 4u, 1u)) return false;

    if (!ImageCreate(renderer, 1u, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false,
                     &renderer->fallbackImage))
        return false;
    if (!UploadImagePixels(renderer, &renderer->fallbackImage, whitePixel, 4u, 1u)) return false;

    if (!ImageCreate(renderer, 1u, 1u, 6u, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, true,
                     &renderer->fallbackCube))
        return false;
    return UploadImagePixels(renderer, &renderer->fallbackCube, cubePixels, 4u, 6u);
}

static RendererContentStatus MapTexturePackLoadStatus(TexturePackLoadStatus status)
{
    switch (status)
    {
    case TEXTURE_PACK_LOAD_OK: return RENDERER_CONTENT_OK;
    case TEXTURE_PACK_LOAD_INCOMPLETE: return RENDERER_CONTENT_INCOMPLETE;
    case TEXTURE_PACK_LOAD_NO_ACTIVE_PACK: return RENDERER_CONTENT_NO_ACTIVE;
    case TEXTURE_PACK_LOAD_INVALID: return RENDERER_CONTENT_INVALID;
    case TEXTURE_PACK_LOAD_IO_ERROR: return RENDERER_CONTENT_IO_ERROR;
    default: return RENDERER_CONTENT_NOT_ATTEMPTED;
    }
}

// === Кубмапа панорамы ===

static void ReleaseCubeResources(Renderer *renderer)
{
    for (uint32_t face = 0; face < 6u; ++face)
    {
        if (renderer->cubeFramebuffers[face] != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(renderer->device, renderer->cubeFramebuffers[face], NULL);
            renderer->cubeFramebuffers[face] = VK_NULL_HANDLE;
        }
    }
    for (uint32_t face = 0; face < 6u; ++face)
    {
        if (renderer->cubeFaceViews[face] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(renderer->device, renderer->cubeFaceViews[face], NULL);
            renderer->cubeFaceViews[face] = VK_NULL_HANDLE;
        }
    }
    ImageDestroy(renderer, &renderer->cubeColor);
    ImageDestroy(renderer, &renderer->cubeDepth);
    renderer->cubeResolution = 0u;
}

static bool EnsureCubeResources(Renderer *renderer, uint32_t resolution)
{
    if (resolution == 0u) return false;
    if (renderer->cubeResolution == resolution) return true;

    WaitForGpu(renderer);
    ReleaseCubeResources(renderer);

    if (!ImageCreate(renderer, resolution, resolution, 6u, COLOR_FORMAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, true,
                     &renderer->cubeColor))
        return false;
    // Глубина общая для всех граней: каждый проход очищает свой прямоугольник.
    if (!ImageCreate(renderer, resolution, resolution, 1u, DEPTH_FORMAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_VIEW_TYPE_2D, false, &renderer->cubeDepth))
    {
        ReleaseCubeResources(renderer);
        return false;
    }

    for (uint32_t face = 0; face < 6u; ++face)
    {
        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = renderer->cubeColor.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = COLOR_FORMAT,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, face, 1u },
        };
        if (vkCreateImageView(renderer->device, &viewInfo, NULL, &renderer->cubeFaceViews[face]) !=
            VK_SUCCESS)
        {
            ReleaseCubeResources(renderer);
            return false;
        }
    }

    if (!CreateCubeFramebuffers(renderer))
    {
        ReleaseCubeResources(renderer);
        return false;
    }

    renderer->cubeResolution = resolution;
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        WriteImageDescriptor(renderer, renderer->resolveSets[frame], BINDING_PANORAMA_CUBE,
                             renderer->cubeColor.view);
    }
    return true;
}

// === Цели кадра ===

static void ReleaseFrameTargets(Renderer *renderer)
{
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame)
    {
        if (renderer->colorFramebuffers[frame] != VK_NULL_HANDLE)
            vkDestroyFramebuffer(renderer->device, renderer->colorFramebuffers[frame], NULL);
        if (renderer->depthFramebuffers[frame] != VK_NULL_HANDLE)
            vkDestroyFramebuffer(renderer->device, renderer->depthFramebuffers[frame], NULL);
        renderer->colorFramebuffers[frame] = VK_NULL_HANDLE;
        renderer->depthFramebuffers[frame] = VK_NULL_HANDLE;
    }
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
        ImageDestroy(renderer, &renderer->colorTargets[frame]);
    ImageDestroy(renderer, &renderer->depthTarget);
    renderer->lastFrameValid = false;
}

static bool CreateFrameTargets(Renderer *renderer, int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0) return false;

    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        if (!ImageCreate(renderer, (uint32_t)width, (uint32_t)height, 1u, COLOR_FORMAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false,
                         &renderer->colorTargets[frame]))
        {
            ReleaseFrameTargets(renderer);
            return false;
        }
    }
    if (!ImageCreate(renderer, (uint32_t)width, (uint32_t)height, 1u, DEPTH_FORMAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_VIEW_TYPE_2D, false, &renderer->depthTarget))
    {
        ReleaseFrameTargets(renderer);
        return false;
    }

    if (!CreateFrameTargetFramebuffers(renderer))
    {
        ReleaseFrameTargets(renderer);
        return false;
    }

    renderer->windowWidth = width;
    renderer->windowHeight = height;
    return true;
}

static bool FrameTargetsReady(const Renderer *renderer)
{
    if (renderer->depthTarget.image == VK_NULL_HANDLE) return false;
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        if (renderer->colorTargets[frame].image == VK_NULL_HANDLE) return false;
    }
    return true;
}

static bool ApplyPendingResize(Renderer *renderer)
{
    if (renderer->resizeWidth <= 0 || renderer->resizeHeight <= 0)
    {
        // Свёрнутое окно: цели кадра не трогаем, кадры пропускаются, но
        // при восстановлении размера swapchain надо пересобрать.
        renderer->resizeRequested = false;
        if (renderer->hasSurface) renderer->swapchainOutOfDate = true;
        return true;
    }
    if (renderer->resizeWidth == renderer->windowWidth &&
        renderer->resizeHeight == renderer->windowHeight && FrameTargetsReady(renderer))
    {
        renderer->resizeRequested = false;
        return true;
    }

    WaitForGpu(renderer);
    ReleaseFrameTargets(renderer);
    if (!CreateFrameTargets(renderer, renderer->resizeWidth, renderer->resizeHeight))
    {
        // Keep the request pending so a transient allocation failure can be
        // retried on the next frame, including when the requested size equals
        // the last successfully created size.
        renderer->resizeRequested = true;
        return false;
    }
    renderer->resizeRequested = false;
    // Размер изменился — swapchain придётся пересобрать целиком.
    if (renderer->hasSurface) renderer->swapchainOutOfDate = true;
    return true;
}

// === Swapchain: вывод готового colorTarget в окно ===

// Сборка без CRT не имеет strcmp, а имена расширений — обычный ASCII.
static bool NameEquals(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

#if defined(_WIN32) || defined(LAIUE_RENDER_HAS_X11) || defined(__ANDROID__)
// Расширения инстанса проверяются до его создания: драйвер без
// VK_KHR_win32_surface не сможет принять HWND, и честный отказ лучше
// падения внутри vkCreateWin32SurfaceKHR.
static bool InstanceExtensionsAvailable(const char *const *names, uint32_t nameCount)
{
    uint32_t count = 0u;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || count == 0u)
        return false;
    VkExtensionProperties *properties = PlatformAllocate(count * sizeof(*properties), false);
    if (properties == NULL) return false;

    bool available = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, properties) == VK_SUCCESS)
    {
        available = true;
        for (uint32_t name = 0; name < nameCount && available; ++name)
        {
            bool found = false;
            for (uint32_t index = 0; index < count; ++index)
            {
                if (NameEquals(properties[index].extensionName, names[name]))
                {
                    found = true;
                    break;
                }
            }
            if (!found) available = false;
        }
    }
    PlatformFree(properties);
    return available;
}
#endif

static bool DeviceExtensionSupported(VkPhysicalDevice device, const char *name)
{
    uint32_t count = 0u;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS || count == 0u)
        return false;
    VkExtensionProperties *properties = PlatformAllocate(count * sizeof(*properties), false);
    if (properties == NULL) return false;

    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, properties) == VK_SUCCESS)
    {
        for (uint32_t index = 0; index < count; ++index)
        {
            if (NameEquals(properties[index].extensionName, name))
            {
                found = true;
                break;
            }
        }
    }
    PlatformFree(properties);
    return found;
}

// Формат swapchain. Приоритет: точное совпадение с форматом offscreen-цели
// (тогда достаточно vkCmdCopyImage, без конверсии и перестановки каналов),
// затем B8G8R8A8_UNORM из задачи (тогда — vkCmdBlitImage), затем любой
// с sRGB-цветовым пространством, затем первый предложенный.
static VkSurfaceFormatKHR SelectSurfaceFormat(const VkSurfaceFormatKHR *formats, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (formats[index].format == COLOR_FORMAT &&
            formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[index];
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[index];
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[index];
    }
    return formats[0];
}

// Present-mode: vsync — FIFO; без vsync — IMMEDIATE, иначе MAILBOX, иначе
// FIFO (он обязан поддерживаться всегда).
static VkPresentModeKHR SelectPresentMode(const VkPresentModeKHR *modes, uint32_t count, bool vsync)
{
    if (vsync)
    {
        for (uint32_t index = 0; index < count; ++index)
            if (modes[index] == VK_PRESENT_MODE_FIFO_KHR) return VK_PRESENT_MODE_FIFO_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    for (uint32_t index = 0; index < count; ++index)
        if (modes[index] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    for (uint32_t index = 0; index < count; ++index)
        if (modes[index] == VK_PRESENT_MODE_MAILBOX_KHR) return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Освобождает образы, представления, семафоры и сам swapchain. Вызывать
// только после WaitForGpu; безопасно на частично построенном состоянии.
static void DestroySwapchainResources(Renderer *renderer)
{
    for (uint32_t index = 0; index < FRAME_COUNT; ++index)
    {
        if (renderer->imageAvailable[index] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(renderer->device, renderer->imageAvailable[index], NULL);
            renderer->imageAvailable[index] = VK_NULL_HANDLE;
        }
    }
    for (uint32_t index = 0; index < MAX_SWAPCHAIN_IMAGES; ++index)
    {
        if (renderer->renderFinished[index] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(renderer->device, renderer->renderFinished[index], NULL);
            renderer->renderFinished[index] = VK_NULL_HANDLE;
        }
        if (renderer->swapchainViews[index] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(renderer->device, renderer->swapchainViews[index], NULL);
            renderer->swapchainViews[index] = VK_NULL_HANDLE;
        }
        renderer->swapchainImages[index] = VK_NULL_HANDLE;
    }
    if (renderer->swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);
        renderer->swapchain = VK_NULL_HANDLE;
    }
    renderer->swapchainImageCount = 0u;
    renderer->swapchainImageIndex = 0u;
}

static bool SwapchainCreate(Renderer *renderer, int32_t width, int32_t height)
{
    if (!renderer->hasSurface || renderer->device == VK_NULL_HANDLE)
        return false;

    VkSurfaceCapabilitiesKHR capabilities;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physicalDevice, renderer->surface,
                                                  &capabilities) != VK_SUCCESS)
        return false;
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0u)
        return false;

    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX &&
        capabilities.currentExtent.height != UINT32_MAX)
    {
        extent = capabilities.currentExtent;
    }
    else
    {
        uint32_t clampedWidth = width > 0 ? (uint32_t)width : 1u;
        uint32_t clampedHeight = height > 0 ? (uint32_t)height : 1u;
        if (clampedWidth < capabilities.minImageExtent.width)
            clampedWidth = capabilities.minImageExtent.width;
        if (clampedWidth > capabilities.maxImageExtent.width)
            clampedWidth = capabilities.maxImageExtent.width;
        if (clampedHeight < capabilities.minImageExtent.height)
            clampedHeight = capabilities.minImageExtent.height;
        if (clampedHeight > capabilities.maxImageExtent.height)
            clampedHeight = capabilities.maxImageExtent.height;
        extent.width = clampedWidth;
        extent.height = clampedHeight;
    }
    if (extent.width == 0u || extent.height == 0u) return false;

    uint32_t formatCount = 0u;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physicalDevice, renderer->surface, &formatCount,
                                             NULL) != VK_SUCCESS ||
        formatCount == 0u)
        return false;
    VkSurfaceFormatKHR *formats = PlatformAllocate(formatCount * sizeof(*formats), false);
    if (formats == NULL) return false;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physicalDevice, renderer->surface, &formatCount,
                                             formats) != VK_SUCCESS)
    {
        PlatformFree(formats);
        return false;
    }
    VkSurfaceFormatKHR surfaceFormat = SelectSurfaceFormat(formats, formatCount);
    PlatformFree(formats);

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t modeCount = 0u;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physicalDevice, renderer->surface,
                                                  &modeCount, NULL) == VK_SUCCESS &&
        modeCount > 0u)
    {
        VkPresentModeKHR *modes = PlatformAllocate(modeCount * sizeof(*modes), false);
        if (modes == NULL) return false;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physicalDevice, renderer->surface,
                                                      &modeCount, modes) == VK_SUCCESS)
            presentMode = SelectPresentMode(modes, modeCount, renderer->verticalSyncEnabled);
        PlatformFree(modes);
    }

    uint32_t imageCount = capabilities.minImageCount;
    if (imageCount < 2u) imageCount = 2u;
    if (capabilities.maxImageCount > 0u && imageCount > capabilities.maxImageCount)
        imageCount = capabilities.maxImageCount;
    if (imageCount > MAX_SWAPCHAIN_IMAGES) return false;

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0u)
    {
        if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0u)
            compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        else if ((capabilities.supportedCompositeAlpha &
                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) != 0u)
            compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        else
            compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }

    VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = renderer->surface,
        .minImageCount = imageCount,
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1u,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0u,
        .pQueueFamilyIndices = NULL,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(renderer->device, &swapchainInfo, NULL, &swapchain) != VK_SUCCESS)
        return false;

    uint32_t actualCount = 0u;
    if (vkGetSwapchainImagesKHR(renderer->device, swapchain, &actualCount, NULL) != VK_SUCCESS ||
        actualCount == 0u || actualCount > MAX_SWAPCHAIN_IMAGES)
    {
        vkDestroySwapchainKHR(renderer->device, swapchain, NULL);
        return false;
    }
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    if (vkGetSwapchainImagesKHR(renderer->device, swapchain, &actualCount, images) != VK_SUCCESS)
    {
        vkDestroySwapchainKHR(renderer->device, swapchain, NULL);
        return false;
    }

    VkImageView views[MAX_SWAPCHAIN_IMAGES];
    uint32_t viewCount = 0u;
    VkResult viewResult = VK_SUCCESS;
    for (; viewCount < actualCount; ++viewCount)
    {
        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[viewCount],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = surfaceFormat.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
        };
        viewResult = vkCreateImageView(renderer->device, &viewInfo, NULL, &views[viewCount]);
        if (viewResult != VK_SUCCESS) break;
    }
    if (viewCount != actualCount)
    {
        for (uint32_t index = 0; index < viewCount; ++index)
            vkDestroyImageView(renderer->device, views[index], NULL);
        vkDestroySwapchainKHR(renderer->device, swapchain, NULL);
        return false;
    }

    VkSemaphore imageAvailable[FRAME_COUNT];
    VkSemaphore renderFinished[MAX_SWAPCHAIN_IMAGES];
    for (uint32_t index = 0; index < FRAME_COUNT; ++index) imageAvailable[index] = VK_NULL_HANDLE;
    for (uint32_t index = 0; index < MAX_SWAPCHAIN_IMAGES; ++index)
        renderFinished[index] = VK_NULL_HANDLE;

    VkSemaphoreCreateInfo semaphoreInfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    uint32_t availableCount = 0u;
    uint32_t finishedCount = 0u;
    bool semaphoresOk = true;
    for (; availableCount < FRAME_COUNT; ++availableCount)
    {
        if (vkCreateSemaphore(renderer->device, &semaphoreInfo, NULL,
                              &imageAvailable[availableCount]) != VK_SUCCESS)
        {
            semaphoresOk = false;
            break;
        }
    }
    if (semaphoresOk)
    {
        for (; finishedCount < actualCount; ++finishedCount)
        {
            if (vkCreateSemaphore(renderer->device, &semaphoreInfo, NULL,
                                  &renderFinished[finishedCount]) != VK_SUCCESS)
            {
                semaphoresOk = false;
                break;
            }
        }
    }
    if (!semaphoresOk)
    {
        for (uint32_t index = 0; index < availableCount; ++index)
            vkDestroySemaphore(renderer->device, imageAvailable[index], NULL);
        for (uint32_t index = 0; index < finishedCount; ++index)
            vkDestroySemaphore(renderer->device, renderFinished[index], NULL);
        for (uint32_t index = 0; index < actualCount; ++index)
            vkDestroyImageView(renderer->device, views[index], NULL);
        vkDestroySwapchainKHR(renderer->device, swapchain, NULL);
        return false;
    }

    renderer->swapchain = swapchain;
    renderer->swapchainFormat = surfaceFormat.format;
    renderer->swapchainExtent = extent;
    renderer->swapchainImageCount = actualCount;
    renderer->swapchainImageIndex = 0u;
    for (uint32_t index = 0; index < actualCount; ++index)
    {
        renderer->swapchainImages[index] = images[index];
        renderer->swapchainViews[index] = views[index];
    }
    for (uint32_t index = 0; index < FRAME_COUNT; ++index)
        renderer->imageAvailable[index] = imageAvailable[index];
    for (uint32_t index = 0; index < actualCount; ++index)
        renderer->renderFinished[index] = renderFinished[index];
    renderer->swapchainOutOfDate = false;
    return true;
}

// Пересобрать swapchain под текущий размер окна и вертикальную
// синхронизацию. Вызывать до записи кадра: старый swapchain уничтожается
// только после полного простоя GPU.
static bool SwapchainRecreate(Renderer *renderer)
{
    if (!renderer->hasSurface) return false;
    WaitForGpu(renderer);
    DestroySwapchainResources(renderer);
    if (!SwapchainCreate(renderer, renderer->windowWidth, renderer->windowHeight))
    {
        renderer->swapchainOutOfDate = true;
        return false;
    }
    renderer->swapchainRecreateCount++;
    return true;
}

// Захват образа и запись копии colorTarget в команду кадра. Кадр уже
// переведён вызывающим в TRANSFER_SRC_OPTIMAL.
static PresentOutcome RecordPresent(Renderer *renderer, VkCommandBuffer commandBuffer)
{
    if (!renderer->hasSurface || renderer->swapchain == VK_NULL_HANDLE)
        return PRESENT_OUTCOME_NONE;

    uint32_t imageIndex = 0u;
    VkResult acquire = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, UINT64_MAX,
                                             renderer->imageAvailable[renderer->frameIndex],
                                             VK_NULL_HANDLE, &imageIndex);
    if (acquire != VK_SUCCESS)
    {
        // OUT_OF_DATE/SUBOPTIMAL — не ошибка кадра: помечаем swapchain
        // устаревшим, пересоберём в начале следующего кадра.
        renderer->swapchainOutOfDate = true;
        return PRESENT_OUTCOME_SKIP;
    }

    renderer->swapchainImageIndex = imageIndex;
    VkImage destination = renderer->swapchainImages[imageIndex];

    VkImageMemoryBarrier toTransfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0u,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = destination,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toTransfer);

    GpuImage *source = &renderer->colorTargets[renderer->frameIndex];
    bool sameFormat = renderer->swapchainFormat == COLOR_FORMAT;
    bool sameExtent = renderer->swapchainExtent.width == source->width &&
                      renderer->swapchainExtent.height == source->height;
    if (sameFormat && sameExtent)
    {
        VkImageCopy region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
            .srcOffset = { 0, 0, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
            .dstOffset = { 0, 0, 0 },
            .extent = { source->width, source->height, 1u },
        };
        vkCmdCopyImage(commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    }
    else
    {
        // Несовпадение формата/размера: blit конвертирует и при
        // необходимости масштабирует.
        VkImageBlit region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
            .srcOffsets = { { 0, 0, 0 },
                            { (int32_t)source->width, (int32_t)source->height, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
            .dstOffsets = { { 0, 0, 0 },
                            { (int32_t)renderer->swapchainExtent.width,
                              (int32_t)renderer->swapchainExtent.height, 1 } },
        };
        vkCmdBlitImage(commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region,
                       VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier toPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0u,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = destination,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
    return PRESENT_OUTCOME_PRESENT;
}

// === Создание и разрушение ===

static bool CreateDeviceObjects(Renderer *renderer, void *windowHandle)
{
    // Оконная поверхность — единственное, ради чего включаются
    // платформенные расширения инстанса.
    const char *instanceExtensions[4];
    uint32_t instanceExtensionCount = 0u;
#if defined(_WIN32)
    if (windowHandle != NULL)
    {
        const char *required[2] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                    VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        if (!InstanceExtensionsAvailable(required, 2u)) return false;
        instanceExtensions[instanceExtensionCount++] = required[0];
        instanceExtensions[instanceExtensionCount++] = required[1];
    }
#elif defined(LAIUE_RENDER_HAS_X11)
    if (windowHandle != NULL)
    {
        const char *required[2] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                    VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
        if (!InstanceExtensionsAvailable(required, 2u)) return false;
        instanceExtensions[instanceExtensionCount++] = required[0];
        instanceExtensions[instanceExtensionCount++] = required[1];
    }
#elif defined(__ANDROID__)
    if (windowHandle != NULL)
    {
        const char *required[2] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        if (!InstanceExtensionsAvailable(required, 2u)) return false;
        instanceExtensions[instanceExtensionCount++] = required[0];
        instanceExtensions[instanceExtensionCount++] = required[1];
    }
#else
    (void)windowHandle;
#endif

    VkApplicationInfo applicationInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "laiue",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &applicationInfo,
        .enabledExtensionCount = instanceExtensionCount,
        .ppEnabledExtensionNames = instanceExtensions,
    };
    if (vkCreateInstance(&instanceInfo, NULL, &renderer->instance) != VK_SUCCESS) return false;

#if defined(_WIN32)
    if (windowHandle != NULL)
    {
        VkWin32SurfaceCreateInfoKHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(NULL),
            .hwnd = (HWND)windowHandle,
        };
        if (vkCreateWin32SurfaceKHR(renderer->instance, &surfaceInfo, NULL, &renderer->surface) !=
            VK_SUCCESS)
            return false;
        renderer->hasSurface = true;
    }
#elif defined(LAIUE_RENDER_HAS_X11)
    if (windowHandle != NULL)
    {
        const LaiueWindowNativeHandleV1 *nativeHandle =
            (const LaiueWindowNativeHandleV1 *)windowHandle;
        if (nativeHandle->display == NULL || nativeHandle->window == 0u)
            return false;
        VkXlibSurfaceCreateInfoKHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = (Display *)nativeHandle->display,
            .window = (Window)nativeHandle->window,
        };
        if (vkCreateXlibSurfaceKHR(renderer->instance, &surfaceInfo, NULL,
                                   &renderer->surface) != VK_SUCCESS)
            return false;
        renderer->hasSurface = true;
    }
#elif defined(__ANDROID__)
    if (windowHandle != NULL)
    {
        VkAndroidSurfaceCreateInfoKHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
            .window = (ANativeWindow *)windowHandle,
        };
        if (vkCreateAndroidSurfaceKHR(renderer->instance, &surfaceInfo, NULL,
                                      &renderer->surface) != VK_SUCCESS)
            return false;
        renderer->hasSurface = true;
    }
#endif

    uint32_t deviceCount = 0u;
    if (vkEnumeratePhysicalDevices(renderer->instance, &deviceCount, NULL) != VK_SUCCESS ||
        deviceCount == 0u)
        return false;
    if (deviceCount > 8u) deviceCount = 8u;
    VkPhysicalDevice devices[8];
    if (vkEnumeratePhysicalDevices(renderer->instance, &deviceCount, devices) != VK_SUCCESS)
        return false;

    // Дискретная карта предпочтительнее, но CPU-драйвер тоже подходит:
    // именно он позволяет проверять рендер там, где GPU нет вовсе.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosenFamily = 0u;
    for (uint32_t pass = 0; pass < 2u && chosen == VK_NULL_HANDLE; ++pass)
    {
        for (uint32_t index = 0; index < deviceCount; ++index)
        {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(devices[index], &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2) continue;
            if (pass == 0u && properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                continue;

            uint32_t familyCount = 0u;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &familyCount, NULL);
            if (familyCount > 16u) familyCount = 16u;
            VkQueueFamilyProperties families[16];
            vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &familyCount, families);
            for (uint32_t family = 0; family < familyCount; ++family)
            {
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) continue;
                // Отдельную present-очередь не поддерживаем: годится
                // только та же семья, что умеет показывать в surface.
                if (renderer->hasSurface)
                {
                    VkBool32 presentSupport = VK_FALSE;
                    if (vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family,
                                                             renderer->surface,
                                                             &presentSupport) != VK_SUCCESS ||
                        presentSupport != VK_TRUE)
                        continue;
                }
                chosen = devices[index];
                chosenFamily = family;
                break;
            }
        }
    }
    if (chosen == VK_NULL_HANDLE) return false;

    renderer->physicalDevice = chosen;
    renderer->queueFamily = chosenFamily;
    vkGetPhysicalDeviceMemoryProperties(chosen, &renderer->memoryProperties);

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(chosen, &properties);
    renderer->uniformAlignment = properties.limits.minUniformBufferOffsetAlignment;
    if (renderer->uniformAlignment < sizeof(ChunkConstants))
        renderer->uniformAlignment = sizeof(ChunkConstants);
    renderer->storageAlignment = properties.limits.minStorageBufferOffsetAlignment;
    if (renderer->storageAlignment < 16u) renderer->storageAlignment = 16u;

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = chosenFamily,
        .queueCount = 1u,
        .pQueuePriorities = &queuePriority,
    };
    // Wireframe требует fillModeNonSolid; отсутствие возможности не должно
    // мешать обычному рендеру, поэтому она включается только при наличии.
    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(chosen, &available);
    VkPhysicalDeviceFeatures enabled = {
        .fillModeNonSolid = available.fillModeNonSolid,
        .imageCubeArray = available.imageCubeArray,
    };
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    renderer->useDynamicRendering = false;
    if (properties.apiVersion >= VK_API_VERSION_1_3)
    {
        VkPhysicalDeviceVulkan13Features available13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        };
        VkPhysicalDeviceFeatures2 availableFeatures = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &available13,
        };
        vkGetPhysicalDeviceFeatures2(chosen, &availableFeatures);
        renderer->useDynamicRendering = available13.dynamicRendering == VK_TRUE &&
                                        available13.synchronization2 == VK_TRUE;
    }
    // VK_KHR_swapchain включается только при наличии поверхности: без неё
    // offscreen-путь не должен зависеть от оконного расширения.
    const char *deviceExtensions[1];
    uint32_t deviceExtensionCount = 0u;
    if (renderer->hasSurface)
    {
        if (!DeviceExtensionSupported(chosen, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;
        deviceExtensions[deviceExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = renderer->useDynamicRendering ? &features13 : NULL,
        .queueCreateInfoCount = 1u,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = deviceExtensionCount,
        .ppEnabledExtensionNames = deviceExtensions,
        .pEnabledFeatures = &enabled,
    };
    if (vkCreateDevice(chosen, &deviceInfo, NULL, &renderer->device) != VK_SUCCESS) return false;
    if (renderer->useDynamicRendering)
    {
        renderer->cmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(
            renderer->device, "vkCmdBeginRendering");
        renderer->cmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(
            renderer->device, "vkCmdEndRendering");
        if (renderer->cmdBeginRendering == NULL || renderer->cmdEndRendering == NULL)
            return false;
    }
    vkGetDeviceQueue(renderer->device, chosenFamily, 0u, &renderer->queue);

    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = chosenFamily,
    };
    if (vkCreateCommandPool(renderer->device, &poolInfo, NULL, &renderer->commandPool) !=
        VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo commandBufferInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = renderer->commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = FRAME_COUNT,
    };
    if (vkAllocateCommandBuffers(renderer->device, &commandBufferInfo, renderer->commandBuffers) !=
        VK_SUCCESS)
        return false;

    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        VkFenceCreateInfo fenceInfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        if (vkCreateFence(renderer->device, &fenceInfo, NULL, &renderer->frameFences[frame]) !=
            VK_SUCCESS)
            return false;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    return vkCreateSampler(renderer->device, &samplerInfo, NULL, &renderer->sampler) == VK_SUCCESS;
}

static bool CreateDescriptorPool(Renderer *renderer)
{
    const uint32_t chunkSetCount = MAX_POOL_BLOCKS * FRAME_COUNT *
                                   INSTANCE_MAX_CHUNKS_PER_FRAME;
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, chunkSetCount + 4u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, chunkSetCount * 2u + 4u },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, chunkSetCount * 2u + 8u },
        { VK_DESCRIPTOR_TYPE_SAMPLER, chunkSetCount + 4u },
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        // Наборы блоков пула освобождаются вместе с миром, а не только
        // вместе с рендерером, поэтому пул обязан это разрешать.
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = chunkSetCount + 2u * FRAME_COUNT,
        .poolSizeCount = 4u,
        .pPoolSizes = sizes,
    };
    return vkCreateDescriptorPool(renderer->device, &poolInfo, NULL, &renderer->descriptorPool) ==
           VK_SUCCESS;
}

static bool CreateFrameBuffers(Renderer *renderer)
{
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        if (!BufferCreate(renderer, CONSTANT_BYTES_PER_FRAME, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          true, &renderer->constantBuffers[frame]))
            return false;
        if (!BufferCreate(renderer, MESH_UPLOAD_BYTES_PER_FRAME, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          true, &renderer->meshUploadBuffers[frame]))
            return false;
        uint32_t instanceChunkBytes = 0u;
        if (!AlignInstanceBytes(renderer, INSTANCE_CHUNK_BYTES, &instanceChunkBytes) ||
            instanceChunkBytes > INSTANCE_MAX_BYTES_PER_FRAME ||
            !CreateVulkanInstanceChunk(renderer, frame, 0u, instanceChunkBytes))
            return false;
        renderer->instanceChunkCount[frame] = 1u;
        renderer->instancePoolBytes[frame] = instanceChunkBytes;
        if (!BufferCreate(renderer, RENDERER_UI_MAX_QUADS * UI_QUAD_BYTES,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                          &renderer->uiQuadBuffers[frame]))
            return false;
    }
    return true;
}

static bool CreateSharedSets(Renderer *renderer)
{
    VkDescriptorSetLayout resolveLayouts[FRAME_COUNT];
    VkDescriptorSetLayout uiLayouts[FRAME_COUNT];
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        resolveLayouts[frame] = renderer->resolveSetLayout;
        uiLayouts[frame] = renderer->uiSetLayout;
    }
    VkDescriptorSetAllocateInfo resolveAllocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = renderer->descriptorPool,
        .descriptorSetCount = FRAME_COUNT,
        .pSetLayouts = resolveLayouts,
    };
    VkDescriptorSetAllocateInfo uiAllocate = resolveAllocate;
    uiAllocate.pSetLayouts = uiLayouts;
    if (vkAllocateDescriptorSets(renderer->device, &resolveAllocate, renderer->resolveSets) !=
            VK_SUCCESS ||
        vkAllocateDescriptorSets(renderer->device, &uiAllocate, renderer->uiSets) != VK_SUCCESS)
        return false;

    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        WriteBufferDescriptor(renderer, renderer->resolveSets[frame], BINDING_CONSTANTS,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                              renderer->constantBuffers[frame].buffer, sizeof(ResolveConstants));
        WriteImageDescriptor(renderer, renderer->resolveSets[frame], BINDING_PANORAMA_CUBE,
                             renderer->fallbackCube.view);
        WriteSamplerDescriptor(renderer, renderer->resolveSets[frame]);

        WriteBufferDescriptor(renderer, renderer->uiSets[frame], BINDING_CONSTANTS,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                              renderer->constantBuffers[frame].buffer, sizeof(UiConstants));
        WriteBufferDescriptor(renderer, renderer->uiSets[frame], BINDING_UI_QUADS,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                              renderer->uiQuadBuffers[frame].buffer, VK_WHOLE_SIZE);
        WriteImageDescriptor(renderer, renderer->uiSets[frame], BINDING_UI_FONT,
                             renderer->fallbackImage.view);
        WriteImageDescriptor(renderer, renderer->uiSets[frame], BINDING_UI_BACKGROUND,
                             renderer->fallbackImage.view);
        WriteSamplerDescriptor(renderer, renderer->uiSets[frame]);
    }
    return true;
}

Renderer *RendererCreate_Vulkan(void *windowHandle, int32_t width, int32_t height)
{
    // Headless Vulkan остаётся допустимым на любой платформе. Если
    // native handle передан, соответствующий surface provider должен быть
    // скомпилирован в этот renderer.
#if !defined(_WIN32) && !defined(LAIUE_RENDER_HAS_X11) && !defined(__ANDROID__)
    if (windowHandle != NULL) return NULL;
#else
    (void)windowHandle;
#endif

    Renderer *renderer = PlatformAllocate(sizeof(*renderer), true);
    if (renderer == NULL) return NULL;

    renderer->verticalSyncEnabled = true;
    renderer->texturePackLoadStatus = RENDERER_CONTENT_NOT_ATTEMPTED;

    if (!CreateDeviceObjects(renderer, windowHandle) || !CreateRenderPasses(renderer) ||
        !CreateDescriptorLayouts(renderer) ||
        !CreateDescriptorPool(renderer) || !CreateFrameBuffers(renderer) ||
        !CreateFallbackImages(renderer) || !CreateSharedSets(renderer))
    {
        RendererDestroy_Vulkan(renderer);
        return NULL;
    }

    // Размер целей кадра держим равным клиентской области surface, иначе
    // на Win32 (minImageExtent == maxImageExtent) swapchain не создастся.
    int32_t initialWidth = width;
    int32_t initialHeight = height;
    if (renderer->hasSurface)
    {
        VkSurfaceCapabilitiesKHR capabilities;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physicalDevice, renderer->surface,
                                                      &capabilities) == VK_SUCCESS &&
            capabilities.currentExtent.width != UINT32_MAX &&
            capabilities.currentExtent.height != UINT32_MAX &&
            capabilities.currentExtent.width > 0u && capabilities.currentExtent.height > 0u)
        {
            initialWidth = (int32_t)capabilities.currentExtent.width;
            initialHeight = (int32_t)capabilities.currentExtent.height;
        }
    }
    if (initialWidth <= 0) initialWidth = width > 0 ? width : 1;
    if (initialHeight <= 0) initialHeight = height > 0 ? height : 1;

    if (!CreateFrameTargets(renderer, initialWidth, initialHeight) ||
        (renderer->hasSurface && !SwapchainCreate(renderer, initialWidth, initialHeight)) ||
        !CreateResolvePipeline(renderer, &renderer->resolvePipeline) ||
        !CreateUiPipeline(renderer, &renderer->uiPipeline))
    {
        RendererDestroy_Vulkan(renderer);
        return NULL;
    }

    renderer->resizeWidth = initialWidth;
    renderer->resizeHeight = initialHeight;
    return renderer;
}

void RendererReleaseWorld_Vulkan(Renderer *renderer)
{
    if (renderer == NULL || !renderer->worldReady) return;

    WaitForGpu(renderer);
    DrainDeferredReleases(renderer, true);

    if (renderer->chunkPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(renderer->device, renderer->chunkPipeline, NULL);
        renderer->chunkPipeline = VK_NULL_HANDLE;
    }
    if (renderer->genericPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(renderer->device, renderer->genericPipeline, NULL);
        renderer->genericPipeline = VK_NULL_HANDLE;
    }
    ImageDestroy(renderer, &renderer->blockTexture);
    ImageDestroy(renderer, &renderer->blockNormalTexture);
    memset(&renderer->blockAnimation, 0, sizeof(renderer->blockAnimation));

    for (uint32_t blockIndex = 0; blockIndex < renderer->poolBlockCount; ++blockIndex)
    {
        GeometryPoolBlock *block = &renderer->poolBlocks[blockIndex];
        BufferDestroy(renderer, &block->buffer);
        if (block->freeRanges != NULL) PlatformFree(block->freeRanges);
        FreeBlockDescriptorSets(renderer, block);
        memset(block, 0, sizeof(*block));
    }
    renderer->poolBlockCount = 0u;
    renderer->poolCapacityBytes = 0u;
    renderer->poolUsedBytes = 0u;
    renderer->pendingUploadCount = 0u;
    renderer->worldReady = false;
    RefreshChunkSetTextures(renderer);
}

bool RendererPrepareWorldFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog)
{
    if (renderer == NULL) return false;
    if (renderer->worldReady) return true;

    TexturePackData pack;
    memset(&pack, 0, sizeof(pack));
    TexturePackLoadStatus status = catalog != NULL ? TexturePackLoadActiveFrom(catalog, renderer->materialNames.pointers,
                                  renderer->materialNames.count, &pack)
                                                   : TexturePackLoadActiveFrom(LaiueContentCatalogDefault(),
                                  renderer->materialNames.pointers,
                                  renderer->materialNames.count, &pack);
    renderer->texturePackLoadStatus = MapTexturePackLoadStatus(status);

    if (status == TEXTURE_PACK_LOAD_OK || status == TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        if (!CreateBlockArrayTexture(renderer, &pack, false, &renderer->blockTexture) ||
            !CreateBlockArrayTexture(renderer, &pack, true, &renderer->blockNormalTexture))
        {
            ImageDestroy(renderer, &renderer->blockTexture);
            ImageDestroy(renderer, &renderer->blockNormalTexture);
            renderer->texturePackLoadStatus = RENDERER_CONTENT_GPU_ERROR;
        }
        else
        {
            TexturePackCaptureAnimation(&renderer->blockAnimation, &pack);
        }
        TexturePackRelease(&pack);
    }

    if (!CreateChunkPipeline(renderer, &renderer->chunkPipeline) ||
        !CreateGenericPipeline(renderer, &renderer->genericPipeline))
    {
        if (renderer->genericPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(renderer->device, renderer->genericPipeline, NULL);
            renderer->genericPipeline = VK_NULL_HANDLE;
        }
        if (renderer->chunkPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(renderer->device, renderer->chunkPipeline, NULL);
            renderer->chunkPipeline = VK_NULL_HANDLE;
        }
        ImageDestroy(renderer, &renderer->blockTexture);
        ImageDestroy(renderer, &renderer->blockNormalTexture);
        memset(&renderer->blockAnimation, 0, sizeof(renderer->blockAnimation));
        return false;
    }

    renderer->worldReady = true;
    RefreshChunkSetTextures(renderer);
    return true;
}

bool RendererPrepareWorld_Vulkan(Renderer *renderer)
{
    return RendererPrepareWorldFrom_Vulkan(renderer, NULL);
}

bool RendererIsWorldReady_Vulkan(const Renderer *renderer)
{
    return renderer != NULL && renderer->worldReady;
}

void RendererDestroy_Vulkan(Renderer *renderer)
{
    if (renderer == NULL) return;
    if (renderer->device != VK_NULL_HANDLE) WaitForGpu(renderer);

    RendererReleaseWorld_Vulkan(renderer);
    if (renderer->device != VK_NULL_HANDLE)
    {
        DrainDeferredReleases(renderer, true);
        for (uint32_t slot = 0; slot < LAIUE_SHADER_SLOT_COUNT; ++slot)
        {
            if (renderer->loadedShaders[slot] != NULL) PlatformFree(renderer->loadedShaders[slot]);
        }
        ReleaseCubeResources(renderer);
        ReleaseFrameTargets(renderer);
        ImageDestroy(renderer, &renderer->fontTexture);
        ImageDestroy(renderer, &renderer->fallbackArray);
        ImageDestroy(renderer, &renderer->fallbackImage);
        ImageDestroy(renderer, &renderer->fallbackCube);
        for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
        {
            BufferDestroy(renderer, &renderer->constantBuffers[frame]);
            BufferDestroy(renderer, &renderer->meshUploadBuffers[frame]);
            BufferDestroy(renderer, &renderer->largeMeshUploadBuffers[frame]);
            for (uint32_t chunk = 0u; chunk < renderer->instanceChunkCount[frame]; ++chunk)
                BufferDestroy(renderer, &renderer->instanceBuffers[frame][chunk]);
            BufferDestroy(renderer, &renderer->uiQuadBuffers[frame]);
            if (renderer->frameFences[frame] != VK_NULL_HANDLE)
                vkDestroyFence(renderer->device, renderer->frameFences[frame], NULL);
        }
        if (renderer->resolvePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, renderer->resolvePipeline, NULL);
        if (renderer->uiPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, renderer->uiPipeline, NULL);
        if (renderer->chunkPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(renderer->device, renderer->chunkPipelineLayout, NULL);
        if (renderer->resolvePipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(renderer->device, renderer->resolvePipelineLayout, NULL);
        if (renderer->uiPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(renderer->device, renderer->uiPipelineLayout, NULL);
        if (renderer->chunkSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(renderer->device, renderer->chunkSetLayout, NULL);
        if (renderer->resolveSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(renderer->device, renderer->resolveSetLayout, NULL);
        if (renderer->uiSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(renderer->device, renderer->uiSetLayout, NULL);
        if (renderer->descriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(renderer->device, renderer->descriptorPool, NULL);
        if (renderer->sampler != VK_NULL_HANDLE)
            vkDestroySampler(renderer->device, renderer->sampler, NULL);
        if (renderer->commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(renderer->device, renderer->commandPool, NULL);
        DestroySwapchainResources(renderer);
        if (renderer->depthRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(renderer->device, renderer->depthRenderPass, NULL);
        if (renderer->colorRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(renderer->device, renderer->colorRenderPass, NULL);
        vkDestroyDevice(renderer->device, NULL);
    }
    if (renderer->surface != VK_NULL_HANDLE && renderer->instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);
    if (renderer->instance != VK_NULL_HANDLE) vkDestroyInstance(renderer->instance, NULL);
    PlatformFree(renderer);
}

// === Меши ===

// Создаёт (один раз на кадровый слот) upload-арену под крупные записи.
// Создание ленивое: приложение, чьи меши помещаются в основное кольцо,
// не платит за неё ни байтом.
static bool EnsureVulkanLargeMeshBuffer(Renderer *renderer, uint32_t frameIndex)
{
    if (renderer->largeMeshUploadBuffers[frameIndex].buffer != VK_NULL_HANDLE)
    {
        return true;
    }
    return BufferCreate(renderer, LARGE_MESH_UPLOAD_BYTES_PER_FRAME,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                        &renderer->largeMeshUploadBuffers[frameIndex]);
}

static RendererMesh *CreateMeshFromBytes_Vulkan(Renderer *renderer, const void *data,
                                                uint32_t sizeBytes, uint32_t elementCount,
                                                bool generic)
{
    if (renderer == NULL || !renderer->worldReady || data == NULL || sizeBytes == 0u ||
        elementCount == 0u ||
        renderer->pendingUploadCount == MAX_PENDING_UPLOADS)
        return NULL;

    uint32_t allocatedBytes = PoolSpanBytes(renderer, sizeBytes);
    uint32_t blockIndex = 0u;
    uint32_t offsetBytes = 0u;
    if (!PoolAllocate(renderer, sizeBytes, &blockIndex, &offsetBytes)) return NULL;
    if (!EnsureBlockDescriptorSets(renderer, blockIndex))
    {
        if (PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, allocatedBytes))
            renderer->poolUsedBytes -= allocatedBytes;
        return NULL;
    }

    GpuBuffer ownedStaging;
    memset(&ownedStaging, 0, sizeof(ownedStaging));
    VkBuffer staging = VK_NULL_HANDLE;
    uint32_t sourceOffset = AlignUp(renderer->meshUploadOffsets[renderer->frameIndex], 16u);
    bool ownsStaging = false;
    bool usedLargeRing = false;
    bool fitsSmallRing = sourceOffset <= MESH_UPLOAD_BYTES_PER_FRAME &&
                         sizeBytes <= MESH_UPLOAD_BYTES_PER_FRAME - sourceOffset;
    if (fitsSmallRing)
    {
        staging = renderer->meshUploadBuffers[renderer->frameIndex].buffer;
        memcpy(renderer->meshUploadBuffers[renderer->frameIndex].mapped + sourceOffset, data,
               sizeBytes);
        renderer->meshUploadOffsets[renderer->frameIndex] = sourceOffset + sizeBytes;
    }
    else
    {
        // Крупная запись: сначала пробуем общую арену того же кадра, и
        // только если запись не помещается и туда — отдельный буфер.
        uint32_t largeOffset =
            AlignUp(renderer->largeMeshUploadOffsets[renderer->frameIndex], 16u);
        bool fitsLargeRing =
            largeOffset <= LARGE_MESH_UPLOAD_BYTES_PER_FRAME &&
            sizeBytes <= LARGE_MESH_UPLOAD_BYTES_PER_FRAME - largeOffset;
        if (fitsLargeRing && EnsureVulkanLargeMeshBuffer(renderer, renderer->frameIndex))
        {
            staging = renderer->largeMeshUploadBuffers[renderer->frameIndex].buffer;
            sourceOffset = largeOffset;
            usedLargeRing = true;
            memcpy(renderer->largeMeshUploadBuffers[renderer->frameIndex].mapped + largeOffset,
                   data, sizeBytes);
            renderer->largeMeshUploadOffsets[renderer->frameIndex] = largeOffset + sizeBytes;
        }
        else
        {
            if (!BufferCreate(renderer, sizeBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                              &ownedStaging))
            {
                if (PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, allocatedBytes))
                    renderer->poolUsedBytes -= allocatedBytes;
                return NULL;
            }
            memcpy(ownedStaging.mapped, data, sizeBytes);
            staging = ownedStaging.buffer;
            sourceOffset = 0u;
            ownsStaging = true;
        }
    }

    RendererMesh *mesh = PlatformAllocate(sizeof(*mesh), false);
    if (mesh == NULL)
    {
        if (ownsStaging) BufferDestroy(renderer, &ownedStaging);
        else if (usedLargeRing)
            renderer->largeMeshUploadOffsets[renderer->frameIndex] = sourceOffset;
        else renderer->meshUploadOffsets[renderer->frameIndex] = sourceOffset;
        if (PoolFree(&renderer->poolBlocks[blockIndex], offsetBytes, allocatedBytes))
            renderer->poolUsedBytes -= allocatedBytes;
        return NULL;
    }

    mesh->blockIndex = blockIndex;
    mesh->offsetBytes = offsetBytes;
    mesh->sizeBytes = sizeBytes;
    mesh->poolSpanBytes = allocatedBytes;
    mesh->quadCount = generic ? 0u : elementCount;
    mesh->vertexCount = generic ? elementCount : 0u;
    mesh->generic = generic;

    PendingUpload *upload = &renderer->pendingUploads[renderer->pendingUploadCount++];
    upload->staging = staging;
    upload->stagingMemory = ownsStaging ? ownedStaging.memory : VK_NULL_HANDLE;
    upload->sourceOffset = sourceOffset;
    upload->blockIndex = blockIndex;
    upload->destinationOffset = offsetBytes;
    upload->sizeBytes = sizeBytes;
    return mesh;
}

RendererMesh *RendererCreateMesh_Vulkan(Renderer *renderer, const ChunkQuad *quads,
                                         uint32_t quadCount)
{
    if (quadCount == 0u || quadCount > UINT32_MAX / (uint32_t)sizeof(ChunkQuad))
        return NULL;
    return CreateMeshFromBytes_Vulkan(renderer, quads,
                                      quadCount * (uint32_t)sizeof(ChunkQuad),
                                      quadCount, false);
}

RendererMesh *RendererCreateGenericMesh_Vulkan(Renderer *renderer,
                                                const RendererGenericVertex *vertices,
                                                uint32_t vertexCount)
{
    if (vertexCount == 0u ||
        vertexCount > UINT32_MAX / (uint32_t)sizeof(RendererGenericVertex))
        return NULL;
    return CreateMeshFromBytes_Vulkan(renderer, vertices,
                                      vertexCount * (uint32_t)sizeof(RendererGenericVertex),
                                      vertexCount, true);
}

static VkFormat TextureFormat_Vulkan(uint32_t format)
{
    switch (format)
    {
    case LAIUE_GRAPHICS_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

RendererTexture *RendererCreateTexture_Vulkan(Renderer *renderer, uint32_t width,
                                               uint32_t height, uint32_t mipLevels,
                                               uint32_t format)
{
    if (renderer == NULL || renderer->device == VK_NULL_HANDLE || width == 0u ||
        height == 0u || mipLevels != 1u)
        return NULL;
    VkFormat vkFormat = TextureFormat_Vulkan(format);
    if (vkFormat == VK_FORMAT_UNDEFINED)
        return NULL;
    RendererTexture *texture = PlatformAllocate(sizeof(*texture), true);
    if (texture == NULL)
        return NULL;
    if (!ImageCreate(renderer, width, height, 1u, vkFormat,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false,
                     &texture->image))
    {
        PlatformFree(texture);
        return NULL;
    }
    return texture;
}

bool RendererUploadTexture_Vulkan(Renderer *renderer, RendererTexture *texture,
                                  const void *data, uint64_t sizeBytes,
                                  uint32_t rowPitchBytes)
{
    if (renderer == NULL || texture == NULL || data == NULL || renderer->frameRecording ||
        texture->image.image == VK_NULL_HANDLE || rowPitchBytes == 0u ||
        texture->image.width > UINT32_MAX / 4u ||
        rowPitchBytes != texture->image.width * 4u ||
        texture->image.height > UINT64_MAX / rowPitchBytes ||
        sizeBytes != (uint64_t)texture->image.height * rowPitchBytes)
        return false;
    return UploadImagePixels(renderer, &texture->image, (const uint8_t *)data, 4u, 1u);
}

void RendererDestroyTexture_Vulkan(Renderer *renderer, RendererTexture *texture)
{
    if (texture == NULL)
        return;
    if (renderer != NULL && renderer->device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(renderer->device);
    if (renderer != NULL)
        ImageDestroy(renderer, &texture->image);
    PlatformFree(texture);
}

void RendererDestroyMesh_Vulkan(Renderer *renderer, RendererMesh *mesh)
{
    if (renderer == NULL || mesh == NULL) return;

    if (renderer->deferredRangeCount == DEFERRED_RELEASE_CAPACITY)
    {
        WaitForGpu(renderer);
        DrainDeferredReleases(renderer, true);
    }

    uint32_t slot =
        (renderer->deferredRangeHead + renderer->deferredRangeCount) % DEFERRED_RELEASE_CAPACITY;
    renderer->deferredRanges[slot].blockIndex = mesh->blockIndex;
    renderer->deferredRanges[slot].offset = mesh->offsetBytes;
    renderer->deferredRanges[slot].size = mesh->poolSpanBytes;
    renderer->deferredRanges[slot].safeFrameIndex = renderer->submittedFrames + FRAME_COUNT;
    renderer->deferredRangeCount++;

    PlatformFree(mesh);
}

// Записывает копию констант в кольцо кадра и возвращает её смещение.
static bool PushConstants(Renderer *renderer, const void *bytes, uint32_t sizeBytes,
                          uint32_t *outOffset)
{
    uint32_t alignment = (uint32_t)renderer->uniformAlignment;
    uint32_t offset = AlignUp(renderer->constantOffsets[renderer->frameIndex], alignment);
    if (offset > CONSTANT_BYTES_PER_FRAME || sizeBytes > CONSTANT_BYTES_PER_FRAME - offset)
        return false;
    memcpy(renderer->constantBuffers[renderer->frameIndex].mapped + offset, bytes, sizeBytes);
    renderer->constantOffsets[renderer->frameIndex] = offset + sizeBytes;
    *outOffset = offset;
    return true;
}

static bool ReserveVulkanInstanceSpace(Renderer *renderer, uint32_t bytes,
                                       uint32_t *outChunkIndex, uint32_t *outOffset)
{
    if (bytes == 0u || bytes > INSTANCE_MAX_BYTES_PER_FRAME) return false;

    uint32_t alignment = InstanceStorageAlignment(renderer);
    if (alignment == 0u) return false;
    uint32_t chunkIndex = renderer->instanceChunkIndex[renderer->frameIndex];
    uint32_t currentOffset = renderer->instanceChunkOffset[renderer->frameIndex];
    if (currentOffset > UINT32_MAX - alignment + 1u) return false;
    uint32_t offset = AlignUp(currentOffset, alignment);

    while (chunkIndex < renderer->instanceChunkCount[renderer->frameIndex])
    {
        GpuBuffer *chunk = &renderer->instanceBuffers[renderer->frameIndex][chunkIndex];
        if (offset <= chunk->sizeBytes && bytes <= chunk->sizeBytes - offset) break;
        ++chunkIndex;
        offset = 0u;
    }

    if (chunkIndex == renderer->instanceChunkCount[renderer->frameIndex])
    {
        if (chunkIndex == INSTANCE_MAX_CHUNKS_PER_FRAME) return false;

        uint32_t capacityBytes = bytes > INSTANCE_CHUNK_BYTES ? bytes : INSTANCE_CHUNK_BYTES;
        if (!AlignInstanceBytes(renderer, capacityBytes, &capacityBytes) ||
            renderer->instancePoolBytes[renderer->frameIndex] > INSTANCE_MAX_BYTES_PER_FRAME ||
            capacityBytes > INSTANCE_MAX_BYTES_PER_FRAME -
                                renderer->instancePoolBytes[renderer->frameIndex])
            return false;
        if (!CreateVulkanInstanceChunk(renderer, renderer->frameIndex, chunkIndex, capacityBytes))
            return false;
        renderer->instanceChunkCount[renderer->frameIndex] = chunkIndex + 1u;
        renderer->instancePoolBytes[renderer->frameIndex] += capacityBytes;
        offset = 0u;
    }

    renderer->instanceChunkIndex[renderer->frameIndex] = chunkIndex;
    renderer->instanceChunkOffset[renderer->frameIndex] = offset + bytes;
    *outChunkIndex = chunkIndex;
    *outOffset = offset;
    return true;
}

static void DrawMeshInternal(Renderer *renderer, const RendererMesh *mesh, uint32_t instanceCount,
                             uint32_t instanceChunkIndex, uint32_t instanceOffset)
{
    if (mesh == NULL || mesh->generic)
        return;
    GeometryPoolBlock *block = &renderer->poolBlocks[mesh->blockIndex];
    if (instanceChunkIndex >= INSTANCE_MAX_CHUNKS_PER_FRAME) return;
    VkDescriptorSet set = block->sets[instanceChunkIndex][renderer->frameIndex];
    if (set == VK_NULL_HANDLE) return;

    uint32_t constantOffset = 0u;
    if (!PushConstants(renderer, &renderer->chunkConstants, sizeof(ChunkConstants),
                       &constantOffset))
        return;

    // VK_WHOLE_SIZE storage descriptors require zero dynamic offsets (06715).
    // VertexIndex and InstanceIndex include these bases without changing shaders.
    uint32_t dynamicOffsets[3] = { constantOffset, 0u, 0u };
    uint32_t firstVertex = (mesh->offsetBytes / (uint32_t)sizeof(ChunkQuad)) * 6u;
    uint32_t firstInstance = instanceOffset / (uint32_t)sizeof(RendererMeshInstance);
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->chunkPipelineLayout, 0u, 1u, &set, 3u, dynamicOffsets);
    vkCmdDraw(commandBuffer, mesh->quadCount * 6u, instanceCount, firstVertex, firstInstance);
    renderer->currentStats.drawCalls++;
    renderer->currentStats.drawnQuads += (uint64_t)mesh->quadCount * instanceCount;
}

static void DrawGenericMeshInternal(Renderer *renderer, const RendererMesh *mesh,
                                    const float originRelative[3], float scale,
                                    uint32_t firstVertex, uint32_t vertexCount)
{
    if (renderer == NULL || mesh == NULL || !mesh->generic ||
        !renderer->renderingActive || renderer->genericPipeline == VK_NULL_HANDLE ||
        firstVertex > mesh->vertexCount ||
        (vertexCount != UINT32_MAX && vertexCount > mesh->vertexCount - firstVertex))
        return;
    if (vertexCount == UINT32_MAX)
        vertexCount = mesh->vertexCount - firstVertex;
    GeometryPoolBlock *block = &renderer->poolBlocks[mesh->blockIndex];
    VkDescriptorSet set = block->sets[0][renderer->frameIndex];
    if (set == VK_NULL_HANDLE)
        return;
    renderer->chunkConstants.chunkOriginRelative[0] =
        originRelative != NULL ? originRelative[0] : 0.0f;
    renderer->chunkConstants.chunkOriginRelative[1] =
        originRelative != NULL ? originRelative[1] : 0.0f;
    renderer->chunkConstants.chunkOriginRelative[2] =
        originRelative != NULL ? originRelative[2] : 0.0f;
    renderer->chunkConstants.meshScale = scale;
    uint32_t constantOffset = 0u;
    if (!PushConstants(renderer, &renderer->chunkConstants,
                       sizeof(renderer->chunkConstants), &constantOffset))
        return;
    // Generic records are allocated from the shared byte pool, whose
    // alignment is the device's storage-buffer alignment rather than a
    // multiple of RendererGenericVertex.  Bind the mesh byte offset as the
    // dynamic storage offset and keep firstVertex relative to that mesh;
    // deriving it by dividing offsetBytes by the 24-byte record size would
    // silently round and read the preceding allocation.
    uint32_t dynamicOffsets[3] = {constantOffset, mesh->offsetBytes, 0u};
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      renderer->genericPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->chunkPipelineLayout, 0u, 1u, &set, 3u,
                            dynamicOffsets);
    vkCmdDraw(commandBuffer, vertexCount, 1u, firstVertex, 0u);
    renderer->currentStats.drawCalls++;
    renderer->currentStats.drawnQuads += vertexCount / 3u;
}

void RendererDrawMesh_Vulkan(Renderer *renderer, const RendererMesh *mesh,
                      const float chunkOriginRelative[3])
{
    if (renderer == NULL || mesh == NULL || mesh->generic || !renderer->renderingActive) return;

    renderer->chunkConstants.chunkOriginRelative[0] = chunkOriginRelative[0];
    renderer->chunkConstants.chunkOriginRelative[1] = chunkOriginRelative[1];
    renderer->chunkConstants.chunkOriginRelative[2] = chunkOriginRelative[2];
    renderer->chunkConstants.meshScale = 1.0f;
    vkCmdBindPipeline(renderer->commandBuffers[renderer->frameIndex],
                      VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->chunkPipeline);
    DrawMeshInternal(renderer, mesh, 1u, 0u, 0u);
}

void RendererDrawGenericMesh_Vulkan(Renderer *renderer, const RendererMesh *mesh,
                                    const float originRelative[3], float scale,
                                    uint32_t firstVertex, uint32_t vertexCount)
{
    DrawGenericMeshInternal(renderer, mesh, originRelative,
                            scale == 0.0f ? 1.0f : scale, firstVertex,
                            vertexCount);
}

void RendererDrawMeshInstances_Vulkan(Renderer *renderer, const RendererMesh *mesh,
                               const RendererMeshInstance *instances, uint32_t instanceCount)
{
    if (renderer == NULL || mesh == NULL || mesh->generic || instances == NULL || instanceCount == 0u ||
        !renderer->renderingActive ||
        instanceCount > INSTANCE_MAX_BYTES_PER_FRAME / (uint32_t)sizeof(RendererMeshInstance))
        return;

    uint32_t bytes = instanceCount * (uint32_t)sizeof(RendererMeshInstance);
    uint32_t chunkIndex = 0u;
    uint32_t offset = 0u;
    if (!ReserveVulkanInstanceSpace(renderer, bytes, &chunkIndex, &offset) ||
        !EnsureBlockInstanceDescriptorSet(renderer, mesh->blockIndex, renderer->frameIndex,
                                          chunkIndex))
        return;
    memcpy(renderer->instanceBuffers[renderer->frameIndex][chunkIndex].mapped + offset, instances,
           bytes);

    // Отрицательный масштаб — признак инстансного пути в chunk.hlsl.
    renderer->chunkConstants.chunkOriginRelative[0] = 0.0f;
    renderer->chunkConstants.chunkOriginRelative[1] = 0.0f;
    renderer->chunkConstants.chunkOriginRelative[2] = 0.0f;
    renderer->chunkConstants.meshScale = -1.0f;
    vkCmdBindPipeline(renderer->commandBuffers[renderer->frameIndex],
                      VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->chunkPipeline);
    DrawMeshInternal(renderer, mesh, instanceCount, chunkIndex, offset);
}

// === Кадр ===

static void EndRenderingIfActive(Renderer *renderer)
{
    if (!renderer->renderingActive) return;
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    if (renderer->useDynamicRendering)
        renderer->cmdEndRendering(commandBuffer);
    else
        vkCmdEndRenderPass(commandBuffer);
    renderer->renderingActive = false;
}

static void BeginLegacyRenderPass(Renderer *renderer, VkFramebuffer framebuffer,
                                  bool depth, int32_t x, int32_t y, uint32_t width,
                                  uint32_t height, const float clearColor[4],
                                  bool clearDepth)
{
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    VkRenderPassBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = depth ? renderer->depthRenderPass : renderer->colorRenderPass,
        .framebuffer = framebuffer,
        .renderArea = { { x, y }, { width, height } },
    };
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkClearAttachment clears[2];
    uint32_t clearCount = 0u;
    if (clearColor != NULL)
    {
        clears[clearCount] = (VkClearAttachment){
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0u,
        };
        memcpy(clears[clearCount].clearValue.color.float32, clearColor, sizeof(float) * 4u);
        ++clearCount;
    }
    if (depth && clearDepth)
    {
        clears[clearCount] = (VkClearAttachment){
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .clearValue = { .depthStencil = { 1.0f, 0u } },
        };
        ++clearCount;
    }
    if (clearCount != 0u)
    {
        VkClearRect rect = {
            .rect = { { x, y }, { width, height } },
            .baseArrayLayer = 0u,
            .layerCount = 1u,
        };
        vkCmdClearAttachments(commandBuffer, clearCount, clears, 1u, &rect);
    }
    renderer->renderingActive = true;
}

static void SetViewportAndScissor(VkCommandBuffer commandBuffer, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height)
{
    // Отрицательная высота возвращает соглашение D3D по оси Y, поэтому
    // HLSL из shaders/ работает без правок и обход граней не меняется.
    VkViewport viewport = {
        .x = (float)x,
        .y = (float)(y + (int32_t)height),
        .width = (float)width,
        .height = -(float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = { { x, y }, { width, height } };
    vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
    vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);
}

static void RecordPendingUploads(Renderer *renderer)
{
    if (renderer->pendingUploadCount == 0u) return;

    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    for (uint32_t index = 0; index < renderer->pendingUploadCount; ++index)
    {
        PendingUpload *upload = &renderer->pendingUploads[index];
        VkBufferCopy region = {
            .srcOffset = upload->sourceOffset,
            .dstOffset = upload->destinationOffset,
            .size = upload->sizeBytes,
        };
        vkCmdCopyBuffer(commandBuffer, upload->staging,
                        renderer->poolBlocks[upload->blockIndex].buffer.buffer, 1u, &region);
        renderer->currentStats.uploadedBytes += upload->sizeBytes;
        if (upload->stagingMemory != VK_NULL_HANDLE)
            DeferBufferRelease(renderer, upload->staging, upload->stagingMemory);
    }
    renderer->pendingUploadCount = 0u;

    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1u, &barrier, 0, NULL, 0, NULL);
}

bool RendererBeginFrame_Vulkan(Renderer *renderer, const RendererFrameSetup *frame)
{
    if (renderer == NULL || frame == NULL || frame->passCount > RENDERER_MAX_SCENE_PASSES ||
        (frame->passCount != 0u && !renderer->worldReady))
        return false;

    vkWaitForFences(renderer->device, 1u, &renderer->frameFences[renderer->frameIndex], VK_TRUE,
                    UINT64_MAX);

    // Свёрнутое окно (нулевой запрошенный размер): кадр пропускаем без
    // ошибки, пока размер не восстановят. Для offscreen это недостижимо.
    if (renderer->hasSurface && (renderer->resizeWidth <= 0 || renderer->resizeHeight <= 0))
        return false;

    if (renderer->resizeRequested && !ApplyPendingResize(renderer)) return false;

    // Resize или смена vsync пометили swapchain устаревшим: пересобираем
    // его до записи кадра.
    if (renderer->hasSurface && renderer->swapchainOutOfDate &&
        !SwapchainRecreate(renderer))
        return false;

    if (renderer->colorTargets[renderer->frameIndex].image == VK_NULL_HANDLE ||
        (frame->passCount != 0u && renderer->depthTarget.image == VK_NULL_HANDLE))
        return false;

    renderer->frame = *frame;
    renderer->uiQuadCount = 0u;
    renderer->constantOffsets[renderer->frameIndex] = 0u;
    renderer->meshUploadOffsets[renderer->frameIndex] = 0u;
    renderer->largeMeshUploadOffsets[renderer->frameIndex] = 0u;
    renderer->instanceChunkIndex[renderer->frameIndex] = 0u;
    renderer->instanceChunkOffset[renderer->frameIndex] = 0u;
    memset(&renderer->currentStats, 0, sizeof(renderer->currentStats));
    renderer->currentStats.scenePasses = frame->passCount;

    if (renderer->frame.panorama && !EnsureCubeResources(renderer, renderer->frame.faceResolution))
        return false;

    DrainDeferredReleases(renderer, false);

    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    vkResetCommandBuffer(commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) return false;
    renderer->frameRecording = true;
    renderer->renderingActive = false;

    RecordPendingUploads(renderer);

    ImageBarrier(commandBuffer, &renderer->colorTargets[renderer->frameIndex],
                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ImageBarrier(commandBuffer, &renderer->depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (renderer->cubeColor.image != VK_NULL_HANDLE)
    {
        ImageBarrier(commandBuffer, &renderer->cubeColor, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ImageBarrier(commandBuffer, &renderer->cubeDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    // Свет и число слоёв одинаковы для всех проходов кадра; проход
    // добавляет свою viewProjection, а отрисовка — смещение чанка.
    float gammaInverse = frame->gamma > 0.01f ? 1.0f / frame->gamma : 1.0f;
    memset(&renderer->chunkConstants, 0, sizeof(renderer->chunkConstants));
    memcpy(renderer->chunkConstants.sunDirection, frame->sunDirection, sizeof(float) * 3u);
    memcpy(renderer->chunkConstants.sunColor, frame->sunColor, sizeof(float) * 3u);
    memcpy(renderer->chunkConstants.ambientColor, frame->ambientColor, sizeof(float) * 3u);
    renderer->chunkConstants.materialCount =
        (float)(renderer->blockAnimation.materialCount > 0u ? renderer->blockAnimation.materialCount
                                                            : 1u);
    TexturePackFillSliceTable(&renderer->blockAnimation, frame->animationSeconds,
                              renderer->chunkConstants.materialSlices);
    renderer->chunkConstants.gammaInverse = gammaInverse;
    renderer->chunkConstants.meshScale = 1.0f;

    if (frame->passCount == 0u)
    {
        const float clearColor[4] = {
            frame->skyColor[0], frame->skyColor[1], frame->skyColor[2], 1.0f,
        };
        if (!renderer->useDynamicRendering)
        {
            BeginLegacyRenderPass(renderer, renderer->colorFramebuffers[renderer->frameIndex],
                                  false, 0, 0, (uint32_t)renderer->windowWidth,
                                  (uint32_t)renderer->windowHeight, clearColor, false);
            SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                                  (uint32_t)renderer->windowHeight);
            EndRenderingIfActive(renderer);
            return true;
        }
        VkRenderingAttachmentInfo colorAttachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = renderer->colorTargets[renderer->frameIndex].view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        colorAttachment.clearValue.color.float32[0] = frame->skyColor[0];
        colorAttachment.clearValue.color.float32[1] = frame->skyColor[1];
        colorAttachment.clearValue.color.float32[2] = frame->skyColor[2];
        colorAttachment.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo renderingInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { { 0, 0 },
                            { (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight } },
            .layerCount = 1u,
            .colorAttachmentCount = 1u,
            .pColorAttachments = &colorAttachment,
        };
        renderer->cmdBeginRendering(commandBuffer, &renderingInfo);
        SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight);
        renderer->cmdEndRendering(commandBuffer);
    }
    return true;
}

void RendererBeginScenePass_Vulkan(Renderer *renderer, uint32_t passIndex)
{
    if (renderer == NULL || !renderer->frameRecording || !renderer->worldReady ||
        passIndex >= renderer->frame.passCount)
        return;

    const RendererScenePass *pass = &renderer->frame.passes[passIndex];
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    EndRenderingIfActive(renderer);

    VkImageView colorView;
    VkImageView depthView;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    if (renderer->frame.panorama)
    {
        uint32_t faceIndex = pass->faceIndex < 6u ? pass->faceIndex : 0u;
        colorView = renderer->cubeFaceViews[faceIndex];
        depthView = renderer->cubeDepth.view;
        x = (int32_t)pass->rectMinX;
        y = (int32_t)pass->rectMinY;
        width = pass->rectMaxX > pass->rectMinX ? pass->rectMaxX - pass->rectMinX : 0u;
        height = pass->rectMaxY > pass->rectMinY ? pass->rectMaxY - pass->rectMinY : 0u;
    }
    else
    {
        colorView = renderer->colorTargets[renderer->frameIndex].view;
        depthView = renderer->depthTarget.view;
        x = 0;
        y = 0;
        width = (uint32_t)renderer->windowWidth;
        height = (uint32_t)renderer->windowHeight;
    }
    if (width == 0u || height == 0u || colorView == VK_NULL_HANDLE) return;

    if (!renderer->useDynamicRendering)
    {
        const float clearColor[4] = {
            renderer->frame.skyColor[0], renderer->frame.skyColor[1],
            renderer->frame.skyColor[2], 1.0f,
        };
        VkFramebuffer framebuffer = renderer->frame.panorama
                                        ? renderer->cubeFramebuffers[pass->faceIndex < 6u
                                                                         ? pass->faceIndex
                                                                         : 0u]
                                        : renderer->depthFramebuffers[renderer->frameIndex];
        BeginLegacyRenderPass(renderer, framebuffer, true, x, y, width, height,
                              clearColor, true);
        SetViewportAndScissor(commandBuffer, x, y, width, height);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          renderer->chunkPipeline);
        memcpy(renderer->chunkConstants.viewProjection, pass->viewProjection,
               sizeof(float) * 16u);
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = colorView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    colorAttachment.clearValue.color.float32[0] = renderer->frame.skyColor[0];
    colorAttachment.clearValue.color.float32[1] = renderer->frame.skyColor[1];
    colorAttachment.clearValue.color.float32[2] = renderer->frame.skyColor[2];
    colorAttachment.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo depthAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    depthAttachment.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { x, y }, { width, height } },
        .layerCount = 1u,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    };
    renderer->cmdBeginRendering(commandBuffer, &renderingInfo);
    renderer->renderingActive = true;

    SetViewportAndScissor(commandBuffer, x, y, width, height);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->chunkPipeline);
    memcpy(renderer->chunkConstants.viewProjection, pass->viewProjection, sizeof(float) * 16u);
}

static void RecordPanoramaResolve(Renderer *renderer)
{
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    ImageBarrier(commandBuffer, &renderer->cubeColor, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ResolveConstants constants = {
        .fovHalfRadians = renderer->frame.fovHalfRadians,
        .verticalScale = renderer->frame.resolveVerticalScale,
        .mapping = (uint32_t)renderer->frame.resolveMapping,
    };
    uint32_t constantOffset = 0u;
    if (!PushConstants(renderer, &constants, sizeof(constants), &constantOffset)) return;

    if (!renderer->useDynamicRendering)
    {
        BeginLegacyRenderPass(renderer, renderer->colorFramebuffers[renderer->frameIndex],
                              false, 0, 0, (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight, NULL, false);
        SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          renderer->resolvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer->resolvePipelineLayout, 0u, 1u,
                                &renderer->resolveSets[renderer->frameIndex], 1u,
                                &constantOffset);
        vkCmdDraw(commandBuffer, 3u, 1u, 0u, 0u);
        EndRenderingIfActive(renderer);
        renderer->currentStats.drawCalls++;
        ImageBarrier(commandBuffer, &renderer->cubeColor, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = renderer->colorTargets[renderer->frameIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 },
                        { (uint32_t)renderer->windowWidth, (uint32_t)renderer->windowHeight } },
        .layerCount = 1u,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &colorAttachment,
    };
    renderer->cmdBeginRendering(commandBuffer, &renderingInfo);
    SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                          (uint32_t)renderer->windowHeight);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->resolvePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->resolvePipelineLayout, 0u, 1u,
                            &renderer->resolveSets[renderer->frameIndex], 1u, &constantOffset);
    vkCmdDraw(commandBuffer, 3u, 1u, 0u, 0u);
    renderer->cmdEndRendering(commandBuffer);
    renderer->currentStats.drawCalls++;

    ImageBarrier(commandBuffer, &renderer->cubeColor, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

static void RecordUiLayer(Renderer *renderer)
{
    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    UiConstants constants = {
        .screenSize = { (float)renderer->windowWidth, (float)renderer->windowHeight },
    };
    uint32_t constantOffset = 0u;
    if (!PushConstants(renderer, &constants, sizeof(constants), &constantOffset)) return;

    if (!renderer->useDynamicRendering)
    {
        BeginLegacyRenderPass(renderer, renderer->colorFramebuffers[renderer->frameIndex],
                              false, 0, 0, (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight, NULL, false);
        SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                              (uint32_t)renderer->windowHeight);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          renderer->uiPipeline);
        uint32_t dynamicOffsets[2] = { constantOffset, 0u };
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                renderer->uiPipelineLayout, 0u, 1u,
                                &renderer->uiSets[renderer->frameIndex], 2u,
                                dynamicOffsets);
        vkCmdDraw(commandBuffer, renderer->uiQuadCount * 6u, 1u, 0u, 0u);
        EndRenderingIfActive(renderer);
        renderer->currentStats.drawCalls++;
        return;
    }

    VkRenderingAttachmentInfo colorAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = renderer->colorTargets[renderer->frameIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 },
                        { (uint32_t)renderer->windowWidth, (uint32_t)renderer->windowHeight } },
        .layerCount = 1u,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &colorAttachment,
    };
    renderer->cmdBeginRendering(commandBuffer, &renderingInfo);
    SetViewportAndScissor(commandBuffer, 0, 0, (uint32_t)renderer->windowWidth,
                          (uint32_t)renderer->windowHeight);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->uiPipeline);
    uint32_t dynamicOffsets[2] = { constantOffset, 0u };
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            renderer->uiPipelineLayout, 0u, 1u,
                            &renderer->uiSets[renderer->frameIndex], 2u, dynamicOffsets);
    vkCmdDraw(commandBuffer, renderer->uiQuadCount * 6u, 1u, 0u, 0u);
    renderer->cmdEndRendering(commandBuffer);
    renderer->currentStats.drawCalls++;
}

bool RendererEndFrame_Vulkan(Renderer *renderer)
{
    if (renderer == NULL || !renderer->frameRecording) return false;

    VkCommandBuffer commandBuffer = renderer->commandBuffers[renderer->frameIndex];
    EndRenderingIfActive(renderer);

    if (renderer->frame.panorama && renderer->cubeResolution != 0u) RecordPanoramaResolve(renderer);
    if (renderer->uiQuadCount > 0u && renderer->fontReady) RecordUiLayer(renderer);

    // Без swapchain «показ» кадра — это перевод цели в состояние, из
    // которого её можно прочитать: именно так кадр проверяется в тестах.
    // С swapchain цель становится источником копии в образ показа.
    ImageBarrier(commandBuffer, &renderer->colorTargets[renderer->frameIndex],
                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    PresentOutcome present = RecordPresent(renderer, commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
    {
        renderer->frameRecording = false;
        return false;
    }
    renderer->frameRecording = false;

    vkResetFences(renderer->device, 1u, &renderer->frameFences[renderer->frameIndex]);
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1u,
        .pCommandBuffers = &commandBuffer,
    };
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (present == PRESENT_OUTCOME_PRESENT)
    {
        // Копия в swapchain ждёт готовности образа, последующий present —
        // готовности копии. Семафоры возвращаются кадровым слотом и
        // индексом образа, поэтому не переиспользуются под живой работой.
        submitInfo.waitSemaphoreCount = 1u;
        submitInfo.pWaitSemaphores = &renderer->imageAvailable[renderer->frameIndex];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1u;
        submitInfo.pSignalSemaphores = &renderer->renderFinished[renderer->swapchainImageIndex];
    }
    if (vkQueueSubmit(renderer->queue, 1u, &submitInfo,
                      renderer->frameFences[renderer->frameIndex]) != VK_SUCCESS)
        return false;

    if (present == PRESENT_OUTCOME_PRESENT)
    {
        uint32_t imageIndex = renderer->swapchainImageIndex;
        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1u,
            .pWaitSemaphores = &renderer->renderFinished[imageIndex],
            .swapchainCount = 1u,
            .pSwapchains = &renderer->swapchain,
            .pImageIndices = &imageIndex,
        };
        VkResult presentResult = vkQueuePresentKHR(renderer->queue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            renderer->swapchainOutOfDate = true;
        else if (presentResult != VK_SUCCESS)
            return false;
    }

    renderer->lastStats = renderer->currentStats;
    renderer->lastFrameIndex = renderer->frameIndex;
    renderer->lastFrameValid = true;
    renderer->submittedFrames++;
    renderer->frameIndex = (renderer->frameIndex + 1u) % FRAME_COUNT;
    return true;
}

void RendererGetStats_Vulkan(const Renderer *renderer, RendererStats *outStats)
{
    if (renderer == NULL || outStats == NULL) return;

    *outStats = renderer->lastStats;
    outStats->geometryPoolCapacityBytes = renderer->poolCapacityBytes;
    outStats->geometryPoolUsedBytes = renderer->poolUsedBytes;
}

void RendererSetVerticalSync_Vulkan(Renderer *renderer, bool enabled)
{
    // Смена режима не трогает GPU немедленно: swapchain помечается
    // устаревшим и пересобирается в начале следующего кадра. Без present
    // значение просто сохраняется: приложение вправе его читать.
    if (renderer == NULL || renderer->verticalSyncEnabled == enabled) return;
    renderer->verticalSyncEnabled = enabled;
    if (renderer->hasSurface) renderer->swapchainOutOfDate = true;
}

bool RendererIsVerticalSyncEnabled_Vulkan(const Renderer *renderer)
{
    return renderer != NULL && renderer->verticalSyncEnabled;
}

void RendererResize_Vulkan(Renderer *renderer, int32_t width, int32_t height)
{
    if (renderer == NULL) return;
    if (width <= 0 || height <= 0)
    {
        // Нулевой размер — свёрнутое окно: кадры пропускаются до
        // восстановления. У offscreen-рендера прежнее поведение (отказ).
        if (!renderer->hasSurface) return;
        width = 0;
        height = 0;
    }
    renderer->resizeWidth = width;
    renderer->resizeHeight = height;
    renderer->resizeRequested = true;
}

// === Контент ===

bool RendererSetMaterialNames_Vulkan(Renderer *renderer, const wchar_t *const *names, uint32_t count)
{
    if (renderer == NULL) return false;
    return TexturePackMaterialNamesSet(&renderer->materialNames, names, count);
}

bool RendererReloadTexturePackFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog)
{
    if (renderer == NULL || !renderer->worldReady) return false;

    TexturePackData pack;
    memset(&pack, 0, sizeof(pack));
    TexturePackLoadStatus status = catalog != NULL ? TexturePackLoadActiveFrom(catalog, renderer->materialNames.pointers,
                                  renderer->materialNames.count, &pack)
                                                   : TexturePackLoadActiveFrom(LaiueContentCatalogDefault(),
                                  renderer->materialNames.pointers,
                                  renderer->materialNames.count, &pack);
    renderer->texturePackLoadStatus = MapTexturePackLoadStatus(status);
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
        return false;

    // Обнуление нужно MSVC: при неудаче первого CreateBlockArrayTexture
    // второй не вызывается, и без него компилятор считает normals
    // возможно неинициализированной (C4701) на общем пути присваивания.
    GpuImage albedo = {0};
    GpuImage normals = {0};
    bool built = CreateBlockArrayTexture(renderer, &pack, false, &albedo) &&
                 CreateBlockArrayTexture(renderer, &pack, true, &normals);
    TexturePackAnimationSet animation;
    TexturePackCaptureAnimation(&animation, &pack);
    TexturePackRelease(&pack);
    if (!built)
    {
        ImageDestroy(renderer, &albedo);
        renderer->texturePackLoadStatus = RENDERER_CONTENT_GPU_ERROR;
        return false;
    }

    // Подмена транзакционна: старые текстуры живут до конца кадров,
    // которые могли их читать.
    WaitForGpu(renderer);
    ImageDestroy(renderer, &renderer->blockTexture);
    ImageDestroy(renderer, &renderer->blockNormalTexture);
    renderer->blockTexture = albedo;
    renderer->blockNormalTexture = normals;
    renderer->blockAnimation = animation;
    RefreshChunkSetTextures(renderer);
    return true;
}

bool RendererReloadTexturePack_Vulkan(Renderer *renderer)
{
    return RendererReloadTexturePackFrom_Vulkan(renderer, NULL);
}

RendererContentStatus RendererGetTexturePackLoadStatus_Vulkan(const Renderer *renderer)
{
    return renderer != NULL ? renderer->texturePackLoadStatus : RENDERER_CONTENT_NOT_ATTEMPTED;
}

void RendererSetWireframe_Vulkan(Renderer *renderer, bool enabled)
{
    if (renderer == NULL || renderer->wireframeEnabled == enabled) return;
    renderer->wireframeEnabled = enabled;
    if (!renderer->worldReady) return;

    VkPipeline replacement = VK_NULL_HANDLE;
    VkPipeline genericReplacement = VK_NULL_HANDLE;
    if (!CreateChunkPipeline(renderer, &replacement) ||
        !CreateGenericPipeline(renderer, &genericReplacement))
    {
        if (replacement != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, replacement, NULL);
        if (genericReplacement != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, genericReplacement, NULL);
        renderer->wireframeEnabled = !enabled;
        return;
    }
    WaitForGpu(renderer);
    vkDestroyPipeline(renderer->device, renderer->chunkPipeline, NULL);
    vkDestroyPipeline(renderer->device, renderer->genericPipeline, NULL);
    renderer->chunkPipeline = replacement;
    renderer->genericPipeline = genericReplacement;
}

bool RendererIsWireframe_Vulkan(const Renderer *renderer)
{
    return renderer != NULL && renderer->wireframeEnabled;
}

static void ReleaseShaderArray(void *shaders[LAIUE_SHADER_SLOT_COUNT])
{
    for (uint32_t slot = 0; slot < LAIUE_SHADER_SLOT_COUNT; ++slot)
    {
        if (shaders[slot] != NULL) PlatformFree(shaders[slot]);
        shaders[slot] = NULL;
    }
}

bool RendererReloadShaderSet_Vulkan(Renderer *renderer, const LaiueShaderSet *shaderSet)
{
    if (renderer == NULL) return false;
    if (shaderSet != NULL && !LaiueShaderSetIsValid(shaderSet)) return false;

    void *replacements[LAIUE_SHADER_SLOT_COUNT];
    uint32_t lengths[LAIUE_SHADER_SLOT_COUNT];
    memset(replacements, 0, sizeof(replacements));
    memset(lengths, 0, sizeof(lengths));

    if (shaderSet != NULL)
    {
        for (uint32_t slot = 0; slot < LAIUE_SHADER_SLOT_COUNT; ++slot)
        {
            if ((shaderSet->overrideMask & LAIUE_SHADER_SLOT_MASK(slot)) == 0u) continue;
            const LaiueShaderBytecode *entry = &shaderSet->bytecode[slot];
            if (entry->bytes == NULL || entry->sizeBytes == 0u)
            {
                ReleaseShaderArray(replacements);
                return false;
            }
            replacements[slot] = PlatformAllocate(entry->sizeBytes, false);
            if (replacements[slot] == NULL)
            {
                ReleaseShaderArray(replacements);
                return false;
            }
            memcpy(replacements[slot], entry->bytes, entry->sizeBytes);
            lengths[slot] = entry->sizeBytes;
        }
    }

    void *previousShaders[LAIUE_SHADER_SLOT_COUNT];
    uint32_t previousLengths[LAIUE_SHADER_SLOT_COUNT];
    memcpy(previousShaders, renderer->loadedShaders, sizeof(previousShaders));
    memcpy(previousLengths, renderer->loadedShaderLengths, sizeof(previousLengths));
    memcpy(renderer->loadedShaders, replacements, sizeof(replacements));
    memcpy(renderer->loadedShaderLengths, lengths, sizeof(lengths));

    VkPipeline chunkPipeline = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    bool ok = CreateResolvePipeline(renderer, &resolvePipeline) &&
              CreateUiPipeline(renderer, &uiPipeline) &&
              (!renderer->worldReady || CreateChunkPipeline(renderer, &chunkPipeline));
    if (!ok)
    {
        // Ни один конвейер не заменён: набор остаётся прежним целиком.
        if (resolvePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, resolvePipeline, NULL);
        if (uiPipeline != VK_NULL_HANDLE) vkDestroyPipeline(renderer->device, uiPipeline, NULL);
        ReleaseShaderArray(replacements);
        memcpy(renderer->loadedShaders, previousShaders, sizeof(previousShaders));
        memcpy(renderer->loadedShaderLengths, previousLengths, sizeof(previousLengths));
        return false;
    }

    WaitForGpu(renderer);
    if (renderer->resolvePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(renderer->device, renderer->resolvePipeline, NULL);
    if (renderer->uiPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(renderer->device, renderer->uiPipeline, NULL);
    renderer->resolvePipeline = resolvePipeline;
    renderer->uiPipeline = uiPipeline;
    if (chunkPipeline != VK_NULL_HANDLE)
    {
        if (renderer->chunkPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(renderer->device, renderer->chunkPipeline, NULL);
        renderer->chunkPipeline = chunkPipeline;
    }
    ReleaseShaderArray(previousShaders);
    return true;
}

bool RendererReloadShaderPackFrom_Vulkan(Renderer *renderer, LaiueContentCatalog *catalog,
                                  ShaderPackLoadStatus *outStatus)
{
    ShaderPackLoadStatus status = SHADER_PACK_LOAD_NOT_ATTEMPTED;
    ShaderPackLoadedSet *loadedSet = ShaderPackLoadActiveSet(catalog, &status);
    bool applied = false;
    if (loadedSet != NULL)
    {
        applied = RendererReloadShaderSet_Vulkan(renderer, ShaderPackLoadedSetGet(loadedSet));
        if (!applied) status = SHADER_PACK_LOAD_PIPELINE_ERROR;
        ShaderPackLoadedSetRelease(loadedSet);
    }
    else if (status == SHADER_PACK_LOAD_NO_ACTIVE_PACK)
    {
        applied = RendererReloadShaderSet_Vulkan(renderer, NULL);
        if (!applied) status = SHADER_PACK_LOAD_PIPELINE_ERROR;
    }
    if (outStatus != NULL) *outStatus = status;
    return applied;
}

bool RendererReloadShaderPack_Vulkan(Renderer *renderer, ShaderPackLoadStatus *outStatus)
{
    return RendererReloadShaderPackFrom_Vulkan(renderer, NULL, outStatus);
}

bool RendererReloadShaders_Vulkan(Renderer *renderer, const void *chunkVS, uint32_t chunkVSLength,
                           const void *chunkPS, uint32_t chunkPSLength, const void *panoramaVS,
                           uint32_t panoramaVSLength, const void *panoramaPS,
                           uint32_t panoramaPSLength, const void *uiVS, uint32_t uiVSLength,
                           const void *uiPS, uint32_t uiPSLength)
{
    LaiueShaderSet shaderSet;
    LaiueShaderSetInitialize(&shaderSet);
    const void *bytecode[LAIUE_SHADER_SLOT_COUNT] = { chunkVS,    chunkPS, panoramaVS,
                                                      panoramaPS, uiVS,    uiPS };
    const uint32_t lengths[LAIUE_SHADER_SLOT_COUNT] = { chunkVSLength,    chunkPSLength,
                                                        panoramaVSLength, panoramaPSLength,
                                                        uiVSLength,       uiPSLength };
    for (uint32_t slot = 0; slot < LAIUE_SHADER_SLOT_COUNT; ++slot)
    {
        if (bytecode[slot] == NULL || lengths[slot] == 0u) continue;
        if (!LaiueShaderSetSetOverride(&shaderSet, (LaiueShaderSlot)slot, bytecode[slot],
                                       lengths[slot]))
            return false;
    }
    return RendererReloadShaderSet_Vulkan(renderer, &shaderSet);
}

// === Слой интерфейса ===

_Static_assert(sizeof(RendererUiQuad) == UI_QUAD_BYTES,
    "RendererUiQuad shares its layout with shaders/ui.hlsl");

bool RendererUiSetFontAtlas_Vulkan(Renderer *renderer, const uint8_t *alphaPixels, uint32_t width,
                            uint32_t height)
{
    if (renderer == NULL || alphaPixels == NULL || width == 0u || height == 0u) return false;

    GpuImage atlas;
    if (!ImageCreate(renderer, width, height, 1u, VK_FORMAT_R8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false, &atlas))
        return false;
    if (!UploadImagePixels(renderer, &atlas, alphaPixels, 1u, 1u))
    {
        ImageDestroy(renderer, &atlas);
        return false;
    }

    WaitForGpu(renderer);
    ImageDestroy(renderer, &renderer->fontTexture);
    renderer->fontTexture = atlas;
    renderer->fontReady = true;
    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame)
    {
        WriteImageDescriptor(renderer, renderer->uiSets[frame], BINDING_UI_FONT,
                             renderer->fontTexture.view);
    }
    return true;
}

bool RendererUiLoadBackground_Vulkan(Renderer *renderer, const wchar_t *path, uint32_t *outWidth,
                              uint32_t *outHeight)
{
    // Единственная картинка оболочки декодируется системным кодеком: на
    // Windows это WIC. Переносимого декодера у движка нет, и добавлять
    // зависимость ради фонового изображения первого этапа не стоит —
    // вызов честно сообщает, что фон недоступен.
    (void)renderer;
    (void)path;
    if (outWidth != NULL) *outWidth = 0u;
    if (outHeight != NULL) *outHeight = 0u;
    return false;
}

void RendererUiQueue_Vulkan(Renderer *renderer, const RendererUiQuad *quads, uint32_t count)
{
    if (renderer == NULL || quads == NULL || count == 0u || !renderer->frameRecording) return;
    if (count > RENDERER_UI_MAX_QUADS - renderer->uiQuadCount)
        count = RENDERER_UI_MAX_QUADS - renderer->uiQuadCount;
    if (count == 0u) return;

    memcpy(renderer->uiQuadBuffers[renderer->frameIndex].mapped +
               (size_t)renderer->uiQuadCount * UI_QUAD_BYTES,
           quads, (size_t)count * UI_QUAD_BYTES);
    renderer->uiQuadCount += count;
}

// === Диагностическое чтение кадра ===

bool RendererCaptureFrame(Renderer *renderer, void *outPixels, uint32_t capacityBytes,
                          uint32_t *outWidth, uint32_t *outHeight)
{
    if (renderer == NULL || outPixels == NULL || !renderer->lastFrameValid) return false;

    GpuImage *source = &renderer->colorTargets[renderer->lastFrameIndex];
    uint32_t width = source->width;
    uint32_t height = source->height;
    if (outWidth != NULL) *outWidth = width;
    if (outHeight != NULL) *outHeight = height;

    VkDeviceSize required = (VkDeviceSize)width * height * 4u;
    if (capacityBytes < required) return false;

    vkDeviceWaitIdle(renderer->device);

    GpuBuffer readback;
    if (!BufferCreate(renderer, required, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &readback))
        return false;

    VkCommandBuffer commandBuffer = BeginImmediate(renderer);
    if (commandBuffer == VK_NULL_HANDLE)
    {
        BufferDestroy(renderer, &readback);
        return false;
    }
    ImageBarrier(commandBuffer, source, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u },
        .imageExtent = { width, height, 1u },
    };
    vkCmdCopyImageToBuffer(commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1u, &region);
    bool ok = EndImmediate(renderer, commandBuffer);
    if (ok) memcpy(outPixels, readback.mapped, (size_t)required);
    BufferDestroy(renderer, &readback);
    return ok;
}
