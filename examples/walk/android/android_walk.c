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
#include "../walk_gameplay.h"
#include "walk_runtime.h"
#include "../walk_math.h"
#include "touch_controls.h"
#include "../walk_visuals.h"
#include "../walk_scenario.h"
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
#define ANDROID_WALK_RAGDOLL_VERTEX_COUNT WALK_RAGDOLL_VISUAL_VERTEX_COUNT
#define ANDROID_WALK_RAGDOLL_STABLE_ID UINT64_C(0x57414C4B52414744)

const LaiueModuleApiV1 *LaiueGraphicsGetStaticModuleApiV1(void);
const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void);

typedef struct AndroidWalkState AndroidWalkState;

struct AndroidWalkState
{
    struct android_app *app;
    LaiueModuleHost *host;
    WalkGameplay game;
    WalkGameplayServices gameServices;
    WalkGameplaySpatialSnapshot spatial;
    const LaiueMesherServiceV1 *mesher;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    uint32_t graphicsServiceSize;
    LaiueGraphicsHandle ragdollBuffer;
    WalkRagdollVisualScratch *ragdollVisualScratch;
    LaiueGraphicsDeviceV2 *device;
    WalkVisualChunkSet chunkSet;
    bool chunksReady;
    bool chunkFrameInvalid;
    LaiueGraphicsHandle texturedTerrainBuffers[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainTextures[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainSampler;
    LaiueGraphicsHandle farTerrainBuffer;
    bool texturedTerrainReady;
    bool windowReady;
    bool focused;
    bool running;
    bool failed;
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
    float lookDeltaX;
    float lookDeltaY;
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
    /* Diagnostics are explicitly enabled by an intent extra. The ordinary
     * game neither
     * samples timings nor emits per-frame scenario output. */
    bool scenarioEnabled;
    bool scenarioFailed;
    bool scenarioFinished;
    bool scenarioCapturePending;
    bool scenarioCaptureWaiting;
    bool scenarioLifecyclePending;
    uint32_t scenarioTick;
    uint32_t scenarioFrames;
    uint32_t scenarioBreaks;
    uint32_t scenarioPlaces;
    uint32_t scenarioGeneration;
    uint32_t scenarioSkippedFrames;
    uint32_t scenarioCaptureTick;
    uint32_t scenarioCaptureCheckpoint;
    uint32_t scenarioRunId;
    uint32_t scenarioCheckpoints;
    uint32_t scenarioAcks;
    uint32_t scenarioRebases;
    bool scenarioMoved;
    bool scenarioAirborne;
    bool scenarioJumpEligible;
    WalkGameplaySpatialSnapshot scenarioJumpStartSpatial;
    bool scenarioPositionValid;
    WalkGameplaySpatialSnapshot scenarioStartSpatial;
    uint64_t scenarioGpuFrame;
    uint64_t scenarioPeakCpuBytes;
    uint64_t scenarioPeakGeometryBytes;
    double scenarioCaptureDeadline;
    WalkScenario scenario;
    WalkScenarioMetrics *scenarioCpu;
    WalkScenarioMetrics *scenarioGpu;
};

static void AndroidLog(AndroidWalkState *state, int priority, const char *message)
{
    (void)state;
    __android_log_print(priority, ANDROID_WALK_LOG_TAG, "%s", message == NULL ? "" : message);
}

/* android_main runs on the glue thread, which is normally detached from
 * ART. Keep local
 * references bounded and never detach an already attached
 * thread. A missing/false extra always
 * leaves normal gameplay unchanged. */
static bool AndroidScenarioIntentEnabled(struct android_app *app, uint32_t *outRunId)
{
    if (outRunId == NULL)
        return false;
    *outRunId = 0u;
    if (app == NULL || app->activity == NULL || app->activity->vm == NULL ||
        app->activity->clazz == NULL)
        return false;
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    bool attached = false;
    bool localFrame = false;
    bool enabled = false;
    const jint status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED)
    {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
            return false;
        attached = true;
    }
    else if (status != JNI_OK)
        return false;
    if ((*env)->PushLocalFrame(env, 8) < 0)
        goto cleanup;
    localFrame = true;
    jclass activityClass = (*env)->GetObjectClass(env, app->activity->clazz);
    if ((*env)->ExceptionCheck(env) || activityClass == NULL)
        goto cleanup;
    jmethodID getIntent =
        (*env)->GetMethodID(env, activityClass, "getIntent", "()Landroid/content/Intent;");
    if ((*env)->ExceptionCheck(env) || getIntent == NULL)
        goto cleanup;
    jobject intent = (*env)->CallObjectMethod(env, app->activity->clazz, getIntent);
    if ((*env)->ExceptionCheck(env) || intent == NULL)
        goto cleanup;
    jclass intentClass = (*env)->GetObjectClass(env, intent);
    if ((*env)->ExceptionCheck(env) || intentClass == NULL)
        goto cleanup;
    jmethodID getExtra =
        (*env)->GetMethodID(env, intentClass, "getBooleanExtra", "(Ljava/lang/String;Z)Z");
    if ((*env)->ExceptionCheck(env) || getExtra == NULL)
        goto cleanup;
    jstring name = (*env)->NewStringUTF(env, "laiueScenario");
    if ((*env)->ExceptionCheck(env) || name == NULL)
        goto cleanup;
    enabled = (*env)->CallBooleanMethod(env, intent, getExtra, name, JNI_FALSE) == JNI_TRUE;
    if ((*env)->ExceptionCheck(env))
        goto cleanup;
    if (enabled)
    {
        jmethodID getRun =
            (*env)->GetMethodID(env, intentClass, "getIntExtra", "(Ljava/lang/String;I)I");
        if ((*env)->ExceptionCheck(env) || getRun == NULL)
            goto cleanup;
        jstring runName = (*env)->NewStringUTF(env, "laiueScenarioRunId");
        if ((*env)->ExceptionCheck(env) || runName == NULL)
            goto cleanup;
        const jint run = (*env)->CallIntMethod(env, intent, getRun, runName, 0);
        if ((*env)->ExceptionCheck(env))
            goto cleanup;
        if (run <= 0)
        {
            enabled = false;
            __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                                "LAIUE_SCENARIO event=failure run=0 stage=intent_run_id");
        }
        else
            *outRunId = (uint32_t)run;
    }
cleanup:
    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        enabled = false;
        __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                            "LAIUE_SCENARIO event=failure run=%u stage=intent_jni", *outRunId);
    }
    if (localFrame)
        (*env)->PopLocalFrame(env, NULL);
    if (attached)
        (void)(*vm)->DetachCurrentThread(vm);
    return enabled;
}

static void AndroidScenarioFail(AndroidWalkState *state, const char *stage)
{
    if (state == NULL || !state->scenarioEnabled || state->scenarioFailed)
        return;
    state->scenarioFailed = true;
    __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                        "LAIUE_SCENARIO event=failure run=%u status=FAIL stage=%s tick=%u",
                        state->scenarioRunId, stage, state->scenarioTick);
}

static bool AndroidGameplayHealthy(AndroidWalkState *state, const char *stage)
{
    const WalkGameplayStatus status = WalkGameplayHealth(&state->game);
    if (status == WALK_GAMEPLAY_OK && !state->failed)
        return true;
    if (!state->failed)
        __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                            "shared gameplay failed at %s status=%u", stage, (uint32_t)status);
    state->failed = true;
    state->running = false;
    AndroidScenarioFail(state, stage);
    return false;
}

