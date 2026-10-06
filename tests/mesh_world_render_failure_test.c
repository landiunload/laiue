/* Only the copied renderer test object maps Platform* to these wrappers.
 * The real world, device mock and production libraries use the ordinary
 * platform allocator. No fault-injection entry point enters the module ABI. */
#include "mesh_world_render/mesh_world_render_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <string.h>

typedef struct Allocation
{
    void *memory;
    size_t size;
} Allocation;

static Allocation allocations[128];
static uint32_t allocateCalls;
static uint32_t reallocateCalls;
static uint32_t failAllocateCall;
static uint32_t failReallocateCall;
static uint32_t liveAllocations;
static bool buffers[64];
static uint32_t liveBuffers;
static uint32_t bufferCreates, bufferUploads, bufferDestroys, nativeInstanceCalls;

static void Expect(bool value, const char *message)
{
    if (value)
        return;
    LaiueTestRuntimeWrite("Mesh world render allocation check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static Allocation *FindAllocation(void *memory)
{
    for (uint32_t i = 0u; i < 128u; ++i)
        if (allocations[i].memory == memory)
            return &allocations[i];
    return NULL;
}

static void *TrackAllocation(size_t size, bool zero)
{
    void *memory = PlatformAllocate(size, zero);
    Expect(memory != NULL, "fixture allocation");
    Allocation *record = FindAllocation(NULL);
    Expect(record != NULL, "allocation tracking capacity");
    record->memory = memory;
    record->size = size;
    ++liveAllocations;
    return memory;
}

void *RendererTestAllocate(size_t size, bool zero)
{
    ++allocateCalls;
    if (allocateCalls == failAllocateCall)
        return NULL;
    return TrackAllocation(size, zero);
}

void RendererTestFree(void *memory)
{
    if (memory == NULL)
        return;
    Allocation *record = FindAllocation(memory);
    Expect(record != NULL, "free preserves allocation ownership");
    record->memory = NULL;
    record->size = 0u;
    --liveAllocations;
    PlatformFree(memory);
}

void *RendererTestReallocate(void *memory, size_t size, bool zeroNew)
{
    ++reallocateCalls;
    if (reallocateCalls == failReallocateCall)
        return NULL;
    /* Always move a successful reallocation, making stale-owner bugs
     * deterministic instead of depending on the OS heap layout. */
    void *grown = TrackAllocation(size, zeroNew);
    if (memory != NULL)
    {
        Allocation *record = FindAllocation(memory);
        Expect(record != NULL, "reallocate preserves allocation ownership");
        memcpy(grown, memory, record->size < size ? record->size : size);
        RendererTestFree(memory);
    }
    return grown;
}

static uint32_t CreateBuffer(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsBufferDescV1 *desc,
                             LaiueGraphicsHandle *out)
{
    (void)device;
    Expect(desc->sizeBytes != 0u && (desc->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX ||
                                     desc->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX),
           "mock vertex buffer");
    *out = 0u;
    for (uint32_t i = 1u; i < 64u; ++i)
    {
        if (buffers[i])
            continue;
        buffers[i] = true;
        ++bufferCreates;
        ++liveBuffers;
        *out = i;
        return 1u;
    }
    return 0u;
}

static uint32_t UploadBuffer(LaiueGraphicsDeviceV2 *device,
                             const LaiueGraphicsBufferUploadV1 *upload)
{
    (void)device;
    Expect(upload->buffer < 64u && buffers[upload->buffer] && upload->data != NULL,
           "mock live upload");
    ++bufferUploads;
    return 1u;
}

static void DestroyBuffer(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle handle)
{
    (void)device;
    Expect(handle < 64u && buffers[handle], "mock buffer freed once");
    buffers[handle] = false;
    ++bufferDestroys;
    --liveBuffers;
}

static uint32_t OverflowingQuery(const LaiueMeshWorldV1 *world, const LaiueMeshPositionV1 *center,
                                 float radius, LaiueMeshCellV1 *out, uint32_t capacity,
                                 uint32_t *outCount)
{
    (void)world;
    (void)center;
    (void)radius;
    (void)out;
    (void)capacity;
    *outCount = UINT32_MAX;
    return 1u;
}

static uint32_t NativeCapabilities(const LaiueGraphicsDeviceV2 *device)
{
    (void)device;
    return LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
}

static uint32_t NativeSubmitInstances(LaiueGraphicsDeviceV2 *device,
                                      const LaiueGraphicsDrawItemV2 *item,
                                      const LaiueGraphicsInstanceV2 *instances, uint32_t count)
{
    (void)device;
    Expect(item != NULL && item->vertexBuffer != 0u && item->vertexBuffer < 64u &&
               buffers[item->vertexBuffer] && item->indexBuffer != 0u && item->indexBuffer < 64u &&
               buffers[item->indexBuffer] && instances != NULL && count == 2u &&
               instances[0].scale == 1.0f && instances[1].scale == 1.0f &&
               instances[0].originRelative[0] == 0.0f && instances[1].originRelative[0] == 16.0f,
           "allocation-free direct submission uses the complete live shared pair and prepared "
           "placement");
    ++nativeInstanceCalls;
    return 1u;
}

static uint32_t NativeSubmit(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsDrawItemV2 *items,
                             uint32_t count)
{
    (void)device;
    for (uint32_t i = 0u; i < count; ++i)
        Expect(items[i].vertexBuffer != 0u && items[i].vertexBuffer < 64u &&
                   buffers[items[i].vertexBuffer],
               "ordinary direct submission uses live geometry");
    return 1u;
}

static void CheckAllocationFreeSubmission(const LaiueMeshWorldRenderServiceV1 *renderApi,
                                          LaiueMeshWorldRendererV1 *renderer,
                                          const LaiueMeshPositionV1 *camera)
{
    const uint32_t previousAllocates = allocateCalls, previousGrows = reallocateCalls;
    const uint32_t previousCreates = bufferCreates, previousUploads = bufferUploads;
    const uint32_t previousInstances = nativeInstanceCalls, held = liveAllocations;
    failAllocateCall = allocateCalls + 1u;
    failReallocateCall = reallocateCalls + 1u;
    for (uint32_t frame = 0u; frame < 3u; ++frame)
        Expect(
            renderApi->submit(renderer, camera, NULL) != 0u,
            "prepared shared geometry submits while every next host allocation is armed to fail");
    Expect(
        allocateCalls == previousAllocates && reallocateCalls == previousGrows &&
            liveAllocations == held && bufferCreates == previousCreates &&
            bufferUploads == previousUploads && nativeInstanceCalls == previousInstances + 3u,
        "direct submit performs no host allocation, growth, GPU creation or upload after update");
    failAllocateCall = failReallocateCall = 0u;
}

static void TestSharedAllocationFailures(const LaiueMeshWorldServiceV1 *worldApi,
                                         const LaiueMeshWorldRenderServiceV1 *renderApi)
{
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "native failure world creates");
    LaiueGraphicsDeviceV2 device = {0};
    device.structSize = sizeof(device);
    device.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION;
    device.createBuffer = CreateBuffer;
    device.uploadBuffer = UploadBuffer;
    device.destroyHandle = DestroyBuffer;
    device.getCapabilities = NativeCapabilities;
    device.submitInstances = NativeSubmitInstances;
    device.submit = NativeSubmit;
    LaiueMeshWorldRendererConfigV1 renderConfig = {sizeof(renderConfig), worldApi, world, &device,
                                                   8u};
    LaiueMeshWorldRendererV1 *renderer = NULL;
    Expect(renderApi->create(&renderConfig, &renderer) != 0u, "native failure renderer creates");
    const LaiueMeshRenderVertexV1 vertices[4] = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xffffffffu},
                                                 {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0xffffffffu},
                                                 {{1, 1, 0}, {0, 0, 1}, {1, 1}, 0xffffffffu},
                                                 {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 geometry = {sizeof(geometry), vertices, 4u, indices, 6u, NULL, 0u};
    Expect(renderApi->registerModel(renderer, 7u, &geometry) != 0u,
           "native failure model registers");
    LaiueMeshInstanceDescV1 desc = {0};
    desc.structSize = sizeof(desc);
    desc.model = 7u;
    desc.flags = LAIUE_MESH_INSTANCE_VISIBLE;
    desc.transform.rotation[3] = 1.0f;
    desc.transform.scale[0] = desc.transform.scale[1] = desc.transform.scale[2] = 1.0f;
    LaiueMeshInstanceV1 instance = 0u;
    Expect(worldApi->add(world, &desc, &instance) != 0u, "native failure placement adds");
    desc.transform.position.cell.x = 1;
    Expect(worldApi->add(world, &desc, &instance) != 0u, "second pure repeated-model cell adds");
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && liveBuffers == 2u,
           "native initial pair caches");
    LaiueGraphicsDrawItemV2 old;
    uint32_t count = 0u;
    Expect(renderApi->draws(renderer, &camera, NULL, &old, 1u, &count) != 0u && count == 2u,
           "native initial ordinary fallback");
    CheckAllocationFreeSubmission(renderApi, renderer, &camera);
    const LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, 1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    Expect(renderApi->setLighting(renderer, &light) != 0u, "native generation invalidates");
    const uint32_t held = liveAllocations;
    for (uint32_t offset = 1u; offset <= 6u; ++offset)
    {
        failAllocateCall = allocateCalls + offset;
        Expect(renderApi->update(renderer, &camera, 40.0f, 1u, NULL) == 0u && liveBuffers == 2u &&
                   liveAllocations == held,
               "every shared-generation allocation failure preserves owners and old pair");
        LaiueGraphicsDrawItemV2 current;
        Expect(renderApi->draws(renderer, &camera, NULL, &current, 1u, &count) != 0u &&
                   count == 2u && current.vertexBuffer == old.vertexBuffer &&
                   current.indexBuffer == old.indexBuffer,
               "failed shared generation preserves complete ordinary items");
        failAllocateCall = 0u;
    }
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && liveBuffers == 2u,
           "shared allocation failure retries");
    Expect(renderApi->draws(renderer, &camera, NULL, &old, 1u, &count) != 0u && count == 2u,
           "current shared generation is visible before replacement fault injection");
    LaiueMeshRenderVertexV1 replacementVertices[4];
    memcpy(replacementVertices, vertices, sizeof(replacementVertices));
    replacementVertices[1].position[0] = replacementVertices[2].position[0] = 2.0f;
    const LaiueMeshRenderModelV1 replacement = {
        sizeof(replacement), replacementVertices, 4u, indices, 6u, NULL, 0u};
    const uint32_t heldReplacement = liveAllocations;
    for (uint32_t offset = 1u; offset <= 3u; ++offset)
    {
        failAllocateCall = allocateCalls + offset;
        Expect(renderApi->registerModel(renderer, 7u, &replacement) == 0u &&
                   liveAllocations == heldReplacement && liveBuffers == 2u,
               "failed model-copy vertex, index and part allocations preserve the registered "
               "generation");
        LaiueGraphicsDrawItemV2 current;
        Expect(renderApi->draws(renderer, &camera, NULL, &current, 1u, &count) != 0u &&
                   count == 2u && current.vertexBuffer == old.vertexBuffer &&
                   current.indexBuffer == old.indexBuffer,
               "failed registration leaves the old model's complete visible draw unchanged");
        failAllocateCall = 0u;
    }
    Expect(renderApi->registerModel(renderer, 7u, &replacement) != 0u &&
               liveAllocations == heldReplacement && liveBuffers == 2u,
           "successful model copy replaces CPU ownership while retaining the old visible GPU "
           "generation");
    for (uint32_t offset = 1u; offset <= 6u; ++offset)
    {
        failAllocateCall = allocateCalls + offset;
        Expect(
            renderApi->update(renderer, &camera, 40.0f, 1u, NULL) == 0u &&
                liveAllocations == heldReplacement && liveBuffers == 2u,
            "every replacement-generation preparation allocation failure releases candidates only");
        LaiueGraphicsDrawItemV2 current;
        Expect(
            renderApi->draws(renderer, &camera, NULL, &current, 1u, &count) != 0u && count == 2u &&
                current.vertexBuffer == old.vertexBuffer && current.indexBuffer == old.indexBuffer,
            "failed replacement preparation preserves the prior model generation for submission");
        failAllocateCall = 0u;
    }
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && liveBuffers == 2u,
           "replacement allocation failures eventually retry to one complete new pair");
    CheckAllocationFreeSubmission(renderApi, renderer, &camera);
    renderApi->destroy(renderer);
    worldApi->destroy(world);
    Expect(liveBuffers == 0u && liveAllocations == 0u && bufferCreates == bufferDestroys,
           "shared and failed replacement generations release every CPU and GPU owner once");
}

