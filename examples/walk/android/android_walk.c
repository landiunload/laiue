#include "character/character_service.h"
#include "graphics/graphics_device_service.h"
#include "mesh/mesher_service.h"
#include "mod/module_host.h"
#include "numeric/numeric_service.h"
#include "physics/physics_service.h"
#include "platform/system.h"
#include "voxel/voxel_service.h"
#include "world/world_service.h"
#include "scene/scene_service.h"
#include "scene/math_service.h"
#include "../walk_world.h"
#include "walk_runtime.h"
#include "../walk_math.h"
#include "touch_controls.h"
#include "../humanoid_ragdoll.h"
#include "../walk_visuals.h"
#include "../walk_humanoid.h"
#include "media/image.h"
#include "math/scalar.h"

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <stdbool.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ANDROID_WALK_LOG_TAG "laiue.walk"
#define ANDROID_WALK_HALF_EXTENT INT64_C(400)
/* Touch controls are deliberately owned by the walk example rather than by
 * the renderer or the Android window provider. The lower-left circle is a
 * fixed virtual stick, the lower-right circles toggle sprint and jump, and
 * the remaining right-hand surface is reserved for camera look. */
#define ANDROID_WALK_TOUCH_LEFT_ZONE 0.48f
#define ANDROID_WALK_TOUCH_LOWER_ZONE 0.46f
#define ANDROID_WALK_TOUCH_LOOK_ZONE 0.46f
#define ANDROID_WALK_TOUCH_JOYSTICK_RADIUS 0.16f
#define ANDROID_WALK_TOUCH_BUTTON_RADIUS 0.095f
#define ANDROID_WALK_TOUCH_MARGIN 0.06f
#define ANDROID_WALK_RAGDOLL_VERTEX_COUNT \
    WALK_RAGDOLL_VISUAL_VERTEX_COUNT
#define ANDROID_WALK_RAGDOLL_STABLE_ID UINT64_C(0x57414C4B52414744)

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void);
const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void);

typedef struct AndroidWalkState AndroidWalkState;