static bool AndroidFieldPresent(uint32_t actualSize, uint32_t declaredSize, size_t offset,
                                size_t size)
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

static void AndroidTouchLayout(const AndroidWalkState *state, float *joystickCenterX,
                               float *joystickCenterY, float *joystickRadius, float *jumpCenterX,
                               float *jumpCenterY, float *sprintCenterX, float *sprintCenterY,
                               float *buttonRadius)
{
    const float width = state != NULL && state->width > 0 ? (float)state->width : 1.0f;
    const float height = state != NULL && state->height > 0 ? (float)state->height : 1.0f;
    const float minimum = AndroidTouchMinimumDimension(state);
    float stickRadius = minimum * ANDROID_WALK_TOUCH_JOYSTICK_RADIUS;
    float actionRadius = minimum * ANDROID_WALK_TOUCH_BUTTON_RADIUS;
    float margin = minimum * ANDROID_WALK_TOUCH_MARGIN;
    if (stickRadius < 56.0f)
        stickRadius = 56.0f;
    if (actionRadius < 48.0f)
        actionRadius = 48.0f;
    if (margin < 24.0f)
        margin = 24.0f;

    if (joystickCenterX != NULL)
        *joystickCenterX = margin + stickRadius;
    if (joystickCenterY != NULL)
        *joystickCenterY = height - margin - stickRadius;
    if (joystickRadius != NULL)
        *joystickRadius = stickRadius;
    if (jumpCenterX != NULL)
        *jumpCenterX = width - margin - actionRadius;
    if (jumpCenterY != NULL)
        *jumpCenterY = height - margin - actionRadius;
    if (sprintCenterX != NULL)
        *sprintCenterX = width - margin - actionRadius * 3.8f;
    if (sprintCenterY != NULL)
        *sprintCenterY = height - margin - actionRadius;
    if (buttonRadius != NULL)
        *buttonRadius = actionRadius;
}

static void AndroidTouchActionLayout(const AndroidWalkState *state, float centers[3][2],
                                     float *radius)
{
    const float width = state != NULL && state->width > 0 ? (float)state->width : 1.0f;
    const float minimum = AndroidTouchMinimumDimension(state);
    float actionRadius = minimum * 0.075f;
    float margin = minimum * ANDROID_WALK_TOUCH_MARGIN;
    if (actionRadius < 32.0f)
        actionRadius = 32.0f;
    if (margin < 18.0f)
        margin = 18.0f;
    const float y = margin + actionRadius;
    if (centers != NULL)
    {
        centers[0][0] = width - margin - actionRadius;
        centers[1][0] = width - margin - actionRadius * 3.1f;
        centers[2][0] = width - margin - actionRadius * 5.2f;
        for (uint32_t i = 0u; i < 3u; ++i)
            centers[i][1] = y;
    }
    if (radius != NULL)
        *radius = actionRadius;
}

static bool AndroidTouchInsideCircle(float x, float y, float centerX, float centerY, float radius)
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
    WalkGameplayResetInput(&state->game);
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
    LaiueModuleBinaryV1 binaries[9] = {0};
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        binaries[index].flags = LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL;
        binaries[index].staticApi = modules[index];
    }
    LaiueModuleLoadReportEntryV1 entries[9];
    LaiueModuleLoadReportV1 report;
    LaiueModuleLoadReportInitialize(&report, entries, moduleCount);
    LaiueModuleDiagnostic diagnostic;
    const LaiueModuleStatus status =
        LaiueModuleHostLoadProfile(state->host, binaries, moduleCount,
                                   LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &report, &diagnostic);
    if (status != LAIUE_MODULE_OK)
    {
        AndroidLog(state, ANDROID_LOG_ERROR, diagnostic.message);
        return 0u;
    }
    for (uint32_t index = 0u; index < report.count; ++index)
        if ((entries[index].flags & LAIUE_MODULE_PROFILE_ENTRY_SKIPPED) != 0u)
            AndroidLog(state, ANDROID_LOG_INFO, entries[index].message);
    WalkGameplayServices *services = &state->gameServices;
    services->character = (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        LAIUE_CHARACTER_SERVICE_V1_LEGACY_SIZE, NULL, &services->characterSize);
    services->physics = (const LaiuePhysicsServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_PHYSICS_SERVICE_NAME, LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
        sizeof(LaiuePhysicsServiceV1), NULL, &services->physicsSize);
    services->voxel = (const LaiueVoxelServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_VOXEL_SERVICE_NAME, LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        LAIUE_VOXEL_SERVICE_V1_LEGACY_SIZE, NULL, &services->voxelSize);
    services->scene = (const LaiueSceneServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_SCENE_SERVICE_NAME, LAIUE_SCENE_SERVICE_ABI_VERSION_1,
        offsetof(LaiueSceneServiceV1, cameraInit), NULL, &services->sceneSize);
    services->sceneMath = (const LaiueSceneMathServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_SCENE_MATH_SERVICE_NAME, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
        offsetof(LaiueSceneMathServiceV1, matrix4Multiply), NULL, &services->sceneMathSize);
    state->mesher = (const LaiueMesherServiceV1 *)LaiueModuleHostQueryService(
        state->host, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        sizeof(LaiueMesherServiceV1), NULL, NULL);
    state->graphics = (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
        state->host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
        LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2, LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE,
        NULL, &state->graphicsServiceSize);
    return 1u;
}

static uint8_t AndroidReadVisualBlock(void *opaque, int64_t x, int64_t y, int64_t z)
{
    AndroidWalkState *state = (AndroidWalkState *)opaque;
    return state == NULL ? 0u : WalkGameplayReadBlock(&state->game, x, y, z);
}

static bool AndroidGetChunkCoordinates(AndroidWalkState *state, int64_t centerBlock[3])
{
    WalkGameplaySpatialSnapshot snapshot;
    if (state == NULL || centerBlock == NULL ||
        WalkGameplaySnapshot(&state->game, &snapshot) != WALK_GAMEPLAY_OK)
        return false;
    memcpy(centerBlock, snapshot.centerBlock, sizeof(snapshot.centerBlock));
    return true;
}

static bool AndroidScenarioRebase(AndroidWalkState *state)
{
    WalkGameplaySpatialSnapshot before, after;
    WalkGameplayRebaseResult rebase;
    double delta[3];
    const int64_t shift[3] = {-8960, 0, 0};
    if (WalkGameplaySnapshot(&state->game, &before) != WALK_GAMEPLAY_OK ||
        WalkGameplayRebase(&state->game, shift, &rebase) != WALK_GAMEPLAY_OK || !rebase.changed ||
        WalkGameplayRebase(&state->game, NULL, &rebase) != WALK_GAMEPLAY_OK || !rebase.changed ||
        WalkGameplaySnapshot(&state->game, &after) != WALK_GAMEPLAY_OK ||
        WalkMathAbs(after.rootLocal[0]) > 64.0 || WalkMathAbs(after.rootLocal[1]) > 64.0 ||
        !WalkGameplaySpatialDelta(&before, &after, delta))
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!WalkMathFinite(delta[axis]) || WalkMathAbs(delta[axis]) > 0.001)
            return false;
    ++state->scenarioRebases;
    return true;
}

