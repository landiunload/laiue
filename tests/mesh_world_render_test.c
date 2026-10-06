#include "mesh_world_render/mesh_world_render_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <string.h>

typedef struct MockBuffer
{
    void *bytes;
    uint64_t size;
    uint32_t usage;
} MockBuffer;

typedef struct MockDevice
{
    LaiueGraphicsDeviceV2 api;
    MockBuffer buffers[32];
    uint32_t creates, uploads, destroys, live;
    uint32_t failUpload;
    uint32_t failAtUpload;
    uint32_t failAtCreate;
    uint32_t capabilities;
    uint32_t capabilityQueries;
    uint32_t ordinaryCalls;
    uint32_t instanceCalls;
    uint32_t submittedInstances;
    uint32_t ordinaryAttempts;
    uint32_t instanceAttempts;
    bool failOrdinarySubmit;
    bool failInstanceSubmit;
    LaiueGraphicsInstanceV2 placements[32];
    LaiueGraphicsDrawItemV2 instanceItem;
    LaiueGraphicsDrawItemV2 ordinaryItem;
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
               (desc->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX ||
                desc->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX),
           "vertex buffer contract");
    *out = 0u;
    if (mock->failAtCreate == mock->creates + 1u)
        return 0u;
    for (uint32_t i = 1u; i < 32u; ++i)
    {
        if (mock->buffers[i].bytes != NULL)
            continue;
        mock->buffers[i].bytes = PlatformAllocate((size_t)desc->sizeBytes, false);
        if (mock->buffers[i].bytes == NULL)
            return 0u;
        mock->buffers[i].size = desc->sizeBytes;
        mock->buffers[i].usage = desc->usageFlags;
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
static const LaiueMeshWorldServiceV1 *realWorldApi;
static LaiueMeshWorldServiceV1 countedWorldApi;
static const LaiueMeshWorldRenderServiceV1 *renderApi;
static MockDevice device;
static uint32_t cellInstanceQueries;
static uint32_t cellRevisionQueries;
static bool failCellInstanceQuery;

static uint32_t CellInstances(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell,
                              LaiueMeshInstanceInfoV1 *instances, uint32_t capacity,
                              uint32_t *outCount)
{
    ++cellInstanceQueries;
    if (failCellInstanceQuery)
    {
        *outCount = 0u;
        return 0u;
    }
    return realWorldApi->cellInstances(world, cell, instances, capacity, outCount);
}

static uint64_t CellRevision(const LaiueMeshWorldV1 *world, const LaiueMeshCellV1 *cell)
{
    ++cellRevisionQueries;
    return realWorldApi->cellRevision(world, cell);
}

static uint32_t Capabilities(const LaiueGraphicsDeviceV2 *api)
{
    MockDevice *mock = api->context;
    Expect(api->structSize >= LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE,
           "a physically truncated graphics table is not queried for capabilities");
    ++mock->capabilityQueries;
    return mock->capabilities;
}

static uint32_t Submit(LaiueGraphicsDeviceV2 *api, const LaiueGraphicsDrawItemV2 *items,
                       uint32_t count)
{
    MockDevice *mock = api->context;
    for (uint32_t i = 0u; i < count; ++i)
        Expect(items[i].vertexBuffer != 0u && mock->buffers[items[i].vertexBuffer].bytes != NULL,
               "ordinary submit uses live geometry");
    ++mock->ordinaryAttempts;
    if (count != 0u)
        mock->ordinaryItem = items[count - 1u];
    if (mock->failOrdinarySubmit)
        return 0u;
    mock->ordinaryCalls += count;
    return 1u;
}

static uint32_t SubmitInstances(LaiueGraphicsDeviceV2 *api, const LaiueGraphicsDrawItemV2 *item,
                                const LaiueGraphicsInstanceV2 *instances, uint32_t count)
{
    MockDevice *mock = api->context;
    Expect(count != 0u && count <= 32u && item->indexBuffer != 0u &&
               mock->buffers[item->vertexBuffer].usage == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX &&
               mock->buffers[item->indexBuffer].usage == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
           "native instances use separate live vertex and index buffers");
    memcpy(mock->placements, instances, count * sizeof(*instances));
    mock->instanceItem = *item;
    ++mock->instanceAttempts;
    if (mock->failInstanceSubmit)
        return 0u;
    ++mock->instanceCalls;
    mock->submittedInstances += count;
    return 1u;
}

static LaiueMeshInstanceV1 AddModel(LaiueMeshWorldV1 *world, int64_t x, float lx, uint32_t model)
{
    LaiueMeshInstanceDescV1 desc = {0};
    desc.structSize = sizeof(desc);
    desc.model = model;
    desc.flags = LAIUE_MESH_INSTANCE_VISIBLE;
    desc.transform.position.cell.x = x;
    desc.transform.position.local[0] = lx;
    desc.transform.rotation[3] = 1.0f;
    desc.transform.scale[0] = desc.transform.scale[1] = desc.transform.scale[2] = 1.0f;
    LaiueMeshInstanceV1 out = 0u;
    Expect(worldApi->add(world, &desc, &out) != 0u, "world instance");
    return out;
}

static LaiueMeshInstanceV1 Add(LaiueMeshWorldV1 *world, int64_t x, float lx)
{
    return AddModel(world, x, lx, 7u);
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

static void CheckProviderFailureRetry(LaiueMeshWorldRendererV1 *renderer,
                                      const LaiueMeshPositionV1 *camera, const float *matrix,
                                      bool native, uint32_t expectedPlacements)
{
    LaiueGraphicsDrawItemV2 before[8], after[8];
    const uint32_t count = Draws(renderer, camera, matrix, before, 8u);
    Expect(count == (native ? expectedPlacements : 1u) && count <= 8u,
           "provider-failure fixture selects one native batch or one ordinary draw");
    const uint32_t creates = device.creates, uploads = device.uploads;
    const uint32_t destroys = device.destroys, live = device.live;
    const uint32_t ordinaryAttempts = device.ordinaryAttempts;
    const uint32_t instanceAttempts = device.instanceAttempts;
    const uint32_t ordinaryCalls = device.ordinaryCalls, instanceCalls = device.instanceCalls;
    const uint32_t placements = device.submittedInstances;
    device.failInstanceSubmit = native;
    device.failOrdinarySubmit = !native;
    Expect(renderApi->submit(renderer, camera, matrix) == 0u &&
               device.instanceAttempts == instanceAttempts + (native ? 1u : 0u) &&
               device.ordinaryAttempts == ordinaryAttempts + (native ? 0u : 1u),
           "a rejected native batch or ordinary draw propagates the provider's failure");
    Expect(device.creates == creates && device.uploads == uploads && device.destroys == destroys &&
               device.live == live && Draws(renderer, camera, matrix, after, 8u) == count &&
               memcmp(before, after, count * sizeof(before[0])) == 0,
           "callback failure preserves every cached draw handle without creating or uploading "
           "geometry");
    for (uint32_t i = 0u; i < count; ++i)
        Expect(
            device.buffers[after[i].vertexBuffer].bytes != NULL &&
                (after[i].indexBuffer == 0u || device.buffers[after[i].indexBuffer].bytes != NULL),
            "rejected submission leaves all referenced vertex and index resources live");
    device.failInstanceSubmit = device.failOrdinarySubmit = false;
    Expect(renderApi->submit(renderer, camera, matrix) != 0u &&
               device.instanceAttempts == instanceAttempts + (native ? 2u : 0u) &&
               device.ordinaryAttempts == ordinaryAttempts + (native ? 0u : 2u) &&
               device.instanceCalls == instanceCalls + (native ? 1u : 0u) &&
               device.ordinaryCalls == ordinaryCalls + (native ? 0u : 1u) &&
               device.submittedInstances == placements + (native ? expectedPlacements : 0u) &&
               device.creates == creates && device.uploads == uploads &&
               device.destroys == destroys && device.live == live,
           "clearing the provider failure retries the selected cached draw without rebuilding it");
    if (native)
    {
        Expect(device.instanceItem.vertexBuffer == before[0].vertexBuffer &&
                   device.instanceItem.indexBuffer == before[0].indexBuffer,
               "native retry uses the original shared pair");
        for (uint32_t i = 0u; i < expectedPlacements; ++i)
            Expect(memcmp(device.placements[i].originRelative, before[i].originRelative,
                          sizeof(before[i].originRelative)) == 0 &&
                       device.placements[i].scale == before[i].scale,
                   "native retry keeps every prepared placement transform");
    }
    else
        Expect(memcmp(&device.ordinaryItem, &before[0], sizeof(before[0])) == 0,
               "ordinary retry preserves the complete singleton or baked draw item");
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

static void RegisterQuad(LaiueMeshWorldRendererV1 *renderer, uint32_t model)
{
    const LaiueMeshRenderVertexV1 vertices[4] = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xffffffffu},
                                                 {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0xffffffffu},
                                                 {{1, 1, 0}, {0, 0, 1}, {1, 1}, 0xffffffffu},
                                                 {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 geometry = {sizeof(geometry), vertices, 4u, indices, 6u, NULL, 0u};
    Expect(renderApi->registerModel(renderer, model, &geometry) != 0u, "shared quad registers");
}

typedef struct LegacyRenderStats
{
    uint32_t structSize, cells, buffers;
    uint64_t vertices;
    uint32_t rebuilt, pending, overBudget, drawn, culled;
} LegacyRenderStats;

_Static_assert(sizeof(LegacyRenderStats) == 48u &&
                   LAIUE_MESH_RENDER_STATS_V1_LEGACY_SIZE == sizeof(LegacyRenderStats),
               "the physically old renderer stats prefix remains exactly 48 bytes");

static void CheckLegacyStats(LaiueMeshWorldRendererV1 *renderer)
{
    struct
    {
        uint64_t before;
        LegacyRenderStats stats;
        uint64_t after[2];
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.stats.structSize = sizeof(guarded.stats);
    const uint64_t canary = UINT64_C(0xa5a5a5a5a5a5a5a5);
    LaiueMeshRenderStatsV1 full = {.structSize = sizeof(full)};
    Expect(
        renderApi->stats(renderer, &full) != 0u &&
            renderApi->stats(renderer, (LaiueMeshRenderStatsV1 *)&guarded.stats) != 0u &&
            guarded.stats.structSize == sizeof(guarded.stats) &&
            guarded.stats.cells == full.cells && guarded.stats.buffers == full.buffers &&
            guarded.stats.vertices == full.vertices && guarded.stats.drawn == full.drawn &&
            guarded.stats.culled == full.culled && guarded.before == canary &&
            guarded.after[0] == canary && guarded.after[1] == canary,
        "stats fills the actual old 48-byte object without writing optional index/instance tails");
}

static void TestNativeSharedGeometry(void)
{
    device.capabilities = LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
    device.api.getCapabilities = Capabilities;
    device.api.submitInstances = SubmitInstances;
    device.api.submit = Submit;
    device.ordinaryCalls = device.instanceCalls = device.submittedInstances = 0u;
    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "native world creates");
    const int64_t base = INT64_C(7000000000);
    const LaiueMeshInstanceV1 first = Add(world, base, 0.25f);
    Add(world, base + 1, 0.5f);
    Add(world, base + 2, 0.75f);
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 4u);
    RegisterQuad(renderer, 7u);
    const LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, 1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    Expect(renderApi->setLighting(renderer, &light) != 0u, "native light configures");
    LaiueMeshPositionV1 camera = {{base, 0, 0}, {0.0f, 0.0f, 0.0f}};
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.live == 2u,
           "three pure repeated-model cells share one VB and one IB without baked copies");
    LaiueGraphicsDrawItemV2 items[4];
    Expect(Draws(renderer, &camera, NULL, items, 1u) == 3u,
           "native ordinary fallback reports complete required count");
    Draws(renderer, &camera, NULL, items, 4u);
    Expect(items[0].vertexBuffer == items[1].vertexBuffer &&
               items[1].vertexBuffer == items[2].vertexBuffer &&
               items[0].indexBuffer == items[1].indexBuffer && items[0].indexCount == 6u &&
               items[1].indexBuffer == items[2].indexBuffer &&
               Near(items[0].originRelative[0], 0.25f) && Near(items[1].originRelative[0], 16.5f) &&
               Near(items[2].originRelative[0], 32.75f),
           "ordinary shared items preserve placement and independent topology");
    const LaiueGraphicsVertexV2 *vertices = device.buffers[items[0].vertexBuffer].bytes;
    Expect(device.buffers[items[0].vertexBuffer].size == 96u &&
               device.buffers[items[0].indexBuffer].size == 24u &&
               vertices[0].colorRGBA == 0xffbfbfbfu && Near(vertices[2].uv[1], 1.0f) &&
               Near(vertices[0].position[0], 0.0f),
           "shared model-local geometry preserves lighting UVs and exact bytes");
    Expect(renderApi->submit(renderer, &camera, NULL) != 0u && device.instanceCalls == 1u &&
               device.submittedInstances == 3u && device.ordinaryCalls == 0u &&
               Near(device.placements[0].originRelative[0], 0.25f) &&
               Near(device.placements[1].originRelative[0], 16.5f) &&
               Near(device.placements[2].originRelative[0], 32.75f),
           "direct submission groups visible placements into one material call");
    const uint32_t warmCreates = device.creates, warmUploads = device.uploads;
    const uint32_t warmQueries = cellInstanceQueries;
    const uint32_t warmRevisions = cellRevisionQueries;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
               device.creates == warmCreates && device.uploads == warmUploads &&
               cellInstanceQueries == warmQueries && cellRevisionQueries == warmRevisions + 3u,
           "repeated pure cells reuse one native pair without warm GPU allocations, uploads or "
           "instance listings and read each cell revision exactly once");
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Expect(renderApi->submit(renderer, &camera, identity) != 0u &&
               device.submittedInstances == 3u && device.instanceCalls == 1u &&
               device.ordinaryCalls == 1u &&
               device.ordinaryItem.vertexBuffer == items[0].vertexBuffer &&
               device.ordinaryItem.indexBuffer == items[0].indexBuffer &&
               Near(device.ordinaryItem.originRelative[0], 0.25f) &&
               Near(device.ordinaryItem.scale, 1.0f),
           "one visible placement preserves culling and transform without an instance-ring call");
    LaiueMeshRenderStatsV1 stats = {.structSize = sizeof(stats)};
    Expect(renderApi->stats(renderer, &stats) != 0u && stats.buffers == 1u &&
               stats.indexBuffers == 1u && stats.vertices == 4u && stats.instances == 3u,
           "native stats preserve vertex-buffer meaning");
    Expect(stats.instanceDrawCalls == 0u,
           "stats excludes the ordinary singleton call from native instance calls");
    CheckLegacyStats(renderer);
    CheckProviderFailureRetry(renderer, &camera, NULL, true, 3u);
    CheckProviderFailureRetry(renderer, &camera, identity, false, 0u);
    const LaiueGraphicsHandle oldVertex = items[0].vertexBuffer;
    const LaiueGraphicsHandle oldIndex = items[0].indexBuffer;
    LaiueMeshRenderLightingV1 changed = light;
    changed.toSun[2] = -1.0f;
    Expect(renderApi->setLighting(renderer, &changed) != 0u, "shared lighting invalidates");
    device.failAtUpload = device.uploads + 2u;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) == 0u && device.live == 2u,
           "index upload failure releases new pair and preserves old shared geometry");
    device.failAtUpload = 0u;
    Draws(renderer, &camera, NULL, items, 4u);
    Expect(items[0].vertexBuffer == oldVertex && items[0].indexBuffer == oldIndex,
           "failed shared generation remains visible");
    Expect(renderApi->update(renderer, &camera, 40.0f, 1u, NULL) != 0u && device.live == 4u,
           "progressive lighting replacement retains previous cells and geometry");
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.live == 2u,
           "last previous cell releases its old pair exactly once");
    LaiueMeshInstanceInfoV1 info = {0};
    Expect(worldApi->get(world, first, &info) != 0u, "read shared transform");
    info.transform.rotation[3] = -1.0f;
    info.transform.scale[0] = info.transform.scale[1] = info.transform.scale[2] = 2.0f;
    Expect(worldApi->setTransform(world, first, &info.transform) != 0u &&
               renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.live == 2u,
           "sign identity and positive uniform scale retain shared geometry");
    Draws(renderer, &camera, NULL, items, 4u);
    Expect(Near(items[0].scale, 2.0f) && Near(items[0].originRelative[0], 0.25f),
           "placement translation stays unscaled");
    info.transform.scale[1] = 3.0f;
    Expect(worldApi->setTransform(world, first, &info.transform) != 0u &&
               renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.live == 3u,
           "nonuniform placement takes baked remainder without duplicating eligible cells");
    const uint32_t ordinaryBeforeMixed = device.ordinaryCalls;
    Expect(renderApi->submit(renderer, &camera, NULL) != 0u &&
               device.ordinaryCalls == ordinaryBeforeMixed + 1u,
           "mixed scene submits baked remainder normally");
    const LaiueMeshCellV1 shift = {base, 0, 0};
    Expect(worldApi->rebase(world, &shift) != 0u, "shared world rebases");
    camera.cell.x = 0;
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
               Draws(renderer, &camera, NULL, items, 4u) == 3u,
           "shared and baked caches survive rebase");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
    Expect(device.live == 0u && device.creates == device.destroys,
           "native pairs and baked remainder all released once");

    for (uint32_t mode = 0u; mode < 5u; ++mode)
    {
        Expect(worldApi->create(&config, &world) != 0u, "legacy capability world creates");
        Add(world, 0, 0.0f);
        Add(world, 1, 0.0f);
        device.api.structSize = mode == 0u
                                    ? (uint32_t)offsetof(LaiueGraphicsDeviceV2, getCapabilities)
                                    : (mode == 3u ? LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE - 1u
                                                  : sizeof(device.api));
        device.capabilities =
            mode == 1u ? LAIUE_GRAPHICS_CAP_NATIVE_INDICES
                       : LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
        device.api.submitInstances = mode == 2u ? NULL : SubmitInstances;
        device.api.getCapabilities = mode == 4u ? NULL : Capabilities;
        const uint32_t previousQueries = device.capabilityQueries;
        const uint32_t previousInstances = device.instanceCalls;
        const uint32_t prefixBytes = device.api.structSize;
        LaiueGraphicsDeviceV2 *prefix = PlatformAllocate(prefixBytes, false);
        Expect(prefix != NULL, "actual legacy graphics table prefix allocates");
        memcpy(prefix, &device.api, prefixBytes);
        const LaiueMeshWorldRendererConfigV1 legacyConfig = {sizeof(legacyConfig), worldApi, world,
                                                             prefix, 2u};
        renderer = NULL;
        Expect(renderApi->create(&legacyConfig, &renderer) != 0u, "legacy prefix renderer creates");
        RegisterQuad(renderer, 7u);
        camera.cell.x = 0;
        Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 4u) == 2u && items[0].indexBuffer == 0u &&
                   items[1].indexBuffer == 0u && items[0].indexCount == 6u &&
                   items[1].indexCount == 6u && device.live == 2u,
               "short table absent capability or missing callback uses complete baking");
        Expect(device.capabilityQueries == previousQueries + (mode == 1u ? 1u : 0u) &&
                   renderApi->submit(renderer, &camera, NULL) != 0u &&
                   device.instanceCalls == previousInstances,
               "missing or truncated capability/instance tails use ordinary baked submission");
        CheckLegacyStats(renderer);
        renderApi->destroy(renderer);
        PlatformFree(prefix);
        worldApi->destroy(world);
    }
    device.api.structSize = sizeof(device.api);
    device.api.getCapabilities = NULL;
    device.api.submitInstances = NULL;
}

