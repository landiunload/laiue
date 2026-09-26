#include "audio/audio_service.h"
#include "audio/audio_pack_service.h"
#include "content/content_service.h"
#include "input/input_service.h"
#include "mesh/mesher_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "graphics/graphics_device_service.h"
#include "render/graphics_service.h"
#include "scene/math_service.h"
#include "scene/scene_service.h"
#include "test_runtime.h"
#include "ui/ui_service.h"
#include "voxel_render/voxel_render_service.h"
#include "world/world_service.h"
#include "platform/window_service.h"

#include <stdbool.h>

#if defined(_WIN32)
#define AUDIO_MODULE_NAME L"laiue_audio.dll"
#define AUDIO_PACK_MODULE_NAME L"laiue_audio_pack.dll"
#define INPUT_MODULE_NAME L"laiue_input.dll"
#define CONTENT_MODULE_NAME L"laiue_content.dll"
#define UI_MODULE_NAME L"laiue_ui.dll"
#define VOXEL_RENDER_MODULE_NAME L"laiue_voxel_render.dll"
#define SCENE_MATH_MODULE_NAME L"laiue_scene_math.dll"
#define WORLD_MODULE_NAME L"laiue_world.dll"
#define MESHER_MODULE_NAME L"laiue_mesher.dll"
#define RENDER_MODULE_NAME L"laiue_render.dll"
#define NUMERIC_MODULE_NAME L"laiue_numeric.dll"
#define SCENE_MODULE_NAME L"laiue_scene.dll"
#define WINDOW_MODULE_NAME L"laiue_window.dll"
#elif defined(__APPLE__)
#define AUDIO_MODULE_NAME L"liblaiue_audio.dylib"
#define AUDIO_PACK_MODULE_NAME L"liblaiue_audio_pack.dylib"
#define INPUT_MODULE_NAME L"liblaiue_input.dylib"
#define CONTENT_MODULE_NAME L"liblaiue_content.dylib"
#define UI_MODULE_NAME L"liblaiue_ui.dylib"
#define VOXEL_RENDER_MODULE_NAME L"liblaiue_voxel_render.dylib"
#define SCENE_MATH_MODULE_NAME L"liblaiue_scene_math.dylib"
#define WORLD_MODULE_NAME L"liblaiue_world.dylib"
#define MESHER_MODULE_NAME L"liblaiue_mesher.dylib"
#define RENDER_MODULE_NAME L"liblaiue_render.dylib"
#define NUMERIC_MODULE_NAME L"liblaiue_numeric.dylib"
#define SCENE_MODULE_NAME L"liblaiue_scene.dylib"
#define WINDOW_MODULE_NAME L"liblaiue_window.dylib"
#else
#define AUDIO_MODULE_NAME L"liblaiue_audio.so"
#define AUDIO_PACK_MODULE_NAME L"liblaiue_audio_pack.so"
#define INPUT_MODULE_NAME L"liblaiue_input.so"
#define CONTENT_MODULE_NAME L"liblaiue_content.so"
#define UI_MODULE_NAME L"liblaiue_ui.so"
#define VOXEL_RENDER_MODULE_NAME L"liblaiue_voxel_render.so"
#define SCENE_MATH_MODULE_NAME L"liblaiue_scene_math.so"
#define WORLD_MODULE_NAME L"liblaiue_world.so"
#define MESHER_MODULE_NAME L"liblaiue_mesher.so"
#define RENDER_MODULE_NAME L"liblaiue_render.so"
#define NUMERIC_MODULE_NAME L"liblaiue_numeric.so"
#define SCENE_MODULE_NAME L"liblaiue_scene.so"
#define WINDOW_MODULE_NAME L"liblaiue_window.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index] = root[index], ++index;
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