static void AndroidScenarioObserve(AndroidWalkState *state, WalkScenarioPhase phase)
{
    WalkGameplaySpatialSnapshot spatial;
    if (WalkGameplaySnapshot(&state->game, &spatial) != WALK_GAMEPLAY_OK ||
        (spatial.valid & WALK_GAMEPLAY_SPATIAL_ACTOR_VALID) == 0u)
    {
        AndroidScenarioFail(state, "position");
        return;
    }
    if (!state->scenarioPositionValid)
    {
        state->scenarioStartSpatial = spatial;
        state->scenarioPositionValid = true;
    }
    double movement[3];
    if (!WalkGameplaySpatialDelta(&state->scenarioStartSpatial, &spatial, movement))
    {
        AndroidScenarioFail(state, "position_delta");
        return;
    }
    state->scenarioMoved |= movement[0] * movement[0] + movement[1] * movement[1] > 0.0025;
    if (phase == WalkScenarioJump && state->scenarioJumpEligible && !spatial.grounded)
    {
        double jump[3];
        if (!WalkGameplaySpatialDelta(&state->scenarioJumpStartSpatial, &spatial, jump))
            AndroidScenarioFail(state, "jump_delta");
        else if (jump[2] > 0.05)
            state->scenarioAirborne = true;
    }
}

static void AndroidScenarioSummary(AndroidWalkState *state)
{
    if (state->scenarioFinished || state->scenarioFailed)
        return;
    if (!AndroidGameplayHealthy(state, "summary_gameplay"))
        return;
    WalkGameplaySpatialSnapshot spatial;
    if (WalkGameplaySnapshot(&state->game, &spatial) != WALK_GAMEPLAY_OK)
    {
        AndroidScenarioFail(state, "summary_snapshot");
        return;
    }
    WalkScenarioSummary cpu = {0}, gpu = {0};
    const bool hasCpu =
        state->scenarioCpu != NULL && WalkScenarioMetricsGetSummary(state->scenarioCpu, &cpu) != 0u;
    const bool hasGpu =
        state->scenarioGpu != NULL && WalkScenarioMetricsGetSummary(state->scenarioGpu, &gpu) != 0u;
    const bool success =
        hasCpu && state->game.ragdollReady && state->chunksReady && state->texturedTerrainReady &&
        state->terrainTextures[0] != 0u && state->terrainTextures[1] != 0u &&
        state->terrainTextures[2] != 0u && state->scenarioFrames != 0u &&
        state->scenarioCheckpoints == (uint32_t)WalkScenarioComplete &&
        state->scenarioAcks == (uint32_t)WalkScenarioComplete && state->scenarioMoved &&
        state->scenarioAirborne && state->scenarioBreaks != 0u && state->scenarioPlaces != 0u &&
        state->scenarioRebases != 0u;
    state->scenarioFinished = true;
    __android_log_print(
        success ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
        "LAIUE_SCENARIO event=summary run=%u status=%s ticks=%u frames=%u checkpoints=%u acks=%u "
        "moved=%u "
        "airborne=%u breaks=%u places=%u rebases=%u frame_wall_count=%u frame_wall_p95_ms=%.6f "
        "frame_wall_p99_ms=%.6f gpu_available=%u gpu_count=%u gpu_p95_ms=%.6f gpu_p99_ms=%.6f "
        "peak_cpu_shadow_bytes=%llu peak_geometry_bytes=%llu capture=external_adb "
        "game_tick=%llu game_revision=%llu yaw=%.5f pitch=%.5f first_person=%u material=%u "
        "root_block_x=%lld root_block_y=%lld root_block_z=%lld "
        "root_fraction_x=%.9f root_fraction_y=%.9f root_fraction_z=%.9f "
        "provider_frame_x=%lld provider_frame_y=%lld provider_frame_z=%lld",
        state->scenarioRunId, success ? "PASS" : "FAIL", state->scenarioTick, state->scenarioFrames,
        state->scenarioCheckpoints, state->scenarioAcks, state->scenarioMoved ? 1u : 0u,
        state->scenarioAirborne ? 1u : 0u, state->scenarioBreaks, state->scenarioPlaces,
        state->scenarioRebases, cpu.count, cpu.p95, cpu.p99, hasGpu ? 1u : 0u, gpu.count, gpu.p95,
        gpu.p99, (unsigned long long)state->scenarioPeakCpuBytes,
        (unsigned long long)state->scenarioPeakGeometryBytes, (unsigned long long)state->game.tick,
        (unsigned long long)state->game.revision, state->game.camera.yaw, state->game.camera.pitch,
        state->game.firstPerson ? 1u : 0u, (uint32_t)state->game.selectedMaterial,
        (long long)spatial.centerBlock[0], (long long)spatial.centerBlock[1],
        (long long)spatial.centerBlock[2], spatial.rootFraction[0], spatial.rootFraction[1],
        spatial.rootFraction[2], (long long)spatial.providerFrameOrigin[0],
        (long long)spatial.providerFrameOrigin[1], (long long)spatial.providerFrameOrigin[2]);
    if (!success)
        state->scenarioFailed = true;
}