static void EnableNativeDevice(void)
{
    device.api.structSize = sizeof(device.api);
    device.capabilities = LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
    device.api.getCapabilities = Capabilities;
    device.api.submitInstances = SubmitInstances;
    device.api.submit = Submit;
}

static void TestFallbackNormalLighting(void)
{
    static LaiueGraphicsVertexV2 reference[4][6];
    const LaiueMeshRenderVertexV1 vertices[4] = {{{0, 0, 0}, {1, 0, 1}, {0, 0}, 0xffffffffu},
                                                 {{1, 0, 0}, {1, 0, 1}, {1, 0}, 0xffffffffu},
                                                 {{1, 1, 0}, {1, 0, 1}, {1, 1}, 0xffffffffu},
                                                 {{0, 1, 0}, {1, 0, 1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 geometry = {sizeof(geometry), vertices, 4u, indices, 6u, NULL, 0u};
    const LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, 1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "normal-transform world creates");
    const LaiueMeshInstanceV1 rotated = Add(world, 0, 0.0f);
    const LaiueMeshInstanceV1 stretched = Add(world, 1, 0.0f);
    Add(world, 2, 0.0f);
    Add(world, 3, 0.0f);
    LaiueMeshInstanceInfoV1 info = {0};
    Expect(worldApi->get(world, rotated, &info) != 0u, "rotated placement reads");
    info.transform.rotation[1] = info.transform.rotation[3] = 0.7071067811865475f;
    Expect(worldApi->setTransform(world, rotated, &info.transform) != 0u,
           "rotated placement turns its model normal away from the sun");
    Expect(worldApi->get(world, stretched, &info) != 0u, "nonuniform placement reads");
    info.transform.scale[2] = 2.0f;
    Expect(worldApi->setTransform(world, stretched, &info.transform) != 0u,
           "nonuniform placement requires inverse-transpose normal lighting");
    EnableNativeDevice();
    for (uint32_t mode = 0u; mode < 2u; ++mode)
    {
        device.capabilities =
            mode == 0u ? 0u
                       : LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
        LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 8u);
        Expect(renderApi->registerModel(renderer, 7u, &geometry) != 0u &&
                   renderApi->setLighting(renderer, &light) != 0u &&
                   renderApi->update(renderer, &camera, 70.0f, 0u, NULL) != 0u,
               "normal-transform model builds on legacy and native providers");
        LaiueGraphicsDrawItemV2 items[4];
        Expect(Draws(renderer, &camera, NULL, items, 4u) == 4u,
               "rotated, nonuniform and two pure identity placements remain visible");
        const uint32_t expectedColors[4] = {0xff404040u, 0xff797979u, 0xff9a9a9au, 0xff9a9a9au};
        for (uint32_t cell = 0u; cell < 4u; ++cell)
        {
            const LaiueGraphicsDrawItemV2 *item = &items[cell];
            const LaiueGraphicsVertexV2 *actual = device.buffers[item->vertexBuffer].bytes;
            Expect(Near(item->originRelative[0], (float)cell * 16.0f) &&
                       actual[0].colorRGBA == expectedColors[cell],
                   "quaternion and inverse-transpose normal shading preserve the expected linear "
                   "light");
            Expect((item->indexBuffer != 0u) == (mode == 1u && cell >= 2u),
                   "only repeated pure identity cells use shared geometry");
            if (mode == 0u)
                memcpy(reference[cell], actual, sizeof(reference[cell]));
            else
                for (uint32_t i = 0u; i < 6u; ++i)
                {
                    const uint32_t vertex = item->indexBuffer == 0u ? i : indices[i];
                    Expect(memcmp(&actual[vertex], &reference[cell][i], sizeof(actual[vertex])) ==
                               0,
                           "hybrid fallback preserves every legacy lit position, UV and color");
                }
        }
        if (mode == 1u)
        {
            const uint32_t ordinary = device.ordinaryCalls, instanced = device.instanceCalls;
            Expect(renderApi->submit(renderer, &camera, NULL) != 0u &&
                       device.ordinaryCalls == ordinary + 2u &&
                       device.instanceCalls == instanced + 1u,
                   "rotated and nonuniform remainder submits normally beside shared identity "
                   "geometry");
        }
        CheckLegacyStats(renderer);
        renderApi->destroy(renderer);
        Expect(device.live == 0u, "both normal-lighting paths release their complete geometry");
    }
    worldApi->destroy(world);
}

static void TestSharedModelReplacement(void)
{
    EnableNativeDevice();
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "replacement world creates");
    Add(world, 0, 0.0f);
    Add(world, 1, 0.0f);
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 4u);
    RegisterQuad(renderer, 7u);
    const LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, 1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    Expect(renderApi->setLighting(renderer, &light) != 0u &&
               renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u,
           "old model generation caches one shared pair");
    LaiueGraphicsDrawItemV2 oldItems[2], items[2];
    Expect(Draws(renderer, &camera, NULL, oldItems, 2u) == 2u, "old model placements are visible");
    const LaiueMeshRenderVertexV1 replacementVertices[4] = {
        {{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xff0000ffu},
        {{2, 0, 0}, {0, 0, 1}, {1, 0}, 0xff0000ffu},
        {{2, 1, 0}, {0, 0, 1}, {1, 1}, 0xff0000ffu},
        {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xff0000ffu}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 replacement = {
        sizeof(replacement), replacementVertices, 4u, indices, 6u, NULL, 0u};
    Expect(renderApi->registerModel(renderer, 7u, &replacement) != 0u,
           "model replacement invalidates shared cells without retiring their visible generation");
    for (uint32_t failure = 0u; failure < 3u; ++failure)
    {
        device.failAtCreate = failure == 0u ? device.creates + 1u : 0u;
        device.failAtUpload = failure == 0u ? 0u : device.uploads + failure;
        Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) == 0u && device.live == 2u &&
                   Draws(renderer, &camera, NULL, items, 2u) == 2u &&
                   memcmp(items, oldItems, sizeof(items)) == 0,
               "replacement create, vertex upload and index upload failures preserve both old draw "
               "items");
        const LaiueGraphicsVertexV2 *held = device.buffers[items[0].vertexBuffer].bytes;
        Expect(Near(held[1].position[0], 1.0f) && held[0].colorRGBA == 0xffbfbfbfu,
               "failed model replacement preserves the old vertex bytes and shaded color");
        device.failAtCreate = device.failAtUpload = 0u;
    }
    Expect(
        renderApi->update(renderer, &camera, 40.0f, 1u, NULL) != 0u && device.live == 4u &&
            Draws(renderer, &camera, NULL, items, 2u) == 2u &&
            items[0].vertexBuffer != oldItems[0].vertexBuffer &&
            items[1].vertexBuffer == oldItems[1].vertexBuffer,
        "progressive replacement keeps old and new shared generations for their remaining cells");
    const uint32_t calls = device.instanceCalls;
    const uint32_t ordinary = device.ordinaryCalls;
    Expect(renderApi->submit(renderer, &camera, NULL) != 0u && device.instanceCalls == calls &&
               device.ordinaryCalls == ordinary + 2u,
           "coexisting singleton generations submit distinct live geometry without instance-ring "
           "calls");
    Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u && device.live == 2u &&
               device.buffers[oldItems[0].vertexBuffer].bytes == NULL &&
               device.buffers[oldItems[0].indexBuffer].bytes == NULL,
           "the final old cell releases its model pair exactly once");
    Draws(renderer, &camera, NULL, items, 2u);
    const LaiueGraphicsVertexV2 *current = device.buffers[items[0].vertexBuffer].bytes;
    Expect(Near(current[1].position[0], 2.0f) && current[0].colorRGBA == 0xff0000bfu &&
               items[0].vertexBuffer == items[1].vertexBuffer,
           "new model generation shares the replacement positions and recomputed lighting");
    renderApi->destroy(renderer);
    worldApi->destroy(world);
    Expect(device.live == 0u && device.creates == device.destroys,
           "successful and failed replacement generations release every owned buffer");
}

