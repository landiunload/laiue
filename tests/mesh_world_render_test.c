#include "mesh_world_render/mesh_world_render_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <string.h>

typedef struct MockBuffer
{
    void *bytes;
    uint64_t size;
} MockBuffer;

typedef struct MockDevice
{
    LaiueGraphicsDeviceV2 api;
    MockBuffer buffers[32];
    uint32_t creates, uploads, destroys, live;
    uint32_t failUpload;
    uint32_t failAtUpload;
} MockDevice;

static void Expect(bool value, const char *message)
{
    if (value)
        return;
    LaiueTestRuntimeWrite("Mesh world render check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(float a, float b)
{
    return a - b < 0.0001f && b - a < 0.0001f;
}

static uint32_t CreateBuffer(LaiueGraphicsDeviceV2 *api, const LaiueGraphicsBufferDescV1 *desc,
                             LaiueGraphicsHandle *out)
{
    MockDevice *mock = api->context;
    Expect(desc->structSize == sizeof(*desc) &&
               desc->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
           "vertex buffer contract");
    *out = 0u;
    for (uint32_t i = 1u; i < 32u; ++i)
    {
        if (mock->buffers[i].bytes != NULL)
            continue;
        mock->buffers[i].bytes = PlatformAllocate((size_t)desc->sizeBytes, false);
        if (mock->buffers[i].bytes == NULL)
            return 0u;
        mock->buffers[i].size = desc->sizeBytes;
        *out = i;
        ++mock->creates;
        ++mock->live;
        return 1u;
    }
    return 0u;
}

static uint32_t UploadBuffer(LaiueGraphicsDeviceV2 *api, const LaiueGraphicsBufferUploadV1 *upload)
{
    MockDevice *mock = api->context;
    const uint32_t slot = (uint32_t)upload->buffer;
    Expect(slot > 0u && slot < 32u && mock->buffers[slot].bytes != NULL &&
               upload->offsetBytes == 0u && upload->sizeBytes == mock->buffers[slot].size,
           "upload respects allocation");
    ++mock->uploads;
    if (mock->failUpload != 0u || mock->failAtUpload == mock->uploads)
        return 0u;
    memcpy(mock->buffers[slot].bytes, upload->data, (size_t)upload->sizeBytes);
    return 1u;
}

static void DestroyHandle(LaiueGraphicsDeviceV2 *api, LaiueGraphicsHandle handle)
{
    MockDevice *mock = api->context;
    const uint32_t slot = (uint32_t)handle;
    Expect(slot > 0u && slot < 32u && mock->buffers[slot].bytes != NULL,
           "each owned buffer destroyed exactly once");
    PlatformFree(mock->buffers[slot].bytes);
    mock->buffers[slot].bytes = NULL;
    ++mock->destroys;
    --mock->live;
}

static const LaiueMeshWorldServiceV1 *worldApi;
static const LaiueMeshWorldRenderServiceV1 *renderApi;
static MockDevice device;

static LaiueMeshInstanceV1 Add(LaiueMeshWorldV1 *world, int64_t x, float lx)
{
    LaiueMeshInstanceDescV1 desc = {0};
    desc.structSize = sizeof(desc);
    desc.model = 7u;
    desc.flags = LAIUE_MESH_INSTANCE_VISIBLE;
    desc.transform.position.cell.x = x;
    desc.transform.position.local[0] = lx;
    desc.transform.rotation[3] = 1.0f;
    desc.transform.scale[0] = desc.transform.scale[1] = desc.transform.scale[2] = 1.0f;
    LaiueMeshInstanceV1 out = 0u;
    Expect(worldApi->add(world, &desc, &out) != 0u, "world instance");
    return out;
}

static LaiueMeshWorldRendererV1 *CreateRenderer(LaiueMeshWorldV1 *world, uint32_t budget)
{
    LaiueMeshWorldRendererConfigV1 config = {sizeof(config), worldApi, world, &device.api, budget};
    LaiueMeshWorldRendererV1 *renderer = NULL;
    LaiueMeshWorldServiceV1 incomplete = *worldApi;
    incomplete.cellRevision = NULL;
    config.worldService = &incomplete;
    Expect(renderApi->create(&config, &renderer) == 0u && renderer == NULL,
           "incomplete world service rejected without dereference");
    config.worldService = worldApi;
    Expect(renderApi->create(&config, &renderer) != 0u, "renderer creates");
    return renderer;
}

static void RegisterTriangle(LaiueMeshWorldRendererV1 *renderer)
{
    LaiueMeshRenderVertexV1 vertices[3] = {
        {{0.0f, 0.0f, 0.25f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, 0xffffffffu},
        {{0.5f, 0.0f, 0.25f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, 0xffffffffu},
        {{0.0f, 0.5f, 0.25f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, 0xffffffffu}};
    const uint32_t indices[3] = {0u, 1u, 2u};
    LaiueMeshRenderModelV1 model = {sizeof(model), vertices, 3u, indices, 3u, NULL, 0u};
    Expect(renderApi->registerModel(renderer, 7u, &model) != 0u, "geometry registration");
    vertices[0].position[0] = 999.0f; /* Registration must own a copy. */
    model.vertexCount = 2u;
    Expect(renderApi->registerModel(renderer, 7u, &model) == 0u, "invalid index refused");
    LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0.0f, 0.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    Expect(renderApi->setLighting(renderer, &light) != 0u, "lighting registration");
    light.toSun[0] = 3.0e38f;
    Expect(renderApi->setLighting(renderer, &light) == 0u, "overflowing sunlight vector rejected");
}

static uint32_t Draws(LaiueMeshWorldRendererV1 *renderer, const LaiueMeshPositionV1 *camera,
                      const float *matrix, LaiueGraphicsDrawItemV2 *items, uint32_t capacity)
{
    uint32_t count = 0u;
    Expect(renderApi->draws(renderer, camera, matrix, items, capacity, &count) != 0u, "draw list");
    return count;
}

static void TestGeometryAndLifecycle(void)
{
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "world creates");
    const int64_t base = INT64_C(1) << 55;
    const LaiueMeshInstanceV1 closestInstance = Add(world, base, 0.0f);
    Add(world, base + 1, 0.0f);
    LaiueMeshPositionV1 camera = {{base, 0, 0}, {0.0f, 0.0f, 0.0f}};
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 8u);
    RegisterTriangle(renderer);
    uint32_t pending = 0u;
    Expect(renderApi->update(renderer, &camera, 40.0f, 1u, &pending) != 0u && pending == 1u,
           "progressive rebuild budget");
    LaiueGraphicsDrawItemV2 items[4];
    Expect(Draws(renderer, &camera, NULL, items, 4u) == 1u &&
               Near(items[0].originRelative[0], 0.0f),
           "nearest cell builds first at huge coordinates");
    const LaiueGraphicsVertexV2 *v = device.buffers[items[0].vertexBuffer].bytes;
    Expect(Near(v[0].position[0], 0.0f) && v[0].colorRGBA == 0xffbfbfbfu && Near(v[1].uv[0], 1.0f),
           "copied geometry, baked lighting and unscaled UV");
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, &pending) != 0u && pending == 0u,
           "remaining cells build");
    const uint32_t uploads = device.uploads;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.uploads == uploads,
           "static cells do not upload again");
    Expect(Draws(renderer, &camera, NULL, items, 1u) == 2u,
           "truncated draw list reports full count");
    Expect(renderApi->setMaterial(renderer, 0u, 123u, 456u) != 0u, "material binds");
    Expect(Draws(renderer, &camera, NULL, items, 4u) == 2u && items[0].texture == 123u &&
               items[0].sampler == 456u && device.uploads == uploads,
           "material updates without rebuild");
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Expect(Draws(renderer, &camera, identity, items, 4u) == 1u, "frustum culls distant geometry");
    LaiueMeshRenderStatsV1 stats = {0};
    stats.structSize = sizeof(stats);
    Expect(renderApi->stats(renderer, &stats) != 0u && stats.culled == 1u && stats.vertices == 6u,
           "culling and vertex statistics");
    LaiueMeshInstanceInfoV1 info = {0};
    Expect(worldApi->get(world, closestInstance, &info) != 0u, "read instance transform");
    info.transform.position.local[0] = 0.25f;
    Expect(worldApi->setTransform(world, closestInstance, &info.transform) != 0u,
           "world revision changes");
    Draws(renderer, &camera, NULL, items, 4u);
    const LaiueGraphicsHandle old = items[0].vertexBuffer;
    const uint32_t live = device.live;
    device.failUpload = 1u;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, &pending) == 0u && pending != 0u &&
               device.live == live,
           "failed upload cleans temporary buffers");
    Expect(Draws(renderer, &camera, NULL, items, 4u) == 2u && items[0].vertexBuffer == old,
           "failed rebuild preserves old geometry");
    device.failUpload = 0u;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u, "failed rebuild retries");
    Draws(renderer, &camera, NULL, items, 4u);
    v = device.buffers[items[0].vertexBuffer].bytes;
    Expect(Near(v[0].position[0], 0.25f), "transform revision rebakes positions");
    const LaiueMeshCellV1 shift = {base, 0, 0};
    Expect(worldApi->rebase(world, &shift) != 0u, "world rebases");
    camera.cell.x = 0;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
               Draws(renderer, &camera, NULL, items, 4u) == 2u &&
               Near(items[0].originRelative[0], 0.0f),
           "renderer survives world rebase");
    camera.cell.x = 100;
    Expect(renderApi->update(renderer, &camera, 1.0f, 0u, NULL) != 0u && device.live == 0u,
           "departed cells release GPU buffers");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
}