struct AndroidWalkState
{
    struct android_app *app;
    LaiueModuleHost *host;
    const LaiuePhysicsServiceV1 *physics;
    uint32_t physicsServiceSize;
    const LaiueMesherServiceV1 *mesher;
    VoxelRagdoll ragdoll;
    VoxelCollisionSource ragdollCollision;
    VoxelRigidStepSettings ragdollRigidSettings;
    VoxelRagdollSettings ragdollSettings;
    void *ragdollScratch;
    uint32_t ragdollScratchBytes;
    LaiueGraphicsHandle ragdollBuffer;
    WalkRagdollVisualScratch *ragdollVisualScratch;
    bool ragdollReady;
    bool ragdollGrounded;
    double ragdollFacingYaw;
    double ragdollGaitPhase;
    WalkHumanoidControllerState humanoidController;
    int64_t ragdollBlockOriginX;
    int64_t ragdollBlockOriginY;
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
    WalkVisualChunkSet chunkSet;
    bool chunksReady;
    LaiueGraphicsHandle texturedTerrainBuffers[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainTextures[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainSampler;
    LaiueGraphicsHandle farTerrainBuffer;
    bool texturedTerrainReady;
    bool firstPerson;
    uint8_t selectedMaterial;
    const LaiueSceneServiceV1 *scene;
    const LaiueSceneMathServiceV1 *sceneMath;
    Camera camera;
    float cameraRelativeEye[3];
    float terrainOriginRelative[3];
    double cameraRenderOrigin[3];
    double lastSafeCameraEye[3];
    bool lastSafeCameraEyeValid;
    int64_t renderOriginBlock[3];
    float viewProjection[16];
    LaiueVoxelProviderV1 walkProvider;
    bool windowReady;
    bool running;
    bool touchActive;
    bool joystickActive;
    bool lookActive;
    bool sprintToggled;
    bool touchUiReady;
    bool renderTelemetryLogged;
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
    bool breakPending;
    bool placePending;
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
    if (sprintCenterX != NULL) *sprintCenterX = width - margin - actionRadius * 3.8f;
    if (sprintCenterY != NULL) *sprintCenterY = height - margin - actionRadius;
    if (buttonRadius != NULL) *buttonRadius = actionRadius;
}

static void AndroidTouchActionLayout(const AndroidWalkState *state,
                                     float centers[3][2], float *radius)
{
    const float width = state != NULL && state->width > 0 ? (float)state->width : 1.0f;
    const float minimum = AndroidTouchMinimumDimension(state);
    float actionRadius = minimum * 0.075f;
    float margin = minimum * ANDROID_WALK_TOUCH_MARGIN;
    if (actionRadius < 32.0f) actionRadius = 32.0f;
    if (margin < 18.0f) margin = 18.0f;
    const float y = margin + actionRadius;
    if (centers != NULL)
    {
        centers[0][0] = width - margin - actionRadius;
        centers[1][0] = width - margin - actionRadius * 3.1f;
        centers[2][0] = width - margin - actionRadius * 5.2f;
        for (uint32_t i = 0u; i < 3u; ++i)
            centers[i][1] = y;
    }
    if (radius != NULL) *radius = actionRadius;
}

static bool AndroidTouchInsideCircle(float x, float y, float centerX, float centerY,
                                     float radius)
{
    const float dx = x - centerX;
    const float dy = y - centerY;
    return dx * dx + dy * dy <= radius * radius;
}

static void AndroidClearTouchState(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    state->touchActive = false;
    state->joystickActive = false;
    state->lookActive = false;
    state->joystickX = 0.0f;
    state->joystickY = 0.0f;
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    state->joystickPointerId = -1;
    state->lookPointerId = -1;
    state->sprintPointerId = -1;
    state->jumpPointerId = -1;
    state->jumpPending = false;
    state->breakPending = false;
    state->placePending = false;
}

static uint32_t AndroidLoadModules(AndroidWalkState *state)
{
    const LaiueModuleApiV1 *modules[9] = {
        LaiueCharacterGetStaticModuleApiV1(),
        LaiueGraphicsGetStaticModuleApiV1(),
        LaiueSceneMathGetStaticModuleApiV1(),
        LaiueSceneGetStaticModuleApiV1(),
    };
    uint32_t moduleCount = 4u;
    modules[moduleCount++] = LaiueNumericGetStaticModuleApiV1();
    modules[moduleCount++] = LaiuePhysicsGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueMesherGetStaticModuleApiV1();
#if defined(LAIUE_ANDROID_WALK_WITH_VOXEL)
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
    state->physicsServiceSize = 0u;
    state->physics = (const LaiuePhysicsServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_PHYSICS_SERVICE_NAME,
        LAIUE_PHYSICS_SERVICE_ABI_VERSION_1, sizeof(LaiuePhysicsServiceV1), NULL,
        &state->physicsServiceSize);
    state->mesher = (const LaiueMesherServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        sizeof(LaiueMesherServiceV1), NULL, NULL);
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
    return state->graphics != NULL && state->physics != NULL;
}

static void AndroidRagdollQueryBlock(void *context, int64_t x, int64_t y, int64_t z,
                                     VoxelBlockPhysics *outBlock)
{
    if (outBlock == NULL)
        return;
    outBlock->flags = VOXEL_BLOCK_PHYSICS_SOLID;
    outBlock->friction = 0.75f;
    AndroidWalkState *state = (AndroidWalkState *)context;
    if (state == NULL || z < INT32_MIN || z > INT32_MAX ||
        (state->ragdollBlockOriginX > 0 && x > INT64_MAX - state->ragdollBlockOriginX) ||
        (state->ragdollBlockOriginX < 0 && x < INT64_MIN - state->ragdollBlockOriginX) ||
        (state->ragdollBlockOriginY > 0 && y > INT64_MAX - state->ragdollBlockOriginY) ||
        (state->ragdollBlockOriginY < 0 && y < INT64_MIN - state->ragdollBlockOriginY))
        return;
    const LaiueVoxelCoordV1 coordinate = {state->ragdollBlockOriginX + x,
                                           state->ragdollBlockOriginY + y,
                                           (int32_t)z};
    LaiueVoxelBlockV1 block = {0u, 0u};
    if (WalkGetBlock(&state->walkProvider, &coordinate, &block) != 0u &&
        block.material == 0u)
        outBlock->flags = 0u;
}

static uint8_t AndroidReadVisualBlock(void *opaque, int64_t x, int64_t y, int64_t z)
{
    AndroidWalkState *state = (AndroidWalkState *)opaque;
    if (state == NULL || state->walkProvider.getBlock == NULL ||
        z < INT32_MIN || z > INT32_MAX)
        return 0u;
    const LaiueVoxelCoordV1 coordinate = {x, y, (int32_t)z};
    LaiueVoxelBlockV1 block = {0u, 0u};
    return WalkGetBlock(&state->walkProvider, &coordinate, &block) != 0u
               ? (uint8_t)block.material : 0u;
}

static bool AndroidSetWorldBlock(void *opaque, int64_t x, int64_t y, int64_t z,
                                 uint8_t material)
{
    AndroidWalkState *state = (AndroidWalkState *)opaque;
    if (state == NULL || state->world == NULL || state->voxel == NULL ||
        z < INT32_MIN || z > INT32_MAX ||
        !AndroidFieldPresent(state->voxelServiceSize, state->voxel->structSize,
                             offsetof(LaiueVoxelServiceV1, setBlock),
                             sizeof(state->voxel->setBlock)) ||
        state->voxel->setBlock == NULL)
        return false;
    const LaiueVoxelCoordV1 coordinate = {x, y, (int32_t)z};
    const LaiueVoxelBlockV1 block = {material, 0u};
    return state->voxel->setBlock(state->world, &coordinate, &block) != 0u;
}

static bool AndroidGetChunkCoordinates(AndroidWalkState *state,
                                       int64_t centerBlock[3])
{
    if (state == NULL || !state->ragdollReady || centerBlock == NULL)
        return false;
    double pelvis[3];
    if (!VoxelRigidBodyLocalPosition(
            &state->ragdoll.bodies[state->ragdoll.rootBody], pelvis))
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double localBlockDouble = WalkMathFloor(pelvis[axis]);
        const double localChunkDouble = WalkMathFloor(pelvis[axis] / 64.0) * 64.0;
        if (!WalkMathFinite(localBlockDouble) || !WalkMathFinite(localChunkDouble) ||
            localBlockDouble < -9223372036854775808.0 ||
            localBlockDouble >= 9223372036854775808.0 ||
            localChunkDouble < -9223372036854775808.0 ||
            localChunkDouble >= 9223372036854775808.0)
            return false;
        const int64_t localBlock = (int64_t)localBlockDouble;
        const int64_t localChunk = (int64_t)localChunkDouble;
        if (axis == 0u)
        {
            if ((state->ragdollBlockOriginX > 0 &&
                 localBlock > INT64_MAX - state->ragdollBlockOriginX) ||
                (state->ragdollBlockOriginX < 0 &&
                 localBlock < INT64_MIN - state->ragdollBlockOriginX) ||
                (state->ragdollBlockOriginX > 0 &&
                 localChunk > INT64_MAX - state->ragdollBlockOriginX) ||
                (state->ragdollBlockOriginX < 0 &&
                 localChunk < INT64_MIN - state->ragdollBlockOriginX))
                return false;
            centerBlock[axis] = state->ragdollBlockOriginX + localBlock;
            state->renderOriginBlock[axis] = state->ragdollBlockOriginX + localChunk;
        }
        else if (axis == 1u)
        {
            if ((state->ragdollBlockOriginY > 0 &&
                 localBlock > INT64_MAX - state->ragdollBlockOriginY) ||
                (state->ragdollBlockOriginY < 0 &&
                 localBlock < INT64_MIN - state->ragdollBlockOriginY) ||
                (state->ragdollBlockOriginY > 0 &&
                 localChunk > INT64_MAX - state->ragdollBlockOriginY) ||
                (state->ragdollBlockOriginY < 0 &&
                 localChunk < INT64_MIN - state->ragdollBlockOriginY))
                return false;
            centerBlock[axis] = state->ragdollBlockOriginY + localBlock;
            state->renderOriginBlock[axis] = state->ragdollBlockOriginY + localChunk;
        }
        else
        {
            centerBlock[axis] = localBlock;
            state->renderOriginBlock[axis] = localChunk;
        }
        state->cameraRenderOrigin[axis] = localChunk;
    }
    return true;
}

static bool AndroidRebaseRagdoll(AndroidWalkState *state)
{
    if (state == NULL || !state->ragdollReady)
        return false;
    double root[3];
    if (!VoxelRigidBodyLocalPosition(
            &state->ragdoll.bodies[state->ragdoll.rootBody], root) ||
        !WalkMathFinite(root[0]) || !WalkMathFinite(root[1]))
    {
        AndroidLog(state, ANDROID_LOG_ERROR, "ragdoll rebase could not read finite root");
        return false;
    }
    const double shiftXD = WalkMathFloor(root[0] / 64.0) * 64.0;
    const double shiftYD = WalkMathFloor(root[1] / 64.0) * 64.0;
    if (WalkMathAbs(shiftXD) < 1.0 && WalkMathAbs(shiftYD) < 1.0)
        return true;
    if (shiftXD < -9223372036854775808.0 || shiftXD >= 9223372036854775808.0 ||
        shiftYD < -9223372036854775808.0 || shiftYD >= 9223372036854775808.0)
    {
        __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                            "ragdoll rebase shift out of range root=(%.3f,%.3f) shift=(%.3f,%.3f)",
                            root[0], root[1], shiftXD, shiftYD);
        return false;
    }
    const int64_t shiftX = (int64_t)shiftXD;
    const int64_t shiftY = (int64_t)shiftYD;
    if (shiftX == INT64_MIN || shiftY == INT64_MIN ||
        (shiftX > 0 && state->ragdollBlockOriginX > INT64_MAX - shiftX) ||
        (shiftX < 0 && state->ragdollBlockOriginX < INT64_MIN - shiftX) ||
        (shiftY > 0 && state->ragdollBlockOriginY > INT64_MAX - shiftY) ||
        (shiftY < 0 && state->ragdollBlockOriginY < INT64_MIN - shiftY))
    {
        __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                            "ragdoll rebase world-coordinate overflow root=(%.3f,%.3f) shift=(%lld,%lld) origin=(%lld,%lld)",
                            root[0], root[1], (long long)shiftX, (long long)shiftY,
                            (long long)state->ragdollBlockOriginX,
                            (long long)state->ragdollBlockOriginY);
        return false;
    }
    /* TranslateBlocks subtracts its argument from local body positions. */
    const int64_t delta[3] = {shiftX, shiftY, 0};
    for (uint32_t body = 0u; body < state->ragdoll.bodyCount; ++body)
        if (!VoxelRigidBodyTranslateBlocks(&state->ragdoll.bodies[body], delta))
        {
            __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                                "ragdoll rebase translation failed body=%u root=(%.3f,%.3f) delta=(%lld,%lld)",
                                body, root[0], root[1], (long long)delta[0],
                                (long long)delta[1]);
            return false;
        }
    WalkHumanoidControllerRebase(&state->humanoidController, delta);
    if (state->lastSafeCameraEyeValid)
    {
        state->lastSafeCameraEye[0] -= shiftXD;
        state->lastSafeCameraEye[1] -= shiftYD;
    }
    state->ragdollBlockOriginX += shiftX;
    state->ragdollBlockOriginY += shiftY;
    return true;
}