static void TestDistinctSharedModelBudget(void)
{
    EnableNativeDevice();
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "shared budget world creates");
    AddModel(world, 4, 0.0f, 9u);
    AddModel(world, 5, 0.0f, 9u);
    LaiueMeshPositionV1 camera = {{5, 0, 0}, {8.0f, 0, 0}};
    LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 4u);
    for (uint32_t model = 7u; model <= 9u; ++model)
        RegisterQuad(renderer, model);
    Expect(renderApi->update(renderer, &camera, 100.0f, 0u, NULL) != 0u && device.live == 2u,
           "two pure distant cells of one repeated model hold one complete shared pair");
    AddModel(world, 0, 0.0f, 7u);
    AddModel(world, 1, 0.0f, 7u);
    AddModel(world, 2, 0.0f, 8u);
    AddModel(world, 3, 0.0f, 8u);
    camera.cell.x = 0;
    const uint32_t creates = device.creates;
    Expect(
        renderApi->update(renderer, &camera, 100.0f, 0u, NULL) != 0u && device.live == 4u &&
            device.creates == creates + 4u,
        "two nearest distinct models replace the farther pair under the combined resource budget");
    LaiueGraphicsDrawItemV2 items[6];
    Expect(Draws(renderer, &camera, NULL, items, 6u) == 4u &&
               Near(items[0].originRelative[0], -8.0f) && Near(items[3].originRelative[0], 40.0f) &&
               items[0].vertexBuffer == items[1].vertexBuffer &&
               items[2].vertexBuffer == items[3].vertexBuffer &&
               items[0].vertexBuffer != items[2].vertexBuffer &&
               items[0].indexBuffer != items[2].indexBuffer,
           "nearest distinct models retain both pure cells while the farther model is undrawn");
    LaiueMeshRenderStatsV1 stats = {.structSize = sizeof(stats)};
    Expect(renderApi->stats(renderer, &stats) != 0u && stats.buffers == 2u &&
               stats.indexBuffers == 2u && stats.instances == 4u && stats.overBudget == 2u &&
               stats.pending == 0u,
           "budget eviction counts both shared vertex and index resources without a false rebuild "
           "backlog");
    const uint32_t warmCreates = device.creates, warmUploads = device.uploads;
    Expect(renderApi->update(renderer, &camera, 100.0f, 0u, NULL) != 0u &&
               device.creates == warmCreates && device.uploads == warmUploads,
           "a refused far model performs no disposable GPU upload on every warm update");
    camera.cell.x = 5;
    Expect(renderApi->update(renderer, &camera, 100.0f, 0u, NULL) != 0u && device.live == 4u &&
               Draws(renderer, &camera, NULL, items, 6u) == 4u &&
               Near(items[0].originRelative[0], -56.0f) && Near(items[3].originRelative[0], -8.0f),
           "moving the camera evicts the old farthest model and restores the new nearest pair");
    renderApi->destroy(renderer);
    for (uint32_t budget = 2u; budget <= 3u; ++budget)
    {
        renderer = CreateRenderer(world, budget);
        for (uint32_t model = 7u; model <= 9u; ++model)
            RegisterQuad(renderer, model);
        Expect(renderApi->update(renderer, &camera, 100.0f, 0u, NULL) != 0u && device.live == 2u &&
                   Draws(renderer, &camera, NULL, items, 6u) == 2u && items[0].indexBuffer != 0u &&
                   items[1].indexBuffer == items[0].indexBuffer &&
                   Near(items[0].originRelative[0], -24.0f) &&
                   Near(items[1].originRelative[0], -8.0f),
               "two- and three-resource budgets retain both nearest pure cells with one complete "
               "pair");
        stats.structSize = sizeof(stats);
        Expect(renderApi->stats(renderer, &stats) != 0u && stats.overBudget == 4u &&
                   stats.buffers == 1u && stats.indexBuffers == 1u && stats.instances == 2u &&
                   stats.pending == 0u,
               "remaining distinct models are reported over budget without partial pairs");
        renderApi->destroy(renderer);
    }
    worldApi->destroy(world);
    Expect(device.live == 0u && device.creates == device.destroys,
           "budget eviction and renderer destruction release each model generation exactly once");
}

