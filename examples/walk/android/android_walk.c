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
#include "media/image.h"

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ANDROID_WALK_LOG_TAG "laiue.walk"
#define ANDROID_WALK_HALF_EXTENT INT64_C(400)
#define ANDROID_WALK_ACTIVE_CHUNK_RADIUS 1
#define ANDROID_WALK_ACTIVE_CHUNK_DIAMETER \
    (ANDROID_WALK_ACTIVE_CHUNK_RADIUS * 2 + 1)
#define ANDROID_WALK_ACTIVE_CHUNK_COUNT \
    (ANDROID_WALK_ACTIVE_CHUNK_DIAMETER * ANDROID_WALK_ACTIVE_CHUNK_DIAMETER)

/* Touch controls are deliberately owned by the walk example rather than by
 * the renderer or the Android window provider.  The left lower quadrant is a
 * fixed virtual stick, the two lower-right circles are sprint and jump, and
 * the remaining right-hand surface is reserved for camera look. */
#define ANDROID_WALK_TOUCH_LEFT_ZONE 0.48f
#define ANDROID_WALK_TOUCH_LOWER_ZONE 0.46f
#define ANDROID_WALK_TOUCH_LOOK_ZONE 0.46f
#define ANDROID_WALK_TOUCH_JOYSTICK_RADIUS 0.16f
#define ANDROID_WALK_TOUCH_BUTTON_RADIUS 0.095f
#define ANDROID_WALK_TOUCH_MARGIN 0.06f
#define ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT 3u
#define ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT 24u
#define ANDROID_WALK_TEXTURED_TERRAIN_MIN_X (-64.0f)
#define ANDROID_WALK_TEXTURED_TERRAIN_MIN_Y (-64.0f)
#define ANDROID_WALK_TEXTURED_TERRAIN_MAX_X 128.0f
#define ANDROID_WALK_TEXTURED_TERRAIN_MAX_Y 128.0f

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void);

typedef struct AndroidWalkState AndroidWalkState;