static void AndroidEditTarget(AndroidWalkState *state, bool place)
{
    if (state == NULL || state->scene == NULL ||
        state->scene->cameraGetForwardVector == NULL)
        return;
    const double origin[3] = {state->cameraRelativeEye[0], state->cameraRelativeEye[1],
                              state->cameraRelativeEye[2]};
    const int64_t offset[3] = {state->renderOriginBlock[0],
                               state->renderOriginBlock[1],
                               state->renderOriginBlock[2]};
    float direction[3] = {0.0f, 1.0f, 0.0f};
    state->scene->cameraGetForwardVector(&state->camera, direction);
    WalkWorldBlockHit hit;
    if (!WalkWorldRaycast(AndroidReadVisualBlock, state, origin, offset,
                          direction, WALK_WORLD_INTERACTION_DISTANCE, &hit))
    {
        AndroidLog(state, ANDROID_LOG_INFO, "block edit ray found no block");
        return;
    }
    if (!WalkWorldEditHit(AndroidSetWorldBlock, state, &hit, place,
                          state->selectedMaterial))
    {
        AndroidLog(state, ANDROID_LOG_WARN, "block edit provider rejected the change");
        return;
    }
    const int64_t *edited = place ? hit.previousBlock : hit.block;
    WalkVisualsInvalidateBlock(&state->chunkSet, state->device,
                               edited[0], edited[1], edited[2]);
}

