#include "character/character_service.h"
#include "graphics/graphics_device_service.h"
#include "render/chunk_geometry.h"
#include "mod/module_host.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "voxel/voxel_service.h"
#include "world/world_service.h"
#include "scene/scene_service.h"
#include "scene/math_service.h"
#include "walk_terrain.h"
#include "walk_runtime.h"

#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ANDROID_WALK_LOG_TAG "laiue.walk"
#define ANDROID_WALK_HALF_EXTENT INT64_C(400)
#define ANDROID_WALK_ACTIVE_CHUNK_RADIUS 1
#define ANDROID_WALK_ACTIVE_CHUNK_DIAMETER \
    (ANDROID_WALK_ACTIVE_CHUNK_RADIUS * 2 + 1)
#define ANDROID_WALK_ACTIVE_CHUNK_COUNT \
    (ANDROID_WALK_ACTIVE_CHUNK_DIAMETER * ANDROID_WALK_ACTIVE_CHUNK_DIAMETER)

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void);

typedef struct AndroidWalkState AndroidWalkState;

struct AndroidWalkState
{
    struct android_app *app;
    LaiueModuleHost *host;
    const LaiueCharacterServiceV1 *character;
    const LaiueVoxelServiceV1 *voxel;
    uint32_t voxelServiceSize;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    uint32_t graphicsServiceSize;
    LaiueCharacterControllerV1 *controller;
    LaiueVoxelWorldV1 *world;
    LaiueVoxelProviderV1 provider;
    WalkVoxelContext walkContext;
    LaiueGraphicsDeviceV2 *device;
    LaiueGraphicsHandle terrainBuffer;
    bool terrainReady;
    const LaiueSceneServiceV1 *scene;
    const LaiueSceneMathServiceV1 *sceneMath;
    Camera camera;
    float cameraRelativeEye[3];
    float terrainOriginRelative[3];
    float viewProjection[16];
    LaiueVoxelProviderV1 walkProvider;
    bool windowReady;
    bool running;
    bool touchActive;
    bool joystickActive;
    bool lookActive;
    float joystickOriginX;
    float joystickOriginY;
    float joystickX;
    float joystickY;
    float lastLookX;
    float lastLookY;
    int32_t lookDeltaX;
    int32_t lookDeltaY;
    int32_t joystickPointerId;
    int32_t lookPointerId;
    bool jumpPending;
    bool keyDown[10];
    double lastTime;
    double accumulator;
    int32_t width;
    int32_t height;
};

static void AndroidLog(AndroidWalkState *state, int priority, const char *message)
{
    (void)state;
    __android_log_print(priority, ANDROID_WALK_LOG_TAG, "%s", message == NULL ? "" : message);
}

static uint32_t AndroidLoadModules(AndroidWalkState *state)
{
    const LaiueModuleApiV1 *modules[7] = {
        LaiueCharacterGetStaticModuleApiV1(),
        LaiueGraphicsGetStaticModuleApiV1(),
        LaiueSceneMathGetStaticModuleApiV1(),
        LaiueSceneGetStaticModuleApiV1(),
    };
    uint32_t moduleCount = 4u;
#if defined(LAIUE_ANDROID_WALK_WITH_VOXEL)
    modules[moduleCount++] = LaiueNumericGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueWorldGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueVoxelGetStaticModuleApiV1();
#endif
    LaiueModuleDiagnostic diagnostic;
    const LaiueModuleStatus status = LaiueModuleHostLoadStatic(
        state->host, modules, moduleCount, &diagnostic);
    if (status != LAIUE_MODULE_OK)
    {
        AndroidLog(state, ANDROID_LOG_ERROR, diagnostic.message);
        return 0u;
    }
    state->character = (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        sizeof(LaiueCharacterServiceV1), NULL, NULL);
    state->voxel = (const LaiueVoxelServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_VOXEL_SERVICE_NAME, LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        LAIUE_VOXEL_SERVICE_V1_LEGACY_SIZE, NULL, &state->voxelServiceSize);
    state->graphicsServiceSize = 0u;
    state->graphics = (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
        state->host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, NULL,
        &state->graphicsServiceSize);
    state->scene = (const LaiueSceneServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_SCENE_SERVICE_NAME, LAIUE_SCENE_SERVICE_ABI_VERSION_1,
        sizeof(LaiueSceneServiceV1), NULL, NULL);
    state->sceneMath = (const LaiueSceneMathServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_SCENE_MATH_SERVICE_NAME,
        LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
        sizeof(LaiueSceneMathServiceV1), NULL, NULL);
    if (state->character == NULL)
        AndroidLog(state, ANDROID_LOG_WARN, "character module unavailable; controls disabled");
    return state->graphics != NULL;
}

