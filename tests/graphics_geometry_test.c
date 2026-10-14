/* Public V2 geometry behavior, checked from the completed GPU image. The
 * first-frame-only mode intentionally needs no new tail, so this same
 * executable can demonstrate the pre-native-index Vulkan regression. */
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

#ifndef LAIUE_GRAPHICS_TEST_D3D12
#define LAIUE_GRAPHICS_TEST_D3D12 0
#endif
#ifndef LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER
#define LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER 0
#endif

#define FRAME_WIDTH 96u
#define FRAME_HEIGHT 96u
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4u)
#define GUARD_BYTES 16u
#define INSTANCE_LIMIT (64u * 1024u * 1024u / 32u)
#define RETIREMENT_MESH_COUNT 300u
#define RETIREMENT_UPLOAD_BATCH 50u
#define PENDING_UPLOAD_LIMIT 128u

typedef struct LegacyDeviceV2
{
    uint32_t structSize, abiVersion;
    void *context;
    LaiueGraphicsV2CreateBufferFn createBuffer;
    LaiueGraphicsV2CreateTextureFn createTexture;
    LaiueGraphicsV2CreateSamplerFn createSampler;
    LaiueGraphicsV2CreatePipelineFn createPipeline;
    LaiueGraphicsV2UploadBufferFn uploadBuffer;
    LaiueGraphicsV2DestroyHandleFn destroyHandle;
    LaiueGraphicsV2BeginFrameFn beginFrame;
    LaiueGraphicsV2SubmitFn submit;
    LaiueGraphicsV2EndFrameFn endFrame;
    LaiueGraphicsV2CreateShaderFn createShader;
    LaiueGraphicsV2SubmitUiFn submitUi;
    LaiueGraphicsV2SetUiFontAtlasFn setUiFontAtlas;
    LaiueGraphicsV2SetCameraFn setCamera;
    LaiueGraphicsV2UploadTextureFn uploadTexture;
    LaiueGraphicsV2GetDiagnosticsFn getDiagnostics;
    LaiueGraphicsV2ReadbackFrameFn readbackFrame;
    LaiueGraphicsV2RequestFrameReadbackFn requestFrameReadback;
} LegacyDeviceV2;

_Static_assert(sizeof(LegacyDeviceV2) == offsetof(LaiueGraphicsDeviceV2, getCapabilities),
               "the legacy provider has physically no new callback tail");
_Static_assert(offsetof(LaiueGraphicsInstanceV2, rotation) == 16u &&
                   offsetof(LaiueGraphicsInstanceV2, scale) == 12u,
               "the public instance layout matches the GPU ring");

static uint8_t pixels[GUARD_BYTES + FRAME_BYTES + GUARD_BYTES];
static uint8_t referencePixels[FRAME_BYTES];
static LaiueGraphicsVertexV2 vertices[8];
static LaiueGraphicsInstanceV2 instances[3];
static LaiueGraphicsHandle retirementMeshes[RETIREMENT_MESH_COUNT];
static const uint32_t indicesA[12] = {0u, 1u, 2u, 0u, 2u, 3u, 4u, 5u, 6u, 4u, 6u, 7u};
static const uint32_t indicesB[12] = {4u, 5u, 6u, 4u, 6u, 7u, 0u, 1u, 2u, 0u, 2u, 3u};

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Graphics geometry check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Stage(const char *message)
{
    LaiueTestRuntimeWrite("Graphics geometry: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static void WriteHex(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[11] = "0x00000000";
    for (uint32_t i = 0u; i < 8u; ++i)
        text[2u + i] = digits[(value >> ((7u - i) * 4u)) & 15u];
    LaiueTestRuntimeWrite(text);
}

static bool Join(wchar_t *out, const wchar_t *directory, const wchar_t *name)
{
    uint32_t count = 0u;
    while (*directory != L'\0' && count + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        out[count++] = *directory++;
    if (*directory != L'\0')
        return false;
    if (count != 0u && out[count - 1u] != L'/' && out[count - 1u] != L'\\')
        out[count++] = L'/';
    while (*name != L'\0' && count + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        out[count++] = *name++;
    out[count] = L'\0';
    return *name == L'\0';
}

#if defined(_WIN32) && LAIUE_GRAPHICS_TEST_D3D12
static HWND CreateTestWindow(void)
{
    const wchar_t *name = L"LaiueGraphicsGeometryTestWindow";
    HINSTANCE module = GetModuleHandleW(NULL);
    WNDCLASSEXW windowClass = {0};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = module;
    windowClass.lpszClassName = name;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    RECT rectangle = {0, 0, (LONG)FRAME_WIDTH, (LONG)FRAME_HEIGHT};
    if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE))
        return NULL;
    return CreateWindowExW(0u, name, L"laiue geometry pixels", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, rectangle.right - rectangle.left,
                           rectangle.bottom - rectangle.top, NULL, NULL, module, NULL);
}
#endif

static LaiueGraphicsDiagnosticsV2 Diagnostics(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsDiagnosticsV2 value = {.structSize = sizeof(value)};
    Expect(device->getDiagnostics(device, &value) != 0u, "real device diagnostics are available");
    return value;
}

static void Begin(LaiueGraphicsDeviceV2 *device)
{
    if (device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_REQUEST_SIZE &&
        device->requestFrameReadback != NULL)
        Expect(device->requestFrameReadback(device) != 0u, "capture requested before recording");
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    camera.viewProjection[0] = camera.viewProjection[5] = camera.viewProjection[10] =
        camera.viewProjection[15] = 1.0f;
    Expect(device->setCamera(device, &camera) != 0u, "identity camera is accepted");
    Expect(device->beginFrame(device, FRAME_WIDTH, FRAME_HEIGHT) != 0u, "frame begins");
}

static void Capture(LaiueGraphicsDeviceV2 *device, uint64_t expectedDraws)
{
    Expect(device->endFrame(device) != 0u, "recorded frame completes");
    memset(pixels, 0xa5, sizeof(pixels));
    LaiueGraphicsDiagnosticsV2 stats = Diagnostics(device);
    Expect(stats.drawCalls == expectedDraws &&
               (stats.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) != 0u,
           "successful commands and diagnostics agree without silent drops");
    LaiueGraphicsFrameReadbackV2 request = {
        .structSize = sizeof(request),
        .expectedFrameIndex = stats.frameIndex,
        .pixels = pixels + GUARD_BYTES,
        .capacityBytes = FRAME_BYTES,
    };
    Expect(device->readbackFrame(device, &request) != 0u && request.width == FRAME_WIDTH &&
               request.height == FRAME_HEIGHT && request.rowPitchBytes == FRAME_WIDTH * 4u &&
               request.writtenBytes == FRAME_BYTES,
           "the exact completed frame is captured");
    for (uint32_t i = 0u; i < GUARD_BYTES; ++i)
        Expect(pixels[i] == 0xa5 && pixels[GUARD_BYTES + FRAME_BYTES + i] == 0xa5,
               "readback does not overwrite surrounding guards");
}

static void ColorAt(float x, float y, uint32_t rgba, const char *reason)
{
    const uint32_t px = (uint32_t)((x + 1.0f) * 0.5f * FRAME_WIDTH);
    const uint32_t py = (uint32_t)((1.0f - y) * 0.5f * FRAME_HEIGHT);
    Expect(px < FRAME_WIDTH && py < FRAME_HEIGHT, "a probe lies inside the image");
    const uint8_t *pixel = pixels + GUARD_BYTES + ((size_t)py * FRAME_WIDTH + px) * 4u;
    for (uint32_t channel = 0u; channel < 3u; ++channel)
    {
        const uint32_t expected = (rgba >> (8u * channel)) & 255u;
        const uint32_t delta =
            pixel[channel] > expected ? pixel[channel] - expected : expected - pixel[channel];
        if (delta >= 12u)
        {
            const uint32_t actual = (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8u) |
                                    ((uint32_t)pixel[2] << 16u) | ((uint32_t)pixel[3] << 24u);
            LaiueTestRuntimeWrite("Graphics geometry pixel: actual=");
            WriteHex(actual);
            LaiueTestRuntimeWrite(" expected=");
            WriteHex(rgba);
            LaiueTestRuntimeWrite("\n");
        }
        Expect(delta < 12u, reason);
    }
}

static LaiueGraphicsHandle Buffer(LaiueGraphicsDeviceV2 *device, uint32_t usage, const void *bytes,
                                  uint32_t size)
{
    LaiueGraphicsHandle handle = 0u;
    const LaiueGraphicsBufferDescV1 desc = {sizeof(desc), usage, size};
    Expect(device->createBuffer(device, &desc, &handle) != 0u && handle != 0u,
           "a buffer with explicit usage is created");
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload),
        .buffer = handle,
        .offsetBytes = 0u,
        .data = bytes,
        .sizeBytes = size,
    };
    Expect(device->uploadBuffer(device, &upload) != 0u, "geometry uploads before beginFrame");
    return handle;
}