static bool AndroidInitializeRagdoll(AndroidWalkState *state)
{
    if (state == NULL || state->physics == NULL ||
        state->physics->ragdollInitialize == NULL)
        return false;
    state->ragdollCollision = (VoxelCollisionSource){
        .context = state,
        .queryBlockPhysics = AndroidRagdollQueryBlock,
    };
    const double origin[3] = {0.0, 0.0, 1.02};
    if (!WalkHumanoidInitialize(
            state->physics, &state->ragdoll, &state->ragdollCollision, origin,
            ANDROID_WALK_RAGDOLL_STABLE_ID, &state->ragdollRigidSettings,
            &state->ragdollSettings, &state->ragdollScratch,
            &state->ragdollScratchBytes))
    {
        return false;
    }
    state->ragdollReady = true;
    state->ragdollGrounded = WalkHumanoidIsGrounded(
        &state->ragdoll, &state->ragdollCollision);
    return true;
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
    WalkVisualsDestroyTerrain(state->device, state->texturedTerrainBuffers,
                              state->terrainTextures, &state->terrainSampler);
    state->texturedTerrainReady = false;
}

static bool AndroidReadAsset(void *context, const char *assetPath,
                             uint8_t **outBytes, uint32_t *outSize)
{
    AndroidWalkState *state = (AndroidWalkState *)context;
    if (outBytes != NULL)
        *outBytes = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (state == NULL || state->app == NULL || state->app->activity == NULL ||
        state->app->activity->assetManager == NULL || assetPath == NULL ||
        outBytes == NULL || outSize == NULL)
        return false;
    AAsset *asset = AAssetManager_open(state->app->activity->assetManager,
                                       assetPath, AASSET_MODE_BUFFER);
    if (asset == NULL)
        return false;
    const off_t length = AAsset_getLength(asset);
    if (length <= 0 || (uint64_t)length > UINT32_MAX)
    {
        AAsset_close(asset);
        return false;
    }
    const uint32_t size = (uint32_t)length;
    uint8_t *bytes = (uint8_t *)PlatformAllocate(size, false);
    uint32_t read = 0u;
    while (bytes != NULL && read < size)
    {
        const int32_t result = AAsset_read(asset, bytes + read, size - read);
        if (result <= 0)
            break;
        read += (uint32_t)result;
    }
    AAsset_close(asset);
    if (bytes == NULL || read != size)
    {
        PlatformFree(bytes);
        return false;
    }
    *outBytes = bytes;
    *outSize = size;
    return true;
}

