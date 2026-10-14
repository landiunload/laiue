#include "animation/animation_service.h"
#include "content/content_service.h"
#include "graphics/graphics_device_service.h"
#include "model/model_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#if defined(LAIUE_ANIMATION_DEMO_GRAPHICS)
#include "render/graphics_service.h"
#endif
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
#include "platform/window_service.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum
{
    WIDTH = 960,
    HEIGHT = 540,
    MODULES = 5,
    FRAME_BYTES = WIDTH * HEIGHT * 4
};

typedef struct Demo
{
    const LaiueModelServiceV1 *modelService;
    const LaiueAnimationServiceV1 *animation;
    const LaiueContentServiceV1 *content;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    LaiueContentCatalog *catalog;
    LaiueModelV1 *model;
    LaiueAnimationV1 *clip;
    LaiueModelViewV1 mesh;
    LaiueGraphicsDeviceV2 *device;
    uint32_t animationBytes, modelBytes, graphicsBytes, joints, scratchBytes;
    uint32_t animated, poseValid, failed, frames, frameLimit, capture, requireFull, skipped;
    float lastTime;
    double startTime;
    void *allocation;
    LaiueAnimationSkinVertexV1 *source;
    LaiueAnimationSkinnedVertexV1 *deformed;
    LaiueGraphicsVertexV2 *vertices;
    LaiueAnimationTransformV1 *locals;
    LaiueAnimationMatrixV1 *globals, *palette;
    void *scratch;
    LaiueGraphicsHandle vertexBuffer, indexBuffer, texture, sampler;
    uint8_t *pixels, *reference;
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    const LaiueWindowServiceV1 *windowService;
    Window *window;
#endif
} Demo;

static uint32_t Equal(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

static uint32_t Tail(uint32_t actual, uint32_t declared, size_t offset, size_t bytes)
{
    return offset <= actual && bytes <= actual - offset && offset <= declared &&
           bytes <= declared - offset;
}

static uint32_t Join(wchar_t *out, const wchar_t *directory, const wchar_t *name)
{
    uint32_t count = 0u;
    while (*directory != L'\0' && count + 2u < LAIUE_PLATFORM_PATH_CAPACITY)
        out[count++] = *directory++;
    if (*directory != L'\0')
        return 0u;
    if (count != 0u && out[count - 1u] != L'/' && out[count - 1u] != L'\\')
        out[count++] = L'/';
    while (*name != L'\0' && count + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        out[count++] = *name++;
    out[count] = L'\0';
    return *name == L'\0';
}

static uint32_t LoadModules(LaiueModuleHost *host, uint32_t graphical, uint32_t offscreen)
{
    LaiueModuleDiagnostic diagnostic = {0};
#if defined(LAIUE_ANIMATION_DEMO_DYNAMIC)
#if defined(_WIN32)
#define BINARY(name) L"laiue_" name L".dll"
#elif defined(__APPLE__)
#define BINARY(name) L"liblaiue_" name L".dylib"
#else
#define BINARY(name) L"liblaiue_" name L".so"
#endif
    const wchar_t *names[MODULES] = {BINARY(L"content"), BINARY(L"model"), BINARY(L"animation")};
    uint32_t count = 3u;
    const char *provider = NULL;
#if defined(LAIUE_ANIMATION_DEMO_GRAPHICS)
    if (graphical)
    {
#if defined(LAIUE_ANIMATION_DEMO_D3D12)
        names[count++] = offscreen ? BINARY(L"graphics_vulkan") : BINARY(L"graphics_d3d12");
        provider = offscreen ? "laiue.graphics.vulkan" : "laiue.graphics.d3d12";
#else
        names[count++] = BINARY(L"graphics_vulkan");
        provider = "laiue.graphics.vulkan";
#endif
    }
#endif
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (graphical && !offscreen)
        names[count++] = BINARY(L"window");
#endif
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t paths[MODULES][LAIUE_PLATFORM_PATH_CAPACITY];
    LaiueModuleBinaryV1 binaries[MODULES] = {0};
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY))
        return 0u;
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (!Join(paths[i], directory, names[i]))
            return 0u;
        binaries[i].path = paths[i];
        binaries[i].flags = LAIUE_MODULE_BINARY_OPTIONAL;
    }
    const LaiueModuleProviderSelectionV1 selection = {.structSize = sizeof(selection),
                                                      .serviceName =
                                                          LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
                                                      .moduleId = provider};
    const LaiueModuleProfileV1 profile = {.structSize = sizeof(profile),
                                          .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
                                          .binaries = binaries,
                                          .binaryCount = count,
                                          .providerSelections =
                                              provider != NULL ? &selection : NULL,
                                          .providerSelectionCount = provider != NULL ? 1u : 0u};
    const LaiueModuleStatus status =
        LaiueModuleHostLoadProfileV1(host, &profile, NULL, &diagnostic);