static LaiueGraphicsDrawItemV2 Item(LaiueGraphicsHandle vb, LaiueGraphicsHandle ib, uint32_t first,
                                    int32_t offset, float x, float y, float scale)
{
    LaiueGraphicsDrawItemV2 item = {
        .structSize = sizeof(item),
        .vertexBuffer = vb,
        .indexBuffer = ib,
        .indexCount = ib != 0u ? 6u : 3u,
        .firstIndex = first,
        .vertexOffset = offset,
        .originRelative = {x, y, 0.5f},
        .scale = scale,
    };
    return item;
}

static void Submit(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsDrawItemV2 *item)
{
    Expect(device->submit(device, item, 1u) != 0u, "valid geometry is submitted");
}

static void InitializeVertices(void)
{
    static const float corners[4][2] = {{0.0f, 0.0f}, {0.25f, 0.0f}, {0.25f, 0.25f}, {0.0f, 0.25f}};
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        vertices[i].position[0] = corners[i & 3u][0];
        vertices[i].position[1] = corners[i & 3u][1];
        vertices[i].uv[0] = corners[i & 3u][0] * 4.0f;
        vertices[i].uv[1] = corners[i & 3u][1] * 4.0f;
        vertices[i].colorRGBA = i < 4u ? UINT32_C(0xff0000ff) : UINT32_C(0xff00ff00);
    }
}

static void FirstIndexedFrame(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb,
                              LaiueGraphicsHandle ib)
{
    Stage("FIRST indexed frame (no warm-up)");
    const LaiueGraphicsDrawItemV2 item = Item(vb, ib, 6u, 0, -0.125f, -0.125f, 1.0f);
    Begin(device);
    Submit(device, &item);
    Capture(device, 1u);
    /* Interior points in BOTH triangles catch a triangle-list fallback too. */
    ColorAt(-0.0625f, 0.0625f, UINT32_C(0xff00ff00),
            "first indexed frame shows the selected green range, including triangle two");
    ColorAt(0.0625f, -0.0625f, UINT32_C(0xff00ff00),
            "first indexed frame shows triangle one without a second-frame upload");
}