static void TestTinyNormalUniformScale(void)
{
    static LaiueGraphicsVertexV2 reference[2][6];
    const LaiueMeshRenderVertexV1 vertices[4] = {{{0, 0, 0}, {0, 0, 1.0e-7f}, {0, 0}, 0xffffffffu},
                                                 {{1, 0, 0}, {0, 0, 1.0e-7f}, {1, 0}, 0xffffffffu},
                                                 {{1, 1, 0}, {0, 0, 1.0e-7f}, {1, 1}, 0xffffffffu},
                                                 {{0, 1, 0}, {0, 0, 1.0e-7f}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 geometry = {sizeof(geometry), vertices, 4u, indices, 6u, NULL, 0u};
    const LaiueMeshRenderLightingV1 light = {
        sizeof(light), {0, 0, -1}, {0.5f, 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f}};
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "tiny-normal world creates");
    for (int64_t cell = 0; cell < 2; ++cell)
    {
        const LaiueMeshInstanceV1 instance = Add(world, cell, 0.0f);
        LaiueMeshInstanceInfoV1 info = {0};
        Expect(worldApi->get(world, instance, &info) != 0u, "tiny-normal placement reads");
        info.transform.scale[0] = info.transform.scale[1] = info.transform.scale[2] = 0.01f;
        Expect(worldApi->setTransform(world, instance, &info.transform) != 0u,
               "positive uniform scaling crosses the legacy normal-length threshold");
    }
    EnableNativeDevice();
    for (uint32_t mode = 0u; mode < 2u; ++mode)
    {
        device.capabilities =
            mode == 0u ? 0u
                       : LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
        LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 2u);
        Expect(renderApi->registerModel(renderer, 7u, &geometry) != 0u &&
                   renderApi->setLighting(renderer, &light) != 0u &&
                   renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u,
               "tiny-normal uniform scale builds with and without native capabilities");
        LaiueGraphicsDrawItemV2 items[2];
        Expect(Draws(renderer, &camera, NULL, items, 2u) == 2u && device.live == 2u,
               "a scale-dependent lighting mismatch retains both repeated cells as baked VBs");
        for (uint32_t cell = 0u; cell < 2u; ++cell)
        {
            const LaiueGraphicsDrawItemV2 *item = &items[cell];
            Expect(item->indexBuffer == 0u && item->indexCount == 6u &&
                       Near(item->originRelative[0], (float)cell * 16.0f),
                   "each repeated tiny-normal cell keeps its complete ordinary draw");
            const LaiueGraphicsVertexV2 *actual = device.buffers[item->vertexBuffer].bytes;
            for (uint32_t i = 0u; i < 6u; ++i)
                Expect(
                    actual[i].colorRGBA == 0xff404040u,
                    "inverse-scaled tiny normals receive the legacy ambient quarter-light color");
            if (mode == 0u)
                memcpy(reference[cell], actual, sizeof(reference[cell]));
            else
                Expect(memcmp(reference[cell], actual, sizeof(reference[cell])) == 0,
                       "native availability preserves each cell's legacy tiny-normal position UV "
                       "and lit bytes");
        }
        const uint32_t ordinary = device.ordinaryCalls, instanced = device.instanceCalls;
        Expect(renderApi->submit(renderer, &camera, NULL) != 0u &&
                   device.ordinaryCalls == ordinary + 2u && device.instanceCalls == instanced,
               "the tiny-normal remainder submits through the ordinary baked path");
        LaiueMeshRenderStatsV1 stats = {.structSize = sizeof(stats)};
        Expect(renderApi->stats(renderer, &stats) != 0u && stats.buffers == 2u &&
                   stats.indexBuffers == 0u && stats.instances == 0u &&
                   stats.instanceDrawCalls == 0u && stats.vertices == 12u,
               "tiny-normal stats describe baked geometry without hidden shared resources or "
               "instances");
        renderApi->destroy(renderer);
    }
    worldApi->destroy(world);
    Expect(device.live == 0u && device.creates == device.destroys,
           "tiny-normal comparison releases every owner exactly once");
}