#else
    const LaiueModuleApiV1 *apis[MODULES] = {0};
    uint32_t count = 0u;
#if defined(LAIUE_ANIMATION_DEMO_STATIC_CONTENT)
    apis[count++] = LaiueContentGetStaticModuleApiV1();
#endif
#if defined(LAIUE_ANIMATION_DEMO_STATIC_MODEL)
    apis[count++] = LaiueModelGetStaticModuleApiV1();
#endif
#if defined(LAIUE_ANIMATION_DEMO_STATIC_ANIMATION)
    apis[count++] = LaiueAnimationGetStaticModuleApiV1();
#endif
#if defined(LAIUE_ANIMATION_DEMO_GRAPHICS)
    if (graphical)
        apis[count++] = LaiueGraphicsGetStaticModuleApiV1();
#endif
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (graphical && !offscreen)
        apis[count++] = LaiueWindowGetStaticModuleApiV1();
#endif
    const LaiueModuleStatus status =
        count == 0u ? LAIUE_MODULE_OK : LaiueModuleHostLoadStatic(host, apis, count, &diagnostic);
#endif
    (void)graphical;
    (void)offscreen;
    if (status != LAIUE_MODULE_OK && status != LAIUE_MODULE_PARTIAL)
    {
        PlatformWriteConsoleUtf8(diagnostic.message);
        return 0u;
    }
    return 1u;
}

static uint32_t LoadAssets(Demo *demo, const wchar_t *root)
{
    if (demo->modelService == NULL)
        return !demo->requireFull;
    if (demo->content != NULL)
    {
        demo->catalog = demo->content->createCatalog(root);
        if (demo->catalog == NULL)
            return 0u;
    }
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t status = 0u;
    if (demo->catalog != NULL)
        demo->model = demo->modelService->loadFrom(demo->catalog, L"robot", &status);
    else if (Join(path, root, L"models/Demo.lop/robot.lo"))
        demo->model = demo->modelService->loadFile(path, &status);
    if (demo->model == NULL || status != LAIUE_MODEL_LOAD_OK ||
        !demo->modelService->getView(demo->model, &demo->mesh) || demo->mesh.vertexCount == 0u ||
        demo->mesh.vertexCount > 65536u)
        return 0u;
    LaiueModelSkinViewV1 skin = {.structSize = sizeof(skin)};
    if (Tail(demo->modelBytes, demo->modelService->structSize,
             offsetof(LaiueModelServiceV1, getSkinView), sizeof(demo->modelService->getSkinView)) &&
        demo->modelService->getSkinView != NULL &&
        !demo->modelService->getSkinView(demo->model, &skin))
        return 0u;
    LaiueAnimationViewV1 clip = {.structSize = sizeof(clip)};
    if (demo->animation != NULL && skin.vertexCount != 0u)
    {
        if (demo->catalog != NULL)
            demo->clip = demo->animation->loadFrom(demo->catalog, L"wave", &status);
        else if (Join(path, root, L"animations/Demo.lkp/wave.lk"))
            demo->clip = demo->animation->loadFile(path, &status);
        if (demo->clip != NULL && status == LAIUE_ANIMATION_LOAD_OK &&
            demo->animation->getView(demo->clip, &clip) && clip.jointCount != 0u)
        {
            if (skin.vertexCount != demo->mesh.vertexCount || skin.jointCount != clip.jointCount ||
                skin.vertices == NULL || clip.jointCount > 1024u)
                return 0u;
            demo->animated = 1u;
            demo->joints = clip.jointCount;
        }
    }
    if (demo->requireFull && !demo->animated)
        return 0u;
    if (demo->animated &&
        Tail(demo->animationBytes, demo->animation->structSize,
             offsetof(LaiueAnimationServiceV1, skinVerticesWithScratch),
             sizeof(demo->animation->skinVerticesWithScratch)) &&
        demo->animation->skinningScratchBytes != NULL &&
        demo->animation->skinVerticesWithScratch != NULL)
        demo->scratchBytes = demo->animation->skinningScratchBytes(demo->joints);
    const uint32_t count = demo->mesh.vertexCount;
    const uint64_t bytes =
        (uint64_t)count *
            (sizeof(*demo->source) + sizeof(*demo->deformed) + sizeof(*demo->vertices)) +
        (uint64_t)demo->joints *
            (sizeof(*demo->locals) + sizeof(*demo->globals) + sizeof(*demo->palette)) +
        demo->scratchBytes;
    demo->allocation = PlatformAllocate((size_t)bytes, false);
    if (demo->allocation == NULL)
        return 0u;
    uint8_t *cursor = demo->allocation;
#define ARRAY(member, amount)                                                                      \
    demo->member = (void *)cursor;                                                                 \
    cursor += (size_t)(amount) * sizeof(*demo->member)
    ARRAY(source, count);
    ARRAY(deformed, count);
    ARRAY(vertices, count);
    ARRAY(locals, demo->joints);
    ARRAY(globals, demo->joints);
    ARRAY(palette, demo->joints);
#undef ARRAY
    demo->scratch = cursor;
    for (uint32_t i = 0u; i < count; ++i)
    {
        memcpy(demo->source[i].position, demo->mesh.vertices[i].position,
               sizeof(demo->source[i].position));
        memcpy(demo->source[i].normal, demo->mesh.vertices[i].normal,
               sizeof(demo->source[i].normal));
        if (demo->animated)
        {
            memcpy(demo->source[i].joints, skin.vertices[i].joints, sizeof(demo->source[i].joints));
            memcpy(demo->source[i].weights, skin.vertices[i].weights,
                   sizeof(demo->source[i].weights));
        }
    }
    return 1u;
}