static void AndroidScenarioFrame(AndroidWalkState *state, double started, bool presented,
                                 bool simulated)
{
    if (!state->scenarioEnabled || state->scenarioFailed)
        return;
    if (!presented)
    {
        if (++state->scenarioSkippedFrames >= 120u)
            AndroidScenarioFail(state, "presentation_timeout");
        return;
    }
    state->scenarioSkippedFrames = 0u;
    ++state->scenarioFrames;
    LaiueGraphicsDiagnosticsV2 diagnostics = {.structSize = sizeof(diagnostics)};
    const bool hasDiagnostics =
        state->device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_DIAGNOSTICS_SIZE &&
        state->device->getDiagnostics != NULL &&
        state->device->getDiagnostics(state->device, &diagnostics) != 0u;
    if (!hasDiagnostics || (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) == 0u ||
        diagnostics.drawCalls == 0u)
    {
        AndroidScenarioFail(state, "graphics_diagnostics");
        return;
    }
    if (hasDiagnostics)
    {
        if (diagnostics.cpuShadowBytes > state->scenarioPeakCpuBytes)
            state->scenarioPeakCpuBytes = diagnostics.cpuShadowBytes;
        if (diagnostics.geometryPoolUsedBytes > state->scenarioPeakGeometryBytes)
            state->scenarioPeakGeometryBytes = diagnostics.geometryPoolUsedBytes;
        if (!state->scenarioFinished && simulated && state->scenarioGpu != NULL &&
            (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_VALID) != 0u &&
            diagnostics.gpuFrameIndex != state->scenarioGpuFrame)
        {
            state->scenarioGpuFrame = diagnostics.gpuFrameIndex;
            (void)WalkScenarioMetricsAdd(state->scenarioGpu,
                                         (double)diagnostics.gpuDurationNanoseconds / 1000000.0);
        }
    }
    /* endFrame may include vsync/presentation, hence frame wall time rather
     * than CPU time.
     * Screenshot holds and ordinary idle frames are excluded. */
    if (!state->scenarioFinished && simulated && state->scenarioCpu != NULL)
        (void)WalkScenarioMetricsAdd(state->scenarioCpu,
                                     (PlatformMonotonicSeconds() - started) * 1000.0);
    if (state->scenarioLifecyclePending)
    {
        WalkGameplaySpatialSnapshot spatial;
        if (!AndroidGameplayHealthy(state, "presented_gameplay"))
            return;
        if (WalkGameplaySnapshot(&state->game, &spatial) != WALK_GAMEPLAY_OK)
        {
            AndroidScenarioFail(state, "presented_snapshot");
            return;
        }
        __android_log_print(
            ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
            "LAIUE_SCENARIO event=presented run=%u generation=%u frame=%u width=%d height=%d "
            "game_tick=%llu game_revision=%llu yaw=%.5f pitch=%.5f first_person=%u material=%u "
            "root_block_x=%lld root_block_y=%lld root_block_z=%lld "
            "root_fraction_x=%.9f root_fraction_y=%.9f root_fraction_z=%.9f "
            "provider_frame_x=%lld provider_frame_y=%lld provider_frame_z=%lld",
            state->scenarioRunId, state->scenarioGeneration, state->scenarioFrames, state->width,
            state->height, (unsigned long long)state->game.tick,
            (unsigned long long)state->game.revision, state->game.camera.yaw,
            state->game.camera.pitch, state->game.firstPerson ? 1u : 0u,
            (uint32_t)state->game.selectedMaterial, (long long)spatial.centerBlock[0],
            (long long)spatial.centerBlock[1], (long long)spatial.centerBlock[2],
            spatial.rootFraction[0], spatial.rootFraction[1], spatial.rootFraction[2],
            (long long)spatial.providerFrameOrigin[0], (long long)spatial.providerFrameOrigin[1],
            (long long)spatial.providerFrameOrigin[2]);
        state->scenarioLifecyclePending = false;
    }
    if (state->scenarioCapturePending)
    {
        WalkScenarioInput input;
        if (WalkScenarioInputAt(&state->scenario, state->scenarioCaptureTick, &input) == 0u)
            AndroidScenarioFail(state, "checkpoint_input");
        else
        {
            ++state->scenarioCheckpoints;
            __android_log_print(
                ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                "LAIUE_SCENARIO event=checkpoint run=%u phase=%s checkpoint=%u tick=%u frame=%u "
                "width=%d "
                "height=%d yaw=%.5f pitch=%.5f first_person=%u capture=external_adb "
                "readback_supported=%u",
                state->scenarioRunId, WalkScenarioPhaseName(input.phase), input.checkpoint,
                state->scenarioCaptureTick, state->scenarioFrames, state->width, state->height,
                state->game.camera.yaw, state->game.camera.pitch, state->game.firstPerson ? 1u : 0u,
                hasDiagnostics &&
                        (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_READBACK_SUPPORTED) != 0u
                    ? 1u
                    : 0u);
            state->scenarioCaptureWaiting = true;
            state->scenarioCaptureCheckpoint = input.checkpoint;
            state->scenarioCaptureDeadline = PlatformMonotonicSeconds() + 30.0;
        }
        state->scenarioCapturePending = false;
    }
    if (!state->scenarioFinished && !state->scenarioCaptureWaiting &&
        state->scenarioTick >= WALK_SCENARIO_TOTAL_TICKS)
        AndroidScenarioSummary(state);
}

static void AndroidReleaseGraphicsHandle(AndroidWalkState *state, LaiueGraphicsHandle *handle)
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
    WalkVisualsDestroyTerrain(state->device, state->texturedTerrainBuffers, state->terrainTextures,
                              &state->terrainSampler);
    state->texturedTerrainReady = false;
}

static bool AndroidReadAsset(void *context, const char *assetPath, uint8_t **outBytes,
                             uint32_t *outSize)
{
    AndroidWalkState *state = (AndroidWalkState *)context;
    if (outBytes != NULL)
        *outBytes = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (state == NULL || state->app == NULL || state->app->activity == NULL ||
        state->app->activity->assetManager == NULL || assetPath == NULL || outBytes == NULL ||
        outSize == NULL)
        return false;
    AAsset *asset =
        AAssetManager_open(state->app->activity->assetManager, assetPath, AASSET_MODE_BUFFER);
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
    if (state == NULL || !state->game.ragdollReady || state->ragdollBuffer != 0u)
        return false;
    state->ragdollVisualScratch =
        (WalkRagdollVisualScratch *)PlatformAllocate(sizeof(*state->ragdollVisualScratch), false);
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
    return state != NULL && state->game.ragdollReady &&
           WalkVisualsUpdateRagdollBuffer(&state->game.physicsContext, state->device,
                                          state->ragdollBuffer, &state->game.ragdoll,
                                          state->spatial.ragdollRenderOrigin,
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
    state->chunkFrameInvalid = false;
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

static bool AndroidUpdateCamera(AndroidWalkState *state, int32_t width, int32_t height);

static void AndroidCreateDevice(AndroidWalkState *state)
{
    if (state != NULL && (state->failed || state->game.failed))
    {
        AndroidDestroyDevice(state);
        return;
    }
    if (state == NULL || !state->game.initialized || state->app == NULL ||
        state->app->window == NULL || state->graphics == NULL ||
        !AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                             offsetof(LaiueGraphicsDeviceServiceV2, destroyDevice),
                             sizeof(state->graphics->destroyDevice)) ||
        state->graphics->destroyDevice == NULL)
    {
        if (state != NULL)
            AndroidScenarioFail(state, "device_service");
        return;
    }
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
        state->graphics->createDeviceWithContext != NULL && state->graphics->context != NULL)
        created = state->graphics->createDeviceWithContext(
            state->graphics->context, state->app->window, width, height,
            LAIUE_GRAPHICS_BACKEND_VULKAN, &state->device);
    else if (width > 0 && height > 0 &&
             AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                                 offsetof(LaiueGraphicsDeviceServiceV2, createDevice),
                                 sizeof(state->graphics->createDevice)) &&
             state->graphics->createDevice != NULL)
        created = state->graphics->createDevice(state->app->window, width, height,
                                                LAIUE_GRAPHICS_BACKEND_VULKAN, &state->device);
    if (created == 0u || state->device == NULL)
    {
        AndroidDestroyDevice(state);
        AndroidScenarioFail(state, "device_create");
        AndroidLog(state, ANDROID_LOG_ERROR, "Vulkan device/surface creation failed");
        return;
    }
    state->windowReady = true;
    state->running = state->focused && !state->failed && !state->game.failed;
    state->width = width;
    state->height = height;
    state->lastTime = PlatformMonotonicSeconds();
    (void)AndroidUpdateCamera(state, width, height);
    if (state->failed || state->game.failed)
    {
        AndroidDestroyDevice(state);
        return;
    }
    if (!AndroidCreateTexturedTerrain(state))
        AndroidLog(state, ANDROID_LOG_WARN, "Android walk terrain textures could not be loaded");
    if (!WalkVisualsCreateFarTerrainBuffer(state->device, &state->farTerrainBuffer))
        AndroidLog(state, ANDROID_LOG_WARN, "far terrain LOD buffer unavailable");
    state->chunksReady = WalkVisualsCreateChunkSet(state->mesher, &state->chunkSet);
    int64_t chunkCenter[3] = {0, 0, 0};
    if (state->chunksReady && AndroidGetChunkCoordinates(state, chunkCenter))
        state->chunksReady =
            WalkVisualsUpdateChunkSet(state->device, state->mesher, &state->chunkSet,
                                      AndroidReadVisualBlock, state, chunkCenter);
    else
        state->chunksReady = false;
    if (!AndroidGameplayHealthy(state, "chunk_initialize_provider"))
    {
        AndroidDestroyDevice(state);
        return;
    }
    if (!state->chunksReady)
    {
        AndroidLog(state, ANDROID_LOG_INFO, "voxel chunk mesher unavailable; using far terrain");
        AndroidScenarioFail(state, "chunk_initialize");
    }
    if (state->chunksReady)
    {
        LaiueGraphicsDrawItemV2 initialDraws[WALK_VISUAL_CHUNK_DRAW_COUNT];
        const uint32_t initialDrawCount =
            WalkVisualsBuildChunkDraws(&state->chunkSet, state->spatial.renderOriginBlock,
                                       state->terrainTextures, state->terrainSampler, initialDraws);
        __android_log_print(ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                            "initial terrain chunks ready; draw items=%u textures=%u/%u/%u",
                            initialDrawCount, state->terrainTextures[0] != 0u,
                            state->terrainTextures[1] != 0u, state->terrainTextures[2] != 0u);
    }
    if (state->game.ragdollReady && !AndroidCreateRagdollBuffer(state))
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
        state->touchUiReady =
            state->device->setUiFontAtlas(state->device, &touchUiAtlasPixel, 1u, 1u) != 0u;
}

