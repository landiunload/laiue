/* Manual public-host A/B benchmark. Point LAIUE_MESH_RENDER_BENCH_MODULE_DIR
 * at an immutable baseline or candidate module directory. Timings separately
 * cover warm renderer updates and CPU submission; frame setup, GPU execution,
 * capture and fence waits remain outside both measurements. */
#include "graphics/graphics_device_service.h"
#include "mesh_world_render/mesh_world_render_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WIDTH 64u
#define HEIGHT 64u
#define PURE_CELLS 40u
#define UNIQUE_MODELS 8u
#define SAMPLES 9u
#define REPETITIONS 128u
#define ITEM_CAPACITY 64u
#define QUERY_RADIUS 1024.0f

static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t modulePaths[3][LAIUE_PLATFORM_PATH_CAPACITY];
static uint8_t pixels[WIDTH * HEIGHT * 4u];
static LaiueGraphicsDrawItemV2 items[ITEM_CAPACITY];

typedef struct Benchmark
{
    const LaiueMeshWorldServiceV1 *worldApi;
    const LaiueMeshWorldRenderServiceV1 *renderApi;
    const LaiueGraphicsDeviceServiceV2 *graphicsApi;
    uint32_t renderTableSize;
    bool directSubmit;
    LaiueGraphicsDeviceV2 *device;
    LaiueMeshWorldV1 *world;
    LaiueMeshWorldRendererV1 *renderer;
    LaiueMeshPositionV1 camera;
} Benchmark;