static uint32_t Pose(Demo *demo, float time)
{
    if (demo->poseValid && (!demo->animated || demo->lastTime == time))
        return 1u;
    if (demo->animated)
    {
        if (!demo->animation->samplePose(demo->clip, time, 1u, demo->locals, demo->globals,
                                         demo->palette, demo->joints))
            return 0u;
        const uint32_t success =
            demo->scratchBytes != 0u
                ? demo->animation->skinVerticesWithScratch(
                      demo->source, demo->mesh.vertexCount, demo->palette, demo->joints,
                      demo->deformed, demo->mesh.vertexCount, demo->scratch, demo->scratchBytes)
                : demo->animation->skinVertices(demo->source, demo->mesh.vertexCount, demo->palette,
                                                demo->joints, demo->deformed,
                                                demo->mesh.vertexCount);
        if (!success)
            return 0u;
    }
    for (uint32_t i = 0u; i < demo->mesh.vertexCount; ++i)
    {
        const float *position =
            demo->animated ? demo->deformed[i].position : demo->source[i].position;
        const float *normal = demo->animated ? demo->deformed[i].normal : demo->source[i].normal;
        memcpy(demo->vertices[i].position, position, sizeof(demo->vertices[i].position));
        memcpy(demo->vertices[i].uv, demo->mesh.vertices[i].uv, sizeof(demo->vertices[i].uv));
        float light = -0.4f * normal[0] - 0.5f * normal[1] + 0.75f * normal[2];
        light = 0.45f + 0.55f * (light > 0.0f ? light : 0.0f);
        if (light > 1.0f)
            light = 1.0f;
        uint32_t color = demo->mesh.vertices[i].colorRGBA & UINT32_C(0xff000000);
        for (uint32_t channel = 0u; channel < 3u; ++channel)
            color |=
                (uint32_t)((float)((demo->mesh.vertices[i].colorRGBA >> (8u * channel)) & 255u) *
                           light)
                << (8u * channel);
        demo->vertices[i].colorRGBA = color;
    }
    if (demo->device != NULL)
    {
        const LaiueGraphicsBufferUploadV1 upload = {.structSize = sizeof(upload),
                                                    .buffer = demo->vertexBuffer,
                                                    .data = demo->vertices,
                                                    .sizeBytes = (uint64_t)demo->mesh.vertexCount *
                                                                 sizeof(*demo->vertices)};
        if (!demo->device->uploadBuffer(demo->device, &upload))
            return 0u;
    }
    demo->lastTime = time;
    demo->poseValid = 1u;
    return 1u;
}