struct AndroidWalkState
{
    struct android_app *app;
    LaiueModuleHost *host;
    const LaiueCharacterServiceV1 *character;
    uint32_t characterServiceSize;
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
    LaiueGraphicsHandle texturedTerrainBuffers[ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT];
    LaiueGraphicsHandle terrainTextures[ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT];
    LaiueGraphicsHandle terrainSampler;
    bool texturedTerrainReady;
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
    bool sprintActive;
    bool touchUiReady;
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
    int32_t sprintPointerId;
    int32_t jumpPointerId;
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

static bool AndroidFieldPresent(uint32_t actualSize, uint32_t declaredSize,
                               size_t offset, size_t size)
{
    return (size_t)actualSize >= offset && (size_t)actualSize - offset >= size &&
           (size_t)declaredSize >= offset && (size_t)declaredSize - offset >= size;
}

static float AndroidTouchMinimumDimension(const AndroidWalkState *state)
{
    if (state == NULL)
        return 1.0f;
    const float width = state->width > 0 ? (float)state->width : 1.0f;
    const float height = state->height > 0 ? (float)state->height : 1.0f;
    return width < height ? width : height;
}

static void AndroidTouchLayout(const AndroidWalkState *state,
                               float *joystickCenterX, float *joystickCenterY,
                               float *joystickRadius, float *jumpCenterX,
                               float *jumpCenterY, float *sprintCenterX,
                               float *sprintCenterY, float *buttonRadius)
{
    const float width = state != NULL && state->width > 0 ? (float)state->width : 1.0f;
    const float height = state != NULL && state->height > 0 ? (float)state->height : 1.0f;
    const float minimum = AndroidTouchMinimumDimension(state);
    float stickRadius = minimum * ANDROID_WALK_TOUCH_JOYSTICK_RADIUS;
    float actionRadius = minimum * ANDROID_WALK_TOUCH_BUTTON_RADIUS;
    float margin = minimum * ANDROID_WALK_TOUCH_MARGIN;
    if (stickRadius < 56.0f) stickRadius = 56.0f;
    if (actionRadius < 48.0f) actionRadius = 48.0f;
    if (margin < 24.0f) margin = 24.0f;

    if (joystickCenterX != NULL) *joystickCenterX = margin + stickRadius;
    if (joystickCenterY != NULL) *joystickCenterY = height - margin - stickRadius;
    if (joystickRadius != NULL) *joystickRadius = stickRadius;
    if (jumpCenterX != NULL) *jumpCenterX = width - margin - actionRadius;
    if (jumpCenterY != NULL) *jumpCenterY = height - margin - actionRadius;
    if (sprintCenterX != NULL) *sprintCenterX = width - margin - actionRadius * 3.0f;
    if (sprintCenterY != NULL) *sprintCenterY = height - margin - actionRadius;
    if (buttonRadius != NULL) *buttonRadius = actionRadius;
}

static bool AndroidTouchInsideCircle(float x, float y, float centerX, float centerY,
                                     float radius)
{
    const float dx = x - centerX;
    const float dy = y - centerY;
    return dx * dx + dy * dy <= radius * radius;
}

static float AndroidTouchClamp(float value)
{
    if (value < -1.0f) return -1.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void AndroidClearTouchState(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    state->touchActive = false;
    state->joystickActive = false;
    state->lookActive = false;
    state->sprintActive = false;
    state->joystickX = 0.0f;
    state->joystickY = 0.0f;
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    state->joystickPointerId = -1;
    state->lookPointerId = -1;
    state->sprintPointerId = -1;
    state->jumpPointerId = -1;
    state->jumpPending = false;
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
    state->characterServiceSize = 0u;
    state->character = (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        LAIUE_CHARACTER_SERVICE_V1_LEGACY_SIZE, NULL, &state->characterServiceSize);
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

static void AndroidReleaseGraphicsHandle(AndroidWalkState *state,
                                         LaiueGraphicsHandle *handle)
{
    if (state == NULL || handle == NULL || *handle == 0u)
        return;
    if (state->device != NULL &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                            sizeof(state->device->destroyHandle)) &&
        state->device->destroyHandle != NULL)
        state->device->destroyHandle(state->device, *handle);
    *handle = 0u;
}

static void AndroidDestroyTexturedTerrain(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    for (uint32_t index = 0u; index < ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT; ++index)
    {
        AndroidReleaseGraphicsHandle(state, &state->texturedTerrainBuffers[index]);
        AndroidReleaseGraphicsHandle(state, &state->terrainTextures[index]);
    }
    AndroidReleaseGraphicsHandle(state, &state->terrainSampler);
    state->texturedTerrainReady = false;
}

static bool AndroidLoadTextureAsset(AndroidWalkState *state, const char *assetPath,
                                    LaiueGraphicsHandle *outTexture)
{
    if (outTexture != NULL)
        *outTexture = 0u;
    if (state == NULL || state->app == NULL || state->app->activity == NULL ||
        state->app->activity->assetManager == NULL || assetPath == NULL ||
        outTexture == NULL || state->device == NULL ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, createTexture),
                             sizeof(state->device->createTexture)) ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, uploadTexture),
                             sizeof(state->device->uploadTexture)) ||
        state->device->createTexture == NULL || state->device->uploadTexture == NULL)
        return false;

    AAsset *asset = AAssetManager_open(state->app->activity->assetManager,
                                       assetPath, AASSET_MODE_BUFFER);
    if (asset == NULL)
        return false;
    const off_t assetLength = AAsset_getLength(asset);
    if (assetLength <= 0 || (uint64_t)assetLength > UINT32_MAX ||
        (uint64_t)assetLength > INT32_MAX)
    {
        AAsset_close(asset);
        return false;
    }

    const uint32_t encodedBytes = (uint32_t)assetLength;
    uint8_t *encoded = (uint8_t *)PlatformAllocate(encodedBytes, false);
    uint8_t *pixels = NULL;
    uint8_t *scratch = NULL;
    bool succeeded = false;
    if (encoded == NULL)
        goto cleanup;
    uint32_t bytesRead = 0u;
    while (bytesRead < encodedBytes)
    {
        const int32_t result = AAsset_read(asset, encoded + bytesRead,
                                           encodedBytes - bytesRead);
        if (result <= 0)
            goto cleanup;
        bytesRead += (uint32_t)result;
    }
    AAsset_close(asset);
    asset = NULL;

    ImageInfo info;
    memset(&info, 0, sizeof(info));
    if (ImageInspect(encoded, encodedBytes, &info) != IMAGE_OK ||
        info.frameCount != 1u || info.frameBytes == 0u ||
        info.pixelBytes != info.frameBytes || info.width > UINT32_MAX / 4u)
        goto cleanup;
    pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    if (info.scratchBytes != 0u)
        scratch = (uint8_t *)PlatformAllocate(info.scratchBytes, false);
    if (pixels == NULL || (info.scratchBytes != 0u && scratch == NULL) ||
        ImageDecode(encoded, encodedBytes, &info, pixels, info.pixelBytes,
                    scratch, info.scratchBytes) != IMAGE_OK)
        goto cleanup;

    LaiueGraphicsTextureDescV1 description = {
        .structSize = sizeof(description),
        .format = LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB,
        .extent = {info.width, info.height, 1u},
        .mipLevels = 1u,
        .usageFlags = 0u,
    };
    if (state->device->createTexture(state->device, &description, outTexture) == 0u)
        goto cleanup;
    LaiueGraphicsTextureUploadV1 upload = {
        .structSize = sizeof(upload),
        .texture = *outTexture,
        .data = pixels,
        .sizeBytes = info.frameBytes,
        .rowPitchBytes = info.width * 4u,
        .reserved = 0u,
    };
    succeeded = state->device->uploadTexture(state->device, &upload) != 0u;