static void Require(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("FAIL: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Number(uint64_t value)
{
    char digits[21], output[22];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (uint32_t i = 0u; i < count; ++i)
        output[i] = digits[count - i - 1u];
    output[count] = '\0';
    LaiueTestRuntimeWrite(output);
}

static void Field(const char *name, uint64_t value)
{
    LaiueTestRuntimeWrite(name);
    Number(value);
}

static uint64_t Nanoseconds(double elapsed, uint32_t repetitions)
{
    return (uint64_t)(elapsed * 1000000000.0 / (double)repetitions + 0.5);
}

static void JoinModulePath(uint32_t index, const wchar_t *name)
{
    uint32_t length = 0u;
    while (directory[length] != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        modulePaths[index][length] = directory[length];
        ++length;
    }
    Require(length + 64u < LAIUE_PLATFORM_PATH_CAPACITY, "module path capacity");
    if (length != 0u && modulePaths[index][length - 1u] != L'/' &&
        modulePaths[index][length - 1u] != L'\\')
        modulePaths[index][length++] = L'/';
    while (*name != L'\0')
        modulePaths[index][length++] = *name++;
    modulePaths[index][length] = L'\0';
}

static LaiueModuleHost *LoadModules(Benchmark *benchmark)
{
    static char directoryUtf8[LAIUE_PLATFORM_PATH_CAPACITY * 4u];
    const uint32_t directoryLength = PlatformGetEnvironmentUtf8(
        "LAIUE_MESH_RENDER_BENCH_MODULE_DIR", directoryUtf8, sizeof(directoryUtf8));
    if (directoryLength != 0u)
        Require(directoryLength < sizeof(directoryUtf8) &&
                    PlatformUtf8ToWide(directoryUtf8, directoryLength, directory,
                                       LAIUE_PLATFORM_PATH_CAPACITY, NULL),
                "module directory environment");
    else
        Require(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
                "executable directory");
#if defined(_WIN32)
#define BENCH_MODULE(name) L"laiue_" name L".dll"
#elif defined(__APPLE__)
#define BENCH_MODULE(name) L"liblaiue_" name L".dylib"
#else
#define BENCH_MODULE(name) L"liblaiue_" name L".so"
#endif
    JoinModulePath(0u, BENCH_MODULE(L"mesh_world"));
    JoinModulePath(1u, BENCH_MODULE(L"render"));
    JoinModulePath(2u, BENCH_MODULE(L"mesh_world_render"));
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic = {0};
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Require(host != NULL, "module host");
    const LaiueModuleBinaryV1 binaries[3] = {
        {modulePaths[0], 0u, NULL}, {modulePaths[1], 0u, NULL}, {modulePaths[2], 0u, NULL}};
    Require(LaiueModuleHostLoad(host, binaries, 3u, &diagnostic) == LAIUE_MODULE_OK,
            diagnostic.message);
    benchmark->worldApi = LaiueModuleHostQueryService(host, LAIUE_MESH_WORLD_SERVICE_NAME,
                                                      LAIUE_MESH_WORLD_SERVICE_ABI_VERSION_1,
                                                      sizeof(LaiueMeshWorldServiceV1), NULL, NULL);
    benchmark->renderApi = LaiueModuleHostQueryService(
        host, LAIUE_MESH_WORLD_RENDER_SERVICE_NAME, LAIUE_MESH_WORLD_RENDER_SERVICE_ABI_VERSION_1,
        (uint32_t)offsetof(LaiueMeshWorldRenderServiceV1, submit), NULL,
        &benchmark->renderTableSize);
    benchmark->graphicsApi = LaiueModuleHostQueryService(
        host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE, NULL, NULL);
    Require(benchmark->worldApi != NULL && benchmark->renderApi != NULL &&
                benchmark->graphicsApi != NULL,
            "public world/render/mesh-world-render services");
    const uint32_t submitSize = (uint32_t)(offsetof(LaiueMeshWorldRenderServiceV1, submit) +
                                           sizeof(benchmark->renderApi->submit));
    benchmark->directSubmit = benchmark->renderTableSize >= submitSize &&
                              benchmark->renderApi->structSize >= submitSize &&
                              benchmark->renderApi->submit != NULL;
    Field("service_table_bytes=", benchmark->renderTableSize);
    Field(" direct_submit=", benchmark->directSubmit ? 1u : 0u);
    LaiueTestRuntimeWrite("\n");
    return host;
}

static LaiueGraphicsDiagnosticsV2 DeviceStats(Benchmark *benchmark)
{
    LaiueGraphicsDiagnosticsV2 result = {.structSize = sizeof(result)};
    Require(benchmark->device->getDiagnostics(benchmark->device, &result) != 0u,
            "device diagnostics");
    return result;
}

static LaiueMeshRenderStatsV1 RendererStats(Benchmark *benchmark)
{
    LaiueMeshRenderStatsV1 result = {.structSize = sizeof(result)};
    Require(benchmark->renderApi->stats(benchmark->renderer, &result) != 0u, "renderer statistics");
    return result;
}

static void Begin(Benchmark *benchmark, bool capture)
{
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    camera.viewProjection[0] = camera.viewProjection[5] = camera.viewProjection[10] =
        camera.viewProjection[15] = 1.0f;
    Require(benchmark->device->setCamera(benchmark->device, &camera) != 0u, "set camera");
    if (capture &&
        benchmark->device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_REQUEST_SIZE &&
        benchmark->device->requestFrameReadback != NULL)
        Require(benchmark->device->requestFrameReadback(benchmark->device) != 0u,
                "request frame capture");
    Require(benchmark->device->beginFrame(benchmark->device, WIDTH, HEIGHT) != 0u,
            "begin offscreen frame");
}

static void End(Benchmark *benchmark)
{
    Require(benchmark->device->endFrame(benchmark->device) != 0u, "end offscreen frame");
}

static uint32_t Submit(Benchmark *benchmark)
{
    if (benchmark->directSubmit)
        return benchmark->renderApi->submit(benchmark->renderer, &benchmark->camera, NULL);
    uint32_t count = 0u;
    if (benchmark->renderApi->draws(benchmark->renderer, &benchmark->camera, NULL, items,
                                    ITEM_CAPACITY, &count) == 0u ||
        count > ITEM_CAPACITY)
        return 0u;
    return benchmark->device->submit(benchmark->device, items, count);
}

static uint64_t Capture(Benchmark *benchmark)
{
    LaiueGraphicsFrameReadbackV2 request = {
        .structSize = sizeof(request), .pixels = pixels, .capacityBytes = sizeof(pixels)};
    Require(benchmark->device->readbackFrame(benchmark->device, &request) != 0u &&
                request.writtenBytes == sizeof(pixels) && request.width == WIDTH &&
                request.height == HEIGHT,
            "read back completed frame");
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t y = 0u; y < HEIGHT; ++y)
        for (uint32_t x = 0u; x < WIDTH; ++x)
        {
            const uint8_t *pixel = pixels + (y * WIDTH + x) * 4u;
            const bool red = pixel[0] > 240u && pixel[1] < 12u && pixel[2] < 12u;
            Require(red == (x >= WIDTH / 4u && x < WIDTH * 3u / 4u && y >= HEIGHT / 4u &&
                            y < HEIGHT * 3u / 4u),
                    "exact visible quad occupancy; remaining real geometry clips outside view");
            for (uint32_t channel = 0u; channel < 4u; ++channel)
            {
                hash ^= pixel[channel];
                hash *= UINT64_C(1099511628211);
            }
        }
    return hash;
}

static void RegisterQuad(Benchmark *benchmark, uint32_t model)
{
    const LaiueMeshRenderVertexV1 vertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, UINT32_C(0xff0000ff)},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, UINT32_C(0xff0000ff)}};
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueMeshRenderModelV1 geometry = {.structSize = sizeof(geometry),
                                             .vertices = vertices,
                                             .vertexCount = 4u,
                                             .indices = indices,
                                             .indexCount = 6u};
    Require(benchmark->renderApi->registerModel(benchmark->renderer, model, &geometry) != 0u,
            "register quad geometry");
    const LaiueMeshShapeV1 shape = {.structSize = sizeof(shape),
                                    .boundsMin = {-0.5f, -0.5f, -0.01f},
                                    .boundsMax = {0.5f, 0.5f, 0.01f}};
    Require(benchmark->worldApi->registerShape(benchmark->world, model, &shape) != 0u,
            "register quad bounds");
}