static void TestNearestBudget(void)
{
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "budget world creates");
    Add(world, 1, 0.0f);
    LaiueMeshPositionV1 camera = {{1, 0, 0}, {0.0f, 0.0f, 0.0f}};
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 1u);
    RegisterTriangle(renderer);
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u, "far cell caches");
    Add(world, 0, 0.0f);
    camera.cell.x = 0;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u,
           "camera approaches new cell");
    LaiueGraphicsDrawItemV2 item;
    Expect(Draws(renderer, &camera, NULL, &item, 1u) == 1u && Near(item.originRelative[0], 0.0f),
           "new nearest cell replaces cached farther cell under buffer budget");
    LaiueMeshRenderStatsV1 stats = {0};
    stats.structSize = sizeof(stats);
    Expect(renderApi->stats(renderer, &stats) != 0u && stats.cells == 2u && stats.buffers == 1u &&
               stats.overBudget == 1u && stats.pending == 0u,
           "eviction counts as over budget in the update that evicts");
    uint32_t count = UINT32_MAX;
    LaiueMeshPositionV1 invalid = camera;
    invalid.cell.x = INT64_MIN;
    Expect(renderApi->draws(renderer, &invalid, NULL, &item, 1u, &count) == 0u && count == 0u,
           "draw origin subtraction cannot overflow");
    invalid.cell.x = INT64_MAX;
    count = UINT32_MAX;
    Expect(renderApi->draws(renderer, &invalid, NULL, &item, 1u, &count) == 0u && count == 0u,
           "draw origin above the world range rejected");
    const uint32_t nanBits = UINT32_C(0x7fc00000);
    invalid = camera;
    memcpy(&invalid.local[1], &nanBits, sizeof(nanBits));
    count = UINT32_MAX;
    Expect(renderApi->draws(renderer, &invalid, NULL, &item, 1u, &count) == 0u && count == 0u,
           "nonfinite origin rejected");
    float matrix[16] = {0};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    memcpy(&matrix[7], &nanBits, sizeof(nanBits));
    count = UINT32_MAX;
    Expect(renderApi->draws(renderer, &camera, matrix, &item, 1u, &count) == 0u && count == 0u,
           "nonfinite view projection rejected");
    invalid = camera;
    invalid.cell.x = LAIUE_MESH_WORLD_MAX_CELL;
    Expect(Draws(renderer, &invalid, NULL, &item, 1u) == 1u,
           "valid extreme origin stays representable");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
    Expect(device.live == 0u && device.creates == device.destroys, "all owned resources released");
}

