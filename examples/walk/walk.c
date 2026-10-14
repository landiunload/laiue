#include "character/character_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "numeric/numeric_service.h"
#include "core/math/scalar.h"
#include "voxel/voxel_service.h"
#include "world/world_service.h"
#include "walk_runtime.h"
#include "walk_gameplay.h"
#include "walk_math.h"
#if defined(LAIUE_WALK_WINDOWED)
#include "walk_world.h"
#endif
#if defined(LAIUE_WALK_WINDOWED)
const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void);
#include "graphics/graphics_device_service.h"
#include "render/graphics_service.h"
#include "scene/scene_service.h"
#include "input/input_service.h"
#include "platform/window_service.h"
#include "ui/ui_service.h"
#include "physics/physics_service.h"
#include "mesh/mesher_service.h"
#include "walk_humanoid.h"
#include "walk_physics.h"
#include "walk_visuals.h"
#include "walk_scenario.h"
#endif

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(LAIUE_WALK_WINDOWED)
#include <math.h>
#include <wchar.h>
#endif

#if defined(LAIUE_WALK_WINDOWED)
typedef struct WalkWindowState
{
    const LaiueWindowServiceV1 *windowService;
    const LaiueInputServiceV1 *inputService;
    const LaiueGraphicsDeviceServiceV2 *graphicsService;
    uint32_t graphicsServiceSize;
    const LaiueUiServiceV1 *uiService;
    const LaiueMesherServiceV1 *mesherService;
    WalkGameplay *game;
    WalkGameplaySpatialSnapshot spatial;
    WalkGameplaySpatialSnapshot scenarioStartSpatial;
    Window *window;
    Input *input;
    LaiueGraphicsDeviceV2 *device;
    void *uiContext;
    const uint8_t *fontPixels;
    uint32_t fontWidth;
    uint32_t fontHeight;
    bool cameraReady;
    WalkRagdollVisualScratch *ragdollVisualScratch;
    WalkVisualChunkSet chunkSet;
    bool chunksReady;
    LaiueGraphicsHandle ragdollBuffer;
    LaiueGraphicsHandle texturedTerrainBuffers[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainTextures[WALK_VISUAL_TEXTURE_COUNT];
    LaiueGraphicsHandle terrainSampler;
    LaiueGraphicsHandle farTerrainBuffer;
    bool texturedTerrainReady;
    double lastTime;
    double accumulator;
    bool failed;
    bool smokeEnabled;
    uint32_t smokeFrames;
    bool scenarioEnabled;
    WalkScenario scenario;
    WalkScenarioInput scenarioInput;
    WalkScenarioMetrics *scenarioMetrics;
    WalkScenarioMetrics *scenarioGpuMetrics;
    uint32_t scenarioTick;
    uint32_t scenarioFrames;
    uint32_t scenarioDrawFrames;
    uint32_t scenarioCheckpoints;
    uint32_t scenarioImages;
    uint32_t scenarioBreaks;
    uint32_t scenarioPlaces;
    uint32_t scenarioCheckpointPhase;
    bool scenarioCheckpointPending;
    bool scenarioBreakPending;
    bool scenarioPlacePending;
    bool scenarioAirborne;
    bool scenarioJumpStarted;
    double scenarioJumpStartZ;
    uint32_t scenarioRebases;
    double scenarioHoldUntil;
    double scenarioMaximumDisplacement;
    uint64_t scenarioLastGpuFrame;
    uint64_t scenarioPeakShadowBytes;
    uint64_t scenarioPeakGeometryBytes;
} WalkWindowState;

static LaiueUiQuadV1 walkUiQuads[LAIUE_GRAPHICS_UI_MAX_QUADS];
static LaiueGraphicsDrawItemV2 walkDraws[WALK_VISUAL_CHUNK_DRAW_COUNT + 2u];

static bool WalkDeviceFieldPresent(const LaiueGraphicsDeviceV2 *device, size_t offset, size_t size)
{
    return device != NULL && (size_t)device->structSize >= offset &&
           (size_t)device->structSize - offset >= size;
}

static uint8_t WalkReadVisualBlock(void *opaque, int64_t x, int64_t y, int64_t z)
{
    return WalkGameplayReadBlock((const WalkGameplay *)opaque, x, y, z);
}

static bool WalkUpdateCamera(WalkWindowState *state)
{
    state->cameraReady = false;
    int32_t width = 0, height = 0;
    state->windowService->getClientSize(state->window, &width, &height);
    if (width <= 0 || height <= 0)
        return WalkGameplaySnapshot(state->game, &state->spatial) == WALK_GAMEPLAY_OK;
    const WalkGameplayStatus status =
        WalkGameplayCamera(state->game, (uint32_t)width, (uint32_t)height, &state->spatial);
    if (status == WALK_GAMEPLAY_UNSUPPORTED)
        return !state->scenarioEnabled &&
               WalkGameplaySnapshot(state->game, &state->spatial) == WALK_GAMEPLAY_OK;
    if (status != WALK_GAMEPLAY_OK ||
        (state->spatial.valid & WALK_GAMEPLAY_SPATIAL_CAMERA_VALID) == 0u)
        return false;
    if (!WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, setCamera),
                                sizeof(state->device->setCamera)) ||
        state->device->setCamera == NULL)
        return !state->scenarioEnabled;
    LaiueGraphicsCameraV2 graphicsCamera = {.structSize = sizeof(graphicsCamera)};
    memcpy(graphicsCamera.viewProjection, state->spatial.viewProjection,
           sizeof(graphicsCamera.viewProjection));
    state->cameraReady = state->device->setCamera(state->device, &graphicsCamera) != 0u;
    return state->cameraReady;
}

static bool WalkCheckGameplayHealth(WalkWindowState *state)
{
    if (WalkGameplayHealth(state->game) == WALK_GAMEPLAY_OK)
        return true;
    PlatformWriteConsoleUtf8(state->scenarioEnabled
                                 ? "LAIUE_SCENARIO event=failure reason=gameplay_health\n"
                                 : "laiue walk: gameplay provider failed; stopping presentation\n");
    return false;
}

static bool WalkApplyEdits(WalkWindowState *state)
{
    /* A minimized camera-capable window defers edits. A profile without
     * camera capability lets the common core consume unsupported actions. */
    if ((state->spatial.valid & WALK_GAMEPLAY_SPATIAL_CAMERA_VALID) == 0u &&
        (state->game->capabilities & WALK_GAMEPLAY_CAP_CAMERA) != 0u)
        return !state->scenarioEnabled;
    WalkGameplayEditResult result;
    const WalkGameplayStatus status = WalkGameplayEdit(state->game, &state->spatial, &result);
    if (status != WALK_GAMEPLAY_OK && status != WALK_GAMEPLAY_UNSUPPORTED)
        return false;
    if (result.count > 2u)
        return false;
    bool changed = false;
    bool sawBreak = false, sawPlace = false;
    for (uint32_t index = 0u; index < result.count; ++index)
    {
        const WalkGameplayEditItem *item = &result.items[index];
        if (item->place)
            sawPlace = true;
        else
            sawBreak = true;
        if (item->status != WALK_GAMEPLAY_OK && item->status != WALK_GAMEPLAY_UNSUPPORTED)
            return false;
        if (item->changed)
        {
            changed = true;
            WalkVisualsInvalidateBlock(&state->chunkSet, state->device, item->coordinate.x,
                                       item->coordinate.y, item->coordinate.z);
            if (state->scenarioEnabled)
            {
                if (item->place)
                    ++state->scenarioPlaces;
                else
                    ++state->scenarioBreaks;
            }
        }
        else if (state->scenarioEnabled &&
                 (item->place ? state->scenarioPlacePending : state->scenarioBreakPending))
            return false;
    }
    if (state->scenarioEnabled &&
        ((state->scenarioBreakPending && !sawBreak) || (state->scenarioPlacePending && !sawPlace)))
        return false;
    state->scenarioBreakPending = false;
    state->scenarioPlacePending = false;
    return !changed || WalkUpdateCamera(state);
}

static void WalkRawInput(void *opaque, void *rawInput)
{
    WalkWindowState *state = (WalkWindowState *)opaque;
    if (state != NULL && state->inputService != NULL && state->input != NULL &&
        state->inputService->handleRawInput != NULL)
        state->inputService->handleRawInput(state->input, rawInput);
}