static void CreateScene(Benchmark *benchmark, bool unique)
{
    Require(benchmark->graphicsApi->createDeviceWithContext(
                benchmark->graphicsApi->context, NULL, WIDTH, HEIGHT, LAIUE_GRAPHICS_BACKEND_VULKAN,
                &benchmark->device) != 0u,
            "create offscreen Vulkan device");
    Require(benchmark->device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_SIZE &&
                benchmark->device->setCamera != NULL && benchmark->device->getDiagnostics != NULL &&
                benchmark->device->readbackFrame != NULL,
            "camera, diagnostic and readback device callbacks");
    const LaiueMeshWorldConfigV1 worldConfig = {
        .structSize = sizeof(worldConfig), .cellSize = 16.0f, .maximumInstances = PURE_CELLS};
    Require(benchmark->worldApi->create(&worldConfig, &benchmark->world) != 0u, "create world");
    const LaiueMeshWorldRendererConfigV1 renderConfig = {.structSize = sizeof(renderConfig),
                                                         .worldService = benchmark->worldApi,
                                                         .world = benchmark->world,
                                                         .device = benchmark->device,
                                                         .maximumBuffers = 256u};
    Require(benchmark->renderApi->create(&renderConfig, &benchmark->renderer) != 0u,
            "create world renderer");
    const LaiueMeshRenderLightingV1 lighting = {.structSize = sizeof(lighting),
                                                .toSun = {0.0f, 0.0f, 1.0f},
                                                .ambientColor = {1.0f, 1.0f, 1.0f}};
    Require(benchmark->renderApi->setLighting(benchmark->renderer, &lighting) != 0u,
            "set exact unattenuated color");
    for (uint32_t model = 0u; model < (unique ? UNIQUE_MODELS : 1u); ++model)
        RegisterQuad(benchmark, 100u + model);
    for (uint32_t i = 0u; i < (unique ? UNIQUE_MODELS : PURE_CELLS); ++i)
    {
        LaiueMeshInstanceDescV1 instance = {.structSize = sizeof(instance),
                                            .model = unique ? 100u + i : 100u,
                                            .flags = LAIUE_MESH_INSTANCE_VISIBLE};
        instance.transform.position.cell.x = unique ? 0 : (int64_t)i;
        instance.transform.position.local[0] = unique && i != 0u ? 12.0f + (float)i * 0.25f : 8.0f;
        instance.transform.position.local[1] = 8.0f;
        instance.transform.position.local[2] = 0.5f;
        instance.transform.rotation[3] = 1.0f;
        instance.transform.scale[0] = instance.transform.scale[1] = instance.transform.scale[2] =
            1.0f;
        LaiueMeshInstanceV1 handle = 0u;
        Require(benchmark->worldApi->add(benchmark->world, &instance, &handle) != 0u,
                "place benchmark quad");
    }
    memset(&benchmark->camera, 0, sizeof(benchmark->camera));
    benchmark->camera.local[0] = benchmark->camera.local[1] = 8.0f;
}