static void AlternatingRanges(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb,
                              LaiueGraphicsHandle ia, LaiueGraphicsHandle ib)
{
    Stage("independent index buffers, signed offsets and alternating ranges");
    const uint64_t used = Diagnostics(device).geometryPoolUsedBytes;
    for (uint32_t frame = 0u; frame < 3u; ++frame)
    {
        const bool reverse = (frame & 1u) != 0u;
        LaiueGraphicsDrawItemV2 draws[4] = {
            Item(vb, ia, reverse ? 6u : 0u, reverse ? -4 : 4, -0.875f, 0.5f, 1.0f),
            Item(vb, ia, reverse ? 0u : 6u, reverse ? 4 : -4, -0.375f, 0.5f, 1.0f),
            Item(vb, ib, reverse ? 6u : 0u, 0, 0.125f, 0.5f, 1.0f),
            Item(vb, ib, reverse ? 0u : 6u, 0, 0.625f, 0.5f, 1.0f),
        };
        Begin(device);
        Expect(device->submit(device, draws, 4u) != 0u, "multiple ranges coexist in one frame");
        Capture(device, 4u);
        ColorAt(-0.75f, 0.625f, reverse ? UINT32_C(0xff0000ff) : UINT32_C(0xff00ff00),
                "first signed index range retains its own vertex base");
        ColorAt(-0.25f, 0.625f, reverse ? UINT32_C(0xff00ff00) : UINT32_C(0xff0000ff),
                "second range does not replace the first mesh");
        ColorAt(0.25f, 0.625f, reverse ? UINT32_C(0xff0000ff) : UINT32_C(0xff00ff00),
                "another index buffer selects the expected range");
        ColorAt(0.75f, 0.625f, reverse ? UINT32_C(0xff00ff00) : UINT32_C(0xff0000ff),
                "the last range survives command ordering");
        Expect(Diagnostics(device).geometryPoolUsedBytes == used,
               "indexed submits do not allocate expanded vertex meshes");
    }
}

static void LargeRawIndices(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb)
{
    Stage("full uint32 raw indices with INT32_MIN base vertex");
    const uint32_t raw[6] = {UINT32_C(0x80000000), UINT32_C(0x80000001), UINT32_C(0x80000002),
                             UINT32_C(0x80000000), UINT32_C(0x80000002), UINT32_C(0x80000003)};
    LaiueGraphicsHandle handle = 0u;
    const LaiueGraphicsBufferDescV1 desc = {sizeof(desc), LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
                                            sizeof(raw)};
    Expect(device->createBuffer(device, &desc, &handle) != 0u,
           "large raw indices have a small typed buffer");
    const uint64_t used = Diagnostics(device).geometryPoolUsedBytes;
    (void)used; /* Only Vulkan may reject an unsupported raw index range. */
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload), .buffer = handle, .data = raw, .sizeBytes = sizeof(raw)};
    if (device->uploadBuffer(device, &upload) == 0u)
    {
        Expect(!LAIUE_GRAPHICS_TEST_D3D12 && Diagnostics(device).geometryPoolUsedBytes == used,
               "limited Vulkan drivers reject unsupported raw indices without GPU allocation");
        device->destroyHandle(device, handle);
        Stage("driver raw-index limit rejected the upload atomically");
        return;
    }
    const LaiueGraphicsDrawItemV2 draw = Item(vb, handle, 0u, INT32_MIN, -0.125f, -0.125f, 1.0f);
    Begin(device);
    Submit(device, &draw);
    Capture(device, 1u);
    ColorAt(
        -0.0625f, 0.0625f, UINT32_C(0xff0000ff),
        "large raw indices plus the signed base resolve triangle two to vertices zero to three");
    ColorAt(0.0625f, -0.0625f, UINT32_C(0xff0000ff),
            "large raw indices plus the signed base resolve triangle one without overflow");
    device->destroyHandle(device, handle);
}

static void PartialIndexUpload(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb,
                               LaiueGraphicsHandle ib)
{
    Stage("partial index upload affects its FIRST following frame");
    const LaiueGraphicsBufferUploadV1 patch = {
        .structSize = sizeof(patch),
        .offsetBytes = 6u * sizeof(uint32_t),
        .buffer = ib,
        .data = indicesA + 6u,
        .sizeBytes = 6u * sizeof(uint32_t),
    };
    Expect(device->uploadBuffer(device, &patch) != 0u, "half of an IB is updated independently");
    const LaiueGraphicsDrawItemV2 item = Item(vb, ib, 6u, 0, 0.375f, -0.125f, 1.0f);
    Begin(device);
    Submit(device, &item);
    Capture(device, 1u);
    ColorAt(0.4375f, 0.0625f, UINT32_C(0xff00ff00), "patched triangle two is immediately green");
    ColorAt(0.5625f, -0.0625f, UINT32_C(0xff00ff00), "patched triangle one is immediately green");
}

static void TexturedIndices(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle ib)
{
    Stage("indexed UV/color and texture bindings");
    LaiueGraphicsVertexV2 white[4];
    memcpy(white, vertices, sizeof(white));
    for (uint32_t i = 0u; i < 4u; ++i)
        white[i].colorRGBA = UINT32_C(0xffffffff);
    LaiueGraphicsHandle vb =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, white, sizeof(white));
    static const uint8_t texels[16] = {255u, 0u, 0u,   255u, 0u,   255u, 0u, 255u,
                                       0u,   0u, 255u, 255u, 255u, 255u, 0u, 255u};
    LaiueGraphicsTextureDescV1 textureDesc = {
        sizeof(textureDesc), LAIUE_GRAPHICS_FORMAT_RGBA8_UNORM, {2u, 2u, 1u}, 1u, 0u};
    LaiueGraphicsHandle texture = 0u, sampler = 0u;
    Expect(device->createTexture(device, &textureDesc, &texture) != 0u && texture != 0u,
           "checker texture creates");
    LaiueGraphicsTextureUploadV1 upload = {.structSize = sizeof(upload),
                                           .texture = texture,
                                           .data = texels,
                                           .sizeBytes = sizeof(texels),
                                           .rowPitchBytes = 8u};
    Expect(device->uploadTexture(device, &upload) != 0u, "checker texture uploads");
    LaiueGraphicsSamplerDescV1 samplerDesc = {
        sizeof(samplerDesc),          LAIUE_GRAPHICS_FILTER_NEAREST, LAIUE_GRAPHICS_FILTER_NEAREST,
        LAIUE_GRAPHICS_ADDRESS_CLAMP, LAIUE_GRAPHICS_ADDRESS_CLAMP,  LAIUE_GRAPHICS_ADDRESS_CLAMP};
    Expect(device->createSampler(device, &samplerDesc, &sampler) != 0u && sampler != 0u,
           "explicit nearest sampler creates");
    LaiueGraphicsDrawItemV2 item = Item(vb, ib, 0u, 0, -0.5f, -0.5f, 4.0f);
    item.texture = texture;
    item.sampler = sampler;
    Begin(device);
    Submit(device, &item);
    Capture(device, 1u);
    ColorAt(-0.25f, -0.25f, UINT32_C(0xff0000ff), "indexed bottom left retains UV (0,0)");
    ColorAt(0.25f, -0.25f, UINT32_C(0xff00ff00), "indexed bottom right retains UV (1,0)");
    ColorAt(-0.25f, 0.25f, UINT32_C(0xffff0000), "indexed top left retains UV (0,1)");
    ColorAt(0.25f, 0.25f, UINT32_C(0xff00ffff), "indexed top right retains UV (1,1)");
    device->destroyHandle(device, sampler);
    device->destroyHandle(device, texture);
    device->destroyHandle(device, vb);
}