static bool AndroidCreateRagdollBuffer(AndroidWalkState *state)
{
    if (state == NULL || !state->ragdollReady || state->ragdollBuffer != 0u)
        return false;
    state->ragdollVisualScratch =
        (WalkRagdollVisualScratch *)PlatformAllocate(sizeof(*state->ragdollVisualScratch),
                                                     false);
    if (state->ragdollVisualScratch == NULL ||
        !WalkVisualsCreateRagdollBuffer(state->device, &state->ragdollBuffer))
    {
        PlatformFree(state->ragdollVisualScratch);
        state->ragdollVisualScratch = NULL;
        return false;
    }
    return true;
}

static bool AndroidUpdateRagdollBuffer(AndroidWalkState *state)
{
    return state != NULL && state->ragdollReady &&
           WalkVisualsUpdateRagdollBuffer(state->device, state->ragdollBuffer,
                                          &state->ragdoll, state->cameraRenderOrigin,
                                          state->ragdollVisualScratch);
}

static bool AndroidCreateTexturedTerrain(AndroidWalkState *state)
{
    if (state == NULL)
        return false;
    state->texturedTerrainReady = WalkVisualsCreateTerrain(
        state->device, AndroidReadAsset, state, state->texturedTerrainBuffers,
        state->terrainTextures, &state->terrainSampler);
    return state->texturedTerrainReady;
}

