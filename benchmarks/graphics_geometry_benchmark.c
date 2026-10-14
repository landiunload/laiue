/* Manual public-device benchmark. Timings cover CPU command recording only;
 * frame setup, presentation, fence waits and captures remain outside them. */
#include "graphics/graphics_device_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WIDTH 64u
#define HEIGHT 64u
#define MAX_INSTANCES 4096u
#define SAMPLES 9u
static uint8_t pixels[WIDTH * HEIGHT * 4u];
static uint8_t reference[sizeof(pixels)];
static LaiueGraphicsInstanceV2 placements[MAX_INSTANCES];
static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];

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

static LaiueGraphicsDiagnosticsV2 Stats(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsDiagnosticsV2 value = {.structSize = sizeof(value)};
    Require(device->getDiagnostics(device, &value) != 0u, "diagnostics");
    return value;
}

static void Begin(LaiueGraphicsDeviceV2 *device, bool capture)
{
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    camera.viewProjection[0] = camera.viewProjection[5] = camera.viewProjection[10] =
        camera.viewProjection[15] = 1.0f;
    Require(device->setCamera(device, &camera) != 0u, "camera");
    if (capture && device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_REQUEST_SIZE &&
        device->requestFrameReadback != NULL)
        Require(device->requestFrameReadback(device) != 0u, "capture request");
    Require(device->beginFrame(device, WIDTH, HEIGHT) != 0u, "begin frame");
}

static void Capture(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsFrameReadbackV2 request = {
        .structSize = sizeof(request), .pixels = pixels, .capacityBytes = sizeof(pixels)};
    Require(device->readbackFrame(device, &request) != 0u && request.writtenBytes == sizeof(pixels),
            "capture completed frame");
    const uint8_t *center = pixels + (HEIGHT / 2u * WIDTH + WIDTH / 2u) * 4u;
    Require(center[0] > 240u && center[1] < 12u && center[2] < 12u,
            "timed geometry has an actual red triangle");
}

static LaiueGraphicsHandle Buffer(LaiueGraphicsDeviceV2 *device, uint32_t usage, const void *data,
                                  uint32_t size)
{
    LaiueGraphicsHandle handle = 0u;
    LaiueGraphicsBufferDescV1 description = {sizeof(description), usage, size};
    Require(device->createBuffer(device, &description, &handle) != 0u, "buffer allocation");
    LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload), .buffer = handle, .data = data, .sizeBytes = size};
    Require(device->uploadBuffer(device, &upload) != 0u, "buffer upload");
    return handle;
}

static uint64_t Record(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsDrawItemV2 *item,
                       uint32_t count, bool batch, uint32_t repetitions)
{
    const double start = PlatformMonotonicSeconds();
    for (uint32_t repetition = 0u; repetition < repetitions; ++repetition)
        if (batch)
            Require(device->submitInstances(device, item, placements, count) != 0u,
                    "instance batch");
        else
            for (uint32_t i = 0u; i < count; ++i)
            {
                LaiueGraphicsDrawItemV2 scalar = *item;
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                    scalar.originRelative[axis] += placements[i].originRelative[axis];
                scalar.scale = item->scale * placements[i].scale;
                Require(device->submit(device, &scalar, 1u) != 0u, "scalar draw");
            }
    const double elapsed = PlatformMonotonicSeconds() - start;
    return (uint64_t)(elapsed * 1000000000.0 / repetitions + 0.5);
}

LAIUE_TEST_ENTRY(GraphicsGeometryBenchmarkEntryPoint)
{
    Require(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY), "directory");
#if defined(_WIN32)
    const wchar_t *name = L"laiue_render.dll";
#elif defined(__APPLE__)
    const wchar_t *name = L"liblaiue_render.dylib";
#else
    const wchar_t *name = L"liblaiue_render.so";