static void FillInstances(void)
{
    memset(instances, 0, sizeof(instances));
    instances[0].originRelative[0] = -0.75f;
    instances[1].originRelative[0] = -0.125f;
    instances[2].originRelative[0] = 0.5f;
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        instances[i].originRelative[1] = -0.5f;
        instances[i].scale = 0.5f;
    }
    /* A quarter turn cannot be reproduced by merely negating the scale. */
    instances[1].rotation[2] = instances[1].rotation[3] = 0.7071067811865475f;
    instances[2].rotation[2] = 1.0f;
    instances[2].scale = -0.5f;
}

static void InstancePixels(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb,
                           LaiueGraphicsHandle ib)
{
    Stage("GPU instances match scalar pixels; quaternion, signed scale and mixed state");
    const LaiueGraphicsHandle triangle =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices + 4u, 3u * sizeof(vertices[0]));
    LaiueGraphicsVertexV2 rotatedVertices[8];
    memcpy(rotatedVertices, vertices, sizeof(rotatedVertices));
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        rotatedVertices[i].position[0] = -vertices[i].position[1];
        rotatedVertices[i].position[1] = vertices[i].position[0];
    }
    const LaiueGraphicsHandle quarterTurn = Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
                                                   rotatedVertices, sizeof(rotatedVertices));
    const LaiueGraphicsDrawItemV2 before = Item(vb, ib, 0u, 0, -0.25f, 0.75f, -1.0f);
    const LaiueGraphicsDrawItemV2 after = Item(vb, ib, 0u, 0, 0.75f, 0.75f, -1.0f);
    const LaiueGraphicsDrawItemV2 batch = Item(vb, ib, 6u, 0, 0.0625f, 0.03125f, 2.0f);
    const LaiueGraphicsDrawItemV2 plain = Item(triangle, 0u, 0u, 0, 0.0f, 0.0f, 1.0f);
    LaiueGraphicsInstanceV2 last = {.originRelative = {0.625f, -0.875f, -0.1f}, .scale = 0.5f};
    FillInstances();
    Begin(device);
    Submit(device, &before);
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        LaiueGraphicsDrawItemV2 scalar = batch;
        scalar.originRelative[0] += instances[i].originRelative[0];
        scalar.originRelative[1] += instances[i].originRelative[1];
        scalar.scale *= instances[i].scale * (i == 2u ? -1.0f : 1.0f);
        if (i == 1u)
            scalar.vertexBuffer = quarterTurn;
        Submit(device, &scalar);
    }
    LaiueGraphicsDrawItemV2 lastScalar = plain;
    lastScalar.originRelative[0] += last.originRelative[0];
    lastScalar.originRelative[1] += last.originRelative[1];
    lastScalar.originRelative[2] += last.originRelative[2];
    lastScalar.scale = last.scale;
    Submit(device, &lastScalar);
    Submit(device, &after);
    Capture(device, 6u);
    memcpy(referencePixels, pixels + GUARD_BYTES, FRAME_BYTES);
    ColorAt(-0.5625f, -0.34375f, UINT32_C(0xff00ff00), "identity instance reference is visible");
    ColorAt(-0.1875f, -0.34375f, UINT32_C(0xff00ff00), "quarter-turn reference is visible");
    ColorAt(0.6875f, -0.34375f, UINT32_C(0xff00ff00),
            "negative-scale rotated reference is visible");
    ColorAt(-0.375f, 0.625f, UINT32_C(0xff0000ff),
            "ordinary negative scale before the batch is visible");
    ColorAt(0.625f, 0.625f, UINT32_C(0xff0000ff),
            "ordinary negative scale after the batch is visible");
    ColorAt(0.71875f, -0.84375f, UINT32_C(0xff00ff00), "non-indexed instance reference is visible");
    const LaiueGraphicsDiagnosticsV2 original = Diagnostics(device);
    for (uint32_t frame = 0u; frame < 3u; ++frame)
    {
        FillInstances();
        Begin(device);
        Submit(device, &before);
        Expect(device->submitInstances(device, &batch, instances, 3u) != 0u,
               "indexed GPU instance batch submits");
        memset(instances, 0, sizeof(instances));
        Expect(device->submitInstances(device, &plain, &last, 1u) != 0u,
               "a following non-indexed batch retains its own ring offset");
        Expect(device->submitInstances(device, NULL, NULL, 0u) != 0u,
               "zero instances is an explicit no-op");
        Submit(device, &after);
        Capture(device, 4u);
        Expect(memcmp(referencePixels, pixels + GUARD_BYTES, FRAME_BYTES) == 0,
               "GPU instance image equals scalar placements after caller memory overwrite");
        const LaiueGraphicsDiagnosticsV2 current = Diagnostics(device);
        Expect(current.geometryPoolUsedBytes <= original.geometryPoolUsedBytes &&
                   current.cpuShadowBytes == original.cpuShadowBytes &&
                   current.resourceHandleCount == original.resourceHandleCount,
               "GPU instance calls retain one mesh without expanded CPU or geometry-pool copies");
    }
    device->destroyHandle(device, triangle);
    device->destroyHandle(device, quarterTurn);
}