LAIUE_TEST_ENTRY(MeshWorldRenderFailureTestEntryPoint)
{
    const LaiueMeshWorldServiceV1 *worldApi = LaiueMeshWorldGetStaticServiceV1();
    const LaiueMeshWorldRenderServiceV1 *renderApi = LaiueMeshWorldRenderGetStaticServiceV1();
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "world creates");
    LaiueGraphicsDeviceV2 device = {0};
    device.structSize = sizeof(device);
    device.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION;
    device.createBuffer = CreateBuffer;
    device.uploadBuffer = UploadBuffer;
    device.destroyHandle = DestroyBuffer;
    LaiueMeshWorldRendererConfigV1 renderConfig = {sizeof(renderConfig), worldApi, world, &device,
                                                   16u};
    LaiueMeshWorldRendererV1 *renderer = NULL;
    const uint32_t fullSize = device.structSize;
    device.structSize = (uint32_t)offsetof(LaiueGraphicsDeviceV2, destroyHandle);
    Expect(renderApi->create(&renderConfig, &renderer) == 0u && renderer == NULL,
           "short device table rejected");
    device.structSize = fullSize;
    device.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION + 1u;
    Expect(renderApi->create(&renderConfig, &renderer) == 0u && renderer == NULL,
           "unknown device ABI rejected");
    device.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION;
    Expect(renderApi->create(&renderConfig, &renderer) != 0u, "renderer creates");
    const LaiueMeshRenderVertexV1 vertices[3] = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xffffffffu},
                                                 {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0xffffffffu},
                                                 {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[3] = {0u, 1u, 2u};
    const LaiueMeshRenderModelV1 geometry = {sizeof(geometry), vertices, 3u, indices, 3u, NULL, 0u};
    Expect(renderApi->registerModel(renderer, 7u, &geometry) != 0u, "model registers");
    LaiueMeshInstanceDescV1 desc = {0};
    desc.structSize = sizeof(desc);
    desc.model = 7u;
    desc.flags = LAIUE_MESH_INSTANCE_VISIBLE;
    desc.transform.rotation[3] = 1.0f;
    desc.transform.scale[0] = desc.transform.scale[1] = desc.transform.scale[2] = 1.0f;
    LaiueMeshInstanceV1 instance = 0u;
    Expect(worldApi->add(world, &desc, &instance) != 0u, "initial cell");
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    Expect(renderApi->update(renderer, &camera, 200.0f, 0u, NULL) != 0u && liveBuffers == 1u,
           "initial cache");
    for (int64_t i = 1; i < 9; ++i)
    {
        desc.transform.position.cell.x = i;
        Expect(worldApi->add(world, &desc, &instance) != 0u, "growth cell");
    }
    const uint32_t previousReallocates = reallocateCalls;
    failAllocateCall = allocateCalls + 1u;
    uint32_t pending = UINT32_MAX;
    Expect(renderApi->update(renderer, &camera, 200.0f, 0u, &pending) == 0u && pending == 0u,
           "cell-cache allocation fails");
    /* The query grows from eight to sixteen first. The candidate array
     * must not move after the following cache allocation failed. */
    Expect(reallocateCalls == previousReallocates + 1u,
           "failed cell-cache allocation must not relocate candidates");
    uint32_t count = 0u;
    Expect(renderApi->draws(renderer, &camera, NULL, NULL, 0u, &count) != 0u && count == 1u &&
               liveBuffers == 1u,
           "failed allocation preserves old cache");
    failAllocateCall = 0u;
    failReallocateCall = reallocateCalls + 1u;
    Expect(renderApi->update(renderer, &camera, 200.0f, 0u, NULL) == 0u && liveBuffers == 1u,
           "candidate allocation failure preserves cache");
    failReallocateCall = 0u;
    Expect(renderApi->update(renderer, &camera, 200.0f, 0u, NULL) != 0u && liveBuffers == 9u,
           "allocation failures retry without stale owners");
    renderApi->destroy(renderer);
    Expect(liveAllocations == 0u && liveBuffers == 0u, "renderer releases every allocation");

    LaiueMeshWorldServiceV1 overflowing = *worldApi;
    overflowing.queryCells = OverflowingQuery;
    renderConfig.worldService = &overflowing;
    Expect(renderApi->create(&renderConfig, &renderer) != 0u, "custom world renderer");
    const uint32_t previousAllocates = allocateCalls;
    const uint32_t previousGrows = reallocateCalls;
    Expect(renderApi->update(renderer, &camera, 1.0f, 0u, NULL) == 0u &&
               allocateCalls == previousAllocates && reallocateCalls == previousGrows,
           "UINT32_MAX query count rejected before allocation or count plus one");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
    Expect(liveAllocations == 0u, "custom renderer releases every allocation");
    TestSharedAllocationFailures(worldApi, renderApi);
    LaiueTestRuntimeWrite("mesh_world_render_failure_test passed\n");
    LAIUE_TEST_SUCCESS();
}