cleanup:
    if (asset != NULL)
        AAsset_close(asset);
    PlatformFree(scratch);
    PlatformFree(pixels);
    PlatformFree(encoded);
    if (!succeeded)
        AndroidReleaseGraphicsHandle(state, outTexture);
    return succeeded;
}

static LaiueGraphicsVertexV2 AndroidTerrainVertex(float x, float y, float z,
                                                  float u, float v)
{
    LaiueGraphicsVertexV2 vertex = {
        .position = {x, y, z},
        .uv = {u, v},
        .colorRGBA = UINT32_MAX,
    };
    return vertex;
}

static void AndroidAppendTerrainQuad(LaiueGraphicsVertexV2 *vertices,
                                    uint32_t *vertexCount,
                                    const float positions[4][3],
                                    const float uv[4][2])
{
    if (vertices == NULL || vertexCount == NULL || positions == NULL || uv == NULL)
        return;
    const LaiueGraphicsVertexV2 corners[4] = {
        AndroidTerrainVertex(positions[0][0], positions[0][1], positions[0][2],
                             uv[0][0], uv[0][1]),
        AndroidTerrainVertex(positions[1][0], positions[1][1], positions[1][2],
                             uv[1][0], uv[1][1]),
        AndroidTerrainVertex(positions[2][0], positions[2][1], positions[2][2],
                             uv[2][0], uv[2][1]),
        AndroidTerrainVertex(positions[3][0], positions[3][1], positions[3][2],
                             uv[3][0], uv[3][1]),
    };
    const uint32_t pattern[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (uint32_t index = 0u; index < 6u; ++index)
        vertices[(*vertexCount)++] = corners[pattern[index]];
}

static bool AndroidCreateGenericTerrainBuffer(AndroidWalkState *state,
                                              const LaiueGraphicsVertexV2 *vertices,
                                              uint32_t vertexCount,
                                              LaiueGraphicsHandle *outBuffer)
{
    if (outBuffer != NULL)
        *outBuffer = 0u;
    if (state == NULL || state->device == NULL || vertices == NULL ||
        vertexCount == 0u || outBuffer == NULL ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, createBuffer),
                             sizeof(state->device->createBuffer)) ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, uploadBuffer),
                             sizeof(state->device->uploadBuffer)) ||
        state->device->createBuffer == NULL || state->device->uploadBuffer == NULL)
        return false;
    LaiueGraphicsBufferDescV1 description = {
        .structSize = sizeof(description),
        .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
        .sizeBytes = (uint64_t)vertexCount * sizeof(*vertices),
    };
    if (state->device->createBuffer(state->device, &description, outBuffer) == 0u)
        return false;
    LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload),
        .buffer = *outBuffer,
        .data = vertices,
        .sizeBytes = description.sizeBytes,
    };
    if (state->device->uploadBuffer(state->device, &upload) != 0u)
        return true;
    AndroidReleaseGraphicsHandle(state, outBuffer);
    return false;
}

static void AndroidBuildTerrainSkirt(LaiueGraphicsVertexV2 *vertices,
                                     float bottomZ, float topZ)
{
    const float minX = ANDROID_WALK_TEXTURED_TERRAIN_MIN_X;
    const float minY = ANDROID_WALK_TEXTURED_TERRAIN_MIN_Y;
    const float maxX = ANDROID_WALK_TEXTURED_TERRAIN_MAX_X;
    const float maxY = ANDROID_WALK_TEXTURED_TERRAIN_MAX_Y;
    const float padding = 0.01f;
    const float uvSide[4][2] = {{0.0f, 0.0f}, {192.0f, 0.0f},
                                {192.0f, topZ - bottomZ}, {0.0f, topZ - bottomZ}};
    const float faces[4][4][3] = {
        {{minX, minY - padding, bottomZ}, {maxX, minY - padding, bottomZ},
         {maxX, minY - padding, topZ}, {minX, minY - padding, topZ}},
        {{maxX + padding, minY, bottomZ}, {maxX + padding, maxY, bottomZ},
         {maxX + padding, maxY, topZ}, {maxX + padding, minY, topZ}},
        {{maxX, maxY + padding, bottomZ}, {minX, maxY + padding, bottomZ},
         {minX, maxY + padding, topZ}, {maxX, maxY + padding, topZ}},
        {{minX - padding, maxY, bottomZ}, {minX - padding, minY, bottomZ},
         {minX - padding, minY, topZ}, {minX - padding, maxY, topZ}},
    };
    uint32_t vertexCount = 0u;
    for (uint32_t face = 0u; face < 4u; ++face)
        AndroidAppendTerrainQuad(vertices, &vertexCount, faces[face], uvSide);
}