static void Reject(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsDrawItemV2 *item,
                   const char *reason)
{
    Expect(device->submit(device, item, 1u) == 0u, reason);
    const LaiueGraphicsInstanceV2 valid = {.scale = 1.0f};
    Expect(device->submitInstances(device, item, &valid, 1u) == 0u, reason);
}

static void RejectNonfiniteFields(LaiueGraphicsDeviceV2 *device,
                                  const LaiueGraphicsDrawItemV2 *item)
{
    static const volatile uint32_t badBits[] = {
        UINT32_C(0x7f800000), UINT32_C(0xff800000), UINT32_C(0x7fc00000), UINT32_C(0xffc00000),
        UINT32_C(0x7f800001), UINT32_C(0xff800001), UINT32_C(0x7fffffff), UINT32_C(0xffffffff)};
    for (uint32_t encoding = 0u; encoding < sizeof(badBits) / sizeof(badBits[0]); ++encoding)
    {
        const uint32_t bits = badBits[encoding];
        for (uint32_t field = 0u; field < 8u; ++field)
        {
            LaiueGraphicsInstanceV2 instance = {.scale = 1.0f};
            float *destination = field < 3u    ? &instance.originRelative[field]
                                 : field == 3u ? &instance.scale
                                               : &instance.rotation[field - 4u];
            // Copy runtime bits into caller memory without forming a float argument.
            memcpy(destination, &bits, sizeof(bits));
            Expect(device->submitInstances(device, item, &instance, 1u) == 0u,
                   "both signs of infinity and NaN are rejected in every instance field");
        }
        for (uint32_t field = 0u; field < 4u; ++field)
        {
            LaiueGraphicsDrawItemV2 invalid = *item;
            LaiueGraphicsInstanceV2 instance = {.scale = 1.0f};
            float *destination = field < 3u ? &invalid.originRelative[field] : &invalid.scale;
            memcpy(destination, &bits, sizeof(bits));
            Expect(device->submit(device, &invalid, 1u) == 0u &&
                       device->submitInstances(device, &invalid, &instance, 1u) == 0u,
                   "both ordinary and instance draws reject nonfinite base transforms");
        }
    }
}

static void InvalidGeometry(LaiueGraphicsDeviceV2 *device, LaiueGraphicsDeviceV2 *other,
                            LaiueGraphicsHandle vb, LaiueGraphicsHandle ib)
{
    Stage("invalid ranges, wrong usage, foreign/stale handles and bounded instance preflight");
    const LaiueGraphicsHandle foreign =
        Buffer(other, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesA, sizeof(indicesA));
    const LaiueGraphicsHandle stale =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesA, sizeof(indicesA));
    device->destroyHandle(device, stale);
    const LaiueGraphicsHandle replacement =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesA, sizeof(indicesA));
    Expect(replacement != stale, "reused slots have a new generation");
    LaiueGraphicsDrawItemV2 item = Item(vb, ib, 0u, 0, 0.0f, 0.0f, 1.0f);
    Begin(device);
    item.firstIndex = UINT32_MAX;
    Reject(device, &item, "overflowing first index is rejected");
    item.firstIndex = 12u;
    Reject(device, &item, "range beyond the index endpoint is rejected");
    item.firstIndex = 0u;
    item.vertexOffset = -1;
    Reject(device, &item, "negative resolved vertex index is rejected");
    item.vertexOffset = INT32_MAX;
    Reject(device, &item, "large signed vertex base is rejected before GPU access");
    item.vertexOffset = INT32_MIN;
    Reject(device, &item, "minimum signed vertex base is rejected without signed overflow");
    item.vertexOffset = 0;
    item.indexCount = 5u;
    Reject(device, &item, "non-triangle index count is rejected");
    item.indexCount = 6u;
    item.indexBuffer = vb;
    Reject(device, &item, "vertex-only buffer cannot become an index buffer");
    item.indexBuffer = foreign;
    Reject(device, &item, "index resource from another device is rejected");
    item.indexBuffer = stale;
    Reject(device, &item, "destroyed index handle remains invalid after slot reuse");
    item.indexBuffer = ib;
    item.vertexBuffer = replacement;
    Reject(device, &item, "index-only buffer cannot become a vertex buffer");
    item.vertexBuffer = vb;
    LaiueGraphicsInstanceV2 valid = {.scale = 1.0f};
    const LaiueGraphicsInstanceV2 *unreadable = (const LaiueGraphicsInstanceV2 *)(uintptr_t)1u;
    Expect(device->submitInstances(device, &item, unreadable, INSTANCE_LIMIT + 1u) == 0u &&
               device->submitInstances(device, &item, unreadable, UINT32_MAX) == 0u,
           "oversized ring request fails BEFORE dereferencing inaccessible instance records");
    Expect(device->submitInstances(device, &item, NULL, 1u) == 0u,
           "nonempty instance call needs records");
    union
    {
        uint32_t bits;
        float value;
    } invalid = {UINT32_C(0x7fc00000)};
    valid.originRelative[1] = invalid.value;
    Expect(device->submitInstances(device, &item, &valid, 1u) == 0u,
           "nonfinite instance translation is rejected");
    valid.originRelative[1] = 0.0f;
    valid.rotation[3] = 2.0f;
    Expect(device->submitInstances(device, &item, &valid, 1u) == 0u,
           "nonunit quaternion is rejected instead of silently distorting geometry");
    valid.rotation[3] = 0.0f;
    valid.rotation[0] = invalid.value;
    Expect(device->submitInstances(device, &item, &valid, 1u) == 0u,
           "nonfinite quaternion is rejected before recording");
    valid.rotation[0] = 0.0f;
    valid.scale = invalid.value;
    Expect(device->submitInstances(device, &item, &valid, 1u) == 0u,
           "nonfinite instance scale is rejected before recording");
    RejectNonfiniteFields(device, &item);
    Capture(device, 0u);
    device->destroyHandle(device, replacement);
    other->destroyHandle(other, foreign);

    const uint32_t invalidIndex = UINT32_MAX;
    LaiueGraphicsBufferUploadV1 patch = {.structSize = sizeof(patch),
                                         .buffer = ib,
                                         .data = &invalidIndex,
                                         .sizeBytes = sizeof(invalidIndex)};
    Expect(device->uploadBuffer(device, &patch) != 0u,
           "index bytes may upload before range validation");
    Begin(device);
    Reject(device, &item, "out-of-range uint32 element is rejected against the selected VB");
    Capture(device, 0u);
    patch.data = indicesA;
    Expect(device->uploadBuffer(device, &patch) != 0u,
           "rejected geometry does not poison later uploads");
}

