#include "graphics/graphics_device_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define RENDER_MODULE_NAME L"laiue_render.dll"
#elif defined(__APPLE__)
#define RENDER_MODULE_NAME L"liblaiue_render.dylib"
#else
#define RENDER_MODULE_NAME L"liblaiue_render.so"
#endif

#define FRAME_WIDTH 64u
#define FRAME_HEIGHT 64u
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4u)
#define GUARD_BYTES 16u

_Static_assert(offsetof(LaiueGraphicsDeviceV2, getDiagnostics) ==
                   offsetof(LaiueGraphicsDeviceV2, uploadTexture) +
                       sizeof(LaiueGraphicsV2UploadTextureFn),
               "diagnostics must append to the old V2 device layout");

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Graphics diagnostics check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t *output, const wchar_t *directory, const wchar_t *name)
{
    uint32_t length = 0u;
    while (*directory != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[length++] = *directory++;
    if (*directory != L'\0')
        return false;
    if (length != 0u && output[length - 1u] != L'/' && output[length - 1u] != L'\\')
        output[length++] = L'/';
    while (*name != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[length++] = *name++;
    if (*name != L'\0')
        return false;
    output[length] = L'\0';
    return true;
}

static void SetIdentity(float matrix[16])
{
    memset(matrix, 0, 16u * sizeof(*matrix));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void CheckGuards(const uint8_t *storage)
{
    for (uint32_t index = 0u; index < GUARD_BYTES; ++index)
        Expect(storage[index] == 0xA5u && storage[GUARD_BYTES + FRAME_BYTES + index] == 0xA5u,
               "capture respects caller capacity and surrounding guard bytes");
}

static void CheckUntouched(const uint8_t *storage)
{
    for (uint32_t index = 0u; index < FRAME_BYTES + 2u * GUARD_BYTES; ++index)
        Expect(storage[index] == 0xA5u, "rejected capture never writes pixels");
}

LAIUE_TEST_ENTRY(GraphicsDiagnosticsTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];
    static uint8_t pixels[FRAME_BYTES + 2u * GUARD_BYTES];
    static uint8_t repeatedPixels[FRAME_BYTES + 2u * GUARD_BYTES];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) &&
               Join(modulePath, directory, RENDER_MODULE_NAME),
           "renderer module path is available");
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic moduleDiagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &moduleDiagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &moduleDiagnostic) == LAIUE_MODULE_OK,
           moduleDiagnostic.message);
    const LaiueGraphicsDeviceServiceV2 *service =
        (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE, NULL, NULL);
    Expect(service != NULL && service->createDeviceWithContext != NULL,
           "V2 device service is available without content provider");
    LaiueGraphicsDeviceV2 *device = NULL;
    if (service->createDeviceWithContext(service->context, NULL, FRAME_WIDTH, FRAME_HEIGHT,
                                         LAIUE_GRAPHICS_BACKEND_VULKAN, &device) == 0u)
    {
        Expect(device == NULL, "unavailable Vulkan returns a null device");
        LaiueModuleHostDestroy(host);
        LaiueTestRuntimeWrite("Graphics diagnostics require an offscreen Vulkan driver\n");
        LaiueTestRuntimeExit(125);
    }
    Expect(device != NULL && device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_SIZE &&
               device->getDiagnostics != NULL && device->readbackFrame != NULL,
           "device publishes the optional diagnostics tail");
    LaiueGraphicsDiagnosticsV2 diagnostics = {.structSize = sizeof(diagnostics)};
    Expect(device->getDiagnostics(device, &diagnostics) != 0u &&
               (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) == 0u &&
               (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_READBACK_SUPPORTED) != 0u &&
               diagnostics.resourceHandleCount == 0u && diagnostics.cpuShadowBytes == 0u,
           "empty offscreen device reports readback capability and no valid frame");
    diagnostics.structSize = sizeof(diagnostics) - 1u;
    diagnostics.drawCalls = UINT64_C(0xabcdef);
    Expect(device->getDiagnostics(device, &diagnostics) == 0u &&
               diagnostics.drawCalls == UINT64_C(0xabcdef),
           "undersized diagnostics output is rejected without a write");
    diagnostics.structSize = sizeof(diagnostics);
    Expect(device->getDiagnostics(device, NULL) == 0u &&
               device->getDiagnostics(NULL, &diagnostics) == 0u && diagnostics.flags == 0u,
           "null device/output is rejected and a full output is cleared");
    const uint32_t deviceSize = device->structSize;
    device->structSize = (uint32_t)offsetof(LaiueGraphicsDeviceV2, context);
    Expect(device->getDiagnostics(device, &diagnostics) == 0u,
           "device without a complete context prefix is rejected");
    device->structSize = deviceSize;
    const uint32_t deviceVersion = device->abiVersion;
    device->abiVersion = UINT32_MAX;
    Expect(device->getDiagnostics(device, &diagnostics) == 0u,
           "incompatible device ABI cannot enter diagnostics");
    device->abiVersion = deviceVersion;

    memset(pixels, 0xA5, sizeof(pixels));
    LaiueGraphicsFrameReadbackV2 capture = {
        .structSize = sizeof(capture),
        .pixels = pixels + GUARD_BYTES,
        .capacityBytes = FRAME_BYTES,
    };
    capture.structSize = sizeof(capture) - 1u;
    capture.frameIndex = UINT64_C(0xabcdef);
    Expect(device->readbackFrame(device, &capture) == 0u &&
               capture.frameIndex == UINT64_C(0xabcdef),
           "undersized readback descriptor is rejected without output writes");
    capture.structSize = sizeof(capture);
    Expect(device->readbackFrame(device, &capture) == 0u && capture.writtenBytes == 0u,
           "capture before a successful frame is rejected");
    CheckUntouched(pixels);

    static const LaiueGraphicsVertexV2 vertices[3] = {
        {{-0.8f, -0.8f, 0.5f}, {0.0f, 0.0f}, UINT32_C(0xffffffff)},
        {{0.8f, -0.8f, 0.5f}, {1.0f, 0.0f}, UINT32_C(0xffffffff)},
        {{0.0f, 0.8f, 0.5f}, {0.5f, 1.0f}, UINT32_C(0xffffffff)},
    };
    const LaiueGraphicsBufferDescV1 bufferDescription = {
        .structSize = sizeof(bufferDescription),
        .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
        .sizeBytes = sizeof(vertices),
    };
    LaiueGraphicsHandle buffer = 0u;
    Expect(device->createBuffer(device, &bufferDescription, &buffer) != 0u,
           "CPU shadow buffer creates");
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload),
        .buffer = buffer,
        .data = vertices,
        .sizeBytes = sizeof(vertices),
    };
    Expect(device->uploadBuffer(device, &upload) != 0u, "triangle uploads");
    Expect(device->getDiagnostics(device, &diagnostics) != 0u &&
               diagnostics.resourceHandleCount == 1u &&
               diagnostics.cpuShadowBytes == sizeof(vertices),
           "resource counters report exact live CPU shadow bytes");
    const uint32_t shaderCode = UINT32_C(0x07230203);
    const LaiueGraphicsShaderDescV1 shaderDescription = {
        .structSize = sizeof(shaderDescription),
        .stage = LAIUE_GRAPHICS_SHADER_STAGE_VERTEX,
        .code = &shaderCode,
        .codeSizeBytes = sizeof(shaderCode),
    };
    LaiueGraphicsHandle shader = 0u;
    Expect(device->createShader(device, &shaderDescription, &shader) != 0u,
           "shader shadow creates");
    Expect(device->getDiagnostics(device, &diagnostics) != 0u &&
               diagnostics.resourceHandleCount == 2u &&
               diagnostics.cpuShadowBytes == sizeof(vertices) + sizeof(shaderCode),
           "shader copy contributes its actual requested bytes");
    device->destroyHandle(device, shader);

    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    SetIdentity(camera.viewProjection);
    Expect(device->setCamera(device, &camera) != 0u &&
               device->beginFrame(device, FRAME_WIDTH, FRAME_HEIGHT) != 0u,
           "camera and frame begin succeed");
    Expect(device->readbackFrame(device, &capture) == 0u, "capture during recording is rejected");
    CheckUntouched(pixels);
    LaiueGraphicsDrawItemV2 item = {
        .structSize = sizeof(item),
        .vertexBuffer = buffer,
        .scale = 1.0f,
    };
    Expect(device->submit(device, &item, 1u) != 0u && device->endFrame(device) != 0u,
           "triangle frame submits successfully");
    Expect(device->getDiagnostics(device, &diagnostics) != 0u &&
               (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) != 0u &&
               diagnostics.frameIndex == 1u && diagnostics.drawCalls == 1u &&
               diagnostics.geometryPoolUsedBytes <= diagnostics.geometryPoolCapacityBytes,
           "completed frame carries real renderer statistics and one-based index");
    if ((diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_VALID) != 0u)
        Expect((diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_SUPPORTED) != 0u,
               "valid GPU timing always has explicit backend support");

    capture.flags = 1u;
    Expect(device->readbackFrame(device, &capture) == 0u, "unknown flags reject capture");
    capture.flags = 0u;
    capture.reserved = 1u;
    Expect(device->readbackFrame(device, &capture) == 0u, "reserved field must be zero");
    capture.reserved = 0u;
    capture.pixels = NULL;
    Expect(device->readbackFrame(device, &capture) == 0u, "null pixel buffer is rejected");
    capture.pixels = pixels + GUARD_BYTES;
    Expect(device->readbackFrame(device, NULL) == 0u && device->readbackFrame(NULL, &capture) == 0u,
           "null device and readback descriptor are rejected");
    capture.expectedFrameIndex = 2u;
    Expect(device->readbackFrame(device, &capture) == 0u, "stale frame selection rejects capture");
    capture.expectedFrameIndex = 1u;
    capture.capacityBytes = FRAME_BYTES - 1u;
    Expect(device->readbackFrame(device, &capture) == 0u && capture.width == FRAME_WIDTH &&
               capture.height == FRAME_HEIGHT && capture.rowPitchBytes == FRAME_WIDTH * 4u &&
               capture.writtenBytes == 0u && capture.frameIndex == 0u,
           "short destination reports dimensions without touching pixels");
    capture.capacityBytes = UINT64_MAX;
    Expect(device->readbackFrame(device, &capture) == 0u,
           "overflowing destination span is rejected");
    capture.capacityBytes = FRAME_BYTES;
    capture.pixels = (uint8_t *)&capture;
    Expect(device->readbackFrame(device, &capture) == 0u,
           "destination cannot overwrite its descriptor");
    capture.pixels = pixels + GUARD_BYTES;
    CheckUntouched(pixels);
    Expect(device->readbackFrame(device, &capture) != 0u && capture.frameIndex == 1u &&
               capture.writtenBytes == FRAME_BYTES && capture.width == FRAME_WIDTH &&
               capture.height == FRAME_HEIGHT && capture.rowPitchBytes == FRAME_WIDTH * 4u,
           "valid frame copies tightly packed RGBA8 into caller storage");
    CheckGuards(pixels);
    Expect(device->getDiagnostics(device, &diagnostics) != 0u,
           "explicit capture leaves diagnostics usable");
    if ((diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_SUPPORTED) != 0u)
        Expect((diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_VALID) != 0u,
               "supported GPU timing becomes valid after explicit completion");
    const uint8_t *corner = pixels + GUARD_BYTES;
    uint32_t differing = 0u;
    for (uint32_t index = 0u; index < FRAME_WIDTH * FRAME_HEIGHT; ++index)
    {
        const uint8_t *pixel = pixels + GUARD_BYTES + index * 4u;
        if (pixel[0] != corner[0] || pixel[1] != corner[1] || pixel[2] != corner[2])
            ++differing;
    }
    Expect(differing > 100u, "captured triangle differs visibly from the sky");
    memset(repeatedPixels, 0xA5, sizeof(repeatedPixels));
    capture.pixels = repeatedPixels + GUARD_BYTES;
    Expect(device->readbackFrame(device, &capture) != 0u &&
               memcmp(pixels + GUARD_BYTES, repeatedPixels + GUARD_BYTES, FRAME_BYTES) == 0,
           "repeated explicit readback preserves the same completed image");
    CheckGuards(repeatedPixels);

    Expect(device->beginFrame(device, FRAME_WIDTH, FRAME_HEIGHT) != 0u &&
               device->getDiagnostics(device, &diagnostics) != 0u &&
               (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) == 0u &&
               device->readbackFrame(device, &capture) == 0u && device->endFrame(device) != 0u,
           "next recording invalidates capture until its successful end");
    Expect(device->readbackFrame(device, &capture) == 0u,
           "previous frame identifier cannot silently capture a newer frame");
    capture.expectedFrameIndex = 0u;
    Expect(device->readbackFrame(device, &capture) != 0u && capture.frameIndex == 2u,
           "zero selector chooses the newest completed frame");
    service->resize(device, FRAME_WIDTH / 2u, FRAME_HEIGHT / 2u);
    Expect(device->readbackFrame(device, &capture) == 0u &&
               device->getDiagnostics(device, &diagnostics) != 0u &&
               (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) == 0u,
           "resize invalidates the previous image before backend resource changes");
    Expect(device->beginFrame(device, FRAME_WIDTH / 2u, FRAME_HEIGHT / 2u) != 0u &&
               device->endFrame(device) != 0u && device->readbackFrame(device, &capture) != 0u &&
               capture.width == FRAME_WIDTH / 2u && capture.height == FRAME_HEIGHT / 2u &&
               capture.writtenBytes == FRAME_BYTES / 4u && capture.frameIndex == 3u,
           "capture after resize reports actual target dimensions");
    device->destroyHandle(device, buffer);
    Expect(device->getDiagnostics(device, &diagnostics) != 0u &&
               diagnostics.resourceHandleCount == 0u && diagnostics.cpuShadowBytes == 0u,
           "released resources disappear from diagnostic accounting");
    service->destroyDevice(device);
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