static bool WalkJoinPath(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
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

static bool WalkReadDesktopAsset(void *context, const char *relativePath, uint8_t **outBytes,
                                 uint32_t *outSize)
{
    (void)context;
    if (outBytes != NULL)
        *outBytes = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (relativePath == NULL || outBytes == NULL || outSize == NULL)
        return false;
    char utf8Path[512] = "assets/";
    const size_t prefixLength = sizeof("assets/") - 1u;
    size_t pathLength = 0u;
    while (relativePath[pathLength] != '\0' && pathLength < sizeof(utf8Path) - prefixLength - 1u)
        ++pathLength;
    if (relativePath[pathLength] != '\0')
        return false;
    memcpy(utf8Path + prefixLength, relativePath, pathLength + 1u);
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t relativeWide[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t fullPath[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t wideLength = 0u;
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !PlatformUtf8ToWide(utf8Path, (uint32_t)(prefixLength + pathLength), relativeWide,
                            LAIUE_PLATFORM_PATH_CAPACITY, &wideLength) ||
        !WalkJoinPath(fullPath, directory, relativeWide))
        return false;
    (void)wideLength;
    uint64_t fileBytes = 0u;
    if (!PlatformReadEntireFile(fullPath, UINT64_C(8) * 1024u * 1024u, outBytes, &fileBytes) ||
        fileBytes == 0u || fileBytes > UINT32_MAX)
    {
        PlatformFree(*outBytes);
        *outBytes = NULL;
        return false;
    }
    *outSize = (uint32_t)fileBytes;
    return true;
}

static void WalkScenarioWriteUnsigned(const char *key, uint64_t value)
{
    char digits[21];
    uint32_t offset = sizeof(digits) - 1u;
    digits[offset] = '\0';
    do
    {
        digits[--offset] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    PlatformWriteConsoleUtf8(key);
    PlatformWriteConsoleUtf8(digits + offset);
}

static uint64_t WalkScenarioScaledUnsigned(double value, double scale)
{
    if (!WalkMathFinite(value) || value < 0.0 || value >= 18446744073709551616.0 / scale)
        return UINT64_MAX;
    return (uint64_t)(value * scale);
}

static void WalkScenarioStore32(uint8_t *output, uint32_t value)
{
    for (uint32_t byte = 0u; byte < 4u; ++byte)
        output[byte] = (uint8_t)(value >> (byte * 8u));
}

static bool WalkScenarioCapture(WalkWindowState *state)
{
    int32_t width = 0, height = 0;
    state->windowService->getClientSize(state->window, &width, &height);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        !WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, readbackFrame),
                                sizeof(state->device->readbackFrame)) ||
        state->device->readbackFrame == NULL)
        return false;
    const uint32_t bytes = (uint32_t)width * (uint32_t)height * 4u;
    uint8_t *file = (uint8_t *)PlatformAllocate((size_t)bytes + 54u, false);
    if (file == NULL)
        return false;
    LaiueGraphicsFrameReadbackV2 readback = {
        .structSize = sizeof(readback),
        .pixels = file + 54u,
        .capacityBytes = bytes,
    };
    bool success = state->device->readbackFrame(state->device, &readback) != 0u &&
                   readback.width == (uint32_t)width && readback.height == (uint32_t)height &&
                   readback.rowPitchBytes == (uint32_t)width * 4u && readback.writtenBytes == bytes;
    if (success)
    {
        const uint32_t left = (uint32_t)width / 3u;
        const uint32_t top = (uint32_t)height * 40u / 100u;
        const uint32_t viewportWidth = (uint32_t)width * 2u / 3u - left;
        const uint32_t viewportHeight = (uint32_t)height * 75u / 100u - top;
        const uint32_t offset = top * readback.rowPitchBytes + left * 4u;
        WalkScenarioImage image;
        success = WalkScenarioAnalyzeRGBA8(file + 54u + offset, bytes - offset, viewportWidth,
                                           viewportHeight, readback.rowPitchBytes, &image) != 0u &&
                  image.useful != 0u;
    }
    static char path[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t widePath[LAIUE_PLATFORM_PATH_CAPACITY];
    const uint32_t directoryLength =
        PlatformGetEnvironmentUtf8("LAIUE_SCENARIO_OUTPUT", path, sizeof(path));
    if (success && directoryLength != 0u)
    {
        const char *phase =
            WalkScenarioPhaseName((WalkScenarioPhase)state->scenarioCheckpointPhase);
        uint32_t length = directoryLength;
        if (length + 32u >= sizeof(path))
            success = false;
        else
        {
            path[length++] = '/';
            while (*phase != '\0')
                path[length++] = *phase++;
            memcpy(path + length, ".bmp", 5u);
            length += 4u;
            uint32_t wideLength = 0u;
            success = PlatformUtf8ToWide(path, length, widePath, LAIUE_PLATFORM_PATH_CAPACITY,
                                         &wideLength);
            (void)wideLength;
            if (success)
            {
                memset(file, 0, 54u);
                file[0] = 'B';
                file[1] = 'M';
                WalkScenarioStore32(file + 2u, bytes + 54u);
                WalkScenarioStore32(file + 10u, 54u);
                WalkScenarioStore32(file + 14u, 40u);
                WalkScenarioStore32(file + 18u, (uint32_t)width);
                WalkScenarioStore32(file + 22u, (uint32_t)-height);
                file[26u] = 1u;
                file[28u] = 32u;
                WalkScenarioStore32(file + 34u, bytes);
                for (uint32_t pixel = 54u; pixel < bytes + 54u; pixel += 4u)
                {
                    const uint8_t red = file[pixel];
                    file[pixel] = file[pixel + 2u];
                    file[pixel + 2u] = red;
                }
                success = PlatformWriteEntireFile(widePath, file, (uint64_t)bytes + 54u);
            }
        }
    }
    PlatformFree(file);
    if (success)
        ++state->scenarioImages;
    return success;
}

static bool WalkScenarioRebaseCheck(WalkWindowState *state)
{
    WalkGameplaySpatialSnapshot before, after;
    WalkGameplayRebaseResult forced, automatic;
    const int64_t shift[3] = {-9000, 0, 0};
    double delta[3];
    if (WalkGameplaySnapshot(state->game, &before) != WALK_GAMEPLAY_OK ||
        WalkGameplayRebase(state->game, shift, &forced) != WALK_GAMEPLAY_OK || !forced.changed ||
        WalkGameplayRebase(state->game, NULL, &automatic) != WALK_GAMEPLAY_OK ||
        !automatic.changed || WalkGameplaySnapshot(state->game, &after) != WALK_GAMEPLAY_OK ||
        !WalkGameplaySpatialDelta(&before, &after, delta))
        return false;
    return WalkMathAbs(delta[0]) < 1.0e-6 && WalkMathAbs(delta[1]) < 1.0e-6 &&
           WalkMathAbs(delta[2]) < 1.0e-6 && WalkMathAbs(after.rootLocal[0]) < 64.0;
}

static bool WalkScenarioReport(WalkWindowState *state)
{
    WalkScenarioSummary summary;
    const bool hasSummary = WalkScenarioMetricsGetSummary(state->scenarioMetrics, &summary) != 0u;
    const bool passed =
        !state->failed && WalkGameplayHealth(state->game) == WALK_GAMEPLAY_OK &&
        state->scenarioTick == WALK_SCENARIO_TOTAL_TICKS && state->scenarioFrames != 0u &&
        state->scenarioDrawFrames == state->scenarioFrames && state->scenarioCheckpoints == 9u &&
        state->scenarioImages == 9u && state->scenarioBreaks != 0u && state->scenarioPlaces != 0u &&
        state->scenarioRebases == 1u && state->scenarioAirborne &&
        state->scenarioMaximumDisplacement > 0.5 && state->game->ragdollReady &&
        state->chunksReady && state->texturedTerrainReady && hasSummary && summary.rejected == 0u;
    PlatformWriteConsoleUtf8(passed ? "LAIUE_SCENARIO event=summary status=PASS"
                                    : "LAIUE_SCENARIO event=summary status=FAIL");
    WalkScenarioWriteUnsigned(" ticks=", state->scenarioTick);
    WalkScenarioWriteUnsigned(" frames=", state->scenarioFrames);
    WalkScenarioWriteUnsigned(" checkpoints=", state->scenarioCheckpoints);
    WalkScenarioWriteUnsigned(" images=", state->scenarioImages);
    WalkScenarioWriteUnsigned(" breaks=", state->scenarioBreaks);
    WalkScenarioWriteUnsigned(" places=", state->scenarioPlaces);
    WalkScenarioWriteUnsigned(" rebases=", state->scenarioRebases);
    WalkScenarioWriteUnsigned(" airborne=", state->scenarioAirborne ? 1u : 0u);
    WalkScenarioWriteUnsigned(
        " moved_mm=", WalkScenarioScaledUnsigned(state->scenarioMaximumDisplacement, 1000.0));
    if (hasSummary)
    {
        WalkScenarioWriteUnsigned(" frame_wall_mean_us=",
                                  WalkScenarioScaledUnsigned(summary.mean, 1000.0));
        WalkScenarioWriteUnsigned(" frame_wall_p95_us=",
                                  WalkScenarioScaledUnsigned(summary.p95, 1000.0));
        WalkScenarioWriteUnsigned(" frame_wall_p99_us=",
                                  WalkScenarioScaledUnsigned(summary.p99, 1000.0));
        WalkScenarioWriteUnsigned(" frame_wall_max_us=",
                                  WalkScenarioScaledUnsigned(summary.maximum, 1000.0));
        WalkScenarioWriteUnsigned(" samples=", summary.count);
        WalkScenarioWriteUnsigned(" rejected=", summary.rejected);
        WalkScenarioWriteUnsigned(" dropped=", summary.dropped);
    }
    WalkScenarioSummary gpuSummary;
    const bool gpuMeasured =
        WalkScenarioMetricsGetSummary(state->scenarioGpuMetrics, &gpuSummary) != 0u;
    WalkScenarioWriteUnsigned(" gpu_samples=", gpuMeasured ? gpuSummary.count : 0u);
    if (gpuMeasured)
    {
        WalkScenarioWriteUnsigned(" gpu_mean_us=",
                                  WalkScenarioScaledUnsigned(gpuSummary.mean, 1000.0));
        WalkScenarioWriteUnsigned(" gpu_p95_us=",
                                  WalkScenarioScaledUnsigned(gpuSummary.p95, 1000.0));
    }
    WalkScenarioWriteUnsigned(" sampled_peak_shadow_bytes=", state->scenarioPeakShadowBytes);
    WalkScenarioWriteUnsigned(" sampled_peak_geometry_bytes=", state->scenarioPeakGeometryBytes);
    PlatformWriteConsoleUtf8("\n");
    return passed;
}

/* Every control goes through these helpers: a profile without the input
 * provider runs the same frame with all keys and buttons released. */
static bool WalkKeyDown(const WalkWindowState *state, InputKey key)
{
    return state->input != NULL && state->inputService != NULL &&
           state->inputService->isKeyDown != NULL &&
           state->inputService->isKeyDown(state->input, key);
}

static bool WalkTakeKeyPress(WalkWindowState *state, InputKey key)
{
    if (state->input == NULL || state->inputService == NULL ||
        state->inputService->wasKeyPressed == NULL ||
        state->inputService->consumeKeyPress == NULL ||
        !state->inputService->wasKeyPressed(state->input, key))
        return false;
    (void)state->inputService->consumeKeyPress(state->input, key);
    return true;
}

static bool WalkMouseButtonPressed(const WalkWindowState *state, InputMouseButton button)
{
    return state->input != NULL && state->inputService != NULL &&
           state->inputService->wasMouseButtonPressed != NULL &&
           state->inputService->wasMouseButtonPressed(state->input, button);
}

static bool WalkMouseButtonDown(const WalkWindowState *state, InputMouseButton button)
{
    return state->input != NULL && state->inputService != NULL &&
           state->inputService->isMouseButtonDown != NULL &&
           state->inputService->isMouseButtonDown(state->input, button);
}

static void WalkWindowFrame(void *opaque)
{
    WalkWindowState *state = (WalkWindowState *)opaque;
    const double frameStart =
        state != NULL && state->scenarioEnabled ? PlatformMonotonicSeconds() : 0.0;
    if (state != NULL && state->failed)
    {
        if (state->windowService != NULL && state->window != NULL &&
            state->windowService->requestClose != NULL)
            state->windowService->requestClose(state->window);
        return;
    }
    if (state == NULL || state->game == NULL || state->window == NULL ||
        state->windowService == NULL || (state->input != NULL && state->inputService == NULL) ||
        state->graphicsService == NULL || state->device == NULL ||
        !WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, beginFrame),
                                sizeof(state->device->beginFrame)) ||
        !WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, endFrame),
                                sizeof(state->device->endFrame)) ||
        state->device->beginFrame == NULL || state->device->endFrame == NULL)
        return;
    if (state->windowService->consumeFocusLoss != NULL &&
        state->windowService->consumeFocusLoss(state->window) != 0)
    {
        WalkGameplayResetInput(state->game);
        state->accumulator = 0.0;
        state->lastTime = PlatformMonotonicSeconds();
        if (state->input != NULL && state->inputService->resetState != NULL)
            state->inputService->resetState(state->input);
    }
    if (state->windowService->consumeResize != NULL &&
        state->windowService->consumeResize(state->window) != 0)
    {
        int32_t width = 0, height = 0;
        state->windowService->getClientSize(state->window, &width, &height);
        if (width > 0 && height > 0 &&
            WalkServiceFieldPresent(state->graphicsServiceSize, state->graphicsService->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, resize),
                                    sizeof(state->graphicsService->resize)) &&
            state->graphicsService->resize != NULL)
            state->graphicsService->resize(state->device, width, height);
    }
    if (WalkTakeKeyPress(state, INPUT_KEY_ESCAPE))
        state->windowService->requestClose(state->window);
    const double now = PlatformMonotonicSeconds();
    double elapsed = state->lastTime == 0.0 ? 0.0 : now - state->lastTime;
    state->lastTime = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > 0.25)
        elapsed = 0.25;
    if (state->scenarioEnabled && now < state->scenarioHoldUntil)
        elapsed = 0.0;
    if (!state->scenarioEnabled)
    {
        bool firstPerson = state->game->firstPerson;
        uint8_t material = state->game->selectedMaterial;
        if (WalkTakeKeyPress(state, INPUT_KEY_V))
            firstPerson = !firstPerson;
        static const InputKey materialKeys[WALK_VISUAL_TEXTURE_COUNT] = {
            INPUT_KEY_1,
            INPUT_KEY_2,
            INPUT_KEY_3,
        };
        for (uint32_t index = 0u; index < WALK_VISUAL_TEXTURE_COUNT; ++index)
            if (WalkTakeKeyPress(state, materialKeys[index]))
                material = (uint8_t)(index + 1u);
        int32_t mouseX = 0, mouseY = 0;
        if (state->input != NULL && state->inputService->getMouseDelta != NULL)
            state->inputService->getMouseDelta(state->input, &mouseX, &mouseY);
        if (WalkGameplaySetView(state->game, firstPerson, material) != WALK_GAMEPLAY_OK ||
            WalkGameplayOrient(state->game, (float)mouseX * 0.0025f, -(float)mouseY * 0.0025f,
                               WALK_GAMEPLAY_ORIENT_RELATIVE) != WALK_GAMEPLAY_OK)
            state->failed = true;
        WalkGameplayInput input = {
            .strafe = (double)((WalkKeyDown(state, INPUT_KEY_D) ? 1 : 0) -
                               (WalkKeyDown(state, INPUT_KEY_A) ? 1 : 0)),
            .forward = (double)((WalkKeyDown(state, INPUT_KEY_W) ? 1 : 0) -
                                (WalkKeyDown(state, INPUT_KEY_S) ? 1 : 0)),
            .sprint = WalkKeyDown(state, INPUT_KEY_SHIFT),
            .jumpHeld = WalkKeyDown(state, INPUT_KEY_SPACE),
        };
        if (WalkTakeKeyPress(state, INPUT_KEY_SPACE))
            input.pulses |= WALK_GAMEPLAY_PULSE_JUMP;
        if (WalkMouseButtonPressed(state, INPUT_MOUSE_BUTTON_LEFT))
            input.pulses |= WALK_GAMEPLAY_PULSE_BREAK;
        if (WalkMouseButtonPressed(state, INPUT_MOUSE_BUTTON_RIGHT))
            input.pulses |= WALK_GAMEPLAY_PULSE_PLACE;
        if (WalkGameplaySubmitInput(state->game, &input) != WALK_GAMEPLAY_OK)
            state->failed = true;
    }
    state->accumulator += elapsed;
    const double fixedStep = 1.0 / (double)LAIUE_CHARACTER_TICK_HZ;
    uint32_t ticks = 0u;
    bool providerFrameChanged = false;
    while (!state->failed && state->game->actor != WALK_GAMEPLAY_ACTOR_STATIC &&
           state->accumulator >= fixedStep && ticks < 8u)
    {
        if (state->scenarioEnabled)
        {
            if (WalkScenarioInputAt(&state->scenario, state->scenarioTick, &state->scenarioInput) ==
                0u)
                break;
            if (WalkGameplayOrient(state->game, state->scenarioInput.yaw,
                                   state->scenarioInput.pitch,
                                   WALK_GAMEPLAY_ORIENT_ABSOLUTE) != WALK_GAMEPLAY_OK ||
                WalkGameplaySetView(state->game, state->scenarioInput.firstPerson != 0u,
                                    state->game->selectedMaterial) != WALK_GAMEPLAY_OK)
            {
                state->failed = true;
                break;
            }
            WalkGameplayInput input = {
                .strafe = state->scenarioInput.moveX,
                .forward = state->scenarioInput.moveY,
                .sprint = state->scenarioInput.sprint != 0u,
            };
            if (state->scenarioInput.jump != 0u)
            {
                WalkGameplaySpatialSnapshot beforeJump;
                if (WalkGameplaySnapshot(state->game, &beforeJump) != WALK_GAMEPLAY_OK ||
                    !beforeJump.grounded)
                {
                    state->failed = true;
                    break;
                }
                state->scenarioJumpStartZ = beforeJump.rootLocal[2];
                state->scenarioJumpStarted = true;
                input.pulses |= WALK_GAMEPLAY_PULSE_JUMP;
            }
            if (state->scenarioInput.breakBlock != 0u)
            {
                input.pulses |= WALK_GAMEPLAY_PULSE_BREAK;
                state->scenarioBreakPending = true;
            }
            if (state->scenarioInput.placeBlock != 0u)
            {
                input.pulses |= WALK_GAMEPLAY_PULSE_PLACE;
                state->scenarioPlacePending = true;
            }
            if (WalkGameplaySubmitInput(state->game, &input) != WALK_GAMEPLAY_OK)
            {
                state->failed = true;
                break;
            }
            if (state->scenarioInput.rebaseCheck != 0u)
            {
                if (!WalkScenarioRebaseCheck(state))
                {
                    state->failed = true;
                    break;
                }
                ++state->scenarioRebases;
            }
        }
        WalkGameplayStepResult step;
        if (WalkGameplayStep(state->game, &step) != WALK_GAMEPLAY_OK)
        {
            state->failed = true;
            break;
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            providerFrameChanged |= step.rebase.providerFrameShift[axis] != 0;
        state->accumulator -= fixedStep;
        ++ticks;
        if (state->scenarioEnabled)
        {
            ++state->scenarioTick;
            WalkGameplaySpatialSnapshot current;
            double delta[3];
            if (WalkGameplaySnapshot(state->game, &current) != WALK_GAMEPLAY_OK ||
                !WalkGameplaySpatialDelta(&state->scenarioStartSpatial, &current, delta))
                state->failed = true;
            else
            {
                const double distance = ScalarSqrtDouble(delta[0] * delta[0] + delta[1] * delta[1]);
                if (distance > state->scenarioMaximumDisplacement)
                    state->scenarioMaximumDisplacement = distance;
                if (state->scenarioInput.phase == WalkScenarioJump && state->scenarioJumpStarted &&
                    !current.grounded && current.rootLocal[2] > state->scenarioJumpStartZ + 0.05)
                    state->scenarioAirborne = true;
            }
            if (state->scenarioInput.capture != 0u)
            {
                state->scenarioCheckpointPhase = (uint32_t)state->scenarioInput.phase;
                state->scenarioCheckpointPending = true;
                state->accumulator = 0.0;
                break;
            }
        }
    }
    if (ticks == 8u || state->game->actor == WALK_GAMEPLAY_ACTOR_STATIC)
        state->accumulator = 0.0;
    /* Provider-frame changes invalidate GPU chunks after all fixed ticks.
     * The shared mesher scratch stays owned for the whole presentation session. */
    if (providerFrameChanged && state->chunksReady)
        WalkVisualsInvalidateChunkSet(state->device, &state->chunkSet);
    if (!state->failed && !WalkUpdateCamera(state))
        state->failed = true;
    if (!state->failed && !WalkApplyEdits(state))
        state->failed = true;
    if (!state->failed && state->chunksReady &&
        !WalkVisualsUpdateChunkSet(state->device, state->mesherService, &state->chunkSet,
                                   WalkReadVisualBlock, state->game, state->spatial.centerBlock))
        state->failed = true;
    state->failed |= !WalkCheckGameplayHealth(state);
    if (!state->failed && state->game->ragdollReady &&
        !WalkVisualsUpdateRagdollBuffer(
            &state->game->physicsContext, state->device, state->ragdollBuffer,
            &state->game->ragdoll, state->spatial.ragdollRenderOrigin, state->ragdollVisualScratch))
        state->failed = true;
    if (state->failed)
    {
        state->windowService->requestClose(state->window);
        return;
    }

    int32_t width = 0;
    int32_t height = 0;
    state->windowService->getClientSize(state->window, &width, &height);
    bool uiFrameReady = false;
    if (width > 0 && height > 0 && state->uiService != NULL && state->uiContext != NULL &&
        state->uiService->begin != NULL && state->uiService->getFontAtlas != NULL)
    {
        int32_t mouseX = 0;
        int32_t mouseY = 0;
        state->windowService->getCursorClientPosition(state->window, &mouseX, &mouseY);
        const uint32_t mouseDown = WalkMouseButtonDown(state, INPUT_MOUSE_BUTTON_LEFT) ? 1u : 0u;
        const uint32_t mousePressed =
            WalkMouseButtonPressed(state, INPUT_MOUSE_BUTTON_LEFT) ? 1u : 0u;
        const float wheel = state->windowService->consumeMouseWheelSteps != NULL
                                ? state->windowService->consumeMouseWheelSteps(state->window)
                                : 0.0f;
        uiFrameReady =
            state->uiService->begin(state->uiContext, width, height, (float)mouseX, (float)mouseY,
                                    mouseDown, mousePressed, wheel, (float)elapsed) != 0u;
        if (uiFrameReady &&
            WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, setUiFontAtlas),
                                   sizeof(state->device->setUiFontAtlas)) &&
            state->device->setUiFontAtlas != NULL)
        {
            const uint8_t *pixels = NULL;
            uint32_t atlasWidth = 0u;
            uint32_t atlasHeight = 0u;
            if (state->uiService->getFontAtlas(state->uiContext, &pixels, &atlasWidth,
                                               &atlasHeight) != 0u &&
                pixels != NULL && atlasWidth != 0u && atlasHeight != 0u &&
                (pixels != state->fontPixels || atlasWidth != state->fontWidth ||
                 atlasHeight != state->fontHeight) &&
                state->device->setUiFontAtlas(state->device, pixels, atlasWidth, atlasHeight) != 0u)
            {
                state->fontPixels = pixels;
                state->fontWidth = atlasWidth;
                state->fontHeight = atlasHeight;
            }
        }
    }
    if (width > 0 && height > 0)
    {
        if (state->scenarioCheckpointPending &&
            (!WalkDeviceFieldPresent(state->device,
                                     offsetof(LaiueGraphicsDeviceV2, requestFrameReadback),
                                     sizeof(state->device->requestFrameReadback)) ||
             state->device->requestFrameReadback == NULL ||
             state->device->requestFrameReadback(state->device) == 0u))
        {
            state->failed = true;
            state->windowService->requestClose(state->window);
            return;
        }
        if (state->device->beginFrame(state->device, (uint32_t)width, (uint32_t)height) == 0u)
            state->failed = true;
        else
        {
            if (uiFrameReady && state->uiService != NULL && state->uiService->rect != NULL &&
                state->uiService->textUtf8 != NULL && state->uiService->copyDrawList != NULL)
            {
                state->uiService->rect(state->uiContext, 16.0f, 16.0f, 370.0f, 150.0f, 8.0f,
                                       0xF4221A16u);
                state->uiService->textUtf8(state->uiContext, 32.0f, 32.0f, 0xFFFFFFFFu,
                                           "LAIUE Walk");
                state->uiService->textUtf8(
                    state->uiContext, 32.0f, 58.0f, 0xFFE8ECF4u,
                    "WASD move  Shift run  Space jump  V view  LMB break  RMB place");
                state->uiService->textUtf8(state->uiContext, 32.0f, 84.0f, 0xFFB8C8FFu,
                                           state->game->ragdollReady
                                               ? "Ragdoll: deterministic physics"
                                               : "Ragdoll: physics module unavailable");
                state->uiService->textUtf8(state->uiContext, 32.0f, 108.0f, 0xFFB8C8FFu,
                                           state->chunksReady && state->texturedTerrainReady
                                               ? "Infinite voxel chunks: grass / dirt / stone"
                                               : "Terrain textures: unavailable");
                state->uiService->textUtf8(state->uiContext, 32.0f, 132.0f, 0xFFE8ECF4u,
                                           state->game->firstPerson ? "View: first person"
                                                                    : "View: third person");
                uint32_t quadCount = 0u;
                if (state->uiService->copyDrawList(state->uiContext, walkUiQuads,
                                                   LAIUE_GRAPHICS_UI_MAX_QUADS, &quadCount) == 0u ||
                    !WalkDeviceFieldPresent(state->device,
                                            offsetof(LaiueGraphicsDeviceV2, submitUi),
                                            sizeof(state->device->submitUi)) ||
                    state->device->submitUi == NULL ||
                    state->device->submitUi(state->device, walkUiQuads, quadCount) == 0u)
                    state->failed = true;
            }
            if (state->cameraReady &&
                WalkDeviceFieldPresent(state->device, offsetof(LaiueGraphicsDeviceV2, submit),
                                       sizeof(state->device->submit)) &&
                state->device->submit != NULL)
            {
                uint32_t drawIndex = 0u;
                if (WalkVisualsBuildFarTerrainDraw(
                        state->farTerrainBuffer, state->spatial.renderOriginBlock,
                        state->terrainTextures, state->terrainSampler, &walkDraws[drawIndex]))
                    ++drawIndex;
                drawIndex += WalkVisualsBuildChunkDraws(
                    &state->chunkSet, state->spatial.renderOriginBlock, state->terrainTextures,
                    state->terrainSampler, &walkDraws[drawIndex]);
                if (state->game->ragdollReady && !state->game->firstPerson)
                {
                    walkDraws[drawIndex] = (LaiueGraphicsDrawItemV2){
                        .structSize = sizeof(walkDraws[drawIndex]),
                        .vertexBuffer = state->ragdollBuffer,
                        .indexCount = WALK_RAGDOLL_VISUAL_VERTEX_COUNT,
                        .originRelative = {0.0f, 0.0f, 0.0f},
                        .scale = 1.0f,
                    };
                    ++drawIndex;
                }
                if (drawIndex != 0u &&
                    state->device->submit(state->device, walkDraws, drawIndex) == 0u)
                    state->failed = true;
                else if (state->scenarioEnabled && drawIndex != 0u)
                    ++state->scenarioDrawFrames;
            }
            if (state->device->endFrame(state->device) == 0u)
            {
                state->failed = true;
                state->windowService->requestClose(state->window);
            }
            else if (state->smokeEnabled)
            {
                if (!state->cameraReady || WalkGameplayHealth(state->game) != WALK_GAMEPLAY_OK)
                    state->failed = true;
                else
                    ++state->smokeFrames;
                if (state->failed || state->smokeFrames == 32u)
                    state->windowService->requestClose(state->window);
            }
            else if (state->scenarioEnabled && !state->failed)
            {
                ++state->scenarioFrames;
                if (ticks != 0u && !state->scenarioCheckpointPending)
                    (void)WalkScenarioMetricsAdd(
                        state->scenarioMetrics, (PlatformMonotonicSeconds() - frameStart) * 1000.0);
                if (WalkDeviceFieldPresent(state->device,
                                           offsetof(LaiueGraphicsDeviceV2, getDiagnostics),
                                           sizeof(state->device->getDiagnostics)) &&
                    state->device->getDiagnostics != NULL)
                {
                    LaiueGraphicsDiagnosticsV2 diagnostics = {.structSize = sizeof(diagnostics)};
                    if (state->device->getDiagnostics(state->device, &diagnostics) == 0u ||
                        (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_FRAME_VALID) == 0u ||
                        diagnostics.drawCalls == 0u || diagnostics.scenePasses == 0u)
                        state->failed = true;
                    else
                    {
                        if (diagnostics.cpuShadowBytes > state->scenarioPeakShadowBytes)
                            state->scenarioPeakShadowBytes = diagnostics.cpuShadowBytes;
                        if (diagnostics.geometryPoolUsedBytes > state->scenarioPeakGeometryBytes)
                            state->scenarioPeakGeometryBytes = diagnostics.geometryPoolUsedBytes;
                        if (ticks != 0u && !state->scenarioCheckpointPending &&
                            (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_GPU_TIMING_VALID) !=
                                0u &&
                            diagnostics.gpuFrameIndex != state->scenarioLastGpuFrame)
                        {
                            state->scenarioLastGpuFrame = diagnostics.gpuFrameIndex;
                            (void)WalkScenarioMetricsAdd(
                                state->scenarioGpuMetrics,
                                (double)diagnostics.gpuDurationNanoseconds / 1000000.0);
                        }
                    }
                }
                else
                    state->failed = true;
                if (state->failed)
                {
                    state->windowService->requestClose(state->window);
                    return;
                }
                if (state->scenarioCheckpointPending)
                {
                    if (!WalkScenarioCapture(state))
                    {
                        PlatformWriteConsoleUtf8(
                            "LAIUE_SCENARIO event=failure reason=frame_pixels\n");
                        state->failed = true;
                        state->windowService->requestClose(state->window);
                        return;
                    }
                    ++state->scenarioCheckpoints;
                    PlatformWriteConsoleUtf8("LAIUE_SCENARIO event=checkpoint phase=");
                    PlatformWriteConsoleUtf8(
                        WalkScenarioPhaseName((WalkScenarioPhase)state->scenarioCheckpointPhase));
                    WalkScenarioWriteUnsigned(" tick=", state->scenarioTick);
                    WalkScenarioWriteUnsigned(" frames=", state->scenarioFrames);
                    WalkScenarioWriteUnsigned(" textures=", state->texturedTerrainReady ? 1u : 0u);
                    WalkScenarioWriteUnsigned(" ragdoll=", state->game->ragdollReady ? 1u : 0u);
                    PlatformWriteConsoleUtf8("\n");
                    state->scenarioCheckpointPending = false;
                    state->scenarioHoldUntil = PlatformMonotonicSeconds() + 2.0;
                }
                if (state->scenarioTick == WALK_SCENARIO_TOTAL_TICKS)
                {
                    state->failed = !WalkScenarioReport(state);
                    state->windowService->requestClose(state->window);
                }
            }
        }
    }
    if (state->input != NULL && state->inputService->endFrame != NULL)
        state->inputService->endFrame(state->input);
}
#endif