static bool AndroidCreateTexturedTerrain(AndroidWalkState *state)
{
    static const char *const textureAssets[ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT] = {
        "textures/grass.png", "textures/dirt.png", "textures/stone.png",
    };
    if (state == NULL || state->device == NULL ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, createSampler),
                             sizeof(state->device->createSampler)) ||
        state->device->createSampler == NULL)
        return false;
    for (uint32_t index = 0u; index < ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT; ++index)
        if (!AndroidLoadTextureAsset(state, textureAssets[index],
                                     &state->terrainTextures[index]))
            goto failed;

    LaiueGraphicsSamplerDescV1 sampler = {
        .structSize = sizeof(sampler),
        .minFilter = LAIUE_GRAPHICS_FILTER_NEAREST,
        .magFilter = LAIUE_GRAPHICS_FILTER_NEAREST,
        .addressModeU = LAIUE_GRAPHICS_ADDRESS_REPEAT,
        .addressModeV = LAIUE_GRAPHICS_ADDRESS_REPEAT,
        .addressModeW = LAIUE_GRAPHICS_ADDRESS_REPEAT,
    };
    if (state->device->createSampler(state->device, &sampler,
                                     &state->terrainSampler) == 0u)
        goto failed;

    LaiueGraphicsVertexV2 top[6];
    const float topPositions[4][3] = {
        {ANDROID_WALK_TEXTURED_TERRAIN_MIN_X, ANDROID_WALK_TEXTURED_TERRAIN_MIN_Y, 1.01f},
        {ANDROID_WALK_TEXTURED_TERRAIN_MAX_X, ANDROID_WALK_TEXTURED_TERRAIN_MIN_Y, 1.01f},
        {ANDROID_WALK_TEXTURED_TERRAIN_MAX_X, ANDROID_WALK_TEXTURED_TERRAIN_MAX_Y, 1.01f},
        {ANDROID_WALK_TEXTURED_TERRAIN_MIN_X, ANDROID_WALK_TEXTURED_TERRAIN_MAX_Y, 1.01f},
    };
    const float topUv[4][2] = {{0.0f, 0.0f}, {192.0f, 0.0f},
                               {192.0f, 192.0f}, {0.0f, 192.0f}};
    uint32_t topCount = 0u;
    AndroidAppendTerrainQuad(top, &topCount, topPositions, topUv);

    LaiueGraphicsVertexV2 dirt[ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT];
    LaiueGraphicsVertexV2 stone[ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT];
    AndroidBuildTerrainSkirt(dirt, -3.0f, 1.0f);
    AndroidBuildTerrainSkirt(stone, -7.0f, -3.0f);
    if (!AndroidCreateGenericTerrainBuffer(state, top, topCount,
                                           &state->texturedTerrainBuffers[0]) ||
        !AndroidCreateGenericTerrainBuffer(state, dirt,
                                           ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT,
                                           &state->texturedTerrainBuffers[1]) ||
        !AndroidCreateGenericTerrainBuffer(state, stone,
                                           ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT,
                                           &state->texturedTerrainBuffers[2]))
        goto failed;
    state->texturedTerrainReady = true;
    return true;

failed:
    AndroidDestroyTexturedTerrain(state);
    return false;
}

static void AndroidDestroyDevice(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    if (state->terrainReady && state->device != NULL &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                            sizeof(state->device->destroyHandle)) &&
        state->device->destroyHandle != NULL)
        state->device->destroyHandle(state->device, state->terrainBuffer);
    state->terrainReady = false;
    state->terrainBuffer = 0u;
    AndroidDestroyTexturedTerrain(state);
    if (state->device != NULL && state->graphics != NULL &&
        AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                            offsetof(LaiueGraphicsDeviceServiceV2, destroyDevice),
                            sizeof(state->graphics->destroyDevice)) &&
        state->graphics->destroyDevice != NULL)
        state->graphics->destroyDevice(state->device);
    state->device = NULL;
    state->windowReady = false;
    state->running = false;
    state->touchUiReady = false;
    AndroidClearTouchState(state);
    memset(state->keyDown, 0, sizeof(state->keyDown));
    state->accumulator = 0.0;
    state->lastTime = 0.0;
}