static uint32_t CreateGeometry(Demo *demo)
{
    const LaiueGraphicsBufferDescV1 vb = {sizeof(vb), LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
                                          (uint64_t)demo->mesh.vertexCount *
                                              sizeof(*demo->vertices)};
    const LaiueGraphicsBufferDescV1 ib = {sizeof(ib), LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
                                          (uint64_t)demo->mesh.indexCount * sizeof(uint32_t)};
    if (!demo->device->createBuffer(demo->device, &vb, &demo->vertexBuffer) ||
        !demo->device->createBuffer(demo->device, &ib, &demo->indexBuffer))
        return 0u;
    const LaiueGraphicsBufferUploadV1 upload = {.structSize = sizeof(upload),
                                                .buffer = demo->indexBuffer,
                                                .data = demo->mesh.indices,
                                                .sizeBytes = ib.sizeBytes};
    uint8_t checker[8u * 8u * 4u];
    for (uint32_t i = 0u; i < 64u; ++i)
    {
        const uint8_t value = ((i / 8u) ^ i) & 1u ? 230u : 175u;
        checker[4u * i] = checker[4u * i + 1u] = checker[4u * i + 2u] = value;
        checker[4u * i + 3u] = 255u;
    }
    const LaiueGraphicsTextureDescV1 texture = {
        .structSize = sizeof(texture), .extent = {8u, 8u, 1u}, .mipLevels = 1u};
    const LaiueGraphicsSamplerDescV1 sampler = {.structSize = sizeof(sampler)};
    if (!demo->device->uploadBuffer(demo->device, &upload) ||
        !demo->device->createTexture(demo->device, &texture, &demo->texture) ||
        !demo->device->createSampler(demo->device, &sampler, &demo->sampler))
        return 0u;
    const LaiueGraphicsTextureUploadV1 pixels = {.structSize = sizeof(pixels),
                                                 .texture = demo->texture,
                                                 .data = checker,
                                                 .sizeBytes = sizeof(checker)};
    return demo->device->uploadTexture(demo->device, &pixels);
}

static uint32_t Draw(Demo *demo, uint32_t width, uint32_t height, float time)
{
    if (!Pose(demo, time))
        return 0u;
    if (demo->device == NULL)
        return 1u;
    if (width == 0u || height == 0u)
        return 1u;
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    // Row vectors, +Y depth, Z up. A fixed orthographic quarter view shows deformation.
    const float horizontal = 1.8f * (float)height / (float)width;
    camera.viewProjection[0] = 0.9f * horizontal;
    camera.viewProjection[4] = 0.4358899f * horizontal;
    camera.viewProjection[9] = 0.9f;
    camera.viewProjection[2] = -0.04358899f;
    camera.viewProjection[6] = 0.09f;
    camera.viewProjection[12] = 0.20f;
    camera.viewProjection[13] = -0.82f;
    camera.viewProjection[14] = 0.5f;
    camera.viewProjection[15] = 1.0f;
    if (demo->capture && !demo->device->requestFrameReadback(demo->device))
        return 0u;
    if (!demo->device->setCamera(demo->device, &camera) ||
        !demo->device->beginFrame(demo->device, width, height))
        return 0u;
    for (uint32_t part = 0u; part < demo->mesh.partCount; ++part)
    {
        const LaiueGraphicsDrawItemV2 draw = {.structSize = sizeof(draw),
                                              .vertexBuffer = demo->vertexBuffer,
                                              .indexBuffer = demo->indexBuffer,
                                              .indexCount = demo->mesh.parts[part].indexCount,
                                              .firstIndex = demo->mesh.parts[part].firstIndex,
                                              .scale = 1.0f,
                                              .texture = demo->texture,
                                              .sampler = demo->sampler};
        if (!demo->device->submit(demo->device, &draw, 1u))
            return 0u;
    }
    return demo->device->endFrame(demo->device);
}

