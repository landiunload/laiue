#include "mesh_world_demo_world.h"

#include "content/content_service.h"
#include "core/math/scalar.h"
#include "graphics/graphics_device_service.h"
#include "mesh_world_render/mesh_world_render_service.h"
#include "model/model_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#if defined(LAIUE_MESH_DEMO_GRAPHICS)
#include "render/graphics_service.h"
#endif
#if defined(LAIUE_MESH_DEMO_WINDOWED)
#include "input/input_service.h"
#include "platform/window_service.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum
{
    DEMO_DRAW_CAPACITY = 1024,
    DEMO_MODULE_CAPACITY = 7,
};

typedef struct DemoOptions
{
    uint32_t headless;
    uint32_t frames;
    uint32_t requireWindow;
    uint32_t help;
    wchar_t assets[LAIUE_PLATFORM_PATH_CAPACITY];
} DemoOptions;

/* An internal validation sink for a profile without offscreen graphics.
 * This is never registered as a module or advertised as a platform renderer. */
typedef struct DemoValidationDevice
{
    LaiueGraphicsDeviceV2 device;
    uint64_t nextHandle;
    uint64_t uploaded;
    uint32_t liveBuffers;
    uint32_t submitted;
} DemoValidationDevice;

typedef struct Demo
{
    const LaiueContentServiceV1 *content;
    const LaiueModelServiceV1 *model;
    const LaiueMeshWorldServiceV1 *worldService;
    const LaiueMeshWorldRenderServiceV1 *renderService;
    uint32_t renderServiceSize;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    uint32_t graphicsSize;
    LaiueContentCatalog *catalog;
    LaiueMeshWorldV1 *world;
    LaiueMeshWorldRendererV1 *renderer;
    LaiueGraphicsDeviceV2 *device;
    DemoValidationDevice validation;
    MeshDemoProvider provider;
    LaiueMeshPositionV1 player;
    float yaw, pitch, verticalSpeed;
    uint32_t grounded, failed, frames, drawnFrames;
    uint32_t simulationSteps;
    uint32_t maximumWindowFrames;
    double previousTime, accumulator;
    LaiueGraphicsDrawItemV2 draws[DEMO_DRAW_CAPACITY];
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    const LaiueWindowServiceV1 *windowService;
    const LaiueInputServiceV1 *inputService;
    Window *window;
    Input *input;
#endif
} Demo;