static void AndroidDestroyDevice(AndroidWalkState *state)
{
    if (state != NULL && state->terrainReady && state->device != NULL &&
        state->device->destroyHandle != NULL)
        state->device->destroyHandle(state->device, state->terrainBuffer);
    if (state != NULL)
    {
        state->terrainReady = false;
        state->terrainBuffer = 0u;
    }
    if (state->device != NULL && state->graphics != NULL && state->graphics->destroyDevice != NULL)
        state->graphics->destroyDevice(state->device);
    state->device = NULL;
    state->windowReady = false;
    state->running = false;
    state->touchActive = false;
    state->joystickActive = false;
    state->lookActive = false;
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    state->joystickPointerId = -1;
    state->lookPointerId = -1;
    state->joystickActive = false;
    state->lookActive = false;
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    state->joystickPointerId = -1;
    state->lookPointerId = -1;
    state->jumpPending = false;
    memset(state->keyDown, 0, sizeof(state->keyDown));
    state->accumulator = 0.0;
    state->lastTime = 0.0;
}

static void AndroidResetInputClock(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    state->touchActive = false;
    state->jumpPending = false;
    memset(state->keyDown, 0, sizeof(state->keyDown));
    state->accumulator = 0.0;
    state->lastTime = PlatformMonotonicSeconds();
}

static void AndroidCreateDevice(AndroidWalkState *state)
{
    if (state == NULL || state->app == NULL || state->app->window == NULL || state->graphics == NULL)
        return;
    AndroidDestroyDevice(state);
    const int32_t width = ANativeWindow_getWidth(state->app->window);
    const int32_t height = ANativeWindow_getHeight(state->app->window);
    uint32_t created = 0u;
    if (width > 0 && height > 0 &&
        state->graphicsServiceSize >= LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE &&
        state->graphics->createDeviceWithContext != NULL &&
        state->graphics->context != NULL)
        created = state->graphics->createDeviceWithContext(
            state->graphics->context, state->app->window, width, height,
            LAIUE_GRAPHICS_BACKEND_VULKAN, &state->device);
    else if (width > 0 && height > 0 && state->graphics->createDevice != NULL)
        created = state->graphics->createDevice(
            state->app->window, width, height, LAIUE_GRAPHICS_BACKEND_VULKAN,
            &state->device);
    if (created == 0u)
    {
        AndroidLog(state, ANDROID_LOG_ERROR, "Vulkan device/surface creation failed");
        return;
    }
    state->windowReady = true;
    state->running = true;
    state->width = width;
    state->height = height;
    state->lastTime = PlatformMonotonicSeconds();
    if (state->device->createBuffer != NULL && state->device->uploadBuffer != NULL)
    {
        ChunkQuad quads[5];
        const uint32_t quadCount = WalkBuildTerrainQuads(quads);
        LaiueGraphicsBufferDescV1 description = {
            .structSize = sizeof(description),
            .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX_PULLING,
            .sizeBytes = (uint64_t)quadCount * sizeof(quads[0]),
        };
        LaiueGraphicsBufferUploadV1 upload = {
            .structSize = sizeof(upload),
            .data = quads,
            .sizeBytes = (uint64_t)quadCount * sizeof(quads[0]),
        };
        state->terrainReady = state->device->createBuffer(
            state->device, &description, &state->terrainBuffer) != 0u;
        if (state->terrainReady)
        {
            upload.buffer = state->terrainBuffer;
            state->terrainReady = state->device->uploadBuffer(
                state->device, &upload) != 0u;
        }
        if (!state->terrainReady && state->terrainBuffer != 0u &&
            state->device->destroyHandle != NULL)
        {
            state->device->destroyHandle(state->device, state->terrainBuffer);
            state->terrainBuffer = 0u;
        }
    }
    if (state->scene != NULL && state->scene->cameraInit != NULL)
        state->scene->cameraInit(&state->camera, 0.0, 0.0, 0.0, 0.0f, 0.0f);
}