static void Store32(uint8_t *out, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
        out[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t ForegroundNear(const uint8_t *rgba, uint32_t x, uint32_t y)
{
    uint32_t count = 0u;
    for (uint32_t row = y - 6u; row <= y + 6u; ++row)
        for (uint32_t column = x - 6u; column <= x + 6u; ++column)
            count += memcmp(rgba + 4u * (row * WIDTH + column), rgba, 3u) != 0;
    return count;
}

static uint32_t Capture(Demo *demo, uint32_t frame)
{
    LaiueGraphicsFrameReadbackV2 readback = {
        .structSize = sizeof(readback), .pixels = demo->pixels + 54u, .capacityBytes = FRAME_BYTES};
    if (!demo->device->readbackFrame(demo->device, &readback) || readback.width != WIDTH ||
        readback.height != HEIGHT || readback.writtenBytes != FRAME_BYTES)
        return 0u;
    const uint8_t *rgba = demo->pixels + 54u;
    uint32_t foreground = 0u;
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; ++i)
        foreground += memcmp(rgba + 4u * i, rgba, 3u) != 0;
    if (foreground < WIDTH * HEIGHT / 100u)
        return 0u;
    if (frame == 0u)
    {
        // These separate image regions follow the shipped rig's analytic
        // sleeve endpoint through the fixed camera, rather than its colours.
        if (demo->animated &&
            (ForegroundNear(rgba, 117u, 175u) < 24u || ForegroundNear(rgba, 232u, 104u) != 0u))
            return 0u;
        memcpy(demo->reference, rgba, FRAME_BYTES);
    }
    if (frame == 2u)
    {
        uint32_t changed = 0u;
        for (uint32_t i = 0u; i < WIDTH * HEIGHT; ++i)
            changed += memcmp(demo->reference + 4u * i, rgba + 4u * i, 4u) != 0;
        if (demo->animated && changed < 128u)
            return 0u;
        if (demo->animated &&
            (ForegroundNear(rgba, 232u, 104u) < 24u || ForegroundNear(rgba, 117u, 175u) != 0u))
            return 0u;
        memcpy(demo->reference, rgba, FRAME_BYTES);
    }
    if (frame == 4u && memcmp(demo->reference, rgba, FRAME_BYTES) != 0)
        return 0u;
    static char directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t wide[LAIUE_PLATFORM_PATH_CAPACITY], path[LAIUE_PLATFORM_PATH_CAPACITY];
    const uint32_t length =
        PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_CAPTURE_DIR", directory, sizeof(directory));
    if (length == 0u)
        return 1u;
    if (!PlatformUtf8ToWide(directory, length, wide, LAIUE_PLATFORM_PATH_CAPACITY, NULL))
        return 0u;
    wchar_t name[] = L"pose-0.bmp";
    name[5] = (wchar_t)(L'0' + frame);
    if (!Join(path, wide, name))
        return 0u;
    memset(demo->pixels, 0, 54u);
    demo->pixels[0] = 'B';
    demo->pixels[1] = 'M';
    Store32(demo->pixels + 2u, FRAME_BYTES + 54u);
    Store32(demo->pixels + 10u, 54u);
    Store32(demo->pixels + 14u, 40u);
    Store32(demo->pixels + 18u, WIDTH);
    Store32(demo->pixels + 22u, (uint32_t)-(int32_t)HEIGHT);
    demo->pixels[26] = 1u;
    demo->pixels[28] = 32u;
    Store32(demo->pixels + 34u, FRAME_BYTES);
    for (uint32_t i = 0u; i < WIDTH * HEIGHT; ++i)
    {
        uint8_t *pixel = demo->pixels + 54u + 4u * i;
        const uint8_t red = pixel[0];
        pixel[0] = pixel[2];
        pixel[2] = red;
    }
    return PlatformWriteFileAtomic(path, demo->pixels, FRAME_BYTES + 54u);
}

#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
static void WindowFrame(void *context)
{
    Demo *demo = context;
    int32_t width = 0, height = 0;
    demo->windowService->getClientSize(demo->window, &width, &height);
    if (width > 0 && height > 0)
    {
        if (!Draw(demo, (uint32_t)width, (uint32_t)height,
                  (float)(PlatformMonotonicSeconds() - demo->startTime)))
            demo->failed = 1u;
        else
            ++demo->frames;
    }
    if (demo->failed || PlatformTerminationRequested() ||
        (demo->frameLimit != 0u && demo->frames >= demo->frameLimit))
        demo->windowService->requestClose(demo->window);
}
#endif

static uint32_t Run(void)
{
    static Demo demo;
    static char mode[32], backendText[32], required[8];
    PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_DEMO_MODE", mode, sizeof(mode));
    PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_DEMO_BACKEND", backendText, sizeof(backendText));
    PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_REQUIRE_FULL", required, sizeof(required));
    demo.requireFull = Equal(required, "1");
    const uint32_t cpu = Equal(mode, "cpu");
    demo.capture = Equal(mode, "capture");
    char requiredWindow[8] = {0};
    PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_REQUIRE_WINDOW", requiredWindow,
                               sizeof(requiredWindow));
    const uint32_t requireWindow = Equal(requiredWindow, "1");
    if (requireWindow && (cpu || demo.capture))
        return 1u;
    if (mode[0] != '\0' && !cpu && !demo.capture && !Equal(mode, "window"))
        return 1u;
    const uint32_t offscreen = demo.capture && !Equal(backendText, "d3d12");
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
        return 1u;
    uint32_t success = LoadModules(host, !cpu, offscreen);
    if (!success)
        goto cleanup;
    demo.modelService =
        LaiueModuleHostQueryService(host, LAIUE_MODEL_SERVICE_NAME, 1u,
                                    LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE, NULL, &demo.modelBytes);
    demo.animation = LaiueModuleHostQueryService(host, LAIUE_ANIMATION_SERVICE_NAME, 1u,
                                                 LAIUE_ANIMATION_SERVICE_V1_BASE_SIZE, NULL,
                                                 &demo.animationBytes);
    demo.content = LaiueModuleHostQueryService(host, LAIUE_CONTENT_SERVICE_NAME, 1u,
                                               sizeof(LaiueContentServiceV1), NULL, NULL);
    demo.graphics = LaiueModuleHostQueryService(host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, 2u,
                                                LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, NULL,
                                                &demo.graphicsBytes);
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY], root[LAIUE_PLATFORM_PATH_CAPACITY];
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !Join(root, directory, L"assets_animation") || !LoadAssets(&demo, root))
    {
        success = 0u;
        goto cleanup;
    }
    if (demo.model == NULL)
    {
        PlatformWriteConsoleUtf8("animation: model module unavailable; no model is displayed\n");
        success = !requireWindow;
        goto cleanup;
    }
    if (!demo.animated)
        PlatformWriteConsoleUtf8(
            "animation: binding or clip unavailable; displaying the bind mesh\n");
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (!cpu && !offscreen)
    {
        demo.windowService = LaiueModuleHostQueryService(host, LAIUE_WINDOW_SERVICE_NAME, 1u,
                                                         sizeof(LaiueWindowServiceV1), NULL, NULL);
        const WindowConfiguration window = {
            .title = L"LAIUE Animation", .width = WIDTH, .height = HEIGHT};
        if (demo.windowService != NULL)
            demo.window = demo.windowService->create(&window);
    }