static void AndroidResetInputClock(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    AndroidClearTouchState(state);
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
        AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                            offsetof(LaiueGraphicsDeviceServiceV2, createDeviceWithContext),
                            sizeof(state->graphics->createDeviceWithContext)) &&
        AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                            offsetof(LaiueGraphicsDeviceServiceV2, context),
                            sizeof(state->graphics->context)) &&
        state->graphics->createDeviceWithContext != NULL &&
        state->graphics->context != NULL)
        created = state->graphics->createDeviceWithContext(
            state->graphics->context, state->app->window, width, height,
            LAIUE_GRAPHICS_BACKEND_VULKAN, &state->device);
    else if (width > 0 && height > 0 &&
             AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                                 offsetof(LaiueGraphicsDeviceServiceV2, createDevice),
                                 sizeof(state->graphics->createDevice)) &&
             state->graphics->createDevice != NULL)
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
    if (AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, createBuffer),
                            sizeof(state->device->createBuffer)) &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, uploadBuffer),
                            sizeof(state->device->uploadBuffer)) &&
        state->device->createBuffer != NULL && state->device->uploadBuffer != NULL)
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
            AndroidFieldPresent(state->device->structSize, state->device->structSize,
                                offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                                sizeof(state->device->destroyHandle)) &&
            state->device->destroyHandle != NULL)
        {
            state->device->destroyHandle(state->device, state->terrainBuffer);
            state->terrainBuffer = 0u;
        }
    }
    if (state->scene != NULL && state->scene->cameraInit != NULL)
        state->scene->cameraInit(&state->camera, 0.0, 0.0, 0.0, 0.0f, -0.32f);
    if (!AndroidCreateTexturedTerrain(state))
        AndroidLog(state, ANDROID_LOG_WARN,
                   "Android walk texture assets unavailable; using flat terrain fallback");

    /* The renderer only records UI quads after a font atlas has been
     * installed.  A 1x1 opaque atlas is enough for the coloured touch
     * controls because their quads do not use the text flag.  This keeps the
     * controls independent from the optional UI technology module. */
    static const uint8_t touchUiAtlasPixel = 255u;
    if (AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, setUiFontAtlas),
                            sizeof(state->device->setUiFontAtlas)) &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, submitUi),
                            sizeof(state->device->submitUi)) &&
        state->device->setUiFontAtlas != NULL && state->device->submitUi != NULL)
        state->touchUiReady = state->device->setUiFontAtlas(
            state->device, &touchUiAtlasPixel, 1u, 1u) != 0u;
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
    if (state->width <= 0 || state->height <= 0)
        return 0;
    const int32_t rawAction = AMotionEvent_getAction(event);
    const int32_t action = rawAction & AMOTION_EVENT_ACTION_MASK;
    const size_t actionIndex = (size_t)((rawAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t pointerCount = AMotionEvent_getPointerCount(event);
    if (action == AMOTION_EVENT_ACTION_CANCEL)
    {
        AndroidClearTouchState(state);
        return 1;
    }
    if (pointerCount == 0u || actionIndex >= pointerCount)
        return 0;
    const float width = state->width > 0 ? (float)state->width : 1.0f;
    const float height = state->height > 0 ? (float)state->height : 1.0f;
    const int32_t pointerId = AMotionEvent_getPointerId(event, actionIndex);
    const float x = AMotionEvent_getX(event, actionIndex);
    const float y = AMotionEvent_getY(event, actionIndex);
    float joystickCenterX = 0.0f;
    float joystickCenterY = 0.0f;
    float joystickRadius = 0.0f;
    float jumpCenterX = 0.0f;
    float jumpCenterY = 0.0f;
    float sprintCenterX = 0.0f;
    float sprintCenterY = 0.0f;
    float buttonRadius = 0.0f;
    AndroidTouchLayout(state, &joystickCenterX, &joystickCenterY, &joystickRadius,
                       &jumpCenterX, &jumpCenterY, &sprintCenterX, &sprintCenterY,
                       &buttonRadius);
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
    {
        state->touchActive = true;
        const float buttonHitRadius = buttonRadius * 1.20f;
        if (state->jumpPointerId < 0 &&
            AndroidTouchInsideCircle(x, y, jumpCenterX, jumpCenterY, buttonHitRadius))
        {
            state->jumpPointerId = pointerId;
            state->jumpPending = true;
        }
        else if (state->sprintPointerId < 0 &&
                 AndroidTouchInsideCircle(x, y, sprintCenterX, sprintCenterY,
                                          buttonHitRadius))
        {
            state->sprintPointerId = pointerId;
            state->sprintActive = true;
        }
        else if (state->joystickPointerId < 0 && x < width * ANDROID_WALK_TOUCH_LEFT_ZONE &&
                 y > height * ANDROID_WALK_TOUCH_LOWER_ZONE)
        {
            state->joystickPointerId = pointerId;
            state->joystickActive = true;
            state->joystickOriginX = joystickCenterX;
            state->joystickOriginY = joystickCenterY;
            state->joystickX = 0.0f;
            state->joystickY = 0.0f;
        }
        else if (state->lookPointerId < 0 && x >= width * ANDROID_WALK_TOUCH_LOOK_ZONE)
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
                float dx = (px - state->joystickOriginX) / joystickRadius;
                float dy = (py - state->joystickOriginY) / joystickRadius;
                state->joystickX = AndroidTouchClamp(dx);
                state->joystickY = -AndroidTouchClamp(dy);
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
    else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP)
    {
        if (pointerId == state->joystickPointerId)
        {
            state->joystickPointerId = -1;
            state->joystickActive = false;
            state->joystickX = 0.0f;
            state->joystickY = 0.0f;
        }
        if (pointerId == state->lookPointerId)
        {
            state->lookPointerId = -1;
            state->lookActive = false;
        }
        if (pointerId == state->sprintPointerId)
        {
            state->sprintPointerId = -1;
            state->sprintActive = false;
        }
        if (pointerId == state->jumpPointerId)
            state->jumpPointerId = -1;
        state->touchActive = state->joystickActive || state->lookActive ||
                             state->sprintActive || state->jumpPointerId >= 0;
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
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (state->windowReady && state->app->window != NULL && state->graphics != NULL &&
                AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, resize),
                                    sizeof(state->graphics->resize)) &&
                state->graphics->resize != NULL)
                state->graphics->resize(state->device,
                                        ANativeWindow_getWidth(state->app->window),
                                        ANativeWindow_getHeight(state->app->window));
            if (state->app->window != NULL)
            {
                state->width = ANativeWindow_getWidth(state->app->window);
                state->height = ANativeWindow_getHeight(state->app->window);
                AndroidClearTouchState(state);
            }
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
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, setCamera),
                             sizeof(state->device->setCamera)) ||
        state->device->setCamera == NULL)
        return;
    int64_t blockX = 0;
    int64_t blockY = 0;
    int64_t localZ = 1400;
    if (state->controller != NULL && state->character != NULL &&
        AndroidFieldPresent(state->characterServiceSize, state->character->structSize,
                            offsetof(LaiueCharacterServiceV1, getPosition),
                            sizeof(state->character->getPosition)) &&
        state->character->getPosition != NULL)
    {
        LaiueCharacterPositionV1 position;
        if (state->character->getPosition(state->controller, &position) != 0u)
        {
            if (WalkPositionAxisToBlock(position.cellX, position.localX, &blockX) == 0u ||
                WalkPositionAxisToBlock(position.cellY, position.localY, &blockY) == 0u)
                return;
            localZ = position.localZ;
        }
    }
    const int64_t originX = AndroidFloorDiv(blockX, 64) * 64;
    const int64_t originY = AndroidFloorDiv(blockY, 64) * 64;
    int64_t fractionX = state->controller != NULL ? 0 : 0;
    int64_t fractionY = state->controller != NULL ? 0 : 0;
    if (state->controller != NULL && state->character != NULL &&
        AndroidFieldPresent(state->characterServiceSize, state->character->structSize,
                            offsetof(LaiueCharacterServiceV1, getPosition),
                            sizeof(state->character->getPosition)) &&
        state->character->getPosition != NULL)
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