static int32_t AndroidKeyIndex(int32_t keyCode)
{
    switch (keyCode)
    {
        case AKEYCODE_W: return 0;
        case AKEYCODE_A: return 1;
        case AKEYCODE_S: return 2;
        case AKEYCODE_D: return 3;
        case AKEYCODE_SHIFT_LEFT:
        case AKEYCODE_SHIFT_RIGHT: return 4;
        case AKEYCODE_SPACE: return 5;
        case AKEYCODE_ESCAPE: return 6;
        default: return -1;
    }
}

static int32_t AndroidHandleInput(struct android_app *app, AInputEvent *event)
{
    AndroidWalkState *state = (AndroidWalkState *)app->userData;
    if (state == NULL || event == NULL)
        return 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
    {
        const int32_t index = AndroidKeyIndex(AKeyEvent_getKeyCode(event));
        if (index < 0)
            return 0;
        const int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN)
        {
            if (index == 5 && !state->keyDown[index])
                state->jumpPending = true;
            state->keyDown[index] = true;
        }
        else if (action == AKEY_EVENT_ACTION_UP)
            state->keyDown[index] = false;
        return 1;
    }
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    const int32_t rawAction = AMotionEvent_getAction(event);
    const int32_t action = rawAction & AMOTION_EVENT_ACTION_MASK;
    const size_t actionIndex = (size_t)(rawAction >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t pointerCount = AMotionEvent_getPointerCount(event);
    if (pointerCount == 0u || actionIndex >= pointerCount)
        return 0;
    const float width = state->width > 0 ? (float)state->width : 1.0f;
    const int32_t pointerId = AMotionEvent_getPointerId(event, actionIndex);
    const float x = AMotionEvent_getX(event, actionIndex);
    const float y = AMotionEvent_getY(event, actionIndex);
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
    {
        state->touchActive = true;
        state->jumpPending = true;
        if (x < width * 0.5f && state->joystickPointerId < 0)
        {
            state->joystickPointerId = pointerId;
            state->joystickActive = true;
            state->joystickOriginX = x;
            state->joystickOriginY = y;
            state->joystickX = 0.0f;
            state->joystickY = 0.0f;
        }
        else if (state->lookPointerId < 0)
        {
            state->lookPointerId = pointerId;
            state->lookActive = true;
            state->lastLookX = x;
            state->lastLookY = y;
        }
    }
    else if (action == AMOTION_EVENT_ACTION_MOVE)
    {
        for (size_t pointer = 0u; pointer < pointerCount; ++pointer)
        {
            const int32_t id = AMotionEvent_getPointerId(event, pointer);
            const float px = AMotionEvent_getX(event, pointer);
            const float py = AMotionEvent_getY(event, pointer);
            if (id == state->joystickPointerId)
            {
                const float radius = 160.0f;
                float dx = (px - state->joystickOriginX) / radius;
                float dy = (py - state->joystickOriginY) / radius;
                if (dx > 1.0f) dx = 1.0f;
                if (dx < -1.0f) dx = -1.0f;
                if (dy > 1.0f) dy = 1.0f;
                if (dy < -1.0f) dy = -1.0f;
                state->joystickX = dx;
                state->joystickY = -dy;
            }
            else if (id == state->lookPointerId)
            {
                state->lookDeltaX += (int32_t)(px - state->lastLookX);
                state->lookDeltaY += (int32_t)(py - state->lastLookY);
                state->lastLookX = px;
                state->lastLookY = py;
            }
        }
    }
    else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP ||
             action == AMOTION_EVENT_ACTION_CANCEL)
    {
        if (pointerId == state->joystickPointerId)
        {
            state->joystickPointerId = -1;
            state->joystickActive = false;
            state->joystickX = 0.0f;
            state->joystickY = 0.0f;
        }
        if (pointerId == state->lookPointerId || action == AMOTION_EVENT_ACTION_CANCEL)
        {
            state->lookPointerId = -1;
            state->lookActive = false;
        }
        state->touchActive = state->joystickActive || state->lookActive;
    }
    return 1;
}