#endif
    void *native = NULL;
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (demo.window != NULL)
        native = demo.windowService->getNativeHandle(demo.window);
#endif
    if (requireWindow && (native == NULL || demo.graphics == NULL))
    {
        success = 0u;
        goto cleanup;
    }
    if (!cpu && demo.graphics != NULL && (offscreen || native != NULL))
    {
        const uint32_t backend =
            offscreen ? LAIUE_GRAPHICS_BACKEND_VULKAN : LAIUE_GRAPHICS_BACKEND_AUTO;
        if (demo.graphics->destroyDevice == NULL)
        {
            success = 0u;
            goto cleanup;
        }
        if (Tail(demo.graphicsBytes, demo.graphics->structSize,
                 offsetof(LaiueGraphicsDeviceServiceV2, context), sizeof(demo.graphics->context)) &&
            demo.graphics->createDeviceWithContext != NULL)
            success = demo.graphics->createDeviceWithContext(demo.graphics->context, native, WIDTH,
                                                             HEIGHT, backend, &demo.device);
        else
            success = demo.graphics->createDevice != NULL &&
                      demo.graphics->createDevice(native, WIDTH, HEIGHT, backend, &demo.device);
        if (!success)
        {
            char requireDriver[8] = {0};
            PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_REQUIRE_DRIVER", requireDriver,
                                       sizeof(requireDriver));
            demo.skipped = demo.capture && !Equal(requireDriver, "1");
            goto cleanup;
        }
        if (demo.device == NULL ||
            demo.device->structSize < offsetof(LaiueGraphicsDeviceV2, uploadTexture) +
                                          sizeof(demo.device->uploadTexture) ||
            demo.device->createBuffer == NULL || demo.device->uploadBuffer == NULL ||
            demo.device->createTexture == NULL || demo.device->createSampler == NULL ||
            demo.device->destroyHandle == NULL || demo.device->beginFrame == NULL ||
            demo.device->submit == NULL || demo.device->endFrame == NULL ||
            demo.device->setCamera == NULL || demo.device->uploadTexture == NULL ||
            (demo.capture &&
             (demo.device->structSize < LAIUE_GRAPHICS_DEVICE_V2_READBACK_REQUEST_SIZE ||
              demo.device->requestFrameReadback == NULL || demo.device->readbackFrame == NULL)))
        {
            success = 0u;
            goto cleanup;
        }
        if (!CreateGeometry(&demo))
        {
            success = 0u;
            goto cleanup;
        }
    }
    if (demo.capture && demo.device == NULL)
    {
        success = 0u;
        goto cleanup;
    }
    if (demo.capture)
    {
        demo.pixels = PlatformAllocate(FRAME_BYTES + 54u, false);
        demo.reference = PlatformAllocate(FRAME_BYTES, false);
        if (demo.pixels == NULL || demo.reference == NULL)
        {
            success = 0u;
            goto cleanup;
        }
    }
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (!cpu && !demo.capture && demo.window != NULL && demo.device != NULL)
    {
        char frameText[16] = {0};
        const uint32_t digits = PlatformGetEnvironmentUtf8("LAIUE_ANIMATION_WINDOW_FRAMES",
                                                           frameText, sizeof(frameText));
        for (uint32_t i = 0u; i < digits; ++i)
        {
            if (frameText[i] < '0' || frameText[i] > '9' || demo.frameLimit > 100000u)
            {
                success = 0u;
                goto cleanup;
            }
            demo.frameLimit = 10u * demo.frameLimit + (uint32_t)(frameText[i] - '0');
        }
        demo.startTime = PlatformMonotonicSeconds();
        PlatformInstallTerminationHandler();
        demo.windowService->runLoop(demo.window, WindowFrame, &demo);
        PlatformRemoveTerminationHandler();
        success = !demo.failed && (demo.frameLimit == 0u || demo.frames == demo.frameLimit);
        if (success)
            PlatformWriteConsoleUtf8("animation: window presentation passed\n");
        goto cleanup;
    }