static void WriteMetric(const char *name, uint64_t value)
{
    char digits[21];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[21];
    for (uint32_t i = 0u; i < count; ++i)
        text[i] = digits[count - i - 1u];
    text[count] = '\0';
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite(text);
}

static void TestConservativeCellPolicy(void)
{
    static LaiueGraphicsVertexV2 reference[6];
    EnableNativeDevice();
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    for (uint32_t mixed = 0u; mixed < 2u; ++mixed)
    {
        LaiueMeshWorldV1 *world = NULL;
        Expect(worldApi->create(&config, &world) != 0u, "conservative-policy world creates");
        const LaiueMeshInstanceV1 first = Add(world, 0, 0.0f);
        LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 4u);
        RegisterQuad(renderer, 7u);
        RegisterQuad(renderer, 8u);
        LaiueGraphicsDrawItemV2 items[3];
        Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 1u && items[0].indexBuffer == 0u &&
                   device.live == 1u &&
                   device.buffers[items[0].vertexBuffer].size == sizeof(reference),
               "a singleton pure cell retains its 144-byte baked draw despite native capabilities");
        memcpy(reference, device.buffers[items[0].vertexBuffer].bytes, sizeof(reference));
        const LaiueGraphicsDrawItemV2 old = items[0];
        Add(world, 1, 0.0f);
        device.failAtCreate = device.creates + 1u;
        Expect(renderApi->update(renderer, &camera, 40.0f, 1u, NULL) == 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 1u && device.live == 1u &&
                   memcmp(&items[0], &old, sizeof(old)) == 0,
               "failed promotion preserves the old singleton's visible baked draw");
        device.failAtCreate = 0u;
        uint32_t pending = 0u;
        Expect(renderApi->update(renderer, &camera, 40.0f, 1u, &pending) != 0u && pending == 1u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 1u && items[0].indexBuffer != 0u &&
                   device.live == 2u,
               "the first pure cell promotes progressively once the model has a second pure cell");
        Expect(renderApi->update(renderer, &camera, 40.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 2u &&
                   items[0].vertexBuffer == items[1].vertexBuffer && device.live == 2u,
               "the second pure cell joins the existing shared pair without a baked duplicate");
        Add(world, 2, 0.0f);
        Expect(renderApi->update(renderer, &camera, 70.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 3u && device.live == 2u,
               "a third repeated pure cell reuses the same native geometry");
        const uint32_t creates = device.creates, uploads = device.uploads;
        const uint32_t queries = cellInstanceQueries, instances = device.instanceCalls;
        const uint32_t revisions = cellRevisionQueries;
        const uint32_t placements = device.submittedInstances;
        Expect(renderApi->update(renderer, &camera, 70.0f, 0u, NULL) != 0u &&
                   device.creates == creates && device.uploads == uploads &&
                   cellInstanceQueries == queries && cellRevisionQueries == revisions + 3u &&
                   renderApi->submit(renderer, &camera, NULL) != 0u &&
                   device.instanceCalls == instances + 1u &&
                   device.submittedInstances == placements + 3u,
               "warm repeated pure cells need no instance listing or GPU upload and join one "
               "native call");
        const LaiueMeshInstanceV1 extra = AddModel(world, 0, 2.0f, mixed == 0u ? 7u : 8u);
        Expect(renderApi->update(renderer, &camera, 70.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 3u && items[0].indexBuffer == 0u &&
                   items[0].indexCount == 12u && items[1].indexBuffer != 0u &&
                   items[1].vertexBuffer == items[2].vertexBuffer && device.live == 3u &&
                   device.buffers[items[0].vertexBuffer].size == 2u * sizeof(reference),
               "mixed models and multiple placements stay in one baked cell draw beside two pure "
               "cells");
        const LaiueGraphicsVertexV2 *baked = device.buffers[items[0].vertexBuffer].bytes;
        Expect(memcmp(baked, reference, sizeof(reference)) == 0,
               "the first placement keeps its exact legacy baked vertex bytes");
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            LaiueGraphicsVertexV2 shifted = reference[i];
            shifted.position[0] += 2.0f;
            Expect(memcmp(&baked[6u + i], &shifted, sizeof(shifted)) == 0,
                   "the second same-material placement stays in the same legacy vertex batch");
        }
        const uint32_t ordinary = device.ordinaryCalls, instanced = device.instanceCalls;
        Expect(
            renderApi->submit(renderer, &camera, NULL) != 0u &&
                device.ordinaryCalls == ordinary + 1u && device.instanceCalls == instanced + 1u,
            "one baked material call and one native pair preserve the old three-item draw count");
        Expect(worldApi->remove(world, extra) != 0u &&
                   renderApi->update(renderer, &camera, 70.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 3u && items[0].indexBuffer != 0u &&
                   device.live == 2u,
               "restoring cell purity releases its baked batch and rejoins the shared pair");
        LaiueGraphicsDrawItemV2 before[3];
        memcpy(before, items, sizeof(before));
        LaiueMeshInstanceInfoV1 info = {0};
        Expect(worldApi->get(world, first, &info) != 0u, "dirty census transform reads");
        info.transform.position.local[0] = 0.25f;
        Expect(worldApi->setTransform(world, first, &info.transform) != 0u,
               "remaining cell dirties before a radius shrink");
        failCellInstanceQuery = true;
        Expect(renderApi->update(renderer, &camera, 1.0f, 0u, NULL) == 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 3u &&
                   memcmp(items, before, sizeof(before)) == 0 && device.live == 2u,
               "a failed dirty census preserves the entire prior cache including departing cells");
        failCellInstanceQuery = false;
        Expect(renderApi->update(renderer, &camera, 1.0f, 0u, NULL) != 0u &&
                   Draws(renderer, &camera, NULL, items, 3u) == 1u && items[0].indexBuffer == 0u &&
                   items[0].indexCount == 6u && device.live == 1u,
               "shrinking to one pure cell demotes the singleton and releases the shared pair");
        baked = device.buffers[items[0].vertexBuffer].bytes;
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            LaiueGraphicsVertexV2 shifted = reference[i];
            shifted.position[0] += 0.25f;
            Expect(memcmp(&baked[i], &shifted, sizeof(shifted)) == 0,
                   "demotion preserves the singleton's complete transformed legacy vertex bytes");
        }
        renderApi->destroy(renderer);
        worldApi->destroy(world);
        Expect(device.live == 0u && device.creates == device.destroys,
               "policy promotion, mixed cells and demotion release each geometry owner once");
    }
}