static void LegacyTables(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle vb,
                         LaiueGraphicsHandle ib)
{
    Stage("physically short tables and legacy ordinary submission");
    struct
    {
        uint32_t structSize, abiVersion;
    } shortTable = {8u, 2u};
    _Static_assert(sizeof(shortTable) == 8u, "the short provider contains only its header");
    LaiueGraphicsDeviceV2 *shortDevice = (LaiueGraphicsDeviceV2 *)&shortTable;
#if defined(_WIN32)
    /* End the actual eight-byte object at an inaccessible page. Reading its
     * nonexistent context or callback tail now fails even in Release. */
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    const SIZE_T pageBytes = systemInfo.dwPageSize;
    uint8_t *guarded = VirtualAlloc(NULL, pageBytes * 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Expect(guarded != NULL && pageBytes >= sizeof(shortTable), "ABI guard pages allocate");
    DWORD oldProtection = 0u;
    Expect(VirtualProtect(guarded + pageBytes, pageBytes, PAGE_NOACCESS, &oldProtection) != 0,
           "the memory beyond the short provider is inaccessible");
    shortDevice = (LaiueGraphicsDeviceV2 *)(guarded + pageBytes - sizeof(shortTable));
    memcpy(shortDevice, &shortTable, sizeof(shortTable));
#endif
    const LaiueGraphicsDrawItemV2 item = Item(vb, ib, 0u, 0, -0.125f, -0.125f, 1.0f);
    const LaiueGraphicsInstanceV2 instance = {.scale = 1.0f};
    const LaiueGraphicsV2GetCapabilitiesFn capabilities = device->getCapabilities;
    const LaiueGraphicsV2SubmitInstancesFn submitInstances = device->submitInstances;
    Expect(capabilities(shortDevice) == 0u &&
               submitInstances(shortDevice, &item, &instance, 1u) == 0u &&
               submitInstances(shortDevice, NULL, NULL, 0u) == 0u,
           "callbacks reject an actual eight-byte table without reading context");
#if defined(_WIN32)
    Expect(VirtualFree(guarded, 0u, MEM_RELEASE) != 0, "ABI guard pages release");
#endif
    LegacyDeviceV2 old;
    memcpy(&old, device, sizeof(old));
    old.structSize = sizeof(old);
    /* This full old prefix has context, but a missing tail must be detected
     * before attempting to use it. The ordinary callback below gets the real
     * context restored and exercises this same physically old object. */
    old.context = (void *)(uintptr_t)1u;
    Expect(capabilities((const LaiueGraphicsDeviceV2 *)&old) == 0u &&
               submitInstances((LaiueGraphicsDeviceV2 *)&old, &item, &instance, 1u) == 0u &&
               submitInstances((LaiueGraphicsDeviceV2 *)&old, NULL, NULL, 0u) == 0u,
           "missing tails return unsupported before touching the invalid context");
    old.context = device->context;
    Begin(device);
    Expect(old.submit((LaiueGraphicsDeviceV2 *)&old, &item, 1u) != 0u,
           "a physically old provider prefix still supports its ordinary draw");
    Capture(device, 1u);
    ColorAt(0.0f, 0.0f, UINT32_C(0xff0000ff), "legacy prefix draws actual pixels");
}

static void DestroyRecordedResources(LaiueGraphicsDeviceV2 *device)
{
    Stage("resource retirement preserves already recorded indexed draws");
    const LaiueGraphicsHandle vb =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices + 4u, 4u * sizeof(vertices[0]));
    const LaiueGraphicsHandle ib =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesA, 6u * sizeof(indicesA[0]));
    const LaiueGraphicsDrawItemV2 item = Item(vb, ib, 0u, 0, -0.125f, -0.125f, 1.0f);
    Begin(device);
    Submit(device, &item);
    device->destroyHandle(device, ib);
    device->destroyHandle(device, vb);
    Reject(device, &item, "destroyed resources cannot be submitted again in the same frame");
    Capture(device, 1u);
    ColorAt(0.0f, 0.0f, UINT32_C(0xff00ff00),
            "retired GPU ranges survive until recorded work completes");
}