static uint32_t JoinPath(wchar_t *destination, const wchar_t *directory, const wchar_t *name)
{
    uint32_t length = 0u;
    while (directory[length] != L'\0')
    {
        if (length + 2u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return 0u;
        destination[length] = directory[length];
        ++length;
    }
    if (length != 0u && destination[length - 1u] != L'/' && destination[length - 1u] != L'\\')
        destination[length++] = L'/';
    for (uint32_t index = 0u; name[index] != L'\0'; ++index)
    {
        if (length + 1u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return 0u;
        destination[length++] = name[index];
    }
    destination[length] = L'\0';
    return 1u;
}

static uint32_t TextEquals(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0u;
    while (left[index] != L'\0' && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

static uint32_t ParseToken(DemoOptions *options, const wchar_t *token, uint32_t *pending)
{
    if (*pending == 1u)
    {
        uint32_t count = 0u;
        for (uint32_t index = 0u; token[index] != L'\0'; ++index)
        {
            if (token[index] < L'0' || token[index] > L'9' || count > 10000u)
                return 0u;
            count = count * 10u + (uint32_t)(token[index] - L'0');
        }
        if (count == 0u || count > 100000u)
            return 0u;
        options->frames = count;
    }
    else if (*pending == 2u)
    {
        uint32_t index = 0u;
        while (token[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        {
            options->assets[index] = token[index];
            ++index;
        }
        if (index == 0u || token[index] != L'\0')
            return 0u;
        options->assets[index] = L'\0';
    }
    else if (TextEquals(token, L"--headless"))
    {
        options->headless = 1u;
        *pending = 1u;
        return 1u;
    }
    else if (TextEquals(token, L"--assets"))
    {
        *pending = 2u;
        return 1u;
    }
    else if (TextEquals(token, L"--frames"))
    {
        options->requireWindow = 1u;
        *pending = 1u;
        return 1u;
    }
    else if (TextEquals(token, L"--help"))
        options->help = 1u;
    else
        return 0u;
    *pending = 0u;
    return 1u;
}

#if defined(LAIUE_MESH_DEMO_WINDOWED) || defined(LAIUE_MESH_DEMO_OFFSCREEN_VULKAN)
static uint32_t HasField(uint32_t published, uint32_t table, size_t offset, size_t size)
{
    return offset <= published && size <= published - offset && offset <= table &&
           size <= table - offset;
}
#endif

static uint32_t ValidationCreateBuffer(LaiueGraphicsDeviceV2 *device,
                                       const LaiueGraphicsBufferDescV1 *desc,
                                       LaiueGraphicsHandle *out)
{
    DemoValidationDevice *state = (DemoValidationDevice *)device->context;
    if (desc == NULL || out == NULL || desc->sizeBytes == 0u)
        return 0u;
    *out = ++state->nextHandle;
    ++state->liveBuffers;
    return 1u;
}

static uint32_t ValidationUpload(LaiueGraphicsDeviceV2 *device,
                                 const LaiueGraphicsBufferUploadV1 *upload)
{
    DemoValidationDevice *state = (DemoValidationDevice *)device->context;
    if (upload == NULL || upload->buffer == 0u || upload->data == NULL)
        return 0u;
    state->uploaded += upload->sizeBytes;
    return 1u;
}

static void ValidationDestroy(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle handle)
{
    DemoValidationDevice *state = (DemoValidationDevice *)device->context;
    if (handle != 0u && state->liveBuffers != 0u)
        --state->liveBuffers;
}

static void InitializeValidationDevice(Demo *demo)
{
    demo->validation.device = (LaiueGraphicsDeviceV2){
        .structSize = sizeof(LaiueGraphicsDeviceV2),
        .abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION,
        .context = &demo->validation,
        .createBuffer = ValidationCreateBuffer,
        .uploadBuffer = ValidationUpload,
        .destroyHandle = ValidationDestroy,
    };
    demo->device = &demo->validation.device;
}

static uint32_t LoadModules(LaiueModuleHost *host, uint32_t offscreen)
{
    LaiueModuleDiagnostic diagnostic = {0};
#if defined(LAIUE_MESH_DEMO_DYNAMIC)
#if defined(_WIN32)
#define DEMO_BINARY(name) L"laiue_" name L".dll"
#elif defined(__APPLE__)
#define DEMO_BINARY(name) L"liblaiue_" name L".dylib"
#else
#define DEMO_BINARY(name) L"liblaiue_" name L".so"
#endif
    const wchar_t *names[DEMO_MODULE_CAPACITY] = {
        DEMO_BINARY(L"content"),
        DEMO_BINARY(L"model"),
        DEMO_BINARY(L"mesh_world"),
        DEMO_BINARY(L"mesh_world_render"),
    };
    uint32_t count = 4u;
    const char *graphicsId = NULL;
#if defined(LAIUE_MESH_DEMO_GRAPHICS)
    const wchar_t *graphicsName = DEMO_BINARY(L"render");
#if defined(LAIUE_MESH_DEMO_GRAPHICS_D3D12)
    graphicsName = DEMO_BINARY(L"graphics_d3d12");
    graphicsId = "laiue.graphics.d3d12";
#elif defined(LAIUE_MESH_DEMO_GRAPHICS_VULKAN)
    graphicsName = DEMO_BINARY(L"graphics_vulkan");
    graphicsId = "laiue.graphics.vulkan";
#endif
#if defined(LAIUE_MESH_DEMO_OFFSCREEN_VULKAN)
    if (offscreen)
    {
        graphicsName = DEMO_BINARY(L"graphics_vulkan");
        graphicsId = "laiue.graphics.vulkan";
    }
#endif
    names[count++] = graphicsName;
#endif
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    names[count++] = DEMO_BINARY(L"window");
    names[count++] = DEMO_BINARY(L"input");
#endif
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t paths[DEMO_MODULE_CAPACITY][LAIUE_PLATFORM_PATH_CAPACITY];
    LaiueModuleBinaryV1 binaries[DEMO_MODULE_CAPACITY] = {0};
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY))
        return 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!JoinPath(paths[index], directory, names[index]))
            return 0u;
        binaries[index].path = paths[index];
        binaries[index].flags = LAIUE_MODULE_BINARY_OPTIONAL;
    }
    LaiueModuleProviderSelectionV1 selection = {
        .structSize = sizeof(selection),
        .serviceName = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
        .moduleId = graphicsId,
    };
    LaiueModuleProfileV1 profile = {
        .structSize = sizeof(profile),
        .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
        .binaries = binaries,
        .binaryCount = count,
        .providerSelections = graphicsId == NULL ? NULL : &selection,
        .providerSelectionCount = graphicsId == NULL ? 0u : 1u,
    };
    LaiueModuleStatus status = LaiueModuleHostLoadProfileV1(host, &profile, NULL, &diagnostic);
#else
    const LaiueModuleApiV1 *apis[DEMO_MODULE_CAPACITY] = {0};
    uint32_t count = 0u;
#if defined(LAIUE_MESH_DEMO_STATIC_CONTENT)
    apis[count++] = LaiueContentGetStaticModuleApiV1();
#endif
#if defined(LAIUE_MESH_DEMO_STATIC_MODEL)
    apis[count++] = LaiueModelGetStaticModuleApiV1();
#endif
#if defined(LAIUE_MESH_DEMO_STATIC_MESH_WORLD)
    apis[count++] = LaiueMeshWorldGetStaticModuleApiV1();
#endif
#if defined(LAIUE_MESH_DEMO_STATIC_MESH_WORLD_RENDER)
    apis[count++] = LaiueMeshWorldRenderGetStaticModuleApiV1();
#endif
#if defined(LAIUE_MESH_DEMO_GRAPHICS)
    apis[count++] = LaiueGraphicsGetStaticModuleApiV1();
#endif
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    apis[count++] = LaiueWindowGetStaticModuleApiV1();
    apis[count++] = LaiueInputGetStaticModuleApiV1();
#endif
    LaiueModuleStatus status =
        count == 0u ? LAIUE_MODULE_OK : LaiueModuleHostLoadStatic(host, apis, count, &diagnostic);
#endif
    (void)offscreen;
    if (status != LAIUE_MODULE_OK && status != LAIUE_MODULE_PARTIAL)
    {
        PlatformWriteConsoleUtf8("mesh world: module profile failed: ");
        PlatformWriteConsoleUtf8(diagnostic.message);
        PlatformWriteConsoleUtf8("\n");
        return 0u;
    }
    return 1u;
}

static void QueryServices(Demo *demo, const LaiueModuleHost *host)
{
#define DEMO_QUERY(type, name, version)                                                            \
    (const type *)LaiueModuleHostQueryService(host, name, version, sizeof(type), NULL, NULL)
    demo->content = DEMO_QUERY(LaiueContentServiceV1, LAIUE_CONTENT_SERVICE_NAME, 1u);
    demo->model = DEMO_QUERY(LaiueModelServiceV1, LAIUE_MODEL_SERVICE_NAME, 1u);
    demo->worldService = DEMO_QUERY(LaiueMeshWorldServiceV1, LAIUE_MESH_WORLD_SERVICE_NAME, 1u);
    demo->renderService = (const LaiueMeshWorldRenderServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_MESH_WORLD_RENDER_SERVICE_NAME, 1u,
        (uint32_t)offsetof(LaiueMeshWorldRenderServiceV1, submit), NULL, &demo->renderServiceSize);
    demo->graphics = (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
        host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, 2u,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, NULL, &demo->graphicsSize);
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    demo->windowService = DEMO_QUERY(LaiueWindowServiceV1, LAIUE_WINDOW_SERVICE_NAME, 1u);
    demo->inputService = DEMO_QUERY(LaiueInputServiceV1, LAIUE_INPUT_SERVICE_NAME, 1u);
#endif
#undef DEMO_QUERY
}

#if defined(LAIUE_MESH_DEMO_WINDOWED) || defined(LAIUE_MESH_DEMO_OFFSCREEN_VULKAN)
static uint32_t CreateDevice(Demo *demo, void *window, uint32_t backend)
{
    const LaiueGraphicsDeviceServiceV2 *service = demo->graphics;
    if (service == NULL || service->destroyDevice == NULL)
        return 0u;
    uint32_t result = 0u;
    if (HasField(demo->graphicsSize, service->structSize,
                 offsetof(LaiueGraphicsDeviceServiceV2, context), sizeof(service->context)) &&
        service->createDeviceWithContext != NULL && service->context != NULL)
        result = service->createDeviceWithContext(service->context, window, 960, 540, backend,
                                                  &demo->device);
    else if (service->createDevice != NULL)
        result = service->createDevice(window, 960, 540, backend, &demo->device);
    if (!result)
        return 0u;
    if (demo->device->beginFrame == NULL || demo->device->submit == NULL ||
        demo->device->endFrame == NULL ||
        !HasField(demo->device->structSize, demo->device->structSize,
                  offsetof(LaiueGraphicsDeviceV2, setCamera), sizeof(demo->device->setCamera)) ||
        demo->device->setCamera == NULL)
    {
        service->destroyDevice(demo->device);
        demo->device = NULL;
        return 0u;
    }
    return 1u;
}
#endif

static uint32_t LoadModel(Demo *demo, const wchar_t *root, const wchar_t *name, uint32_t id,
                          uint32_t *outAvailable)
{
    *outAvailable = 0u;
    if (demo->model == NULL)
        return 1u;
    uint32_t status = 0u;
    LaiueModelV1 *model = NULL;
    if (demo->catalog != NULL)
        model = demo->model->loadFrom(demo->catalog, name, &status);
    else
    {
        static wchar_t pack[LAIUE_PLATFORM_PATH_CAPACITY], relative[LAIUE_PLATFORM_PATH_CAPACITY];
        static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
        if (!JoinPath(pack, root, L"models/Demo.lop") || !JoinPath(relative, pack, name))
            return 0u;
        /* Explicit loading remains available after removal of the catalog.
         * Extension is appended to the bounded relative path, not joined. */
        uint32_t length = 0u;
        while (relative[length] != L'\0')
            ++length;
        if (length + 5u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return 0u;
        memcpy(path, relative, length * sizeof(wchar_t));
        memcpy(path + length, L".obj", 5u * sizeof(wchar_t));
        model = demo->model->loadFile(path, &status);
    }
    if (model == NULL)
        return 0u;
    LaiueModelViewV1 view = {.structSize = sizeof(view)};
    uint32_t result = demo->model->getView(model, &view);
    if (result && (status != LAIUE_MODEL_LOAD_OK || (view.flags & LAIUE_MODEL_FLAG_PLACEHOLDER)))
        PlatformWriteConsoleUtf8(
            "mesh world: model unavailable; using the model pack placeholder\n");
    if (result && demo->world != NULL)
    {
        LaiueMeshShapeV1 shape = {
            .structSize = sizeof(shape),
            .flags = LAIUE_MESH_SHAPE_COLLIDE_BOUNDS,
            .boxes = (const LaiueMeshBoxV1 *)view.boxes,
            .boxCount = view.boxCount,
        };
        memcpy(shape.boundsMin, view.boundsMin, sizeof(shape.boundsMin));
        memcpy(shape.boundsMax, view.boundsMax, sizeof(shape.boundsMax));
        result = demo->worldService->registerShape(demo->world, id, &shape);
    }
    if (result && demo->renderer != NULL)
    {
        LaiueMeshRenderPartV1 *parts =
            view.partCount == 0u
                ? NULL
                : (LaiueMeshRenderPartV1 *)PlatformAllocate(view.partCount * sizeof(*parts), false);
        if (view.partCount != 0u && parts == NULL)
            result = 0u;
        for (uint32_t index = 0u; result && index < view.partCount; ++index)
        {
            parts[index].firstIndex = view.parts[index].firstIndex;
            parts[index].indexCount = view.parts[index].indexCount;
            parts[index].material = id * 8u + index;
        }
        LaiueMeshRenderModelV1 geometry = {
            .structSize = sizeof(geometry),
            .vertices = (const LaiueMeshRenderVertexV1 *)view.vertices,
            .vertexCount = view.vertexCount,
            .indices = view.indices,
            .indexCount = view.indexCount,
            .parts = parts,
            .partCount = view.partCount,
        };
        if (result)
            result = demo->renderService->registerModel(demo->renderer, id, &geometry);
        PlatformFree(parts);
    }
    if (result)
        *outAvailable = id;
    demo->model->release(model);
    return result;
}

static uint32_t InitializeWorld(Demo *demo, const wchar_t *root)
{
    if (demo->content != NULL)
    {
        demo->catalog = demo->content->createCatalog(root);
        if (demo->catalog == NULL)
            return 0u;
    }
    if (demo->worldService != NULL)
    {
        LaiueMeshWorldConfigV1 config = {
            .structSize = sizeof(config),
            .cellSize = MESH_DEMO_CELL_SIZE,
            .maximumInstances = 8192u,
        };
        if (!demo->worldService->create(&config, &demo->world))
            return 0u;
    }
    if (demo->world != NULL && demo->renderService != NULL)
    {
        LaiueMeshWorldRendererConfigV1 config = {
            .structSize = sizeof(config),
            .worldService = demo->worldService,
            .world = demo->world,
            .device = demo->device,
            .maximumBuffers = DEMO_DRAW_CAPACITY,
        };
        if (!demo->renderService->create(&config, &demo->renderer))
            return 0u;
        LaiueMeshRenderLightingV1 lighting = {
            .structSize = sizeof(lighting),
            .toSun = {-0.4f, -0.3f, 0.85f},
            .sunColor = {0.8f, 0.75f, 0.65f},
            .ambientColor = {0.25f, 0.32f, 0.4f},
        };
        if (!demo->renderService->setLighting(demo->renderer, &lighting))
            return 0u;
    }
    if (!LoadModel(demo, root, L"ground", MESH_DEMO_MODEL_GROUND, &demo->provider.groundModel) ||
        !LoadModel(demo, root, L"tree", MESH_DEMO_MODEL_TREE, &demo->provider.treeModel))
        return 0u;
    demo->player.local[0] = 8.0f;
    demo->player.local[1] = 1.0f;
    demo->player.local[2] = 1.2f;
    demo->pitch = -0.14f;
    if (demo->world != NULL)
    {
        LaiueMeshCellProviderV1 provider = {
            .structSize = sizeof(provider),
            .maximumInstancesPerCell = 3u,
            .context = &demo->provider,
            .populate = MeshDemoPopulate,
        };
        if (!demo->worldService->setProvider(demo->world, &provider) ||
            !demo->worldService->stream(demo->world, &demo->player, 48.0f, 0u, NULL))
            return 0u;
    }
    return 1u;
}

static uint32_t Step(Demo *demo, float forward, float sideways, float speed, uint32_t jump)
{
    if (demo->world == NULL || demo->provider.groundModel == 0u)
        return 1u;
    const float fixedStep = 1.0f / 60.0f;
    const float length = ScalarSqrt(forward * forward + sideways * sideways);
    if (length > 1.0f)
    {
        forward /= length;
        sideways /= length;
    }
    if (jump && demo->grounded)
        demo->verticalSpeed = 7.0f;
    demo->verticalSpeed -= 20.0f * fixedStep;
    const float sy = ScalarSin(demo->yaw), cy = ScalarCos(demo->yaw);
    const float delta[3] = {(sy * forward + cy * sideways) * speed * fixedStep,
                            (cy * forward - sy * sideways) * speed * fixedStep,
                            demo->verticalSpeed * fixedStep};
    const float halfExtent[3] = {0.3f, 0.3f, 0.9f};
    LaiueMeshMoveResultV1 result = {0};
    if (!demo->worldService->moveBox(demo->world, &demo->player, halfExtent, delta, &result))
        return 0u;
    demo->player = result.position;
    demo->grounded = result.grounded;
    if (result.grounded)
        demo->verticalSpeed = 0.0f;
    ++demo->simulationSteps;
    if (demo->player.cell.x > 8 || demo->player.cell.x < -8 || demo->player.cell.y > 8 ||
        demo->player.cell.y < -8 || demo->player.cell.z > 8 || demo->player.cell.z < -8)
        return MeshDemoRebase(demo->worldService, demo->world, &demo->provider, &demo->player);
    return 1u;
}

static uint32_t Stream(Demo *demo)
{
    /* Streaming can allocate cells; it runs outside the allocation-free
     * fixed physics steps. The radius covers the following frame's motion. */
    return demo->world == NULL || demo->provider.groundModel == 0u ||
           demo->worldService->stream(demo->world, &demo->player, 48.0f, 32u, NULL);
}

static uint32_t Draw(Demo *demo, uint32_t width, uint32_t height)
{
    if (width == 0u || height == 0u)
        return 1u;
    LaiueMeshPositionV1 camera = demo->player;
    camera.local[2] += 0.65f;
    LaiueGraphicsCameraV2 view = {.structSize = sizeof(view)};
    MeshDemoCamera(demo->yaw, demo->pitch, (float)width / (float)height, view.viewProjection);
    const uint32_t direct = demo->renderer != NULL && demo->device != &demo->validation.device &&
                            HasField(demo->renderServiceSize, demo->renderService->structSize,
                                     offsetof(LaiueMeshWorldRenderServiceV1, submit),
                                     sizeof(demo->renderService->submit)) &&
                            demo->renderService->submit != NULL;
    uint32_t count = 0u;
    if (demo->renderer != NULL &&
        (!demo->renderService->update(demo->renderer, &camera, 48.0f, 16u, NULL) ||
         !demo->renderService->draws(demo->renderer, &camera, view.viewProjection,
                                     direct ? NULL : demo->draws, direct ? 0u : DEMO_DRAW_CAPACITY,
                                     &count) ||
         (direct == 0u && count > DEMO_DRAW_CAPACITY)))
        return 0u;
    if (demo->device == &demo->validation.device)
        demo->validation.submitted += count;
    else if (!demo->device->setCamera(demo->device, &view) ||
             !demo->device->beginFrame(demo->device, width, height) ||
             (count != 0u &&
              !(direct ? demo->renderService->submit(demo->renderer, &camera, view.viewProjection)
                       : demo->device->submit(demo->device, demo->draws, count))) ||
             !demo->device->endFrame(demo->device))
        return 0u;
    if (count != 0u)
        ++demo->drawnFrames;
    ++demo->frames;
    return 1u;
}

static uint32_t ValidateWorld(Demo *demo)
{
    if (demo->world == NULL || demo->provider.groundModel == 0u)
        return 1u;
    const float halfExtent[3] = {0.3f, 0.3f, 0.9f}, falling[3] = {0.0f, 0.0f, -4.0f};
    LaiueMeshMoveResultV1 result = {0};
    if (demo->worldService->instanceCount(demo->world) == 0u ||
        !demo->worldService->moveBox(demo->world, &demo->player, halfExtent, falling, &result) ||
        !result.grounded || !result.collided)
        return 0u;
    demo->player = result.position;
    demo->grounded = 1u;
    /* A large initial position never becomes a float. Stream, then rebase the
     * same terrain around it; the absolute provider origin preserves its seed. */
    demo->player.cell.x = INT64_C(1000000000000);
    demo->player.cell.y = -INT64_C(1000000000000);
    if (!demo->worldService->stream(demo->world, &demo->player, 48.0f, 0u, NULL) ||
        !MeshDemoRebase(demo->worldService, demo->world, &demo->provider, &demo->player) ||
        !demo->worldService->stream(demo->world, &demo->player, 48.0f, 0u, NULL) ||
        !demo->worldService->moveBox(demo->world, &demo->player, halfExtent, falling, &result) ||
        !result.grounded || demo->player.cell.x != 0 || demo->player.cell.y != 0)
        return 0u;
    demo->player = result.position;
    return 1u;
}

#if defined(LAIUE_MESH_DEMO_WINDOWED)
static void RawInput(void *context, void *nativeInput)
{
    Demo *demo = (Demo *)context;
    if (demo->input != NULL)
        demo->inputService->handleRawInput(demo->input, nativeInput);
}

static void WindowFrame(void *context)
{
    Demo *demo = (Demo *)context;
    const double now = PlatformMonotonicSeconds();
    double elapsed = now - demo->previousTime;
    demo->previousTime = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > 0.25)
        elapsed = 0.25;
    demo->accumulator += elapsed;
    if (demo->windowService->consumeFocusLoss(demo->window) && demo->input != NULL)
    {
        demo->inputService->resetState(demo->input);
        demo->windowService->setMouseLook(demo->window, false);
    }
    float forward = 0.0f, sideways = 0.0f, speed = 4.5f;
    uint32_t jump = 0u;
    if (demo->input != NULL)
    {
        const LaiueInputServiceV1 *input = demo->inputService;
        if (input->consumeKeyPress(demo->input, INPUT_KEY_ESCAPE))
            demo->windowService->requestClose(demo->window);
        if (input->consumeKeyPress(demo->input, INPUT_KEY_E))
            demo->windowService->setMouseLook(
                demo->window, !demo->windowService->isMouseLookEnabled(demo->window));
        if (input->consumeKeyPress(demo->input, INPUT_KEY_F7))
            demo->windowService->setFullscreen(demo->window,
                                               !demo->windowService->isFullscreen(demo->window));
        if (demo->windowService->isMouseLookEnabled(demo->window))
        {
            int32_t x = 0, y = 0;
            input->getMouseDelta(demo->input, &x, &y);
            demo->yaw = ScalarWrap(demo->yaw + (float)x * 0.0025f);
            demo->pitch = ScalarClamp(demo->pitch - (float)y * 0.0025f, -1.45f, 1.45f);
        }
        forward = (input->isKeyDown(demo->input, INPUT_KEY_W) ? 1.0f : 0.0f) -
                  (input->isKeyDown(demo->input, INPUT_KEY_S) ? 1.0f : 0.0f);
        sideways = (input->isKeyDown(demo->input, INPUT_KEY_D) ? 1.0f : 0.0f) -
                   (input->isKeyDown(demo->input, INPUT_KEY_A) ? 1.0f : 0.0f);
        speed = input->isKeyDown(demo->input, INPUT_KEY_SHIFT) ? 9.0f : 4.5f;
        if (demo->accumulator >= 1.0 / 60.0)
            jump = input->consumeKeyPress(demo->input, INPUT_KEY_SPACE);
    }
    if (!Stream(demo))
        demo->failed = 1u;
    while (demo->accumulator >= 1.0 / 60.0 && !demo->failed)
    {
        if (!Step(demo, forward, sideways, speed, jump))
            demo->failed = 1u;
        jump = 0u;
        demo->accumulator -= 1.0 / 60.0;
    }
    int32_t width = 0, height = 0;
    demo->windowService->getClientSize(demo->window, &width, &height);
    if (width > 0 && height > 0)
    {
        if (demo->windowService->consumeResize(demo->window) &&
            demo->device != &demo->validation.device && demo->graphics->resize != NULL)
            demo->graphics->resize(demo->device, width, height);
        if (!Draw(demo, (uint32_t)width, (uint32_t)height))
            demo->failed = 1u;
    }
    if (demo->input != NULL)
        demo->inputService->endFrame(demo->input);
    if (demo->failed || PlatformTerminationRequested() ||
        (demo->maximumWindowFrames != 0u && demo->frames >= demo->maximumWindowFrames))
        demo->windowService->requestClose(demo->window);
    PlatformSleepMilliseconds(1u);
}
#endif

static uint32_t Run(const DemoOptions *options)
{
    static Demo demo;
    memset(&demo, 0, sizeof(demo));
    LaiueModuleHostConfigV1 config = {0};
    LaiueModuleDiagnostic diagnostic = {0};
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
        return 0u;
    uint32_t success = LoadModules(host, options->headless);
    if (!success)
        goto cleanup;
    QueryServices(&demo, host);
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY], root[LAIUE_PLATFORM_PATH_CAPACITY];
    static char environment[LAIUE_PLATFORM_PATH_CAPACITY];
    if (options->assets[0] != L'\0')
        memcpy(root, options->assets, sizeof(root));
    else if (PlatformGetEnvironmentUtf8("LAIUE_MESH_WORLD_ASSETS", environment,
                                        sizeof(environment)) != 0u)
    {
        uint32_t length = 0u;
        while (environment[length] != '\0')
            ++length;
        if (!PlatformUtf8ToWide(environment, length, root, LAIUE_PLATFORM_PATH_CAPACITY, NULL))
        {
            success = 0u;
            goto cleanup;
        }
    }
    else if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
             !JoinPath(root, directory, L"assets_mesh_world"))
    {
        success = 0u;
        goto cleanup;
    }
    uint32_t interactive = 0u;
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    if (!options->headless && demo.windowService != NULL && demo.graphics != NULL)
    {
        const WindowConfiguration window = {
            .title = L"LAIUE Mesh World — WASD / Space / E", .width = 960, .height = 540};
        demo.window = demo.windowService->create(&window);
        if (demo.window != NULL)
        {
            if (CreateDevice(&demo, demo.windowService->getNativeHandle(demo.window),
                             LAIUE_GRAPHICS_BACKEND_AUTO))
                interactive = 1u;
            else
            {
                demo.windowService->destroy(demo.window);
                demo.window = NULL;
            }
        }
    }
#endif
    if (!interactive)
    {
        if (options->requireWindow)
        {
            PlatformWriteConsoleUtf8(
                "mesh world: --frames requires a working window and graphics provider\n");
            success = 0u;
            goto cleanup;
        }
#if defined(LAIUE_MESH_DEMO_OFFSCREEN_VULKAN)
        if (demo.graphics != NULL && !CreateDevice(&demo, NULL, LAIUE_GRAPHICS_BACKEND_VULKAN))
        {
            /* A normal desktop launch may have selected a D3D12-only
             * provider before losing its window. Its independent CPU
             * features still work. An explicitly requested offscreen run
             * with the Vulkan provider must actually create that device. */
            if (options->headless)
            {
                PlatformWriteConsoleUtf8("mesh world: Vulkan offscreen device creation failed\n");
                success = 0u;
                goto cleanup;
            }
        }
#endif
        if (demo.device == NULL)
        {
            InitializeValidationDevice(&demo);
            PlatformWriteConsoleUtf8(
                "mesh world: offscreen graphics unavailable; validating CPU geometry\n");
        }
    }
    if (demo.model == NULL || demo.worldService == NULL || demo.renderService == NULL)
        PlatformWriteConsoleUtf8("mesh world: optional module absent; its feature is disabled\n");
    if (demo.content == NULL && demo.model != NULL)
        PlatformWriteConsoleUtf8(
            "mesh world: content catalog absent; loading explicit Demo.lop OBJ files\n");
    success = InitializeWorld(&demo, root);
    if (!success)
        goto cleanup;
    (void)PlatformInstallTerminationHandler();
    if (interactive)
    {
#if defined(LAIUE_MESH_DEMO_WINDOWED)
        if (demo.inputService != NULL)
            demo.input =
                demo.inputService->create(demo.windowService->getNativeHandle(demo.window));
        if (demo.input == NULL)
            PlatformWriteConsoleUtf8("mesh world: input unavailable; camera controls disabled\n");
        demo.windowService->setRawInputCallback(demo.window, RawInput, &demo);
        demo.windowService->setMouseLook(demo.window, demo.input != NULL);
        demo.previousTime = PlatformMonotonicSeconds();
        demo.maximumWindowFrames = options->frames;
        PlatformWriteConsoleUtf8("mesh world: WASD move, mouse look, Space jump, Shift run, E "
                                 "capture, F7 fullscreen, Esc exit\n");
        demo.windowService->runLoop(demo.window, WindowFrame, &demo);
        demo.windowService->setRawInputCallback(demo.window, NULL, NULL);
        demo.windowService->setMouseLook(demo.window, false);
        if (options->requireWindow && demo.renderer != NULL && demo.provider.groundModel != 0u &&
            demo.drawnFrames == 0u)
            demo.failed = 1u;
#endif
    }
    else
    {
        success = ValidateWorld(&demo);
        uint32_t frameCount = options->headless ? options->frames : 12u;
        /* Six fixed steps per diagnostic frame exercise walking even in a
         * short CTest run; the real window loop uses the wall clock. */
        for (uint32_t frame = 0u; success && frame < frameCount; ++frame)
        {
            success = Stream(&demo);
            for (uint32_t step = 0u; success && step < 6u; ++step)
                success = Step(&demo, 1.0f, frame < frameCount / 2u ? -0.4f : 0.4f, 4.5f, 0u);
            if (success)
                success = Draw(&demo, 960u, 540u);
        }
        if (success && demo.renderer != NULL && demo.provider.groundModel != 0u &&
            (demo.drawnFrames == 0u || demo.simulationSteps == 0u ||
             (demo.device == &demo.validation.device && demo.validation.uploaded == 0u)))
            success = 0u;
        if (success &&
            (demo.model == NULL || demo.worldService == NULL || demo.renderService == NULL))
            PlatformWriteConsoleUtf8(
                "mesh world: headless available features passed (partial profile)\n");
        else if (success)
            PlatformWriteConsoleUtf8(
                demo.device == &demo.validation.device
                    ? "mesh world: headless world/movement/rebase/geometry validation passed\n"
                    : "mesh world: headless world/movement/rebase/offscreen frames passed\n");
    }
    PlatformRemoveTerminationHandler();
    success = success && !demo.failed;
cleanup:
    if (demo.renderer != NULL)
        demo.renderService->destroy(demo.renderer);
    if (demo.device != NULL && demo.device != &demo.validation.device)
        demo.graphics->destroyDevice(demo.device);
#if defined(LAIUE_MESH_DEMO_WINDOWED)
    if (demo.input != NULL)
        demo.inputService->destroy(demo.input);
    if (demo.window != NULL)
        demo.windowService->destroy(demo.window);
#endif
    if (demo.world != NULL)
        demo.worldService->destroy(demo.world);
    if (demo.catalog != NULL)
        demo.content->destroyCatalog(demo.catalog);
    if (demo.validation.liveBuffers != 0u)
        success = 0u;
    LaiueModuleHostDestroy(host);
    if (!success)
        PlatformWriteConsoleUtf8("mesh world: validation or runtime operation failed\n");
    return success;
}