#if defined(LAIUE_WALK_DYNAMIC)
static bool JoinPath(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
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
#endif

static LaiueModuleStatus LoadWalkModules(LaiueModuleHost *host, LaiueModuleLoadReportV1 *report,
                                         LaiueModuleLoadReportEntryV1 *reportEntries,
                                         LaiueModuleDiagnostic *diagnostic)
{
#if defined(LAIUE_WALK_DYNAMIC)
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t characterPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t worldPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t voxelPath[LAIUE_PLATFORM_PATH_CAPACITY];
#if defined(LAIUE_WALK_WINDOWED)
    static wchar_t windowPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t inputPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t physicsPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t mesherPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t renderPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t uiPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t sceneMathPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t scenePath[LAIUE_PLATFORM_PATH_CAPACITY];
#endif
#if defined(_WIN32)
    const wchar_t *characterName = L"laiue_character.dll";
    const wchar_t *numericName = L"laiue_numeric.dll";
    const wchar_t *worldName = L"laiue_world.dll";
    const wchar_t *voxelName = L"laiue_voxel.dll";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *mesherName = L"laiue_mesher.dll";
    const wchar_t *windowName = L"laiue_window.dll";
    const wchar_t *inputName = L"laiue_input.dll";
    const wchar_t *physicsName = L"laiue_physics.dll";
#if defined(LAIUE_WALK_GRAPHICS_PROVIDER_D3D12)
    const wchar_t *renderName = L"laiue_graphics_d3d12.dll";
#if defined(LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER)
    const char *renderModuleId = "laiue.graphics.d3d12";
#endif
#elif defined(LAIUE_WALK_GRAPHICS_PROVIDER_VULKAN)
    const wchar_t *renderName = L"laiue_graphics_vulkan.dll";
#if defined(LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER)
    const char *renderModuleId = "laiue.graphics.vulkan";
#endif
#else
    const wchar_t *renderName = L"laiue_render.dll";
#endif
    const wchar_t *uiName = L"laiue_ui.dll";
    const wchar_t *sceneMathName = L"laiue_scene_math.dll";
    const wchar_t *sceneName = L"laiue_scene.dll";
#endif
#elif defined(__APPLE__)
    const wchar_t *characterName = L"liblaiue_character.dylib";
    const wchar_t *numericName = L"liblaiue_numeric.dylib";
    const wchar_t *worldName = L"liblaiue_world.dylib";
    const wchar_t *voxelName = L"liblaiue_voxel.dylib";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *mesherName = L"liblaiue_mesher.dylib";
    const wchar_t *windowName = L"liblaiue_window.dylib";
    const wchar_t *inputName = L"liblaiue_input.dylib";
    const wchar_t *physicsName = L"liblaiue_physics.dylib";
#if defined(LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER)
    const wchar_t *renderName = L"liblaiue_graphics_vulkan.dylib";
    const char *renderModuleId = "laiue.graphics.vulkan";
#else
    const wchar_t *renderName = L"liblaiue_render.dylib";
#endif
    const wchar_t *uiName = L"liblaiue_ui.dylib";
    const wchar_t *sceneMathName = L"liblaiue_scene_math.dylib";
    const wchar_t *sceneName = L"liblaiue_scene.dylib";
#endif
#else
    const wchar_t *characterName = L"liblaiue_character.so";
    const wchar_t *numericName = L"liblaiue_numeric.so";
    const wchar_t *worldName = L"liblaiue_world.so";
    const wchar_t *voxelName = L"liblaiue_voxel.so";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *mesherName = L"liblaiue_mesher.so";
    const wchar_t *windowName = L"liblaiue_window.so";
    const wchar_t *inputName = L"liblaiue_input.so";
    const wchar_t *physicsName = L"liblaiue_physics.so";
#if defined(LAIUE_WALK_GRAPHICS_PROVIDER_VULKAN)
    const wchar_t *renderName = L"liblaiue_graphics_vulkan.so";
    const char *renderModuleId = "laiue.graphics.vulkan";
#else
    const wchar_t *renderName = L"liblaiue_render.so";
#endif
    const wchar_t *uiName = L"liblaiue_ui.so";
    const wchar_t *sceneMathName = L"liblaiue_scene_math.so";
    const wchar_t *sceneName = L"liblaiue_scene.so";
#endif
#endif
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !JoinPath(characterPath, directory, characterName) ||
        !JoinPath(numericPath, directory, numericName) ||
        !JoinPath(worldPath, directory, worldName) || !JoinPath(voxelPath, directory, voxelName))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    LaiueModuleBinaryV1 binaries[12];
    uint32_t binaryCount = 0u;
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){characterPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){numericPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){worldPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){voxelPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
#if defined(LAIUE_WALK_WINDOWED)
    if (!JoinPath(windowPath, directory, windowName) ||
        !JoinPath(inputPath, directory, inputName) ||
        !JoinPath(physicsPath, directory, physicsName) ||
        !JoinPath(mesherPath, directory, mesherName) ||
        !JoinPath(renderPath, directory, renderName) || !JoinPath(uiPath, directory, uiName) ||
        !JoinPath(sceneMathPath, directory, sceneMathName) ||
        !JoinPath(scenePath, directory, sceneName))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    binaries[binaryCount++] = (LaiueModuleBinaryV1){windowPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){inputPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){physicsPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){mesherPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){renderPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){uiPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){sceneMathPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){scenePath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
#endif
    LaiueModuleLoadReportInitialize(report, reportEntries, binaryCount);
#if defined(LAIUE_WALK_EXPLICIT_GRAPHICS_PROVIDER)
    /* The executable chooses exactly one standalone provider at configure
     * time and pins its descriptor.  If that artifact is removed or cannot
     * be loaded (no Vulkan loader on the system), the partial profile only
     * loses the graphics branch and walk falls back to its diagnostic
     * headless mode. */
    LaiueModuleProviderSelectionV1 graphicsSelection = {
        .structSize = sizeof(graphicsSelection),
        .serviceName = LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
        .moduleId = renderModuleId,
    };
    LaiueModuleProfileV1 profile = {
        .structSize = sizeof(profile),
        .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
        .binaries = binaries,
        .binaryCount = binaryCount,
        .providerSelections = &graphicsSelection,
        .providerSelectionCount = 1u,
    };
    return LaiueModuleHostLoadProfileV1(host, &profile, report, diagnostic);
#else
    return LaiueModuleHostLoadProfile(host, binaries, binaryCount,
                                      LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, report, diagnostic);
#endif
#else
    (void)report;
    (void)reportEntries;
    const LaiueModuleApiV1 *modules[12] = {LaiueCharacterGetStaticModuleApiV1()};
    uint32_t moduleCount = 1u;
#if defined(LAIUE_WALK_STATIC_WITH_VOXEL)
    modules[moduleCount++] = LaiueNumericGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueWorldGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueVoxelGetStaticModuleApiV1();
#endif
#if defined(LAIUE_WALK_WINDOWED)
#if !defined(LAIUE_WALK_STATIC_WITH_VOXEL)
    modules[moduleCount++] = LaiueNumericGetStaticModuleApiV1();
#endif
    modules[moduleCount++] = LaiuePhysicsGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueWindowGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueInputGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueGraphicsGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueUiGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueSceneMathGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueSceneGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueMesherGetStaticModuleApiV1();
#endif
    return LaiueModuleHostLoadStatic(host, modules, moduleCount, diagnostic);
#endif
}

static bool RunWalkExample(bool headless, bool scenarioEnabled, bool smokeEnabled)
{
    if (smokeEnabled && (headless || scenarioEnabled))
        return false;
    LaiueModuleHostConfigV1 hostConfig;
    LaiueModuleHostConfigInitialize(&hostConfig);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&hostConfig, &diagnostic);
    if (host == NULL)
    {
        PlatformWriteConsoleUtf8("laiue walk: bootstrap creation failed\n");
        return false;
    }
    static LaiueModuleLoadReportEntryV1 reportEntries[12];
    LaiueModuleLoadReportV1 report;
    const LaiueModuleStatus moduleStatus =
        LoadWalkModules(host, &report, reportEntries, &diagnostic);
    if (moduleStatus != LAIUE_MODULE_OK && moduleStatus != LAIUE_MODULE_PARTIAL)
    {
        PlatformWriteConsoleUtf8("laiue walk: module graph failed: ");
        PlatformWriteConsoleUtf8(diagnostic.message);
        PlatformWriteConsoleUtf8("\n");
        LaiueModuleHostDestroy(host);
        return false;
    }

    WalkGameplayServices services = {0};
    services.character = (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        LAIUE_CHARACTER_SERVICE_V1_LEGACY_SIZE, NULL, &services.characterSize);
    services.voxel = (const LaiueVoxelServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_VOXEL_SERVICE_NAME, LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        LAIUE_VOXEL_SERVICE_V1_LEGACY_SIZE, NULL, &services.voxelSize);
#if defined(LAIUE_WALK_WINDOWED)
    if (!headless)
        services.physics = (const LaiuePhysicsServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_PHYSICS_SERVICE_NAME, LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
            sizeof(LaiuePhysicsServiceV1), NULL, &services.physicsSize);
    services.scene = (const LaiueSceneServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_SCENE_SERVICE_NAME, LAIUE_SCENE_SERVICE_ABI_VERSION_1,
        sizeof(LaiueSceneServiceV1), NULL, &services.sceneSize);
    services.sceneMath = (const LaiueSceneMathServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_SCENE_MATH_SERVICE_NAME, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
        sizeof(LaiueSceneMathServiceV1), NULL, &services.sceneMathSize);
#else
    (void)headless;
#endif
    WalkGameplayConfig gameConfig;
    WalkGameplayConfigDefault(&gameConfig);
    gameConfig.initialPosition = (LaiueCharacterPositionV1){
        .cellX = INT64_C(1) << 40,
        .cellY = -(INT64_C(1) << 39),
        .localX = LAIUE_CHARACTER_LOCAL_CELL_SIZE - 100,
        .localY = 0,
        .localZ = 1400,
    };
    if (scenarioEnabled)
        gameConfig.requiredCapabilities = WALK_GAMEPLAY_CAP_RAGDOLL |
                                          WALK_GAMEPLAY_CAP_SPARSE_WORLD | WALK_GAMEPLAY_CAP_EDIT |
                                          WALK_GAMEPLAY_CAP_CAMERA | WALK_GAMEPLAY_CAP_REBASE;
    /* The game address is fixed before its providers install callbacks. */
    static WalkGameplay game;
    bool success = WalkGameplayInit(&game, &services, &gameConfig) == WALK_GAMEPLAY_OK;
    if (!success)
    {
        PlatformWriteConsoleUtf8(scenarioEnabled
                                     ? "LAIUE_SCENARIO event=failure reason=gameplay_init\n"
                                     : "laiue walk: gameplay initialization failed\n");
        WalkGameplayRelease(&game);
        LaiueModuleHostUnloadAll(host);
        LaiueModuleHostDestroy(host);
        return false;
    }
    if ((game.capabilities & WALK_GAMEPLAY_CAP_SPARSE_WORLD) == 0u)
        PlatformWriteConsoleUtf8(
            "laiue walk: sparse voxel provider unavailable; using read-only base strata\n");
    if (game.actor == WALK_GAMEPLAY_ACTOR_STATIC)
        PlatformWriteConsoleUtf8(
            "laiue walk: character/physics providers unavailable; controls disabled\n");
    bool ranWindow = false;

#if defined(LAIUE_WALK_WINDOWED)
    if (!headless)
    {
        uint32_t windowServiceSize = 0u, inputServiceSize = 0u, mesherServiceSize = 0u;
        const LaiueWindowServiceV1 *windowService =
            (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
                sizeof(LaiueWindowServiceV1), NULL, &windowServiceSize);
        const LaiueInputServiceV1 *inputService =
            (const LaiueInputServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_INPUT_SERVICE_NAME, LAIUE_INPUT_SERVICE_ABI_VERSION_1,
                sizeof(LaiueInputServiceV1), NULL, &inputServiceSize);
        const LaiueMesherServiceV1 *mesherService =
            (const LaiueMesherServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
                sizeof(LaiueMesherServiceV1), NULL, &mesherServiceSize);
        uint32_t graphicsServiceSize = 0u, uiServiceSize = 0u;
        const LaiueGraphicsDeviceServiceV2 *graphicsService =
            (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
                host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
                LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
                LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, NULL, &graphicsServiceSize);
        const LaiueUiServiceV1 *uiService = (const LaiueUiServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_UI_SERVICE_NAME, LAIUE_UI_SERVICE_ABI_VERSION_1,
            LAIUE_UI_SERVICE_V1_LEGACY_SIZE, NULL, &uiServiceSize);
        if (windowService != NULL &&
            (windowServiceSize < sizeof(*windowService) ||
             windowService->structSize < sizeof(*windowService) ||
             windowService->abiVersion != LAIUE_WINDOW_SERVICE_ABI_VERSION_1 ||
             windowService->create == NULL || windowService->destroy == NULL ||
             windowService->getNativeHandle == NULL || windowService->getClientSize == NULL ||
             windowService->runLoop == NULL || windowService->requestClose == NULL ||
             windowService->setRawInputCallback == NULL || windowService->setMouseLook == NULL ||
             windowService->getCursorClientPosition == NULL))
            windowService = NULL;
        if (inputService != NULL &&
            (inputServiceSize < sizeof(*inputService) ||
             inputService->structSize < sizeof(*inputService) ||
             inputService->abiVersion != LAIUE_INPUT_SERVICE_ABI_VERSION_1 ||
             inputService->create == NULL || inputService->destroy == NULL))
            inputService = NULL;
        if (mesherService != NULL &&
            (mesherServiceSize < sizeof(*mesherService) ||
             mesherService->structSize < sizeof(*mesherService) ||
             mesherService->abiVersion != LAIUE_MESHER_SERVICE_ABI_VERSION_1))
            mesherService = NULL;
        if (graphicsService != NULL &&
            (graphicsServiceSize < LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE ||
             graphicsService->structSize < LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE ||
             graphicsService->abiVersion != LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2))
            graphicsService = NULL;
        if (uiService != NULL && (uiServiceSize < LAIUE_UI_SERVICE_V1_LEGACY_SIZE ||
                                  uiService->structSize < LAIUE_UI_SERVICE_V1_LEGACY_SIZE ||
                                  uiService->abiVersion != LAIUE_UI_SERVICE_ABI_VERSION_1))
            uiService = NULL;
        const bool canDestroyDevice =
            graphicsService != NULL &&
            WalkServiceFieldPresent(graphicsServiceSize, graphicsService->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, destroyDevice),
                                    sizeof(graphicsService->destroyDevice)) &&
            graphicsService->destroyDevice != NULL;
        const bool canCreateWithContext =
            graphicsService != NULL &&
            WalkServiceFieldPresent(graphicsServiceSize, graphicsService->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, createDeviceWithContext),
                                    sizeof(graphicsService->createDeviceWithContext)) &&
            WalkServiceFieldPresent(graphicsServiceSize, graphicsService->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, context),
                                    sizeof(graphicsService->context)) &&
            graphicsService->createDeviceWithContext != NULL && graphicsService->context != NULL;
        const bool canCreateDevice =
            graphicsService != NULL &&
            WalkServiceFieldPresent(graphicsServiceSize, graphicsService->structSize,
                                    offsetof(LaiueGraphicsDeviceServiceV2, createDevice),
                                    sizeof(graphicsService->createDevice)) &&
            graphicsService->createDevice != NULL;
        if (windowService != NULL && canDestroyDevice && (canCreateWithContext || canCreateDevice))
        {
            const WindowConfiguration configuration = {
                .title = L"LAIUE Walk",
                .width = 1280,
                .height = 720,
            };
            Window *window = windowService->create(&configuration);
            Input *input = window == NULL || inputService == NULL
                               ? NULL
                               : inputService->create(windowService->getNativeHandle(window));
            if (window != NULL && input == NULL)
                PlatformWriteConsoleUtf8(
                    "laiue walk: input provider unavailable; controls disabled\n");
            void *uiContext = NULL;
            if (window != NULL && uiService != NULL &&
                WalkServiceFieldPresent(uiServiceSize, uiService->structSize,
                                        offsetof(LaiueUiServiceV1, contextCreateWithContext),
                                        sizeof(uiService->contextCreateWithContext)) &&
                WalkServiceFieldPresent(uiServiceSize, uiService->structSize,
                                        offsetof(LaiueUiServiceV1, context),
                                        sizeof(uiService->context)) &&
                WalkServiceFieldPresent(uiServiceSize, uiService->structSize,
                                        offsetof(LaiueUiServiceV1, contextDestroy),
                                        sizeof(uiService->contextDestroy)) &&
                uiService->contextCreateWithContext != NULL && uiService->contextDestroy != NULL &&
                uiService->context != NULL)
            {
                const uint32_t created =
                    uiService->contextCreateWithContext(uiService->context, &uiContext);
                if (created == 0u && uiContext != NULL)
                {
                    uiService->contextDestroy(uiContext);
                    uiContext = NULL;
                }
            }
            LaiueGraphicsDeviceV2 *device = NULL;
            bool deviceReady = false;
            if (window != NULL)
            {
                const uint32_t created =
                    canCreateWithContext
                        ? graphicsService->createDeviceWithContext(
                              graphicsService->context, windowService->getNativeHandle(window),
                              1280, 720, LAIUE_GRAPHICS_BACKEND_AUTO, &device)
                        : graphicsService->createDevice(windowService->getNativeHandle(window),
                                                        1280, 720, LAIUE_GRAPHICS_BACKEND_AUTO,
                                                        &device);
                /* Preserve a partial output for its provider's destroy callback. */
                deviceReady =
                    created != 0u && device != NULL &&
                    WalkDeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, abiVersion),
                                           sizeof(device->abiVersion)) &&
                    device->abiVersion == LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION &&
                    WalkDeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, beginFrame),
                                           sizeof(device->beginFrame)) &&
                    WalkDeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, endFrame),
                                           sizeof(device->endFrame)) &&
                    device->beginFrame != NULL && device->endFrame != NULL;
            }
            if (window != NULL && deviceReady)
            {
                static WalkWindowState state;
                memset(&state, 0, sizeof(state));
                state.windowService = windowService;
                state.inputService = inputService;
                state.graphicsService = graphicsService;
                state.graphicsServiceSize = graphicsServiceSize;
                state.uiService = uiService;
                state.mesherService = mesherService;
                state.game = &game;
                state.window = window;
                state.input = input;
                state.device = device;
                state.uiContext = uiContext;
                state.lastTime = PlatformMonotonicSeconds();
                state.scenarioEnabled = scenarioEnabled;
                state.smokeEnabled = smokeEnabled;
                WalkScenarioInitialize(&state.scenario);
                if (!game.ragdollReady)
                    PlatformWriteConsoleUtf8("laiue walk: ragdoll provider unavailable; using "
                                             "character/static fallback\n");
                state.failed = !WalkUpdateCamera(&state);
                const bool chunksWanted = mesherService != NULL;
                if (!chunksWanted)
                    PlatformWriteConsoleUtf8(
                        "laiue walk: mesher provider unavailable; near voxel chunks disabled\n");
                state.chunksReady =
                    chunksWanted && WalkVisualsCreateChunkSet(mesherService, &state.chunkSet);
                if (state.chunksReady)
                    state.chunksReady = WalkVisualsUpdateChunkSet(
                        device, mesherService, &state.chunkSet, WalkReadVisualBlock, &game,
                        state.spatial.centerBlock);
                state.failed |= !WalkCheckGameplayHealth(&state);
                state.texturedTerrainReady = WalkVisualsCreateTerrain(
                    device, WalkReadDesktopAsset, NULL, state.texturedTerrainBuffers,
                    state.terrainTextures, &state.terrainSampler);
                if (!state.texturedTerrainReady)
                    PlatformWriteConsoleUtf8(
                        "laiue walk: bundled terrain textures could not be loaded\n");
                const bool farTerrainReady =
                    WalkVisualsCreateFarTerrainBuffer(device, &state.farTerrainBuffer);
                if (game.ragdollReady)
                    state.ragdollVisualScratch = (WalkRagdollVisualScratch *)PlatformAllocate(
                        sizeof(*state.ragdollVisualScratch), false);
                const bool ragdollBufferReady =
                    game.ragdollReady && state.ragdollVisualScratch != NULL &&
                    WalkVisualsCreateRagdollBuffer(device, &state.ragdollBuffer);
                state.failed |= (chunksWanted && !state.chunksReady) ||
                                !state.texturedTerrainReady || !farTerrainReady ||
                                (game.ragdollReady && !ragdollBufferReady) ||
                                WalkGameplayHealth(&game) != WALK_GAMEPLAY_OK;
                if (scenarioEnabled)
                {
                    state.scenarioMetrics = (WalkScenarioMetrics *)PlatformAllocate(
                        sizeof(*state.scenarioMetrics), false);
                    state.scenarioGpuMetrics = (WalkScenarioMetrics *)PlatformAllocate(
                        sizeof(*state.scenarioGpuMetrics), false);
                    if (state.scenarioMetrics != NULL)
                        WalkScenarioMetricsInitialize(state.scenarioMetrics);
                    if (state.scenarioGpuMetrics != NULL)
                        WalkScenarioMetricsInitialize(state.scenarioGpuMetrics);
                    state.failed |=
                        !state.cameraReady || !game.ragdollReady || !state.chunksReady ||
                        state.scenarioMetrics == NULL || state.scenarioGpuMetrics == NULL ||
                        !WalkDeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, submit),
                                                sizeof(device->submit)) ||
                        device->submit == NULL ||
                        WalkGameplaySnapshot(&game, &state.scenarioStartSpatial) !=
                            WALK_GAMEPLAY_OK;
                    PlatformWriteConsoleUtf8("LAIUE_SCENARIO event=start tick_rate=128\n");
                }
                windowService->setRawInputCallback(window, WalkRawInput, &state);
                windowService->setMouseLook(window,
                                            input != NULL && !scenarioEnabled && !smokeEnabled);
                PlatformWriteConsoleUtf8(
                    "laiue walk: walk mode (WASD, Shift, Space, V view, mouse break/place, Esc)\n");
                windowService->runLoop(window, WalkWindowFrame, &state);
                success = success && !state.failed;
                if (scenarioEnabled && state.scenarioTick != WALK_SCENARIO_TOTAL_TICKS)
                    success = false;
                if (smokeEnabled && state.smokeFrames != 32u)
                    success = false;
                if (smokeEnabled && !state.failed && state.smokeFrames == 32u)
                    PlatformWriteConsoleUtf8("laiue walk: windowed smoke32 passed\n");
                ranWindow = true;
                if (!state.failed)
                    PlatformWriteConsoleUtf8("laiue walk: windowed session ended cleanly\n");
                WalkVisualsDestroyChunkSet(device, mesherService, &state.chunkSet);
                WalkVisualsDestroyTerrain(device, state.texturedTerrainBuffers,
                                          state.terrainTextures, &state.terrainSampler);
                WalkVisualsDestroyBuffer(device, &state.farTerrainBuffer);
                if (state.ragdollBuffer != 0u &&
                    WalkDeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                                           sizeof(device->destroyHandle)) &&
                    device->destroyHandle != NULL)
                    device->destroyHandle(device, state.ragdollBuffer);
                PlatformFree(state.ragdollVisualScratch);
                PlatformFree(state.scenarioMetrics);
                PlatformFree(state.scenarioGpuMetrics);
            }
            else
                PlatformWriteConsoleUtf8(
                    "laiue walk: graphics/window unavailable; using diagnostic headless mode\n");
            if (device != NULL)
                graphicsService->destroyDevice(device);
            if (uiContext != NULL)
                uiService->contextDestroy(uiContext);
            if (input != NULL)
                inputService->destroy(input);
            if (window != NULL)
                windowService->destroy(window);
        }
        else
            PlatformWriteConsoleUtf8(
                "laiue walk: graphics/window modules missing; using diagnostic headless mode\n");
    }