static void AndroidDestroyDevice(AndroidWalkState *state)
{
    if (state == NULL)
        return;
    AndroidReleaseGraphicsHandle(state, &state->ragdollBuffer);
    WalkVisualsDestroyBuffer(state->device, &state->farTerrainBuffer);
    PlatformFree(state->ragdollVisualScratch);
    state->ragdollVisualScratch = NULL;
    WalkVisualsDestroyChunkSet(state->device, state->mesher, &state->chunkSet);
    state->chunksReady = false;
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
    if (state->scene != NULL && state->scene->cameraInit != NULL)
        state->scene->cameraInit(&state->camera, 0.0, 0.0, 0.0, 0.0f, -0.32f);
    if (!AndroidCreateTexturedTerrain(state))
        AndroidLog(state, ANDROID_LOG_WARN,
                   "Android walk terrain textures could not be loaded");
    if (!WalkVisualsCreateFarTerrainBuffer(state->device,
                                           &state->farTerrainBuffer))
        AndroidLog(state, ANDROID_LOG_WARN, "far terrain LOD buffer unavailable");
    state->chunksReady = WalkVisualsCreateChunkSet(state->mesher, &state->chunkSet);
    int64_t chunkCenter[3] = {0, 0, 0};
    if (state->chunksReady && AndroidGetChunkCoordinates(state, chunkCenter))
        state->chunksReady = WalkVisualsUpdateChunkSet(
            state->device, state->mesher, &state->chunkSet,
            AndroidReadVisualBlock, state, chunkCenter);
    else
        state->chunksReady = false;
    if (!state->chunksReady)
        AndroidLog(state, ANDROID_LOG_ERROR, "real voxel chunk mesh initialization failed");
    if (state->chunksReady)
    {
        LaiueGraphicsDrawItemV2 initialDraws[WALK_VISUAL_CHUNK_DRAW_COUNT];
        const uint32_t initialDrawCount = WalkVisualsBuildChunkDraws(
            &state->chunkSet, state->renderOriginBlock, state->terrainTextures,
            state->terrainSampler, initialDraws);
        __android_log_print(ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                            "initial terrain chunks ready; draw items=%u textures=%u/%u/%u",
                            initialDrawCount, state->terrainTextures[0] != 0u,
                            state->terrainTextures[1] != 0u,
                            state->terrainTextures[2] != 0u);
    }
    if (state->ragdollReady && !AndroidCreateRagdollBuffer(state))
        AndroidLog(state, ANDROID_LOG_WARN, "ragdoll mesh buffer unavailable");

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
        const int32_t keyCode = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) == 0)
        {
            if (keyCode == AKEYCODE_V)
            {
                state->firstPerson = !state->firstPerson;
                return 1;
            }
            if (keyCode >= AKEYCODE_1 && keyCode <= AKEYCODE_3)
            {
                state->selectedMaterial = (uint8_t)(keyCode - AKEYCODE_1 + 1);
                return 1;
            }
        }
        const int32_t index = AndroidKeyIndex(keyCode);
        if (index < 0)
            return 0;
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
    float actionCenters[3][2];
    float actionRadius = 0.0f;
    AndroidTouchLayout(state, &joystickCenterX, &joystickCenterY, &joystickRadius,
                       &jumpCenterX, &jumpCenterY, &sprintCenterX, &sprintCenterY,
                       &buttonRadius);
    AndroidTouchActionLayout(state, actionCenters, &actionRadius);
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
    {
        state->touchActive = true;
        const float buttonHitRadius = buttonRadius * 1.20f;
        if (AndroidTouchInsideCircle(x, y, actionCenters[0][0], actionCenters[0][1],
                                     actionRadius * 1.25f))
        {
            state->firstPerson = !state->firstPerson;
        }
        else if (AndroidTouchInsideCircle(x, y, actionCenters[1][0], actionCenters[1][1],
                                          actionRadius * 1.25f))
        {
            state->breakPending = true;
        }
        else if (AndroidTouchInsideCircle(x, y, actionCenters[2][0], actionCenters[2][1],
                                          actionRadius * 1.25f))
        {
            state->placePending = true;
        }
        else if (state->jumpPointerId < 0 &&
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
            state->sprintToggled = !state->sprintToggled;
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
                dy = -dy;
                AndroidWalkClampStick(&dx, &dy);
                state->joystickX = dx;
                state->joystickY = dy;
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
            state->sprintPointerId = -1;
        if (pointerId == state->jumpPointerId)
            state->jumpPointerId = -1;
        state->touchActive = state->joystickActive || state->lookActive ||
                             state->sprintPointerId >= 0 || state->jumpPointerId >= 0;
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
    if (state->scene->cameraUpdate != NULL)
        state->scene->cameraUpdate(&state->camera, elapsed, false, false, false, false, false,
                                   state->lookDeltaX, state->lookDeltaY, 0.0f, 0.0025f);
    state->lookDeltaX = 0;
    state->lookDeltaY = 0;
    memset(state->cameraRenderOrigin, 0, sizeof(state->cameraRenderOrigin));
    memset(state->renderOriginBlock, 0, sizeof(state->renderOriginBlock));
    if (state->ragdollReady)
    {
        double pelvis[3];
        if (!VoxelRigidBodyLocalPosition(
                &state->ragdoll.bodies[state->ragdoll.rootBody], pelvis))
            return;
        int64_t centerBlock[3];
        if (!AndroidGetChunkCoordinates(state, centerBlock))
            return;
        state->cameraRenderOrigin[2] = (double)state->renderOriginBlock[2];
        float forward[3] = {0.0f, 1.0f, 0.0f};
        if (state->scene->cameraGetForwardVector != NULL)
            state->scene->cameraGetForwardVector(&state->camera, forward);
        double eye[3];
        const double fallbackEye[3] = {pelvis[0], pelvis[1], pelvis[2] + 1.8};
        if (!WalkHumanoidResolveCameraEye(
                &state->ragdoll, &state->ragdollCollision, forward,
                state->firstPerson, fallbackEye, state->lastSafeCameraEye,
                &state->lastSafeCameraEyeValid, eye))
            return;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            state->cameraRelativeEye[axis] =
                (float)(eye[axis] - state->cameraRenderOrigin[axis]);
    }
    else
    {
        state->cameraRelativeEye[0] = 0.0f;
        state->cameraRelativeEye[1] = 0.0f;
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
                int64_t fractionX = position.localX % 1000;
                int64_t fractionY = position.localY % 1000;
                if (fractionX < 0) fractionX += 1000;
                if (fractionY < 0) fractionY += 1000;
                const int64_t originX = AndroidFloorDiv(blockX, 64) * 64;
                const int64_t originY = AndroidFloorDiv(blockY, 64) * 64;
                state->renderOriginBlock[0] = originX;
                state->renderOriginBlock[1] = originY;
                state->renderOriginBlock[2] =
                    AndroidFloorDiv(AndroidFloorDiv(localZ, 1000), 64) * 64;
                state->cameraRelativeEye[0] =
                    (float)(blockX - originX) + (float)fractionX / 1000.0f;
                state->cameraRelativeEye[1] =
                    (float)(blockY - originY) + (float)fractionY / 1000.0f;
            }
        }
        if (state->character == NULL || state->controller == NULL)
        {
            state->cameraRelativeEye[0] = 0.0f;
            state->cameraRelativeEye[1] = 0.0f;
        }
        const int64_t originZFixed =
            AndroidFloorDiv(localZ, INT64_C(64000)) * INT64_C(64000);
        state->renderOriginBlock[2] = originZFixed / 1000;
        state->cameraRenderOrigin[2] = (double)state->renderOriginBlock[2];
        state->cameraRelativeEye[2] =
            (float)(localZ - originZFixed) / 1000.0f + 1.6f;
    }
    float view[16];
    float projection[16];
    state->scene->cameraGetViewMatrix(&state->camera, state->cameraRelativeEye, view);
    state->scene->cameraGetProjectionMatrix(
        height > 0 ? (float)width / (float)height : 1.0f,
        1.04719755f, 0.1f, 1024.0f, projection);
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