static void Usage(void)
{
    PlatformWriteConsoleUtf8(
        "Usage: laiue_mesh_world_demo [--headless N | --frames N] [--assets ROOT]\n"
        "ROOT contains models/active.txt and a .lop model pack.\n"
        "Default: assets_mesh_world beside the executable; env "
        "LAIUE_MESH_WORLD_ASSETS overrides it.\n");
}

#if defined(_WIN32)
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);
__declspec(dllimport) const wchar_t *__stdcall GetCommandLineW(void);

void MeshWorldDemoEntryPoint(void)
{
    static DemoOptions options;
    uint32_t pending = 0u, argument = 0u, valid = 1u;
    const wchar_t *command = GetCommandLineW();
    while (command != NULL && *command != L'\0' && valid)
    {
        while (*command == L' ' || *command == L'\t')
            ++command;
        if (*command == L'\0')
            break;
        static wchar_t token[LAIUE_PLATFORM_PATH_CAPACITY];
        uint32_t length = 0u, quoted = 0u;
        while (*command != L'\0' && (quoted || (*command != L' ' && *command != L'\t')))
        {
            if (*command == L'"')
                quoted = !quoted;
            else if (length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
                token[length++] = *command;
            else
                valid = 0u;
            ++command;
        }
        token[length] = L'\0';
        if (quoted || (argument++ != 0u && !ParseToken(&options, token, &pending)))
            valid = 0u;
    }
    valid = valid && !(options.headless && options.requireWindow);
    if (!valid || pending != 0u || options.help)
    {
        Usage();
        ExitProcess(valid && pending == 0u ? 0u : 2u);
    }
    ExitProcess(Run(&options) ? 0u : 1u);
}
#else
int main(int argc, char **argv)
{
    DemoOptions options = {0};
    uint32_t pending = 0u, valid = 1u;
    for (int index = 1; index < argc && valid; ++index)
    {
        wchar_t token[LAIUE_PLATFORM_PATH_CAPACITY];
        uint32_t length = 0u;
        while (argv[index][length] != '\0')
            ++length;
        valid =
            PlatformUtf8ToWide(argv[index], length, token, LAIUE_PLATFORM_PATH_CAPACITY, NULL) &&
            ParseToken(&options, token, &pending);
    }
    valid = valid && !(options.headless && options.requireWindow);
    if (!valid || pending != 0u || options.help)
    {
        Usage();
        return valid && pending == 0u ? 0 : 2;
    }
    return Run(&options) ? 0 : 1;
}
#endif