#endif

    if (!ranWindow)
    {
        const LaiueVoxelProviderV1 *provider = &game.walkProvider;
        const LaiueVoxelCoordV1 coordinates[4] = {{0, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 0, -4}};
        const uint16_t materials[4] = {1u, 0u, 2u, 3u};
        for (uint32_t index = 0u; success && index < 4u; ++index)
        {
            LaiueVoxelBlockV1 block = {0};
            success = provider->getBlock(provider, &coordinates[index], &block) != 0u &&
                      block.material == materials[index];
        }
        if (game.actor != WALK_GAMEPLAY_ACTOR_STATIC)
        {
            WalkGameplaySpatialSnapshot before, after;
            success = success && WalkGameplaySnapshot(&game, &before) == WALK_GAMEPLAY_OK;
            for (uint32_t tick = 0u; success && tick < LAIUE_CHARACTER_TICK_HZ * 2u; ++tick)
            {
                WalkGameplayInput input = {
                    .strafe = 1.0,
                    .sprint = true,
                    .pulses = tick == 0u ? WALK_GAMEPLAY_PULSE_JUMP : 0u,
                };
                WalkGameplayStepResult step;
                success = WalkGameplaySubmitInput(&game, &input) == WALK_GAMEPLAY_OK &&
                          WalkGameplayStep(&game, &step) == WALK_GAMEPLAY_OK;
            }
            double delta[3];
            success = success && WalkGameplaySnapshot(&game, &after) == WALK_GAMEPLAY_OK &&
                      after.grounded && WalkGameplaySpatialDelta(&before, &after, delta) &&
                      delta[0] > 0.5;
            if (success && game.actor == WALK_GAMEPLAY_ACTOR_CHARACTER)
            {
                const bool crossed = game.lastPosition.cellX != gameConfig.initialPosition.cellX;
                PlatformWriteConsoleUtf8("laiue walk: SDK character/voxel graph passed\n");
                PlatformWriteConsoleUtf8(crossed
                                             ? "laiue walk: infinite-coordinate rebase passed\n"
                                             : "laiue walk: infinite-coordinate rebase failed\n");
                success = crossed;
            }
            else if (success)
                PlatformWriteConsoleUtf8("laiue walk: shared ragdoll/terrain diagnostics passed\n");
        }
        else if (success)
            PlatformWriteConsoleUtf8(
                "laiue walk: terrain diagnostics passed; character controls disabled\n");
    }
    if (scenarioEnabled || smokeEnabled)
        success = success && ranWindow;
    /* No provider is stopped while the game still owns its bodies/world. */
    WalkGameplayRelease(&game);
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    return success;
}