#endif
    uint32_t length = 0u;
    while (directory[length] != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        modulePath[length] = directory[length];
        ++length;
    }
    Require(length + 32u < LAIUE_PLATFORM_PATH_CAPACITY, "module path capacity");
    if (length != 0u && modulePath[length - 1u] != L'/' && modulePath[length - 1u] != L'\\')
        modulePath[length++] = L'/';
    while (*name != L'\0')
        modulePath[length++] = *name++;
    modulePath[length] = L'\0';
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Require(host != NULL, "host");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Require(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
            diagnostic.message);
    const LaiueGraphicsDeviceServiceV2 *service = LaiueModuleHostQueryService(
        host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE, NULL, NULL);
    Require(service != NULL, "device service");
    LaiueGraphicsDeviceV2 *device = NULL;
    Require(service->createDeviceWithContext(service->context, NULL, WIDTH, HEIGHT,
                                             LAIUE_GRAPHICS_BACKEND_VULKAN, &device) != 0u,
            "offscreen Vulkan driver");
    const bool batchSupported =
        device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_INSTANCES_SIZE &&
        device->getCapabilities != NULL && device->submitInstances != NULL &&
        (device->getCapabilities(device) & LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES) != 0u;
    Field("native_instances=", batchSupported ? 1u : 0u);
    LaiueTestRuntimeWrite("\n");
    char quadOption[2] = {0};
    const bool quad =
        PlatformGetEnvironmentUtf8("LAIUE_GRAPHICS_BENCHMARK_QUAD", quadOption, 2u) == 1u &&
        quadOption[0] == '1';
    LaiueTestRuntimeWrite(quad ? "shape=quad\n" : "shape=triangle\n");
    char longOption[2] = {0};
    const bool longRun =
        PlatformGetEnvironmentUtf8("LAIUE_GRAPHICS_BENCHMARK_LONG", longOption, 2u) == 1u &&
        longOption[0] == '1';
    const uint32_t recordingBudget = longRun ? 8192u : MAX_INSTANCES;
    const uint32_t repetitionLimit = longRun ? 8192u : 128u;
    const uint32_t recordingFrames = longRun ? 8u : 1u;
    const LaiueGraphicsVertexV2 quadVertices[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, UINT32_C(0xff0000ff)},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}, UINT32_C(0xff0000ff)}};
    const LaiueGraphicsVertexV2 triangleVertices[3] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, UINT32_C(0xff0000ff)},
        {{0.0f, 0.5f, 0.0f}, {0.5f, 1.0f}, UINT32_C(0xff0000ff)}};
    const LaiueGraphicsVertexV2 *vertices = quad ? quadVertices : triangleVertices;
    const uint32_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const uint32_t vertexCount = quad ? 4u : 3u;
    const uint32_t indexCount = quad ? 6u : 3u;
    const LaiueGraphicsHandle vb = Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices,
                                          vertexCount * (uint32_t)sizeof(*vertices));
    const LaiueGraphicsHandle ib = Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indices,
                                          indexCount * (uint32_t)sizeof(*indices));
    LaiueGraphicsDrawItemV2 item = {.structSize = sizeof(item),
                                    .vertexBuffer = vb,
                                    .indexBuffer = ib,
                                    .indexCount = indexCount,
                                    .originRelative = {0.0f, 0.0f, 0.5f},
                                    .scale = 1.0f};
    for (uint32_t i = 0u; i < MAX_INSTANCES; ++i)
    {
        placements[i].scale = 1.0f;
        placements[i].originRelative[0] = i == 0u ? 0.0f : 8.0f + (float)(i & 15u);
    }
    /* The old implementation queues expanded vertices inside submit. Warm
     * twice before timing, then verify the baseline actually renders them. */
    for (uint32_t warm = 0u; warm < 3u; ++warm)
    {
        Begin(device, warm == 2u);
        (void)Record(device, &item, 1u, false, 1u);
        Require(device->endFrame(device) != 0u, "warm frame");
    }
    Capture(device);
    const uint32_t counts[4] = {1u, 32u, 256u, MAX_INSTANCES};
    for (uint32_t workload = 0u; workload < 4u; ++workload)
        for (uint32_t sample = 0u; sample < SAMPLES; ++sample)
            for (uint32_t order = 0u; order < (batchSupported ? 2u : 1u); ++order)
            {
                const bool batch = batchSupported && ((sample + order) & 1u) != 0u;
                uint32_t repetitions = recordingBudget / counts[workload];
                if (repetitions > repetitionLimit)
                    repetitions = repetitionLimit;
                uint64_t elapsedTotal = 0u;
                LaiueGraphicsDiagnosticsV2 stats = {0};
                for (uint32_t frame = 0u; frame < recordingFrames; ++frame)
                {
                    Begin(device, sample == 0u && frame == 0u);
                    elapsedTotal += Record(device, &item, counts[workload], batch, repetitions);
                    Require(device->endFrame(device) != 0u, "timed frame");
                    stats = Stats(device);
                    Require(stats.drawCalls == repetitions * (batch ? 1u : counts[workload]),
                            "draw call count");
                    if (sample == 0u && frame == 0u)
                    {
                        Capture(device);
                        if (!batch)
                            memcpy(reference, pixels, sizeof(pixels));
                        else
                            Require(memcmp(reference, pixels, sizeof(pixels)) == 0,
                                    "scalar/batch pixels");
                    }
                }
                const uint64_t elapsed = elapsedTotal / recordingFrames;
                LaiueTestRuntimeWrite(batch ? "mode=batch" : "mode=scalar");
                Field(" count=", counts[workload]);
                Field(" sample=", sample);
                Field(" repetitions=", repetitions);
                Field(" recording_frames=", recordingFrames);
                Field(" submit_ns=", elapsed);
                Field(" cpu_shadow_bytes=", stats.cpuShadowBytes);
                Field(" geometry_used_bytes=", stats.geometryPoolUsedBytes);
                Field(" geometry_capacity_bytes=", stats.geometryPoolCapacityBytes);
                LaiueTestRuntimeWrite("\n");
            }
    device->destroyHandle(device, ib);
    device->destroyHandle(device, vb);
    service->destroyDevice(device);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("verify=pixels_equal\nLAIUE_TEST_SUCCESS\n");
    LAIUE_TEST_SUCCESS();
}