static void TestSameCellDistinctModelFootprint(void)
{
    static LaiueGraphicsVertexV2 reference[8u * 6u];
    EnableNativeDevice();
    const LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 128u};
    const LaiueMeshPositionV1 camera = {{0, 0, 0}, {0, 0, 0}};
    LaiueMeshWorldV1 *world = NULL;
    Expect(worldApi->create(&config, &world) != 0u, "same-cell distinct-model world creates");
    for (uint32_t i = 0u; i < 8u; ++i)
        AddModel(world, 0, (float)i * 1.5f, 70u + i);
    for (uint32_t mode = 0u; mode < 2u; ++mode)
    {
        device.capabilities =
            mode == 0u ? 0u
                       : LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES;
        LaiueMeshWorldRendererV1 *renderer = CreateRenderer(world, 32u);
        for (uint32_t model = 70u; model < 78u; ++model)
            RegisterQuad(renderer, model);
        Expect(
            renderApi->update(renderer, &camera, 10.0f, 0u, NULL) != 0u,
            "eight same-material models in one cell prepare within the explicit resource budget");
        LaiueGraphicsDrawItemV2 items[8];
        const uint32_t draws = Draws(renderer, &camera, NULL, items, 8u);
        Expect(draws == 1u && items[0].indexBuffer == 0u && items[0].indexCount == 8u * 6u &&
                   device.live == 1u &&
                   device.buffers[items[0].vertexBuffer].size == sizeof(reference),
               "eight unique models in one cell retain one complete 1152-byte baked material draw");
        uint32_t renderedIndices = 0u;
        for (uint32_t i = 0u; i < draws; ++i)
            renderedIndices += items[i].indexCount;
        Expect(renderedIndices == 8u * 6u,
               "same-cell sharing policy describes all eight quads without losing triangles");
        const LaiueGraphicsVertexV2 *actual = device.buffers[items[0].vertexBuffer].bytes;
        if (mode == 0u)
            memcpy(reference, actual, sizeof(reference));
        else
            Expect(memcmp(reference, actual, sizeof(reference)) == 0,
                   "native capabilities preserve the entire legacy same-cell vertex batch");
        CheckProviderFailureRetry(renderer, &camera, NULL, false, 0u);
        const uint32_t ordinary = device.ordinaryCalls, instanced = device.instanceCalls;
        const uint32_t placements = device.submittedInstances;
        Expect(renderApi->submit(renderer, &camera, NULL) != 0u &&
                   device.ordinaryCalls == ordinary + 1u && device.instanceCalls == instanced &&
                   device.submittedInstances == placements,
               "same-cell distinct models submit once without native instance calls");
        uint64_t requestedBytes = 0u;
        for (uint32_t slot = 1u; slot < 32u; ++slot)
            if (device.buffers[slot].bytes != NULL)
                requestedBytes += device.buffers[slot].size;
        LaiueMeshRenderStatsV1 stats = {.structSize = sizeof(stats)};
        Expect(renderApi->stats(renderer, &stats) != 0u && stats.cells == 1u &&
                   stats.buffers == 1u && stats.indexBuffers == 0u && stats.instances == 0u &&
                   stats.instanceDrawCalls == 0u && stats.vertices == 8u * 6u &&
                   requestedBytes == sizeof(reference),
               "conservative same-cell policy preserves the legacy draw count and exact footprint");
        LaiueTestRuntimeWrite(mode == 0u ? "same-cell unique-models legacy:"
                                         : "same-cell unique-models native:");
        WriteMetric(" ordinaryDrawItems=", draws);
        WriteMetric(" ordinaryCalls=", device.ordinaryCalls - ordinary);
        WriteMetric(" instanceCalls=", device.instanceCalls - instanced);
        WriteMetric(" instances=", device.submittedInstances - placements);
        WriteMetric(" vertexBuffers=", stats.buffers);
        WriteMetric(" indexBuffers=", stats.indexBuffers);
        WriteMetric(" requestedGeometryBytes=", requestedBytes);
        LaiueTestRuntimeWrite("\n");
        renderApi->destroy(renderer);
        Expect(device.live == 0u && device.creates == device.destroys,
               "distinct-model footprint comparison releases each cached geometry resource");
    }
    worldApi->destroy(world);
}

LAIUE_TEST_ENTRY(MeshWorldRenderTestEntryPoint)
{
    realWorldApi = LaiueMeshWorldGetStaticServiceV1();
    countedWorldApi = *realWorldApi;
    countedWorldApi.cellInstances = CellInstances;
    countedWorldApi.cellRevision = CellRevision;
    worldApi = &countedWorldApi;
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
    TestNativeSharedGeometry();
    TestFallbackNormalLighting();
    TestSharedModelReplacement();
    TestDistinctSharedModelBudget();
    TestTinyNormalUniformScale();
    TestConservativeCellPolicy();
    TestSameCellDistinctModelFootprint();
    LaiueTestRuntimeWrite("mesh_world_render_test passed\n");
    LAIUE_TEST_SUCCESS();
}