static void AndroidHandleCommand(struct android_app *app, int32_t command)
{
    AndroidWalkState *state = (AndroidWalkState *)app->userData;
    if (state == NULL)
        return;
    switch (command)
    {
        case APP_CMD_INIT_WINDOW:
            AndroidCreateDevice(state);
            break;
        case APP_CMD_TERM_WINDOW:
            AndroidDestroyDevice(state);
            break;
        case APP_CMD_GAINED_FOCUS:
            if (state->windowReady)
            {
                AndroidResetInputClock(state);
                state->running = true;
            }
            break;
        case APP_CMD_LOST_FOCUS:
            state->running = false;
            AndroidResetInputClock(state);
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (state->windowReady && state->app->window != NULL && state->graphics != NULL &&
                state->graphics->resize != NULL)
                state->graphics->resize(state->device,
                                        ANativeWindow_getWidth(state->app->window),
                                        ANativeWindow_getHeight(state->app->window));
            break;
        default:
            break;
    }
}

static int64_t AndroidFloorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if (value % divisor < 0)
        --quotient;
    return quotient;
}

static void AndroidUpdateCamera(AndroidWalkState *state, int32_t width, int32_t height,
                                float elapsed)
{
    if (state == NULL || state->scene == NULL || state->sceneMath == NULL ||
        state->scene->cameraGetViewMatrix == NULL ||
        state->scene->cameraGetProjectionMatrix == NULL ||
        state->sceneMath->matrix4Multiply == NULL || state->device == NULL ||
        state->device->setCamera == NULL)
        return;
    int64_t blockX = 0;
    int64_t blockY = 0;
    int64_t localZ = 1400;
    if (state->controller != NULL && state->character != NULL &&
        state->character->getPosition != NULL)
    {
        LaiueCharacterPositionV1 position;
        if (state->character->getPosition(state->controller, &position) != 0u)
        {
            const int64_t blocksPerCell =
                LAIUE_CHARACTER_LOCAL_CELL_SIZE / 1000;
            blockX = position.cellX * blocksPerCell + position.localX / 1000;
            blockY = position.cellY * blocksPerCell + position.localY / 1000;
            localZ = position.localZ;
        }
    }
    const int64_t originX = AndroidFloorDiv(blockX, 64) * 64;
    const int64_t originY = AndroidFloorDiv(blockY, 64) * 64;
    int64_t fractionX = state->controller != NULL ? 0 : 0;
    int64_t fractionY = state->controller != NULL ? 0 : 0;
    if (state->controller != NULL && state->character->getPosition != NULL)
    {
        LaiueCharacterPositionV1 position;
        if (state->character->getPosition(state->controller, &position) != 0u)
        {
            fractionX = position.localX % 1000;
            fractionY = position.localY % 1000;
            if (fractionX < 0) fractionX += 1000;
            if (fractionY < 0) fractionY += 1000;
        }
    }
    state->terrainOriginRelative[0] = 0.0f;
    state->terrainOriginRelative[1] = 0.0f;
    state->terrainOriginRelative[2] = 0.0f;
    state->cameraRelativeEye[0] = (float)(blockX - originX) + (float)fractionX / 1000.0f;
    state->cameraRelativeEye[1] = (float)(blockY - originY) + (float)fractionY / 1000.0f;
    state->cameraRelativeEye[2] = (float)localZ / 1000.0f + 1.6f;
    if (state->scene->cameraUpdate != NULL)
        state->scene->cameraUpdate(&state->camera, elapsed, false, false, false, false, false,
                                   state->lookDeltaX, state->lookDeltaY, 0.0f, 0.0025f);
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    float view[16];
    float projection[16];
    state->scene->cameraGetViewMatrix(&state->camera, state->cameraRelativeEye, view);
    state->scene->cameraGetProjectionMatrix(
        height > 0 ? (float)width / (float)height : 1.0f,
        1.04719755f, 0.05f, 4096.0f, projection);
    state->sceneMath->matrix4Multiply(view, projection, state->viewProjection);
    LaiueGraphicsCameraV2 camera = {
        .structSize = sizeof(camera),
        .flags = 0u,
    };
    memcpy(camera.viewProjection, state->viewProjection, sizeof(camera.viewProjection));
    (void)state->device->setCamera(state->device, &camera);
}