static int32_t AndroidKeyIndex(int32_t keyCode)
{
    switch (keyCode)
    {
    case AKEYCODE_W:
        return 0;
    case AKEYCODE_A:
        return 1;
    case AKEYCODE_S:
        return 2;
    case AKEYCODE_D:
        return 3;
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT:
        return 4;
    case AKEYCODE_SPACE:
        return 5;
    case AKEYCODE_ESCAPE:
        return 6;
    default:
        return -1;
    }
}

static int32_t AndroidHandleInput(struct android_app *app, AInputEvent *event)
{
    AndroidWalkState *state = (AndroidWalkState *)app->userData;
    if (state == NULL || event == NULL)
        return 0;
    if (state->scenarioEnabled)
    {
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY &&
            AKeyEvent_getKeyCode(event) == AKEYCODE_F12 &&
            AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN &&
            AKeyEvent_getRepeatCount(event) == 0 && state->scenarioCaptureWaiting &&
            !state->scenarioFailed)
        {
            const double acknowledged = PlatformMonotonicSeconds();
            if (acknowledged >= state->scenarioCaptureDeadline)
            {
                AndroidScenarioFail(state, "capture_ack_timeout");
                state->scenarioCaptureWaiting = false;
                return 1;
            }
            state->scenarioCaptureWaiting = false;
            state->scenarioCaptureDeadline = 0.0;
            state->accumulator = 0.0;
            state->lastTime = acknowledged;
            ++state->scenarioAcks;
            __android_log_print(ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                                "LAIUE_SCENARIO event=capture_ack run=%u checkpoint=%u tick=%u",
                                state->scenarioRunId, state->scenarioCaptureCheckpoint,
                                state->scenarioTick);
        }
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
    {
        const int32_t keyCode = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) == 0)
        {
            if (keyCode == AKEYCODE_V)
            {
                (void)WalkGameplaySetView(&state->game, !state->game.firstPerson,
                                          state->game.selectedMaterial);
                return 1;
            }
            if (keyCode >= AKEYCODE_1 && keyCode <= AKEYCODE_3)
            {
                (void)WalkGameplaySetView(&state->game, state->game.firstPerson,
                                          (uint8_t)(keyCode - AKEYCODE_1 + 1));
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
    if (!WalkMathFinite(x) || !WalkMathFinite(y))
        return 1;
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
    AndroidTouchLayout(state, &joystickCenterX, &joystickCenterY, &joystickRadius, &jumpCenterX,
                       &jumpCenterY, &sprintCenterX, &sprintCenterY, &buttonRadius);
    AndroidTouchActionLayout(state, actionCenters, &actionRadius);
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
    {
        state->touchActive = true;
        const float buttonHitRadius = buttonRadius * 1.20f;
        if (AndroidTouchInsideCircle(x, y, actionCenters[0][0], actionCenters[0][1],
                                     actionRadius * 1.25f))
        {
            (void)WalkGameplaySetView(&state->game, !state->game.firstPerson,
                                      state->game.selectedMaterial);
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
                 AndroidTouchInsideCircle(x, y, sprintCenterX, sprintCenterY, buttonHitRadius))
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
            if (!WalkMathFinite(px) || !WalkMathFinite(py))
                continue;
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
                state->lookDeltaX += px - state->lastLookX;
                state->lookDeltaY += py - state->lastLookY;
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
    if (state->scenarioEnabled)
    {
        const char *name = NULL;
        switch (command)
        {
        case APP_CMD_INIT_WINDOW:
            name = "init_window";
            break;
        case APP_CMD_TERM_WINDOW:
            name = "term_window";
            break;
        case APP_CMD_GAINED_FOCUS:
            name = "gained_focus";
            break;
        case APP_CMD_LOST_FOCUS:
            name = "lost_focus";
            break;
        case APP_CMD_CONFIG_CHANGED:
            name = "config_changed";
            break;
        case APP_CMD_WINDOW_RESIZED:
            name = "window_resized";
            break;
        case APP_CMD_CONTENT_RECT_CHANGED:
            name = "content_rect_changed";
            break;
        default:
            break;
        }
        if (name != NULL)
        {
            ++state->scenarioGeneration;
            state->scenarioLifecyclePending = true;
            __android_log_print(ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                                "LAIUE_SCENARIO event=lifecycle run=%u command=%s generation=%u "
                                "game_tick=%llu game_revision=%llu yaw=%.5f pitch=%.5f "
                                "first_person=%u material=%u",
                                state->scenarioRunId, name, state->scenarioGeneration,
                                (unsigned long long)state->game.tick,
                                (unsigned long long)state->game.revision, state->game.camera.yaw,
                                state->game.camera.pitch, state->game.firstPerson ? 1u : 0u,
                                (uint32_t)state->game.selectedMaterial);
        }
    }
    switch (command)
    {
    case APP_CMD_INIT_WINDOW:
        AndroidCreateDevice(state);
        break;
    case APP_CMD_TERM_WINDOW:
        AndroidDestroyDevice(state);
        break;
    case APP_CMD_GAINED_FOCUS:
        state->focused = true;
        if (state->windowReady)
        {
            AndroidResetInputClock(state);
            state->running = state->game.initialized && !state->game.failed && !state->failed &&
                             !state->scenarioFailed;
        }
        break;
    case APP_CMD_LOST_FOCUS:
        state->focused = false;
        state->running = false;
        AndroidResetInputClock(state);
        break;
    case APP_CMD_CONFIG_CHANGED:
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        if (state->app->window != NULL)
        {
            const int32_t width = ANativeWindow_getWidth(state->app->window);
            const int32_t height = ANativeWindow_getHeight(state->app->window);
            if (width > 0 && height > 0 && state->windowReady && state->graphics != NULL &&
                AndroidFieldPresent(state->graphicsServiceSize, state->graphics->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, resize),
                                    sizeof(state->graphics->resize)) &&
                state->graphics->resize != NULL)
                state->graphics->resize(state->device, width, height);
            state->width = width;
            state->height = height;
            AndroidClearTouchState(state);
        }
        break;
    default:
        break;
    }
}

static bool AndroidUpdateCamera(AndroidWalkState *state, int32_t width, int32_t height)
{
    if (state == NULL || width <= 0 || height <= 0)
        return false;
    const WalkGameplayStatus status =
        WalkGameplayCamera(&state->game, (uint32_t)width, (uint32_t)height, &state->spatial);
    if (status == WALK_GAMEPLAY_UNSUPPORTED && !state->scenarioEnabled)
    {
        /* A deliberately partial profile may have no camera service. Its
         * terrain snapshot remains usable; no invalid matrix is submitted. */
        return false;
    }
    if (status != WALK_GAMEPLAY_OK ||
        (state->spatial.valid & WALK_GAMEPLAY_SPATIAL_CAMERA_VALID) == 0u)
    {
        state->failed = true;
        state->running = false;
        AndroidLog(state, ANDROID_LOG_ERROR, "shared gameplay camera failed");
        AndroidScenarioFail(state, "required_camera");
        return false;
    }
    if (state->device == NULL ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
                             offsetof(LaiueGraphicsDeviceV2, setCamera),
                             sizeof(state->device->setCamera)) ||
        state->device->setCamera == NULL)
    {
        state->failed = true;
        state->running = false;
        AndroidLog(state, ANDROID_LOG_ERROR, "graphics camera callback unavailable");
        AndroidScenarioFail(state, "set_camera_unavailable");
        return false;
    }
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    memcpy(camera.viewProjection, state->spatial.viewProjection, sizeof(camera.viewProjection));
    if (state->device->setCamera(state->device, &camera) == 0u)
    {
        state->failed = true;
        state->running = false;
        AndroidLog(state, ANDROID_LOG_ERROR, "graphics camera submission failed");
        AndroidScenarioFail(state, "set_camera");
        return false;
    }
    return true;
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

static void AndroidAppendTouchGlyph(LaiueGraphicsUiQuadV1 *quads, uint32_t capacity,
                                    uint32_t *count, float centerX, float centerY, float radius,
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
                quads[(*count)++] = AndroidTouchQuad(x0, y0, x0 + cell, y0 + cell, cell * 0.12f,
                                                     UINT32_C(0xFFF5F7FA));
            }
}