static LaiueGraphicsUiQuadV1 AndroidTouchQuad(float x0, float y0, float x1, float y1,
                                               float cornerRadius, uint32_t color)
{
    LaiueGraphicsUiQuadV1 quad = {
        .rect = {x0, y0, x1, y1},
        .uv = {0.0f, 0.0f, 1.0f, 1.0f},
        .colorRGBA = color,
        .cornerRadius = cornerRadius,
        .flags = 0u,
        .reserved = 0u,
    };
    return quad;
}

static void AndroidSubmitTouchUi(AndroidWalkState *state, int32_t width, int32_t height)
{
    if (state == NULL || !state->touchUiReady || state->device == NULL || width <= 0 ||
        height <= 0 || !AndroidFieldPresent(state->device->structSize,
                                            state->device->structSize,
                                            offsetof(LaiueGraphicsDeviceV2, submitUi),
                                            sizeof(state->device->submitUi)) ||
        state->device->submitUi == NULL)
        return;

    float joystickCenterX = 0.0f;
    float joystickCenterY = 0.0f;
    float joystickRadius = 0.0f;
    float jumpCenterX = 0.0f;
    float jumpCenterY = 0.0f;
    float sprintCenterX = 0.0f;
    float sprintCenterY = 0.0f;
    float buttonRadius = 0.0f;
    AndroidTouchLayout(state, &joystickCenterX, &joystickCenterY, &joystickRadius,
                       &jumpCenterX, &jumpCenterY, &sprintCenterX, &sprintCenterY,
                       &buttonRadius);

    LaiueGraphicsUiQuadV1 quads[4];
    uint32_t quadCount = 0u;
    quads[quadCount++] = AndroidTouchQuad(
        joystickCenterX - joystickRadius, joystickCenterY - joystickRadius,
        joystickCenterX + joystickRadius, joystickCenterY + joystickRadius,
        joystickRadius, UINT32_C(0x702A3448));
    const float knobX = joystickCenterX + state->joystickX * joystickRadius * 0.62f;
    const float knobY = joystickCenterY - state->joystickY * joystickRadius * 0.62f;
    const float knobRadius = joystickRadius * 0.42f;
    quads[quadCount++] = AndroidTouchQuad(
        knobX - knobRadius, knobY - knobRadius, knobX + knobRadius, knobY + knobRadius,
        knobRadius, state->joystickActive ? UINT32_C(0xD0E7F1FF) : UINT32_C(0xA0B9C7D8));

    const uint32_t sprintColor = state->sprintActive
                                     ? UINT32_C(0xE0FFB347)
                                     : UINT32_C(0x906B5528);
    quads[quadCount++] = AndroidTouchQuad(
        sprintCenterX - buttonRadius, sprintCenterY - buttonRadius,
        sprintCenterX + buttonRadius, sprintCenterY + buttonRadius,
        buttonRadius, sprintColor);
    const uint32_t jumpColor = state->jumpPointerId >= 0
                                   ? UINT32_C(0xE06EC8FF)
                                   : UINT32_C(0x906E4C9C);
    quads[quadCount++] = AndroidTouchQuad(
        jumpCenterX - buttonRadius, jumpCenterY - buttonRadius,
        jumpCenterX + buttonRadius, jumpCenterY + buttonRadius,
        buttonRadius, jumpColor);
    if (state->device->submitUi(state->device, quads, quadCount) == 0u)
        state->touchUiReady = false;
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
           AndroidFieldPresent(state->characterServiceSize, state->character->structSize,
                               offsetof(LaiueCharacterServiceV1, step),
                               sizeof(state->character->step)) &&
           state->character->step != NULL && state->accumulator >= fixedStep && ticks < 8u)
    {
        LaiueCharacterInputV1 input = {0};
        float strafe = (float)((state->keyDown[3] ? 1 : 0) -
                               (state->keyDown[1] ? 1 : 0));
        float forwardInput = (float)((state->keyDown[0] ? 1 : 0) -
                                     (state->keyDown[2] ? 1 : 0));
        if (state->joystickActive)
        {
            strafe = state->joystickX;
            forwardInput = state->joystickY;
        }
        float forward[3] = {0.0f, 1.0f, 0.0f};
        if (state->scene != NULL && state->scene->cameraGetForwardVector != NULL)
            state->scene->cameraGetForwardVector(&state->camera, forward);
        const float worldX = forwardInput * forward[0] + strafe * forward[1];
        const float worldY = forwardInput * forward[1] - strafe * forward[0];
        input.moveX = worldX > 0.35f ? 1 : (worldX < -0.35f ? -1 : 0);
        input.moveY = worldY > 0.35f ? 1 : (worldY < -0.35f ? -1 : 0);
        if (state->keyDown[4] || state->sprintActive)
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
        if (WalkRebaseWorldAndCharacter(
                state->voxel, state->voxelServiceSize, state->world,
                state->character, state->characterServiceSize,
                state->controller) == 0u)
        {
            AndroidLog(state, ANDROID_LOG_ERROR,
                       "world/character rebase transaction failed");
            state->running = false;
            break;
        }
        state->accumulator -= fixedStep;
        ++ticks;
    }
    if (ticks == 8u && state->accumulator >= fixedStep)
        state->accumulator = 0.0;
    if (state->device != NULL &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, beginFrame),
                            sizeof(state->device->beginFrame)) &&
        AndroidFieldPresent(state->device->structSize, state->device->structSize,
                            offsetof(LaiueGraphicsDeviceV2, endFrame),
                            sizeof(state->device->endFrame)) &&
        state->device->beginFrame != NULL && state->device->endFrame != NULL)
    {
        const int32_t width = ANativeWindow_getWidth(state->app->window);
        const int32_t height = ANativeWindow_getHeight(state->app->window);
        if ((width != state->width || height != state->height) && state->graphics != NULL &&
            AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                                offsetof(LaiueGraphicsDeviceServiceV2, resize),
                                sizeof(state->graphics->resize)) &&
            state->graphics->resize != NULL)
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
            if (began != 0u && state->terrainReady &&
                AndroidFieldPresent(state->device->structSize, state->device->structSize,
                                    offsetof(LaiueGraphicsDeviceV2, submit),
                                    sizeof(state->device->submit)) &&
                state->device->submit != NULL)
            {
                LaiueGraphicsDrawItemV2 draws[ANDROID_WALK_ACTIVE_CHUNK_COUNT + 3u];
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
                if (state->texturedTerrainReady)
                    for (uint32_t material = 0u;
                         material < ANDROID_WALK_TEXTURED_TERRAIN_MATERIAL_COUNT;
                         ++material)
                    {
                        const uint32_t vertexCount = material == 0u
                                                         ? 6u
                                                         : ANDROID_WALK_TEXTURED_TERRAIN_VERTEX_COUNT;
                        draws[drawIndex] = (LaiueGraphicsDrawItemV2){
                            .structSize = sizeof(draws[drawIndex]),
                            .vertexBuffer = state->texturedTerrainBuffers[material],
                            .indexCount = vertexCount,
                            .originRelative = {0.0f, 0.0f, 0.0f},
                            .scale = 1.0f,
                            .texture = state->terrainTextures[material],
                            .sampler = state->terrainSampler,
                        };
                        ++drawIndex;
                    }
                submitted = state->device->submit(state->device, draws, drawIndex);
            }
            if (began != 0u)
                AndroidSubmitTouchUi(state, width, height);
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
        const bool voxelHasContextCreate =
            state.voxel != NULL &&
            AndroidFieldPresent(state.voxelServiceSize, state.voxel->structSize,
                                offsetof(LaiueVoxelServiceV1, createWithContext),
                                sizeof(state.voxel->createWithContext)) &&
            AndroidFieldPresent(state.voxelServiceSize, state.voxel->structSize,
                                offsetof(LaiueVoxelServiceV1, context),
                                sizeof(state.voxel->context)) &&
            state.voxel->createWithContext != NULL && state.voxel->context != NULL;
        if (voxelHasContextCreate)
            (void)state.voxel->createWithContext(state.voxel->context,
                                                  &voxelConfig, &state.world);
        else if (state.voxel != NULL &&
                 AndroidFieldPresent(state.voxelServiceSize, state.voxel->structSize,
                                     offsetof(LaiueVoxelServiceV1, create),
                                     sizeof(state.voxel->create)) &&
                 state.voxel->create != NULL)
            (void)state.voxel->create(&voxelConfig, &state.world);
        if (state.world != NULL && state.voxel != NULL &&
            AndroidFieldPresent(state.voxelServiceSize, state.voxel->structSize,
                                offsetof(LaiueVoxelServiceV1, getProvider),
                                sizeof(state.voxel->getProvider)) &&
            state.voxel->getProvider != NULL)
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
        if (state.character != NULL &&
            AndroidFieldPresent(state.characterServiceSize, state.character->structSize,
                                offsetof(LaiueCharacterServiceV1, create),
                                sizeof(state.character->create)) &&
            AndroidFieldPresent(state.characterServiceSize, state.character->structSize,
                                offsetof(LaiueCharacterServiceV1, setPosition),
                                sizeof(state.character->setPosition)) &&
            state.character->create != NULL && state.character->setPosition != NULL &&
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
    if (state.controller != NULL && state.character != NULL &&
        AndroidFieldPresent(state.characterServiceSize, state.character->structSize,
                            offsetof(LaiueCharacterServiceV1, destroy),
                            sizeof(state.character->destroy)) &&
        state.character->destroy != NULL)
        state.character->destroy(state.controller);
    if (state.world != NULL && state.voxel != NULL &&
        AndroidFieldPresent(state.voxelServiceSize, state.voxel->structSize,
                            offsetof(LaiueVoxelServiceV1, destroy),
                            sizeof(state.voxel->destroy)) &&
        state.voxel->destroy != NULL)
        state.voxel->destroy(state.world);
    if (state.host != NULL)
    {
        LaiueModuleHostUnloadAll(state.host);
        LaiueModuleHostDestroy(state.host);
    }
}