static void RunScene(Benchmark *benchmark, bool unique)
{
    CreateScene(benchmark, unique);
    Begin(benchmark, false);
    End(benchmark);
    const LaiueGraphicsDiagnosticsV2 base = DeviceStats(benchmark);
    uint32_t pending = 0u;
    uint32_t coldUpdates = 0u;
    uint64_t uploadedBytes = 0u;
    double coldSeconds = 0.0;
    do
    {
        const double start = PlatformMonotonicSeconds();
        const uint32_t result = benchmark->renderApi->update(
            benchmark->renderer, &benchmark->camera, QUERY_RADIUS, 16u, &pending);
        coldSeconds += PlatformMonotonicSeconds() - start;
        Require(result != 0u && ++coldUpdates <= PURE_CELLS, "bounded cold rebuild");
        /* Drain each group of at most 16 legacy VB uploads before the next
         * update. Neither baseline's 64-upload queue nor native uploads spill. */
        Begin(benchmark, false);
        End(benchmark);
        uploadedBytes += DeviceStats(benchmark).uploadedBytes;
    } while (pending != 0u);
    const LaiueMeshRenderStatsV1 ready = RendererStats(benchmark);
    Require(ready.cells == (unique ? 1u : PURE_CELLS) && ready.overBudget == 0u &&
                ready.pending == 0u,
            "complete scene inside radius and resource budget");
    uint32_t ordinaryItems = 0u;
    Require(benchmark->renderApi->draws(benchmark->renderer, &benchmark->camera, NULL, items,
                                        ITEM_CAPACITY, &ordinaryItems) != 0u &&
                ordinaryItems == (unique ? 1u : PURE_CELLS),
            "AUTO preserves legacy ordinary-item count");
    const bool shared = ready.structSize >= sizeof(ready) && ready.indexBuffers != 0u;
    for (uint32_t i = 0u; i < ordinaryItems; ++i)
    {
        const bool indexed = items[i].indexBuffer != 0u;
        const float expectedX = unique ? -8.0f : (float)i * 16.0f - (indexed ? 0.0f : 8.0f);
        Require(items[i].originRelative[0] == expectedX &&
                    items[i].originRelative[1] == (indexed ? 0.0f : -8.0f) &&
                    items[i].originRelative[2] == (indexed ? 0.5f : 0.0f) &&
                    items[i].indexCount == (unique ? UNIQUE_MODELS * 6u : 6u) &&
                    items[i].scale == 1.0f,
                "ordinary geometry spans every real cell with the expected indexed/baked origins");
    }
    Require(!unique || (!shared && ready.buffers == 1u && ready.vertices == UNIQUE_MODELS * 6u),
            "unique models retain one complete baked material batch");
    if (shared)
        Require(ready.buffers == 1u && ready.indexBuffers == 1u && ready.instances == PURE_CELLS &&
                    ready.vertices == 4u,
                "pure-cell cohort owns one shared quad pair");
    const uint64_t requested = ready.vertices * sizeof(LaiueGraphicsVertexV2) +
                               (shared ? (uint64_t)ready.indexBuffers * 6u * sizeof(uint32_t) : 0u);
    const uint64_t expected =
        unique ? UNIQUE_MODELS * 6u * sizeof(LaiueGraphicsVertexV2)
               : (shared ? 120u : PURE_CELLS * 6u * sizeof(LaiueGraphicsVertexV2));
    Require(requested == expected, "exact requested geometry byte bound");
    uint64_t imageHash = 0u;
    for (uint32_t warm = 0u; warm < 3u; ++warm)
    {
        Begin(benchmark, warm == 0u);
        Require(Submit(benchmark) != 0u, "warm complete submission");
        End(benchmark);
        if (warm == 0u)
            imageHash = Capture(benchmark);
    }
    LaiueTestRuntimeWrite(unique ? "scene=unique8" : "scene=pure40");
    Field(" shared=", shared ? 1u : 0u);
    Field(" cold_updates=", coldUpdates);
    Field(" cold_update_ns_total=", Nanoseconds(coldSeconds, 1u));
    Field(" cold_uploaded_bytes=", uploadedBytes);
    Field(" ordinary_items=", ordinaryItems);
    Field(" requested_geometry_bytes=", requested);
    Field(" image_hash=", imageHash);
    LaiueTestRuntimeWrite("\n");
    char longOption[2] = {0};
    const bool longRun =
        PlatformGetEnvironmentUtf8("LAIUE_MESH_RENDER_BENCH_LONG", longOption, 2u) == 1u &&
        longOption[0] == '1';
    const uint32_t updateRepetitions = longRun ? 8192u : REPETITIONS;
    const uint32_t submitRepetitions = longRun && unique ? 8192u : REPETITIONS;
    for (uint32_t sample = 0u; sample < SAMPLES; ++sample)
    {
        uint32_t updateResult = 1u;
        double start = PlatformMonotonicSeconds();
        for (uint32_t repeat = 0u; repeat < updateRepetitions; ++repeat)
            updateResult &= benchmark->renderApi->update(benchmark->renderer, &benchmark->camera,
                                                         QUERY_RADIUS, 16u, &pending);
        const uint64_t updateTime =
            Nanoseconds(PlatformMonotonicSeconds() - start, updateRepetitions);
        const LaiueMeshRenderStatsV1 warmStats = RendererStats(benchmark);
        Require(updateResult != 0u && pending == 0u && warmStats.rebuilt == 0u &&
                    warmStats.overBudget == 0u,
                "unchanged warm updates do no rebuilds");
        Begin(benchmark, sample == 0u);
        uint32_t submitResult = 1u;
        start = PlatformMonotonicSeconds();
        for (uint32_t repeat = 0u; repeat < submitRepetitions; ++repeat)
            submitResult &= Submit(benchmark);
        const uint64_t submitTime =
            Nanoseconds(PlatformMonotonicSeconds() - start, submitRepetitions);
        Require(submitResult != 0u, "all timed scene submissions succeed");
        End(benchmark);
        const LaiueGraphicsDiagnosticsV2 diagnostics = DeviceStats(benchmark);
        const uint32_t drawsPerSubmit = shared && benchmark->directSubmit ? 1u : ordinaryItems;
        Require(diagnostics.drawCalls == (uint64_t)submitRepetitions * drawsPerSubmit,
                "timing corresponds to all expected recorded draws");
        Require(diagnostics.geometryPoolUsedBytes >= base.geometryPoolUsedBytes &&
                    diagnostics.cpuShadowBytes >= base.cpuShadowBytes &&
                    diagnostics.resourceHandleCount >= base.resourceHandleCount,
                "device resource counter bounds");
        if (sample == 0u)
            Require(Capture(benchmark) == imageHash, "warm and repeated submission pixels match");
        LaiueTestRuntimeWrite(unique ? "sample_scene=unique8" : "sample_scene=pure40");
        Field(" sample=", sample);
        Field(" repetitions=", submitRepetitions);
        Field(" update_repetitions=", updateRepetitions);
        Field(" warm_update_ns=", updateTime);
        Field(" submit_ns=", submitTime);
        Field(" draws_per_submit=", drawsPerSubmit);
        Field(" requested_geometry_bytes=", requested);
        Field(" vertex_buffers=", ready.buffers);
        Field(" index_buffers=", shared ? ready.indexBuffers : 0u);
        Field(" instances=", shared ? ready.instances : 0u);
        Field(" instance_bytes_per_submit=", shared && benchmark->directSubmit
                                                 ? PURE_CELLS * sizeof(LaiueGraphicsInstanceV2)
                                                 : 0u);
        Field(" cpu_shadow_bytes=", diagnostics.cpuShadowBytes);
        Field(" cpu_shadow_delta=", diagnostics.cpuShadowBytes - base.cpuShadowBytes);
        Field(" geometry_used_bytes=", diagnostics.geometryPoolUsedBytes);
        Field(" geometry_used_delta=",
              diagnostics.geometryPoolUsedBytes - base.geometryPoolUsedBytes);
        Field(" geometry_capacity_bytes=", diagnostics.geometryPoolCapacityBytes);
        Field(" resource_handles=", diagnostics.resourceHandleCount);
        Field(" resource_handle_delta=",
              diagnostics.resourceHandleCount - base.resourceHandleCount);
        LaiueTestRuntimeWrite("\n");
    }
    benchmark->renderApi->destroy(benchmark->renderer);
    benchmark->worldApi->destroy(benchmark->world);
    benchmark->graphicsApi->destroyDevice(benchmark->device);
    benchmark->renderer = NULL;
    benchmark->world = NULL;
    benchmark->device = NULL;
}

LAIUE_TEST_ENTRY(MeshWorldRenderBenchmarkEntryPoint)
{
    Benchmark benchmark = {0};
    LaiueModuleHost *host = LoadModules(&benchmark);
    RunScene(&benchmark, false);
    RunScene(&benchmark, true);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("verify=expected_pixels_and_complete_geometry\nLAIUE_TEST_SUCCESS\n");
    LAIUE_TEST_SUCCESS();
}