#endif
    const float times[5] = {0.0f, 0.5f, 1.0f, 1.5f, 1.0f};
    for (uint32_t frame = 0u; success && frame < 5u; ++frame)
    {
        success = Draw(&demo, WIDTH, HEIGHT, times[frame]);
        if (success && demo.capture)
            success = Capture(&demo, frame);
    }
    if (success)
        PlatformWriteConsoleUtf8(
            !demo.animated ? "animation: static bind geometry passed\n"
            : demo.capture
                ? "animation: changing GPU poses and repeated frame passed\n"
                : "animation: model binding, pose sampling and CPU deformation passed\n");
cleanup:
    if (demo.device != NULL)
    {
        if (demo.device->structSize >= offsetof(LaiueGraphicsDeviceV2, destroyHandle) +
                                           sizeof(demo.device->destroyHandle) &&
            demo.device->destroyHandle != NULL)
        {
            demo.device->destroyHandle(demo.device, demo.sampler);
            demo.device->destroyHandle(demo.device, demo.texture);
            demo.device->destroyHandle(demo.device, demo.indexBuffer);
            demo.device->destroyHandle(demo.device, demo.vertexBuffer);
        }
        demo.graphics->destroyDevice(demo.device);
    }
#if defined(LAIUE_ANIMATION_DEMO_WINDOW)
    if (demo.window != NULL)
        demo.windowService->destroy(demo.window);
#endif
    PlatformFree(demo.reference);
    PlatformFree(demo.pixels);
    PlatformFree(demo.allocation);
    if (demo.clip != NULL)
        demo.animation->release(demo.clip);
    if (demo.model != NULL)
        demo.modelService->release(demo.model);
    if (demo.catalog != NULL)
        demo.content->destroyCatalog(demo.catalog);
    LaiueModuleHostDestroy(host);
    if (demo.skipped)
        PlatformWriteConsoleUtf8("animation: SKIP requested graphics driver unavailable\n");
    else if (!success)
        PlatformWriteConsoleUtf8("animation: asset, deformation or rendering failed\n");
    return success ? 0u : demo.skipped ? 125u : 1u;
}

#if defined(_WIN32)
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);
void AnimationDemoEntryPoint(void)
{
    ExitProcess(Run());
}
#else
int main(void)
{
    return (int)Run();
}
#endif