static void AndroidStep(AndroidWalkState *state)
{
    if (state == NULL || !state->windowReady)
        return;
    const double now = PlatformMonotonicSeconds();
    double elapsed = state->lastTime == 0.0 ? 0.0 : now - state->lastTime;
    state->lastTime = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > 0.25)
        elapsed = 0.25;
    state->accumulator += elapsed;
    const double fixedStep = 1.0 / (double)LAIUE_CHARACTER_TICK_HZ;
    uint32_t ticks = 0u;
    while (state->controller != NULL && state->character != NULL &&
           state->accumulator >= fixedStep && ticks < 8u)
    {
        LaiueCharacterInputV1 input = {0};
        input.moveX = (state->keyDown[3] ? 1 : 0) - (state->keyDown[1] ? 1 : 0);
        input.moveY = (state->keyDown[0] ? 1 : 0) - (state->keyDown[2] ? 1 : 0);
        if (state->joystickActive)
        {
            input.moveX = state->joystickX > 0.35f ? 1 : (state->joystickX < -0.35f ? -1 : 0);
            input.moveY = state->joystickY > 0.35f ? 1 : (state->joystickY < -0.35f ? -1 : 0);
        }
        if (state->keyDown[4] || state->joystickActive)
            input.flags |= LAIUE_CHARACTER_INPUT_SPRINT;
        if (state->jumpPending)
        {
            input.flags |= LAIUE_CHARACTER_INPUT_JUMP;
            state->jumpPending = false;
        }
        if (state->character == NULL || state->controller == NULL ||
            state->character->step(state->controller, &input) == 0u)
        {
            AndroidLog(state, ANDROID_LOG_ERROR, "deterministic character step failed");
            state->running = false;
            break;
        }
        state->accumulator -= fixedStep;
        ++ticks;
    }
    if (ticks == 8u && state->accumulator >= fixedStep)
        state->accumulator = 0.0;
    if (state->device != NULL && state->device->beginFrame != NULL &&
        state->device->endFrame != NULL)
    {
        const int32_t width = ANativeWindow_getWidth(state->app->window);
        const int32_t height = ANativeWindow_getHeight(state->app->window);
        if ((width != state->width || height != state->height) && state->graphics->resize != NULL)
        {
            state->graphics->resize(state->device, width, height);
            state->width = width;
            state->height = height;
        }
        if (width > 0 && height > 0)
        {
            AndroidUpdateCamera(state, width, height, (float)elapsed);
            const uint32_t began = state->device->beginFrame(
                state->device, (uint32_t)width, (uint32_t)height);
            uint32_t submitted = 1u;
            if (began != 0u && state->terrainReady && state->device->submit != NULL)
            {
                LaiueGraphicsDrawItemV2 draws[ANDROID_WALK_ACTIVE_CHUNK_COUNT];
                uint32_t drawIndex = 0u;
                for (int32_t y = -ANDROID_WALK_ACTIVE_CHUNK_RADIUS;
                     y <= ANDROID_WALK_ACTIVE_CHUNK_RADIUS; ++y)
                    for (int32_t x = -ANDROID_WALK_ACTIVE_CHUNK_RADIUS;
                         x <= ANDROID_WALK_ACTIVE_CHUNK_RADIUS; ++x)
                    {
                        draws[drawIndex] = (LaiueGraphicsDrawItemV2){
                            .structSize = sizeof(draws[drawIndex]),
                            .vertexBuffer = state->terrainBuffer,
                            .indexCount = 30u,
                            .originRelative = {(float)x * 64.0f,
                                                (float)y * 64.0f, 0.0f},
                            .scale = 1.0f,
                        };
                        ++drawIndex;
                    }
                submitted = state->device->submit(state->device, draws,
                                                  ANDROID_WALK_ACTIVE_CHUNK_COUNT);
            }
            if (began == 0u || submitted == 0u ||
                state->device->endFrame(state->device) == 0u)
            AndroidLog(state, ANDROID_LOG_WARN, "frame skipped after surface change");
        }
    }
}