static void OverflowRecordedRetirement(LaiueGraphicsDeviceV2 *device)
{
    Stage("more than 256 retired meshes preserve recorded work and reclaim every range");
    /* Complete both frame slots so earlier tests cannot contribute retired
     * ranges to either the baseline or the queue-overflow assertion. */
    for (uint32_t frame = 0u; frame < 2u; ++frame)
    {
        Begin(device);
        Capture(device, 0u);
    }
    const LaiueGraphicsDiagnosticsV2 baseline = Diagnostics(device);
    const uint32_t meshBytes = 3u * sizeof(vertices[0]);
    for (uint32_t first = 0u; first < RETIREMENT_MESH_COUNT; first += RETIREMENT_UPLOAD_BATCH)
    {
        for (uint32_t i = first; i < first + RETIREMENT_UPLOAD_BATCH; ++i)
            retirementMeshes[i] =
                Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices + 4u, meshBytes);
        /* Each upload batch stays below the bounded upload budget. No
         * resource is pending when the retirement frame starts recording. */
        Begin(device);
        Capture(device, 0u);
    }
    const LaiueGraphicsDiagnosticsV2 live = Diagnostics(device);
    Expect(live.geometryPoolUsedBytes >=
                   baseline.geometryPoolUsedBytes + (uint64_t)RETIREMENT_MESH_COUNT * meshBytes &&
               live.cpuShadowBytes ==
                   baseline.cpuShadowBytes + (uint64_t)RETIREMENT_MESH_COUNT * meshBytes &&
               live.resourceHandleCount == baseline.resourceHandleCount + RETIREMENT_MESH_COUNT,
           "all 300 independent meshes are resident before retirement");
    const LaiueGraphicsDrawItemV2 first =
        Item(retirementMeshes[0], 0u, 0u, 0, -0.625f, -0.125f, 1.0f);
    const LaiueGraphicsDrawItemV2 last =
        Item(retirementMeshes[RETIREMENT_MESH_COUNT - 1u], 0u, 0u, 0, 0.375f, -0.125f, 1.0f);
    Begin(device);
    Submit(device, &first);
    Submit(device, &last);
    for (uint32_t i = 0u; i < RETIREMENT_MESH_COUNT; ++i)
    {
        device->destroyHandle(device, retirementMeshes[i]);
        retirementMeshes[i] = 0u;
    }
    Capture(device, 2u);
    const LaiueGraphicsDiagnosticsV2 retired = Diagnostics(device);
    /* Readback waits for the submitted work but does not drain retirement.
     * The old full-queue path freed ranges while this frame was unsent, so
     * this completed-frame counter fails regardless of GPU scheduling or
     * whether the freed bytes happened to retain the right vertex values. */
    Expect(retired.geometryPoolUsedBytes == live.geometryPoolUsedBytes,
           "queue overflow cannot release geometry during command recording");
    Expect(retired.cpuShadowBytes == baseline.cpuShadowBytes &&
               retired.resourceHandleCount == baseline.resourceHandleCount,
           "retirement releases every public handle and CPU shadow immediately");
    ColorAt(-0.4375f, -0.0625f, UINT32_C(0xff00ff00),
            "the first retired mesh survives overflow of the fixed queue");
    ColorAt(0.5625f, -0.0625f, UINT32_C(0xff00ff00),
            "the last retired mesh survives the overflow list");
    for (uint32_t frame = 0u; frame < 2u; ++frame)
    {
        Begin(device);
        Capture(device, 0u);
    }
    Expect(Diagnostics(device).geometryPoolUsedBytes == baseline.geometryPoolUsedBytes,
           "completed frame slots reclaim both the fixed queue and every overflow node");
}

static void PendingUploadExhaustion(LaiueGraphicsDeviceV2 *device)
{
    Stage("128 prepared uploads render immediately; rejected upload 129 preserves CPU indices");
    const LaiueGraphicsDiagnosticsV2 baseline = Diagnostics(device);
    const uint32_t meshBytes = 3u * sizeof(vertices[0]);
    /* The unused range exceeds the VB so the selected triangle must use
     * CPU subrange bounds rather than the whole-buffer bounds shortcut. */
    static const uint32_t validIndices[6] = {0u, 1u, 2u, 0u, 1u, 3u};
    const LaiueGraphicsHandle vb =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices + 4u, meshBytes);
    const LaiueGraphicsHandle ib =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, validIndices, sizeof(validIndices));
    Begin(device);
    Capture(device, 0u);
    for (uint32_t i = 0u; i < PENDING_UPLOAD_LIMIT; ++i)
        retirementMeshes[i] =
            Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices + 4u, meshBytes);
    const uint64_t used = Diagnostics(device).geometryPoolUsedBytes;
    const uint32_t invalidIndex = UINT32_MAX;
    LaiueGraphicsBufferUploadV1 replacement = {.structSize = sizeof(replacement),
                                               .buffer = ib,
                                               .data = &invalidIndex,
                                               .sizeBytes = sizeof(invalidIndex)};
    Expect(device->uploadBuffer(device, &replacement) == 0u &&
               Diagnostics(device).geometryPoolUsedBytes == used,
           "upload 129 rejects before publishing a replacement or consuming a GPU range");
    LaiueGraphicsDrawItemV2 indexed = Item(vb, ib, 0u, 0, -0.125f, -0.125f, 1.0f);
    indexed.indexCount = 3u;
    const LaiueGraphicsDrawItemV2 last =
        Item(retirementMeshes[PENDING_UPLOAD_LIMIT - 1u], 0u, 0u, 0, 0.375f, -0.125f, 1.0f);
    Begin(device);
    /* This first-ever subrange lookup reads the CPU index shadow rather
     * than the cached whole-buffer bounds. Publishing the rejected poison
     * index would reject this draw even if the original GPU IB survived. */
    Submit(device, &indexed);
    Submit(device, &last);
    Capture(device, 2u);
    Expect(Diagnostics(device).uploadedBytes == (uint64_t)PENDING_UPLOAD_LIMIT * meshBytes,
           "exactly 128 prepared native meshes upload in their first frame");
    ColorAt(0.0625f, -0.0625f, UINT32_C(0xff00ff00),
            "a rejected replacement preserves both the CPU shadow and resident GPU indices");
    ColorAt(0.5625f, -0.0625f, UINT32_C(0xff00ff00),
            "prepared mesh 128 renders without a second-frame retry");
    const uint32_t validIndex = 0u;
    replacement.data = &validIndex;
    Expect(device->uploadBuffer(device, &replacement) != 0u,
           "a new upload succeeds once the previous frame drains the pending queue");
    Begin(device);
    Submit(device, &indexed);
    Capture(device, 1u);
    Expect(Diagnostics(device).uploadedBytes == sizeof(validIndices),
           "queue recovery stages only the successful replacement");
    ColorAt(0.0625f, -0.0625f, UINT32_C(0xff00ff00),
            "a recovered partial replacement preserves the remaining indices");
    for (uint32_t i = 0u; i < PENDING_UPLOAD_LIMIT; ++i)
    {
        device->destroyHandle(device, retirementMeshes[i]);
        retirementMeshes[i] = 0u;
    }
    device->destroyHandle(device, ib);
    device->destroyHandle(device, vb);
    /* These resources retire after EndFrame, so Vulkan needs two later
     * submissions before the third BeginFrame drains their ranges. */
    for (uint32_t frame = 0u; frame < 3u; ++frame)
    {
        Begin(device);
        Capture(device, 0u);
    }
    const LaiueGraphicsDiagnosticsV2 reclaimed = Diagnostics(device);
    Expect(reclaimed.geometryPoolUsedBytes == baseline.geometryPoolUsedBytes &&
               reclaimed.cpuShadowBytes == baseline.cpuShadowBytes &&
               reclaimed.resourceHandleCount == baseline.resourceHandleCount,
           "upload exhaustion and recovery leave no resource or pool range behind");
}