LAIUE_TEST_ENTRY(OptionalModulesTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t missingPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t audioPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t audioPackPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t inputPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t contentPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t uiPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t voxelRenderPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t sceneMathPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t worldPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t mesherPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t renderPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t scenePath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t windowPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(missingPath, directory, L"laiue_optional_module_absent_9f3c.dll"),
           "missing path fits");
    Expect(Join(audioPath, directory, AUDIO_MODULE_NAME), "audio path fits");
    Expect(Join(audioPackPath, directory, AUDIO_PACK_MODULE_NAME), "audio pack path fits");
    Expect(Join(inputPath, directory, INPUT_MODULE_NAME), "input path fits");
    Expect(Join(contentPath, directory, CONTENT_MODULE_NAME), "content path fits");
    Expect(Join(uiPath, directory, UI_MODULE_NAME), "UI path fits");
    Expect(Join(voxelRenderPath, directory, VOXEL_RENDER_MODULE_NAME),
           "voxel render path fits");
    Expect(Join(sceneMathPath, directory, SCENE_MATH_MODULE_NAME),
           "scene math path fits");
    Expect(Join(worldPath, directory, WORLD_MODULE_NAME), "world path fits");
    Expect(Join(mesherPath, directory, MESHER_MODULE_NAME), "mesher path fits");
    Expect(Join(renderPath, directory, RENDER_MODULE_NAME), "render path fits");
    Expect(Join(numericPath, directory, NUMERIC_MODULE_NAME), "numeric path fits");
    Expect(Join(scenePath, directory, SCENE_MODULE_NAME), "scene path fits");
    Expect(Join(windowPath, directory, WINDOW_MODULE_NAME), "window path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 absent = {
        missingPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    Expect(LaiueModuleHostLoad(host, &absent, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "missing optional module is skipped");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "optional absence leaves the host usable");

    LaiueModuleBinaryV1 audio = {audioPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &audio, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueAudioServiceV1 *audioService =
        (const LaiueAudioServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_SERVICE_NAME, LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
            sizeof(LaiueAudioServiceV1), &version, &size);
    Expect(audioService != NULL && version == LAIUE_AUDIO_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*audioService) && audioService->deviceCreate != NULL &&
               audioService->deviceCreateWithContext != NULL &&
               audioService->context != NULL,
           "audio service is published");
    AudioDeviceConfiguration offscreenConfiguration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = 0u,
        .frameCountHint = 0u,
        .masterVolume = 1.0f,
    };
    AudioDevice *offscreenDevice = NULL;
    Expect(audioService->deviceCreateWithContext(audioService->context,
                                                 &offscreenConfiguration,
                                                 &offscreenDevice) == AUDIO_RESULT_OK &&
               offscreenDevice != NULL,
           "audio device uses the owning module instance");
    audioService->deviceDestroy(offscreenDevice);
    LaiueModuleHostUnloadAll(host);

    /* A present pack provider must not silently degrade when its mixer
     * dependency is absent: the host rejects only that graph and remains
     * usable for unrelated technologies. */
    LaiueModuleBinaryV1 packWithoutAudio[] = {
        {audioPackPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, packWithoutAudio,
                               sizeof(packWithoutAudio) / sizeof(packWithoutAudio[0]),
                               &diagnostic) == LAIUE_MODULE_DEPENDENCY_MISSING,
           "audio pack without mixer must report its missing dependency");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed audio pack graph must roll back completely");

    /* The pack provider is a real dependency graph: its DLL has no imports
     * from either provider and receives both tables only after the host has
     * started them.  Listing it first also checks that ordering is resolved
     * from the manifest rather than from filesystem order. */
    LaiueModuleBinaryV1 packBinaries[] = {
        {audioPackPath, 0u, NULL},
        {audioPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, packBinaries,
                               sizeof(packBinaries) / sizeof(packBinaries[0]), &diagnostic) ==
               LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueAudioPackServiceV1 *packService =
        (const LaiueAudioPackServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_PACK_SERVICE_NAME, LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1,
            sizeof(LaiueAudioPackServiceV1), &version, &size);
    Expect(packService != NULL && version == LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*packService) && packService->loadMemory != NULL,
           "audio pack service is published after dependencies");
    LaiueModuleHostUnloadAll(host);

    /* Window and input are platform providers, not a D3D12-only feature.
     * On Windows the same graph must load when the graphics backend is
     * explicitly Vulkan. */
    LaiueModuleBinaryV1 platformGraph[] = {
        {inputPath, 0u, NULL},
        {windowPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, platformGraph,
                               (uint32_t)(sizeof(platformGraph) /
                                          sizeof(platformGraph[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueInputServiceV1 *inputService =
        (const LaiueInputServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_INPUT_SERVICE_NAME, LAIUE_INPUT_SERVICE_ABI_VERSION_1,
            sizeof(LaiueInputServiceV1), &version, &size);
    const LaiueWindowServiceV1 *windowService =
        (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
            sizeof(LaiueWindowServiceV1), &version, &size);
    Expect(inputService != NULL && inputService->create != NULL &&
               windowService != NULL && windowService->create != NULL,
           "platform providers load independently of the render backend");
    LaiueModuleHostUnloadAll(host);

    LaiueModuleBinaryV1 uiOnly = {uiPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &uiOnly, 1u, &diagnostic) ==
               LAIUE_MODULE_DEPENDENCY_MISSING,
           "UI without graphics reports missing dependency");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed UI graph rolls back completely");

    /* Graphics owns a built-in texture/shader fallback and only uses the
     * content catalog when a pack is requested. Removing the asset DLL must
     * therefore leave the renderer service available. */
    LaiueModuleBinaryV1 renderWithoutContent = {renderPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &renderWithoutContent, 1u, &diagnostic) ==
               LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueGraphicsServiceV1 *fallbackGraphics =
        (const LaiueGraphicsServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_SERVICE_NAME, LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
            sizeof(LaiueGraphicsServiceV1), &version, &size);
    Expect(fallbackGraphics != NULL && fallbackGraphics->createWithBackend != NULL,
           "graphics service survives missing content provider");
    const LaiueGraphicsDeviceServiceV1 *fallbackDevice =
        (const LaiueGraphicsDeviceServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
            sizeof(LaiueGraphicsDeviceServiceV1), &version, &size);
    Expect(fallbackDevice != NULL && fallbackDevice->createDevice != NULL &&
               fallbackDevice->destroyDevice != NULL &&
               fallbackDevice->createDeviceWithContext != NULL &&
               fallbackDevice->context != NULL,
           "generic graphics device service survives missing content provider");
    const LaiueGraphicsDeviceServiceV2 *fallbackDeviceV2 =
        (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, &version, &size);
    Expect(fallbackDeviceV2 != NULL && version == LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2 &&
               size >= LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE &&
               fallbackDeviceV2->createDevice != NULL &&
               fallbackDeviceV2->createDeviceWithContext != NULL,
           "graphics device v2 is published independently of content");

    /* Exercise the ownership rules through the public service, not through
     * renderer internals.  This is deliberately an offscreen device: the
     * same check works for D3D12 and Vulkan providers and does not require a
     * platform window. */
    LaiueGraphicsDeviceV2 *resourceDevice = NULL;
    Expect(fallbackDeviceV2->createDeviceWithContext(
               fallbackDeviceV2->context, NULL, 16, 16,
               LAIUE_GRAPHICS_BACKEND_VULKAN, &resourceDevice) != 0u &&
               resourceDevice != NULL,
           "graphics device v2 creates an offscreen instance");
    static const uint8_t shaderBytes[4] = {0x03u, 0x02u, 0x23u, 0x07u};
    LaiueGraphicsShaderDescV1 shaderDescription = {
        sizeof(shaderDescription), LAIUE_GRAPHICS_SHADER_STAGE_VERTEX,
        shaderBytes, sizeof(shaderBytes)};
    LaiueGraphicsHandle shader = 0u;
    Expect(resourceDevice->createShader(resourceDevice, &shaderDescription, &shader) != 0u &&
               shader != 0u,
           "graphics device creates a shader handle");
    LaiueGraphicsPipelineDescV1 pipelineDescription = {
        sizeof(pipelineDescription), LAIUE_GRAPHICS_TOPOLOGY_TRIANGLES,
        sizeof(LaiueGraphicsVertexV2), shader, 0u};
    LaiueGraphicsHandle firstPipeline = 0u;
    Expect(resourceDevice->createPipeline(resourceDevice, &pipelineDescription,
                                          &firstPipeline) != 0u &&
               firstPipeline != 0u,
           "graphics device records pipeline shader dependencies");
    /* Destroying a referenced shader is intentionally a no-op in the void
     * ABI.  Creating another pipeline proves that the original handle stayed
     * live instead of becoming a dangling pointer. */
    resourceDevice->destroyHandle(resourceDevice, shader);
    LaiueGraphicsHandle secondPipeline = 0u;
    Expect(resourceDevice->createPipeline(resourceDevice, &pipelineDescription,
                                          &secondPipeline) != 0u &&
               secondPipeline != 0u,
           "referenced shader remains live until all pipelines are gone");
    resourceDevice->destroyHandle(resourceDevice, firstPipeline);
    resourceDevice->destroyHandle(resourceDevice, secondPipeline);
    resourceDevice->destroyHandle(resourceDevice, shader);

    LaiueGraphicsTextureDescV1 invalidTextureDescription = {
        sizeof(invalidTextureDescription), 0u, {4u, 4u, 1u}, 0u, 0u};
    LaiueGraphicsHandle invalidTexture = UINT64_C(1);
    Expect(resourceDevice->createTexture(resourceDevice, &invalidTextureDescription,
                                         &invalidTexture) == 0u && invalidTexture == 0u,
           "invalid texture mip count is rejected and output is cleared");
    LaiueGraphicsTextureDescV1 unsupportedTextureDescription = {
        sizeof(unsupportedTextureDescription), UINT32_C(99), {4u, 4u, 1u}, 1u, 0u};
    LaiueGraphicsHandle unsupportedTexture = UINT64_C(1);
    Expect(resourceDevice->createTexture(resourceDevice, &unsupportedTextureDescription,
                                         &unsupportedTexture) == 0u && unsupportedTexture == 0u,
           "unsupported texture format is rejected before handle publication");
    LaiueGraphicsTextureDescV1 textureDescription = {
        sizeof(textureDescription), 0u, {4u, 4u, 1u}, 1u, 0u};
    LaiueGraphicsHandle texture = 0u;
    Expect(resourceDevice->createTexture(resourceDevice, &textureDescription, &texture) != 0u &&
               texture != 0u,
           "graphics device stores texture descriptor state");
    uint8_t texturePixels[4u * 4u * 4u] = {0};
    LaiueGraphicsTextureUploadV1 textureUpload = {
        sizeof(textureUpload), texture, texturePixels, sizeof(texturePixels), 0u, 0u};
    Expect(resourceDevice->uploadTexture != NULL &&
               resourceDevice->uploadTexture(resourceDevice, &textureUpload) != 0u,
           "graphics device uploads texture pixels to the backend image");
    LaiueGraphicsSamplerDescV1 samplerDescription = {
        sizeof(samplerDescription), LAIUE_GRAPHICS_FILTER_LINEAR,
        LAIUE_GRAPHICS_FILTER_LINEAR, LAIUE_GRAPHICS_ADDRESS_REPEAT,
        LAIUE_GRAPHICS_ADDRESS_CLAMP, LAIUE_GRAPHICS_ADDRESS_BORDER};
    LaiueGraphicsHandle sampler = 0u;
    Expect(resourceDevice->createSampler(resourceDevice, &samplerDescription, &sampler) != 0u &&
               sampler != 0u,
           "graphics device stores sampler descriptor state");
    LaiueGraphicsBufferDescV1 boundVertexDescription = {
        sizeof(boundVertexDescription), LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
        3u * sizeof(LaiueGraphicsVertexV2)};
    LaiueGraphicsHandle boundVertexBuffer = 0u;
    static const LaiueGraphicsVertexV2 boundVertices[3] = {
        {{-0.75f, -0.75f, 0.0f}, {0.0f, 0.0f}, UINT32_C(0xFFFFFFFF)},
        {{ 0.75f, -0.75f, 0.0f}, {1.0f, 0.0f}, UINT32_C(0xFFFFFFFF)},
        {{ 0.00f,  0.75f, 0.0f}, {0.5f, 1.0f}, UINT32_C(0xFFFFFFFF)},
    };
    LaiueGraphicsBufferUploadV1 boundVertexUpload = {
        sizeof(boundVertexUpload), 0u, 0u, boundVertices, sizeof(boundVertices)};
    Expect(resourceDevice->createBuffer(resourceDevice, &boundVertexDescription,
                                         &boundVertexBuffer) != 0u &&
               boundVertexBuffer != 0u,
           "graphics device creates a generic vertex buffer for resource binding");
    boundVertexUpload.buffer = boundVertexBuffer;
    Expect(resourceDevice->uploadBuffer(resourceDevice, &boundVertexUpload) != 0u,
           "graphics device uploads the generic vertex buffer for resource binding");
    Expect(resourceDevice->beginFrame(resourceDevice, 16u, 16u) != 0u,
           "graphics device begins a resource-binding frame");
    LaiueGraphicsDrawItemV2 boundItem = {
        .structSize = sizeof(boundItem),
        .pipeline = 0u,
        .vertexBuffer = boundVertexBuffer,
        .indexBuffer = 0u,
        .texture = texture,
        .sampler = sampler,
    };
    Expect(resourceDevice->submit(resourceDevice, &boundItem, 1u) != 0u,
           "graphics device validates texture and sampler bindings");
    Expect(resourceDevice->endFrame(resourceDevice) != 0u,
           "graphics device ends a resource-binding frame");
    resourceDevice->destroyHandle(resourceDevice, boundVertexBuffer);
    resourceDevice->destroyHandle(resourceDevice, texture);
    resourceDevice->destroyHandle(resourceDevice, sampler);
    fallbackDeviceV2->destroyDevice(resourceDevice);

    LaiueModuleHost *secondRenderHost = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(secondRenderHost != NULL, "second concurrent graphics host creates");
    LaiueModuleBinaryV1 secondRender = {renderPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(secondRenderHost, &secondRender, 1u, &diagnostic) ==
               LAIUE_MODULE_START_FAILED,
           "second concurrent graphics host is rejected by the owner guard");
    Expect(LaiueModuleHostLoadedCount(secondRenderHost) == 0u,
           "failed concurrent graphics host rolls back independently");
    LaiueModuleHostDestroy(secondRenderHost);
    Expect(LaiueModuleHostQueryService(host, LAIUE_GRAPHICS_SERVICE_NAME,
                                       LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
                                       sizeof(LaiueGraphicsServiceV1), NULL, NULL) != NULL,
           "first graphics host remains usable after second host failure");
    LaiueModuleHostUnloadAll(host);

    LaiueModuleBinaryV1 uiGraph[] = {
        {uiPath, 0u, NULL},
        {renderPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, uiGraph,
                               (uint32_t)(sizeof(uiGraph) / sizeof(uiGraph[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueUiServiceV1 *uiService =
        (const LaiueUiServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_UI_SERVICE_NAME, LAIUE_UI_SERVICE_ABI_VERSION_1,
            sizeof(LaiueUiServiceV1), &version, &size);
    Expect(uiService != NULL && version == LAIUE_UI_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*uiService) && uiService->contextCreate != NULL &&
               uiService->contextDestroy != NULL &&
               uiService->contextCreateWithContext != NULL &&
               uiService->context != NULL,
           "UI service is published");
    void *uiContext = NULL;
    Expect(uiService->contextCreateWithContext(uiService->context, &uiContext) != 0u &&
               uiContext != NULL,
           "UI context uses the owning module instance");
    uiService->contextDestroy(uiContext);
    uiContext = NULL;
    Expect(uiService->contextCreate(&uiContext) != 0u && uiContext != NULL,
           "legacy UI context creation remains available");
    uiService->contextDestroy(uiContext);
    LaiueModuleHostUnloadAll(host);

    LaiueModuleBinaryV1 voxelRenderGraph[] = {
        {voxelRenderPath, 0u, NULL},
        {mesherPath, 0u, NULL},
        {sceneMathPath, 0u, NULL},
        {renderPath, 0u, NULL},
        {scenePath, 0u, NULL},
        {worldPath, 0u, NULL},
        {numericPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, voxelRenderGraph,
                               (uint32_t)(sizeof(voxelRenderGraph) /
                                          sizeof(voxelRenderGraph[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueVoxelRenderServiceV1 *voxelRenderService =
        (const LaiueVoxelRenderServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_VOXEL_RENDER_SERVICE_NAME,
            LAIUE_VOXEL_RENDER_SERVICE_ABI_VERSION_1,
            sizeof(LaiueVoxelRenderServiceV1), &version, &size);
    Expect(voxelRenderService != NULL &&
               version == LAIUE_VOXEL_RENDER_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*voxelRenderService) && voxelRenderService->create != NULL &&
               voxelRenderService->draw != NULL &&
               voxelRenderService->createWithContext != NULL &&
               voxelRenderService->context != NULL,
           "voxel render service is published");
    ChunkStreaming *instanceStreaming = voxelRenderService->createWithContext(
        voxelRenderService->context, NULL, NULL, 0);
    Expect(instanceStreaming != NULL,
           "voxel render creates an instance with bound services");
    voxelRenderService->destroy(instanceStreaming);
    const LaiueGraphicsServiceV1 *graphicsService =
        (const LaiueGraphicsServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_SERVICE_NAME, LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
            sizeof(LaiueGraphicsServiceV1), &version, &size);
    const LaiueMesherServiceV1 *mesherService =
        (const LaiueMesherServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
            sizeof(LaiueMesherServiceV1), &version, &size);
    const LaiueSceneMathServiceV1 *sceneMathService =
        (const LaiueSceneMathServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_SCENE_MATH_SERVICE_NAME,
            LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
            sizeof(LaiueSceneMathServiceV1), &version, &size);
    const LaiueSceneServiceV1 *sceneService =
        (const LaiueSceneServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_SCENE_SERVICE_NAME, LAIUE_SCENE_SERVICE_ABI_VERSION_1,
            sizeof(LaiueSceneServiceV1), &version, &size);
    const LaiueWorldServiceV1 *worldService =
        (const LaiueWorldServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1,
            sizeof(LaiueWorldServiceV1), &version, &size);
    Expect(graphicsService != NULL && graphicsService->createWithBackend != NULL &&
               mesherService != NULL && mesherService->buildChunkMesh != NULL &&
               sceneMathService != NULL && sceneMathService->matrix4Multiply != NULL &&
               sceneService != NULL && sceneService->cameraUpdate != NULL &&
               worldService != NULL && worldService->getBlock != NULL,
           "graphics dependency graph services are published");
    const LaiueGraphicsDeviceServiceV1 *deviceService =
        (const LaiueGraphicsDeviceServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
            sizeof(LaiueGraphicsDeviceServiceV1), &version, &size);
    Expect(deviceService != NULL && deviceService->getBackend != NULL,
           "graphics dependency graph exposes the backend-neutral device");
    const LaiueGraphicsDeviceServiceV2 *deviceServiceV2 =
        (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, &version, &size);
    Expect(deviceServiceV2 != NULL && deviceServiceV2->getBackend != NULL,
           "graphics dependency graph exposes device v2");
    LaiueModuleHostUnloadAll(host);

    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