static void TestMaterialsAndLighting(void)
{
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "material world creates");
    Add(world, 0, 0.0f);
    Add(world, 0, 1.0f);
    LaiueMeshPositionV1 camera = {{0, 0, 0}, {0.0f, 0.0f, 0.0f}};
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 2u);
    const LaiueMeshRenderVertexV1 vertices[6] = {
        {{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xffffffffu},  {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0xffffffffu},
        {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xffffffffu},  {{0, 0, 1}, {0, 0, -1}, {0, 0}, 0xffffffffu},
        {{1, 0, 1}, {0, 0, -1}, {1, 0}, 0xffffffffu}, {{0, 1, 1}, {0, 0, -1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
    const LaiueMeshRenderPartV1 parts[2] = {{0u, 3u, 2u}, {3u, 3u, 3u}};
    const LaiueMeshRenderModelV1 model = {sizeof(model), vertices, 6u, indices, 6u, parts, 2u};
    Expect(renderApi->registerModel(renderer, 7u, &model) != 0u, "two material model");
    LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, 1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    Expect(renderApi->setLighting(renderer, &light) != 0u &&
               renderApi->setMaterial(renderer, 2u, 2u, 0u) != 0u &&
               renderApi->setMaterial(renderer, 3u, 3u, 0u) != 0u &&
               renderApi->update(renderer, &camera, 10.0f, 0u, NULL) != 0u,
           "material batching builds");
    LaiueGraphicsDrawItemV2 items[2];
    Expect(Draws(renderer, &camera, NULL, items, 2u) == 2u && items[0].indexCount == 6u &&
               items[1].indexCount == 6u && device.live == 2u,
           "instances in one cell share one buffer per material");
    const LaiueGraphicsVertexV2 *up = device.buffers[items[0].vertexBuffer].bytes;
    const LaiueGraphicsVertexV2 *down = device.buffers[items[1].vertexBuffer].bytes;
    Expect(up[0].colorRGBA == 0xffbfbfbfu && down[0].colorRGBA == 0xff404040u,
           "opposing normals bake sun and ambient separately");
    light.toSun[2] = -1.0f;
    Expect(renderApi->setLighting(renderer, &light) != 0u, "sun changes");
    const uint32_t uploads = device.uploads;
    device.failAtUpload = uploads + 2u;
    Expect(renderApi->update(renderer, &camera, 10.0f, 0u, NULL) == 0u && device.live == 2u,
           "failure of second material releases all temporary buffers");
    device.failAtUpload = 0u;
    Expect(renderApi->update(renderer, &camera, 10.0f, 0u, NULL) != 0u, "lighting retry");
    Draws(renderer, &camera, NULL, items, 2u);
    up = device.buffers[items[0].vertexBuffer].bytes;
    down = device.buffers[items[1].vertexBuffer].bytes;
    Expect(up[0].colorRGBA == 0xff404040u && down[0].colorRGBA == 0xffbfbfbfu,
           "new lighting rebakes every material");
    renderApi->destroy(renderer);
    renderer = CreateRenderer(world, 1u);
    Expect(renderApi->registerModel(renderer, 7u, &model) != 0u &&
               renderApi->update(renderer, &camera, 10.0f, 0u, NULL) != 0u && device.live == 0u,
           "cell needing more buffers than budget stays undrawn");
    LaiueMeshRenderStatsV1 stats = {0};
    stats.structSize = sizeof(stats);
    Expect(renderApi->stats(renderer, &stats) != 0u && stats.overBudget == 1u,
           "over-budget cell is reported");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
}

LAIUE_TEST_ENTRY(MeshWorldRenderTestEntryPoint)
{
    worldApi = LaiueMeshWorldGetStaticServiceV1();
    renderApi = LaiueMeshWorldRenderGetStaticServiceV1();
    device.api.structSize = sizeof(device.api);
    device.api.abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION;
    device.api.context = &device;
    device.api.createBuffer = CreateBuffer;
    device.api.uploadBuffer = UploadBuffer;
    device.api.destroyHandle = DestroyHandle;
    TestGeometryAndLifecycle();
    TestNearestBudget();
    TestMaterialsAndLighting();
    LaiueTestRuntimeWrite("mesh_world_render_test passed\n");
    LAIUE_TEST_SUCCESS();
}