static void AndroidSubmitTouchUi(AndroidWalkState *state, int32_t width, int32_t height)
{
    if (state == NULL || !state->touchUiReady || state->device == NULL || width <= 0 ||
        height <= 0 ||
        !AndroidFieldPresent(state->device->structSize, state->device->structSize,
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
    AndroidTouchLayout(state, &joystickCenterX, &joystickCenterY, &joystickRadius, &jumpCenterX,
                       &jumpCenterY, &sprintCenterX, &sprintCenterY, &buttonRadius);

    LaiueGraphicsUiQuadV1 quads[48];
    uint32_t quadCount = 0u;
    quads[quadCount++] =
        AndroidTouchQuad(joystickCenterX - joystickRadius, joystickCenterY - joystickRadius,
                         joystickCenterX + joystickRadius, joystickCenterY + joystickRadius,
                         joystickRadius, UINT32_C(0x702A3448));
    const float knobX = joystickCenterX + state->joystickX * joystickRadius * 0.58f;
    const float knobY = joystickCenterY - state->joystickY * joystickRadius * 0.58f;
    const float knobRadius = joystickRadius * 0.42f;
    quads[quadCount++] = AndroidTouchQuad(
        knobX - knobRadius, knobY - knobRadius, knobX + knobRadius, knobY + knobRadius, knobRadius,
        state->joystickActive ? UINT32_C(0xD0E7F1FF) : UINT32_C(0xA0B9C7D8));

    const uint32_t sprintColor = state->sprintToggled ? UINT32_C(0xE047B3FF) : UINT32_C(0x90425A70);
    quads[quadCount++] = AndroidTouchQuad(
        sprintCenterX - buttonRadius, sprintCenterY - buttonRadius, sprintCenterX + buttonRadius,
        sprintCenterY + buttonRadius, buttonRadius, sprintColor);
    const uint32_t jumpColor =
        state->jumpPointerId >= 0 ? UINT32_C(0xE06EC8FF) : UINT32_C(0x906E4C9C);
    quads[quadCount++] = AndroidTouchQuad(jumpCenterX - buttonRadius, jumpCenterY - buttonRadius,
                                          jumpCenterX + buttonRadius, jumpCenterY + buttonRadius,
                                          buttonRadius, jumpColor);
    float actionCenters[3][2];
    float actionRadius = 0.0f;
    AndroidTouchActionLayout(state, actionCenters, &actionRadius);
    const uint32_t viewColor =
        state->game.firstPerson ? UINT32_C(0xE047B3FF) : UINT32_C(0x90425A70);
    quads[quadCount++] =
        AndroidTouchQuad(actionCenters[0][0] - actionRadius, actionCenters[0][1] - actionRadius,
                         actionCenters[0][0] + actionRadius, actionCenters[0][1] + actionRadius,
                         actionRadius, viewColor);
    quads[quadCount++] =
        AndroidTouchQuad(actionCenters[1][0] - actionRadius, actionCenters[1][1] - actionRadius,
                         actionCenters[1][0] + actionRadius, actionCenters[1][1] + actionRadius,
                         actionRadius, UINT32_C(0xC0D85D56));
    quads[quadCount++] =
        AndroidTouchQuad(actionCenters[2][0] - actionRadius, actionCenters[2][1] - actionRadius,
                         actionCenters[2][0] + actionRadius, actionCenters[2][1] + actionRadius,
                         actionRadius, UINT32_C(0xC05FAF68));
    AndroidAppendTouchGlyph(quads, 48u, &quadCount, actionCenters[0][0], actionCenters[0][1],
                            actionRadius, 0u);
    AndroidAppendTouchGlyph(quads, 48u, &quadCount, actionCenters[1][0], actionCenters[1][1],
                            actionRadius, 1u);
    AndroidAppendTouchGlyph(quads, 48u, &quadCount, actionCenters[2][0], actionCenters[2][1],
                            actionRadius, 2u);
    if (state->device->submitUi(state->device, quads, quadCount) == 0u)
        state->touchUiReady = false;
}

static void AndroidStep(AndroidWalkState *state)
{
    if (state == NULL || !state->windowReady || !state->game.initialized || state->failed ||
        state->game.failed)
        return;
    const double now = PlatformMonotonicSeconds();
    double elapsed = state->lastTime == 0.0 ? 0.0 : now - state->lastTime;
    state->lastTime = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > 0.25)
        elapsed = 0.25;
    state->accumulator += elapsed;
    if (state->scenarioEnabled &&
        (state->scenarioCaptureWaiting || state->scenarioFinished || state->scenarioFailed))
        state->accumulator = 0.0;
    const double fixedStep = 1.0 / (double)LAIUE_CHARACTER_TICK_HZ;
    uint32_t ticks = 0u;
    if (!state->scenarioEnabled)
    {
        const WalkGameplayStatus oriented =
            WalkGameplayOrient(&state->game, state->lookDeltaX * 0.0025f,
                               -state->lookDeltaY * 0.0025f, WALK_GAMEPLAY_ORIENT_RELATIVE);
        state->lookDeltaX = 0.0f;
        state->lookDeltaY = 0.0f;
        WalkGameplayInput input = {
            .strafe = (double)((state->keyDown[3] ? 1 : 0) - (state->keyDown[1] ? 1 : 0)),
            .forward = (double)((state->keyDown[0] ? 1 : 0) - (state->keyDown[2] ? 1 : 0)),
            .sprint = state->keyDown[4] || state->sprintToggled,
            .jumpHeld = state->keyDown[5] || state->jumpPointerId >= 0,
            .pulses = (state->jumpPending ? WALK_GAMEPLAY_PULSE_JUMP : 0u) |
                      (state->breakPending ? WALK_GAMEPLAY_PULSE_BREAK : 0u) |
                      (state->placePending ? WALK_GAMEPLAY_PULSE_PLACE : 0u),
        };
        if (state->joystickActive)
        {
            input.strafe = state->joystickX;
            input.forward = state->joystickY;
        }
        if (oriented != WALK_GAMEPLAY_OK ||
            WalkGameplaySubmitInput(&state->game, &input) != WALK_GAMEPLAY_OK)
        {
            AndroidLog(state, ANDROID_LOG_ERROR, "shared gameplay rejected native input");
            state->running = false;
            return;
        }
        state->jumpPending = false;
        state->breakPending = false;
        state->placePending = false;
    }
    while (state->game.initialized && state->accumulator >= fixedStep && ticks < 8u &&
           (!state->scenarioEnabled ||
            (!state->scenarioCapturePending && !state->scenarioCaptureWaiting &&
             !state->scenarioFinished && !state->scenarioFailed)))
    {
        WalkScenarioInput scenarioInput = {0};
        if (state->scenarioEnabled)
        {
            if (WalkScenarioInputAt(&state->scenario, state->scenarioTick, &scenarioInput) == 0u)
                break;
            WalkGameplayInput input = {
                .strafe = scenarioInput.moveX,
                .forward = scenarioInput.moveY,
                .sprint = scenarioInput.sprint != 0u,
                .pulses = (scenarioInput.jump != 0u ? WALK_GAMEPLAY_PULSE_JUMP : 0u) |
                          (scenarioInput.breakBlock != 0u ? WALK_GAMEPLAY_PULSE_BREAK : 0u) |
                          (scenarioInput.placeBlock != 0u ? WALK_GAMEPLAY_PULSE_PLACE : 0u),
            };
            if (WalkGameplayOrient(&state->game, scenarioInput.yaw, scenarioInput.pitch,
                                   WALK_GAMEPLAY_ORIENT_ABSOLUTE) != WALK_GAMEPLAY_OK ||
                WalkGameplaySetView(&state->game, scenarioInput.firstPerson != 0u,
                                    state->game.selectedMaterial) != WALK_GAMEPLAY_OK ||
                WalkGameplaySubmitInput(&state->game, &input) != WALK_GAMEPLAY_OK)
            {
                AndroidScenarioFail(state, "scenario_input");
                break;
            }
            state->sprintToggled = input.sprint;
            if (scenarioInput.jump != 0u)
            {
                WalkGameplaySpatialSnapshot spatial;
                if (WalkGameplaySnapshot(&state->game, &spatial) != WALK_GAMEPLAY_OK ||
                    (spatial.valid & WALK_GAMEPLAY_SPATIAL_ACTOR_VALID) == 0u || !spatial.grounded)
                    AndroidScenarioFail(state, "jump_not_grounded");
                else
                {
                    state->scenarioJumpEligible = true;
                    state->scenarioJumpStartSpatial = spatial;
                }
            }
        }
        WalkGameplayStepResult stepResult = {0};
        const WalkGameplayStatus stepped = WalkGameplayStep(&state->game, &stepResult);
        if (stepped != WALK_GAMEPLAY_OK)
        {
            __android_log_print(ANDROID_LOG_ERROR, ANDROID_WALK_LOG_TAG,
                                "shared gameplay step failed status=%u humanoid_stage=%u",
                                (uint32_t)stepped, (uint32_t)stepResult.humanoidFailure);
            state->running = false;
            AndroidScenarioFail(state, "gameplay_step");
            break;
        }
        if (stepResult.rebase.providerFrameShift[0] != 0 ||
            stepResult.rebase.providerFrameShift[1] != 0 ||
            stepResult.rebase.providerFrameShift[2] != 0)
            state->chunkFrameInvalid = true;
        if (state->scenarioEnabled)
        {
            AndroidScenarioObserve(state, scenarioInput.phase);
            if (scenarioInput.rebaseCheck != 0u && !AndroidScenarioRebase(state))
                AndroidScenarioFail(state, "rebase_check");
            if (scenarioInput.capture != 0u)
            {
                state->scenarioCaptureTick = state->scenarioTick;
                state->scenarioCapturePending = true;
            }
            ++state->scenarioTick;
        }
        state->accumulator -= fixedStep;
        ++ticks;
    }
    if (ticks == 8u && state->accumulator >= fixedStep)
        state->accumulator = 0.0;
    bool scenarioPresented = false;
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
        if (width > 0 && height > 0 && (width != state->width || height != state->height) &&
            state->graphics != NULL &&
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
            bool cameraReady = AndroidUpdateCamera(state, width, height);
            if (state->failed || state->game.failed)
                return;
            if ((state->game.pendingPulses &
                 (WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE)) != 0u)
            {
                WalkGameplayEditResult edits = {0};
                const WalkGameplayStatus edited =
                    WalkGameplayEdit(&state->game, &state->spatial, &edits);
                bool changed = false;
                for (uint32_t index = 0u; index < edits.count; ++index)
                {
                    const WalkGameplayEditItem *item = &edits.items[index];
                    if (item->changed)
                    {
                        WalkVisualsInvalidateBlock(&state->chunkSet, state->device,
                                                   item->coordinate.x, item->coordinate.y,
                                                   item->coordinate.z);
                        changed = true;
                    }
                    if (state->scenarioEnabled)
                    {
                        if (item->place)
                            state->scenarioPlaces += item->changed ? 1u : 0u;
                        else
                            state->scenarioBreaks += item->changed ? 1u : 0u;
                        if (!item->changed || item->status != WALK_GAMEPLAY_OK)
                            AndroidScenarioFail(state,
                                                item->place ? "place_readback" : "break_readback");
                    }
                }
                if (edited != WALK_GAMEPLAY_OK && state->scenarioEnabled)
                    AndroidScenarioFail(state, "edit");
                else if (edited != WALK_GAMEPLAY_OK && edited != WALK_GAMEPLAY_UNSUPPORTED)
                    AndroidLog(state, ANDROID_LOG_INFO, "shared block edit unavailable this frame");
                if (changed)
                    cameraReady = AndroidUpdateCamera(state, width, height);
            }
            if (state->failed || state->game.failed)
            {
                state->running = false;
                return;
            }
            if (state->chunkFrameInvalid)
            {
                WalkVisualsInvalidateChunkSet(state->device, &state->chunkSet);
                state->chunkFrameInvalid = false;
            }
            int64_t chunkCenter[3] = {0, 0, 0};
            if (state->chunksReady)
            {
                if (!AndroidGetChunkCoordinates(state, chunkCenter))
                {
                    state->failed = true;
                    state->running = false;
                    AndroidLog(state, ANDROID_LOG_ERROR, "shared chunk origin failed");
                    AndroidScenarioFail(state, "chunk_origin");
                    return;
                }
                if (!WalkVisualsUpdateChunkSet(state->device, state->mesher, &state->chunkSet,
                                               AndroidReadVisualBlock, state, chunkCenter))
                {
                    state->failed = true;
                    state->running = false;
                    AndroidLog(state, ANDROID_LOG_ERROR, "voxel chunk rebuild failed");
                    AndroidScenarioFail(state, "chunk_rebuild");
                    return;
                }
            }
            if (!AndroidGameplayHealthy(state, "chunk_rebuild_provider"))
                return;
            const bool ragdollMeshReady = AndroidUpdateRagdollBuffer(state);
            const uint32_t began =
                state->device->beginFrame(state->device, (uint32_t)width, (uint32_t)height);
            uint32_t submitted = 1u;
            uint32_t sceneDrawCount = 0u;
            if (began != 0u && cameraReady &&
                (state->chunksReady || ragdollMeshReady || state->farTerrainBuffer != 0u) &&
                AndroidFieldPresent(state->device->structSize, state->device->structSize,
                                    offsetof(LaiueGraphicsDeviceV2, submit),
                                    sizeof(state->device->submit)) &&
                state->device->submit != NULL)
            {
                LaiueGraphicsDrawItemV2 draws[WALK_VISUAL_CHUNK_DRAW_COUNT + 2u];
                uint32_t drawIndex = 0u;
                if (WalkVisualsBuildFarTerrainDraw(
                        state->farTerrainBuffer, state->spatial.renderOriginBlock,
                        state->terrainTextures, state->terrainSampler, &draws[drawIndex]))
                    ++drawIndex;
                drawIndex += WalkVisualsBuildChunkDraws(
                    &state->chunkSet, state->spatial.renderOriginBlock, state->terrainTextures,
                    state->terrainSampler, &draws[drawIndex]);
                if (ragdollMeshReady && !state->game.firstPerson)
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
            const uint32_t ended = began != 0u ? state->device->endFrame(state->device) : 0u;
            scenarioPresented =
                began != 0u && submitted != 0u && ended != 0u && sceneDrawCount != 0u;
            if (!state->renderTelemetryLogged)
            {
                __android_log_print(
                    ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                    "frame telemetry begin=%u draws=%u submit=%u end=%u camera=(%.2f,%.2f,%.2f) "
                    "vp=(%.3f,%.3f,%.3f,%.3f)",
                    began, sceneDrawCount, submitted, ended, state->spatial.cameraRelativeEye[0],
                    state->spatial.cameraRelativeEye[1], state->spatial.cameraRelativeEye[2],
                    state->spatial.viewProjection[0], state->spatial.viewProjection[5],
                    state->spatial.viewProjection[10], state->spatial.viewProjection[15]);
                state->renderTelemetryLogged = true;
            }
            if (began == 0u || submitted == 0u || ended == 0u)
                AndroidLog(state, ANDROID_LOG_WARN, "frame skipped after surface change");
        }
    }
    AndroidScenarioFrame(state, now, scenarioPresented, ticks != 0u);
}

void android_main(struct android_app *app)
{
    AndroidWalkState state;
    memset(&state, 0, sizeof(state));
    state.app = app;
    state.scenarioEnabled = AndroidScenarioIntentEnabled(app, &state.scenarioRunId);
    if (state.scenarioEnabled)
    {
        WalkScenarioInitialize(&state.scenario);
        state.scenarioCpu =
            (WalkScenarioMetrics *)PlatformAllocate(sizeof(*state.scenarioCpu), false);
        state.scenarioGpu =
            (WalkScenarioMetrics *)PlatformAllocate(sizeof(*state.scenarioGpu), false);
        if (state.scenarioCpu == NULL || state.scenarioGpu == NULL)
            AndroidScenarioFail(&state, "metrics_allocate");
        else
        {
            WalkScenarioMetricsInitialize(state.scenarioCpu);
            WalkScenarioMetricsInitialize(state.scenarioGpu);
            __android_log_print(
                ANDROID_LOG_INFO, ANDROID_WALK_LOG_TAG,
                "LAIUE_SCENARIO event=start run=%u tick_hz=%u ticks=%u capture=external_adb",
                state.scenarioRunId, WALK_SCENARIO_TICK_RATE, WALK_SCENARIO_TOTAL_TICKS);
        }
    }
    app->userData = &state;
    app->onAppCmd = AndroidHandleCommand;
    app->onInputEvent = AndroidHandleInput;

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    state.host = LaiueModuleHostCreate(&config, &diagnostic);
    if (state.host == NULL || !AndroidLoadModules(&state))
    {
        AndroidLog(&state, ANDROID_LOG_ERROR, "module graph failed");
        AndroidScenarioFail(&state, "module_graph");
    }
    else
    {
        WalkGameplayConfig gameplayConfig;
        WalkGameplayConfigDefault(&gameplayConfig);
        gameplayConfig.initialPosition = (LaiueCharacterPositionV1){
            .cellX = 0,
            .cellY = 0,
            .localX = 0,
            .localY = 0,
            .localZ = 1400,
        };
        gameplayConfig.characterHalfExtent = ANDROID_WALK_HALF_EXTENT;
        gameplayConfig.stableIdBase = ANDROID_WALK_RAGDOLL_STABLE_ID;
        /* Preserve Android projection/spawn baseline as explicit game settings.
         * These are never reset by surface or device recreation. */
        gameplayConfig.initialPitch = -0.32f;
        gameplayConfig.nearPlane = 0.1f;
        gameplayConfig.farPlane = 1024.0f;
        if (state.scenarioEnabled)
            gameplayConfig.requiredCapabilities =
                WALK_GAMEPLAY_CAP_RAGDOLL | WALK_GAMEPLAY_CAP_EDIT | WALK_GAMEPLAY_CAP_CAMERA |
                WALK_GAMEPLAY_CAP_REBASE;
        const WalkGameplayStatus gameplayStatus =
            WalkGameplayInit(&state.game, &state.gameServices, &gameplayConfig);
        if (gameplayStatus != WALK_GAMEPLAY_OK)
        {
            AndroidLog(&state, ANDROID_LOG_ERROR, "shared gameplay initialization failed");
            AndroidScenarioFail(&state, "gameplay_init");
        }
    }

    while (app->destroyRequested == 0)
    {
        if (state.scenarioCaptureWaiting &&
            PlatformMonotonicSeconds() >= state.scenarioCaptureDeadline)
        {
            AndroidScenarioFail(&state, "capture_ack_timeout");
            state.scenarioCaptureWaiting = false;
        }
        int events = 0;
        struct android_poll_source *source = NULL;
        int timeout = state.running && state.windowReady ? 0
                      : state.scenarioCaptureWaiting     ? 250
                                                         : -1;
        while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0)
        {
            if (source != NULL && source->process != NULL)
                source->process(app, source);
            if (state.scenarioCaptureWaiting &&
                PlatformMonotonicSeconds() >= state.scenarioCaptureDeadline)
            {
                AndroidScenarioFail(&state, "capture_ack_timeout");
                state.scenarioCaptureWaiting = false;
                break;
            }
            if (app->destroyRequested != 0)
                break;
            timeout = state.running && state.windowReady ? 0
                      : state.scenarioCaptureWaiting     ? 250
                                                         : -1;
            if (timeout == 0)
                break;
        }
        if (state.running && state.windowReady)
            AndroidStep(&state);
    }

    AndroidDestroyDevice(&state);
    PlatformFree(state.scenarioCpu);
    PlatformFree(state.scenarioGpu);
    WalkGameplayRelease(&state.game);
    if (state.host != NULL)
    {
        LaiueModuleHostUnloadAll(state.host);
        LaiueModuleHostDestroy(state.host);
    }
}