static void AndroidAppendTouchGlyph(LaiueGraphicsUiQuadV1 *quads,
                                    uint32_t capacity, uint32_t *count,
                                    float centerX, float centerY, float radius,
                                    uint32_t glyph)
{
    static const uint8_t rows[3][7] = {
        {17u, 17u, 17u, 17u, 17u, 10u, 4u}, /* V: view */
        {17u, 10u, 4u, 4u, 4u, 10u, 17u},   /* X: break */
        {4u, 4u, 4u, 31u, 4u, 4u, 4u},      /* +: place */
    };
    if (quads == NULL || count == NULL || glyph >= 3u)
        return;
    const float cell = radius * 0.095f;
    const float gap = radius * 0.035f;
    const float width = 5.0f * cell + 4.0f * gap;
    const float height = 7.0f * cell + 6.0f * gap;
    const float left = centerX - width * 0.5f;
    const float top = centerY - height * 0.5f;
    for (uint32_t y = 0u; y < 7u && *count < capacity; ++y)
        for (uint32_t x = 0u; x < 5u && *count < capacity; ++x)
            if ((rows[glyph][y] & (1u << (4u - x))) != 0u)
            {
                const float x0 = left + (float)x * (cell + gap);
                const float y0 = top + (float)y * (cell + gap);
                quads[(*count)++] = AndroidTouchQuad(
                    x0, y0, x0 + cell, y0 + cell, cell * 0.12f,
                    UINT32_C(0xFFF5F7FA));
            }
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

    LaiueGraphicsUiQuadV1 quads[48];
    uint32_t quadCount = 0u;
    quads[quadCount++] = AndroidTouchQuad(
        joystickCenterX - joystickRadius, joystickCenterY - joystickRadius,
        joystickCenterX + joystickRadius, joystickCenterY + joystickRadius,
        joystickRadius, UINT32_C(0x702A3448));
    const float knobX = joystickCenterX + state->joystickX * joystickRadius * 0.58f;
    const float knobY = joystickCenterY - state->joystickY * joystickRadius * 0.58f;
    const float knobRadius = joystickRadius * 0.42f;
    quads[quadCount++] = AndroidTouchQuad(
        knobX - knobRadius, knobY - knobRadius, knobX + knobRadius, knobY + knobRadius,
        knobRadius, state->joystickActive ? UINT32_C(0xD0E7F1FF) : UINT32_C(0xA0B9C7D8));

    const uint32_t sprintColor = state->sprintToggled
                                     ? UINT32_C(0xE047B3FF)
                                     : UINT32_C(0x90425A70);
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
    float actionCenters[3][2];
    float actionRadius = 0.0f;
    AndroidTouchActionLayout(state, actionCenters, &actionRadius);
    const uint32_t viewColor = state->firstPerson
                                   ? UINT32_C(0xE047B3FF)
                                   : UINT32_C(0x90425A70);
    quads[quadCount++] = AndroidTouchQuad(
        actionCenters[0][0] - actionRadius, actionCenters[0][1] - actionRadius,
        actionCenters[0][0] + actionRadius, actionCenters[0][1] + actionRadius,
        actionRadius, viewColor);
    quads[quadCount++] = AndroidTouchQuad(
        actionCenters[1][0] - actionRadius, actionCenters[1][1] - actionRadius,
        actionCenters[1][0] + actionRadius, actionCenters[1][1] + actionRadius,
        actionRadius, UINT32_C(0xC0D85D56));
    quads[quadCount++] = AndroidTouchQuad(
        actionCenters[2][0] - actionRadius, actionCenters[2][1] - actionRadius,
        actionCenters[2][0] + actionRadius, actionCenters[2][1] + actionRadius,
        actionRadius, UINT32_C(0xC05FAF68));
    AndroidAppendTouchGlyph(quads, 48u, &quadCount,
                            actionCenters[0][0], actionCenters[0][1], actionRadius, 0u);
    AndroidAppendTouchGlyph(quads, 48u, &quadCount,
                            actionCenters[1][0], actionCenters[1][1], actionRadius, 1u);
    AndroidAppendTouchGlyph(quads, 48u, &quadCount,
                            actionCenters[2][0], actionCenters[2][1], actionRadius, 2u);
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
    const bool characterCanStep =
        state->controller != NULL && state->character != NULL &&
        AndroidFieldPresent(state->characterServiceSize, state->character->structSize,
                            offsetof(LaiueCharacterServiceV1, step),
                            sizeof(state->character->step)) &&
        state->character->step != NULL;
    const bool ragdollCanStep = state->ragdollReady && state->physics != NULL &&
                                state->physics->ragdollStep != NULL &&
                                state->physics->ragdollDrive != NULL;
    while ((ragdollCanStep || characterCanStep) && state->accumulator >= fixedStep &&
           ticks < 8u)
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
        float worldX = 0.0f;
        float worldY = 0.0f;
        AndroidWalkCameraRelativeMovement(
            strafe, forwardInput, forward[0], forward[1], &worldX, &worldY);
        if (ragdollCanStep)
        {
            const bool sprint = state->keyDown[4] || state->sprintToggled;
            const bool jump = state->jumpPending;
            state->jumpPending = false;
            WalkHumanoidStepFailure stepFailure = WALK_HUMANOID_STEP_OK;
            const bool humanoidStepped = WalkHumanoidStep(
                    state->physics, &state->ragdoll, &state->humanoidController,
                    &state->ragdollCollision,
                    &state->ragdollRigidSettings, &state->ragdollSettings,
                    state->ragdollScratch, state->ragdollScratchBytes,
                    worldX, worldY, sprint, jump, fixedStep,
                    &state->ragdollGrounded, &state->ragdollFacingYaw,
                    &state->ragdollGaitPhase, &stepFailure);
            if (!humanoidStepped || !AndroidRebaseRagdoll(state))
            {
                __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                                    "deterministic ragdoll step failed at stage %u (rebase=%u)",
                                    (uint32_t)stepFailure, humanoidStepped ? 1u : 0u);
                state->running = false;
                break;
            }
        }
        else
        {
            input.moveX = AndroidWalkAxisToFixed(worldX);
            input.moveY = AndroidWalkAxisToFixed(worldY);
            input.flags |= LAIUE_CHARACTER_INPUT_ANALOG;
            if (state->keyDown[4] || state->sprintToggled)
                input.flags |= LAIUE_CHARACTER_INPUT_SPRINT;
            if (state->jumpPending)
            {
                input.flags |= LAIUE_CHARACTER_INPUT_JUMP;
                state->jumpPending = false;
            }
            if (state->character->step(state->controller, &input) == 0u)
            {
                AndroidLog(state, ANDROID_LOG_ERROR,
                           "deterministic character step failed");
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
            if (state->breakPending)
                AndroidEditTarget(state, false);
            if (state->placePending)
                AndroidEditTarget(state, true);
            state->breakPending = false;
            state->placePending = false;
            int64_t chunkCenter[3] = {0, 0, 0};
            if (state->chunksReady &&
                AndroidGetChunkCoordinates(state, chunkCenter) &&
                !WalkVisualsUpdateChunkSet(state->device, state->mesher,
                                           &state->chunkSet, AndroidReadVisualBlock,
                                           state, chunkCenter))
            {
                AndroidLog(state, ANDROID_LOG_ERROR, "voxel chunk rebuild failed");
                state->running = false;
            }
            const bool ragdollMeshReady = AndroidUpdateRagdollBuffer(state);
            const uint32_t began = state->device->beginFrame(
                state->device, (uint32_t)width, (uint32_t)height);
            uint32_t submitted = 1u;
            uint32_t sceneDrawCount = 0u;
            if (began != 0u && (state->chunksReady || ragdollMeshReady) &&
                AndroidFieldPresent(state->device->structSize, state->device->structSize,
                                    offsetof(LaiueGraphicsDeviceV2, submit),
                                    sizeof(state->device->submit)) &&
                state->device->submit != NULL)
            {
                LaiueGraphicsDrawItemV2 draws[WALK_VISUAL_CHUNK_DRAW_COUNT + 2u];
                uint32_t drawIndex = 0u;
                if (WalkVisualsBuildFarTerrainDraw(
                        state->farTerrainBuffer, state->renderOriginBlock,
                        state->terrainTextures, state->terrainSampler,
                        &draws[drawIndex]))
                    ++drawIndex;
                drawIndex += WalkVisualsBuildChunkDraws(
                    &state->chunkSet, state->renderOriginBlock, state->terrainTextures,
                    state->terrainSampler, &draws[drawIndex]);
                if (ragdollMeshReady && !state->firstPerson)
                {
                    draws[drawIndex] = (LaiueGraphicsDrawItemV2){
                        .structSize = sizeof(draws[drawIndex]),
                        .vertexBuffer = state->ragdollBuffer,
                        .indexCount = ANDROID_WALK_RAGDOLL_VERTEX_COUNT,
                        .originRelative = {0.0f, 0.0f, 0.0f},
                        .scale = 1.0f,
                    };
                    ++drawIndex;
                }
                sceneDrawCount = drawIndex;
                submitted = state->device->submit(state->device, draws, drawIndex);
            }
            if (began != 0u)
                AndroidSubmitTouchUi(state, width, height);
            const uint32_t ended = began != 0u
                ? state->device->endFrame(state->device) : 0u;
            if (!state->renderTelemetryLogged)
            {
                __android_log_print(ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                                    "frame telemetry begin=%u draws=%u submit=%u end=%u camera=(%.2f,%.2f,%.2f) vp=(%.3f,%.3f,%.3f,%.3f)",
                                    began, sceneDrawCount, submitted, ended,
                                    state->cameraRelativeEye[0], state->cameraRelativeEye[1],
                                    state->cameraRelativeEye[2], state->viewProjection[0],
                                    state->viewProjection[5], state->viewProjection[10],
                                    state->viewProjection[15]);
                state->renderTelemetryLogged = true;
            }
            if (began == 0u || submitted == 0u || ended == 0u)
                AndroidLog(state, ANDROID_LOG_WARN, "frame skipped after surface change");
        }
    }
}

void android_main(struct android_app *app)
{
    AndroidWalkState state;
    memset(&state, 0, sizeof(state));
    state.selectedMaterial = 1u;
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
        if (!AndroidInitializeRagdoll(&state))
            AndroidLog(&state, ANDROID_LOG_ERROR,
                       "deterministic ragdoll initialization failed");
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
    WalkHumanoidRelease(state.physics, &state.ragdoll, &state.ragdollScratch);
    state.ragdollScratchBytes = 0u;
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