LAIUE_TEST_ENTRY(GraphicsGeometryTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];
    char firstOnly[8] = {0};
    const bool baseline = PlatformGetEnvironmentUtf8("LAIUE_GRAPHICS_FIRST_FRAME_ONLY", firstOnly,
                                                     sizeof(firstOnly)) != 0u &&
                          firstOnly[0] == '1';
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) &&
               Join(modulePath, directory, RENDER_MODULE_NAME),
           "renderer module path resolves");
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueGraphicsDeviceServiceV2 *service = LaiueModuleHostQueryService(
        host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE, NULL, NULL);
    Expect(service != NULL && service->createDeviceWithContext != NULL,
           "graphics V2 service exists");
    void *window = NULL, *otherWindow = NULL;
#if defined(_WIN32) && LAIUE_GRAPHICS_TEST_D3D12
    window = CreateTestWindow();
    Expect(window != NULL, "native D3D12 capture has a real hidden HWND");
#endif
    const uint32_t backend =
        LAIUE_GRAPHICS_TEST_D3D12 ? LAIUE_GRAPHICS_BACKEND_D3D12 : LAIUE_GRAPHICS_BACKEND_VULKAN;
    LaiueGraphicsDeviceV2 *device = NULL;
    if (!service->createDeviceWithContext(service->context, window, FRAME_WIDTH, FRAME_HEIGHT,
                                          backend, &device))
    {
        Expect(device == NULL, "driver failure clears the device output");
        LaiueModuleHostDestroy(host);
#if defined(_WIN32) && LAIUE_GRAPHICS_TEST_D3D12
        DestroyWindow(window);
#endif
        Stage(LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER ? "FAIL: required native driver unavailable"
                                                 : "SKIP: requested native driver unavailable");
        LaiueTestRuntimeExit(LAIUE_GRAPHICS_TEST_REQUIRE_DRIVER ? 1 : 125);
    }
    Expect(device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_SIZE &&
               device->readbackFrame != NULL && device->getDiagnostics != NULL &&
               device->setCamera != NULL &&
               (Diagnostics(device).flags & LAIUE_GRAPHICS_DIAGNOSTICS_READBACK_SUPPORTED) != 0u,
           "test backend supports real explicit capture");
    InitializeVertices();
    const LaiueGraphicsHandle vb =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, vertices, sizeof(vertices));
    const LaiueGraphicsHandle ia =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesA, sizeof(indicesA));
    const LaiueGraphicsHandle ib =
        Buffer(device, LAIUE_GRAPHICS_BUFFER_USAGE_INDEX, indicesB, sizeof(indicesB));
    FirstIndexedFrame(device, vb, ia);
    if (!baseline)
    {
        Expect(device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_INSTANCES_SIZE &&
                   device->getCapabilities != NULL && device->submitInstances != NULL &&
                   (device->getCapabilities(device) &
                    (LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES)) ==
                       (LAIUE_GRAPHICS_CAP_NATIVE_INDICES | LAIUE_GRAPHICS_CAP_GENERIC_INSTANCES),
               "provider advertises implemented native indices and generic GPU instances");
        const LaiueGraphicsDiagnosticsV2 firstStats = Diagnostics(device);
        const uint64_t nativeBytes = sizeof(vertices) + sizeof(indicesA) + sizeof(indicesB);
        /* Vulkan allocates each resource at storage-buffer alignment (up to
         * 256 bytes); D3D12 accounts the requested bytes directly. */
        Expect(firstStats.cpuShadowBytes == nativeBytes &&
                   firstStats.uploadedBytes == nativeBytes &&
                   firstStats.resourceHandleCount == 3u &&
                   firstStats.geometryPoolUsedBytes >= nativeBytes &&
                   firstStats.geometryPoolUsedBytes <= 3u * 256u,
               "first indexed frame uploads only its original VB and two native IB resources");
        AlternatingRanges(device, vb, ia, ib);
        LargeRawIndices(device, vb);
        PartialIndexUpload(device, vb, ib);
        TexturedIndices(device, ia);
        InstancePixels(device, vb, ia);
#if defined(_WIN32) && LAIUE_GRAPHICS_TEST_D3D12
        otherWindow = CreateTestWindow();
        Expect(otherWindow != NULL, "second device has its own HWND");
#endif
        LaiueGraphicsDeviceV2 *other = NULL;
        Expect(service->createDeviceWithContext(service->context, otherWindow, FRAME_WIDTH,
                                                FRAME_HEIGHT, backend, &other) != 0u,
               "independent device creates");
        InvalidGeometry(device, other, vb, ia);
        service->destroyDevice(other);
        LegacyTables(device, vb, ia);
        DestroyRecordedResources(device);
        OverflowRecordedRetirement(device);
        PendingUploadExhaustion(device);
    }
    device->destroyHandle(device, ib);
    device->destroyHandle(device, ia);
    device->destroyHandle(device, vb);
    Expect(Diagnostics(device).cpuShadowBytes == 0u &&
               Diagnostics(device).resourceHandleCount == 0u,
           "every owned test resource is released");
    service->destroyDevice(device);
    LaiueModuleHostDestroy(host);
#if defined(_WIN32) && LAIUE_GRAPHICS_TEST_D3D12
    if (otherWindow != NULL)
        DestroyWindow(otherWindow);
    DestroyWindow(window);
#else
    (void)otherWindow;
#endif
    Stage(baseline ? "FIRST_FRAME_PASS" : "PASS");
    LAIUE_TEST_SUCCESS();
}