#if defined(_WIN32)
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);
__declspec(dllimport) const wchar_t *__stdcall GetCommandLineW(void);

static bool WalkArgument(const wchar_t *expected)
{
    const wchar_t *commandLine = GetCommandLineW();
    if (commandLine == NULL)
        return false;
    for (uint32_t index = 0u; commandLine[index] != L'\0'; ++index)
    {
        if (index != 0u && commandLine[index - 1u] != L' ' && commandLine[index - 1u] != L'\t' &&
            commandLine[index - 1u] != L'"')
            continue;
        uint32_t matched = 0u;
        while (expected[matched] != L'\0' && commandLine[index + matched] != L'\0' &&
               commandLine[index + matched] == expected[matched])
            ++matched;
        if (expected[matched] == L'\0' &&
            (commandLine[index + matched] == L'\0' || commandLine[index + matched] == L' ' ||
             commandLine[index + matched] == L'\t' || commandLine[index + matched] == L'"'))
            return true;
    }
    return false;
}

void WalkExampleEntryPoint(void)
{
    ExitProcess(RunWalkExample(WalkArgument(L"--headless"), WalkArgument(L"--scenario"),
                               WalkArgument(L"--smoke"))
                    ? 0u
                    : 1u);
}
#else
static bool WalkArgument(int argc, char **argv, const char *expected)
{
    for (int32_t argument = 1; argument < argc; ++argument)
    {
        const char *value = argv[argument];
        uint32_t index = 0u;
        if (value == NULL)
            continue;
        while (value[index] != '\0' && expected[index] != '\0' && value[index] == expected[index])
            ++index;
        if (value[index] == '\0' && expected[index] == '\0')
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    return RunWalkExample(WalkArgument(argc, argv, "--headless"),
                          WalkArgument(argc, argv, "--scenario"),
                          WalkArgument(argc, argv, "--smoke"))
               ? 0
               : 1;
}
#endif