void android_main(struct android_app *app)
{
    AndroidWalkState state;
    memset(&state, 0, sizeof(state));
    state.app = app;
    app->userData = &state;
    app->onAppCmd = AndroidHandleCommand;
    app->onInputEvent = AndroidHandleInput;

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    state.host = LaiueModuleHostCreate(&config, &diagnostic);
    if (state.host == NULL || !AndroidLoadModules(&state))
        AndroidLog(&state, ANDROID_LOG_ERROR, "module graph failed");
    else
    {
#if defined(LAIUE_ANDROID_WALK_WITH_VOXEL)
        LaiueVoxelWorldConfigV1 voxelConfig = {
            .structSize = sizeof(voxelConfig),
            .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
            .defaultBlock = {0u, 0u},
        };
        if (state.voxel != NULL &&
            state.voxelServiceSize >= LAIUE_VOXEL_SERVICE_V1_CONTEXT_SIZE &&
            state.voxel->createWithContext != NULL && state.voxel->context != NULL)
            (void)state.voxel->createWithContext(state.voxel->context,
                                                  &voxelConfig, &state.world);
        else if (state.voxel != NULL && state.voxel->create != NULL)
            (void)state.voxel->create(&voxelConfig, &state.world);
        if (state.world != NULL && state.voxel->getProvider != NULL)
            (void)state.voxel->getProvider(state.world, &state.provider);
#endif
        state.walkContext.sparse = state.provider;
        state.walkProvider.structSize = sizeof(state.walkProvider);
        state.walkProvider.abiVersion = LAIUE_VOXEL_ABI_VERSION_1;
        state.walkProvider.context = &state.walkContext;
        state.walkProvider.getBlock = WalkGetBlock;
        LaiueCharacterCollisionV1 collision = {
            .structSize = sizeof(collision),
            .abiVersion = LAIUE_CHARACTER_ABI_VERSION_1,
            .context = &state.walkProvider,
            .sweepAabb = WalkSweepAabb,
        };
        if (state.character != NULL && state.character->create != NULL &&
            state.character->setPosition != NULL &&
            state.character->create(&collision, ANDROID_WALK_HALF_EXTENT,
                                    &state.controller) != 0u)
        {
            LaiueCharacterPositionV1 start = {
                .cellX = 0, .cellY = 0, .localX = 0, .localY = 0, .localZ = 1400,
            };
            (void)state.character->setPosition(state.controller, &start, 1u);
        }
    }

    while (app->destroyRequested == 0)
    {
        int events = 0;
        struct android_poll_source *source = NULL;
        int timeout = state.running && state.windowReady ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0)
        {
            if (source != NULL && source->process != NULL)
                source->process(app, source);
            if (app->destroyRequested != 0)
                break;
            timeout = state.running && state.windowReady ? 0 : -1;
            if (timeout == 0)
                break;
        }
        if (state.running && state.windowReady)
            AndroidStep(&state);
    }

    AndroidDestroyDevice(&state);
    if (state.controller != NULL && state.character != NULL)
        state.character->destroy(state.controller);
    if (state.world != NULL && state.voxel != NULL)
        state.voxel->destroy(state.world);
    if (state.host != NULL)
    {
        LaiueModuleHostUnloadAll(state.host);
        LaiueModuleHostDestroy(state.host);
    }
}
