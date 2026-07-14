#include "api.h"
#include "core/application_config.h"
#include "core/chunk_streaming.h"
#include "core/debug_overlay.h"
#include "core/math.h"
#include "core/numeric.h"
#include "game/camera.h"
#include "game/game_mode.h"
#include "game/player_controller.h"
#include "game/voxel_interaction.h"
#include "input/input.h"
#include "platform/time.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "world/world.h"

#include <stddef.h>
#include <windows.h>

typedef struct ApplicationState
{
    Window* window;
    Input* input;
    World* world;
    Renderer* renderer;
    ChunkStreaming* chunkStreaming;
    Camera camera;
    PlayerController player;
    GameMode gameMode;
    int32_t windowWidth;
    int32_t windowHeight;
    double previousTimeSeconds;
    double fpsSampleStartSeconds;
    uint32_t fpsFrameCount;
    uint32_t framesPerSecond;
    int64_t coordinateOverlayBlock[3];
    bool overlayDirty;
} ApplicationState;

static bool IsSolidWorldBlock(
    void* context, int64_t x, int64_t y, int64_t z)
{
    return WorldGetBlock((World*)context, x, y, z) != BLOCK_AIR;
}

static PlayerCollisionSource CreatePlayerCollisionSource(World* world)
{
    PlayerCollisionSource source = {
        .context = world,
        .isSolidBlock = IsSolidWorldBlock,
    };
    return source;
}

static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static int64_t CalculateChunkAlignedShift(double position)
{
    int64_t block = FloorDoubleToInt64(position);
    int64_t chunk = block / CHUNK_SIZE;
    if (block < 0 && block % CHUNK_SIZE != 0)
    {
        --chunk;
    }
    return chunk * CHUNK_SIZE;
}

static int64_t CalculateRebaseShift(double position)
{
    double threshold =
        (double)g_applicationConfiguration.rebaseThresholdBlocks;
    if (position >= -threshold && position <= threshold)
    {
        return 0;
    }
    return CalculateChunkAlignedShift(position);
}

static bool RebaseWorldIfNeeded(ApplicationState* application)
{
    int64_t shift[3] = {
        CalculateRebaseShift(application->camera.position[0]),
        CalculateRebaseShift(application->camera.position[1]),
        CalculateRebaseShift(application->camera.position[2]),
    };
    if (shift[0] == 0 && shift[1] == 0 && shift[2] == 0)
    {
        return true;
    }

    if (!ChunkStreamingPause(application->chunkStreaming))
    {
        return false;
    }
    if (!WorldRebase(application->world, shift[0], shift[1], shift[2]))
    {
        return false;
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        application->camera.position[axis] -= (double)shift[axis];
    }
    application->overlayDirty = true;

    int64_t centerX =
        FloorDoubleToInt64(application->camera.position[0]) >> CHUNK_SIZE_LOG2;
    int64_t centerY =
        FloorDoubleToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ =
        FloorDoubleToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, true,
        shift[0] / CHUNK_SIZE, shift[1] / CHUNK_SIZE,
        shift[2] / CHUNK_SIZE, centerX, centerY, centerZ);
}

static bool SquareAbsoluteX(ApplicationState* application)
{
    int64_t currentBlockX =
        FloorDoubleToInt64(application->camera.position[0]);
    double fractionalX =
        application->camera.position[0] - (double)currentBlockX;

    if (!ChunkStreamingPause(application->chunkStreaming))
    {
        return false;
    }

    int64_t squaredLocalBlockX;
    bool chunkOriginDeltaFits;
    int64_t chunkOriginDeltaX;
    if (!WorldSquareAbsoluteX(application->world, currentBlockX,
            &squaredLocalBlockX, &chunkOriginDeltaFits,
            &chunkOriginDeltaX))
    {
        return false;
    }

    application->camera.position[0] =
        (double)squaredLocalBlockX + fractionalX;
    if (application->gameMode == GAME_MODE_WALK)
    {
        PlayerCollisionSource collision =
            CreatePlayerCollisionSource(application->world);
        PlayerControllerReset(&application->player, &application->camera);
        if (!PlayerControllerResolvePenetration(&application->player,
                &collision, &application->camera))
        {
            application->gameMode = GAME_MODE_FLY;
        }
    }
    application->overlayDirty = true;

    int64_t centerX = squaredLocalBlockX >> CHUNK_SIZE_LOG2;
    int64_t centerY =
        FloorDoubleToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ =
        FloorDoubleToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, chunkOriginDeltaFits,
        chunkOriginDeltaX, 0, 0, centerX, centerY, centerZ);
}

static void RecordPresentedFrame(ApplicationState* application)
{
    double currentTimeSeconds = PlatformTimeSeconds();
    application->fpsFrameCount++;

    double elapsedSeconds =
        currentTimeSeconds - application->fpsSampleStartSeconds;
    if (elapsedSeconds < 0.5)
    {
        return;
    }

    uint32_t framesPerSecond = (uint32_t)(
        (double)application->fpsFrameCount / elapsedSeconds + 0.5);
    if (application->framesPerSecond != framesPerSecond)
    {
        application->framesPerSecond = framesPerSecond;
        application->overlayDirty = true;
    }
    application->fpsFrameCount = 0;
    application->fpsSampleStartSeconds = currentTimeSeconds;
}

static void UpdateOverlay(ApplicationState* application,
    const int64_t cameraBlockPosition[3])
{
    if (!application->overlayDirty
        && application->coordinateOverlayBlock[0] == cameraBlockPosition[0]
        && application->coordinateOverlayBlock[1] == cameraBlockPosition[1]
        && application->coordinateOverlayBlock[2] == cameraBlockPosition[2])
    {
        return;
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        application->coordinateOverlayBlock[axis] =
            cameraBlockPosition[axis];
    }

    wchar_t text[160];
    DebugOverlayBuildText(application->world, &application->player,
        application->gameMode, application->framesPerSecond,
        cameraBlockPosition, text, 160);
    WindowSetOverlayText(application->window, text);
    application->overlayDirty = false;
}

static void HandleBlockEditing(ApplicationState* application)
{
    VoxelEdit edit;
    bool breakPressed = InputWasMouseButtonPressed(
        application->input, INPUT_MOUSE_BUTTON_LEFT);
    bool placePressed = InputWasMouseButtonPressed(
        application->input, INPUT_MOUSE_BUTTON_RIGHT);

    if (!VoxelInteractionTryCreateEdit(application->world,
            &application->camera, &application->player,
            application->gameMode == GAME_MODE_WALK,
            breakPressed, placePressed,
            g_applicationConfiguration.editReachDistance, &edit))
    {
        return;
    }

    WorldSetBlock(application->world,
        edit.block[0], edit.block[1], edit.block[2], edit.replacement);
    ChunkStreamingInvalidateBlock(application->chunkStreaming,
        edit.block[0], edit.block[1], edit.block[2]);
}

static void ToggleGameMode(ApplicationState* application)
{
    if (application->gameMode == GAME_MODE_FLY)
    {
        application->gameMode = GAME_MODE_WALK;
        PlayerCollisionSource collision =
            CreatePlayerCollisionSource(application->world);
        PlayerControllerReset(&application->player, &application->camera);
        if (!PlayerControllerResolvePenetration(&application->player,
                &collision, &application->camera))
        {
            application->gameMode = GAME_MODE_FLY;
        }
    }
    else
    {
        application->gameMode = GAME_MODE_FLY;
        PlayerControllerReset(&application->player, &application->camera);
    }
    application->overlayDirty = true;
}

static void UpdatePlayer(ApplicationState* application,
    float deltaSeconds, int32_t mouseDeltaX, int32_t mouseDeltaY)
{
    if (application->gameMode == GAME_MODE_FLY)
    {
        CameraUpdate(&application->camera, deltaSeconds,
            InputIsKeyDown(application->input, INPUT_KEY_W),
            InputIsKeyDown(application->input, INPUT_KEY_A),
            InputIsKeyDown(application->input, INPUT_KEY_S),
            InputIsKeyDown(application->input, INPUT_KEY_D),
            InputIsKeyDown(application->input, INPUT_KEY_SPACE),
            mouseDeltaX, mouseDeltaY,
            g_applicationConfiguration.cameraSpeed,
            g_applicationConfiguration.mouseSensitivity);
        return;
    }

    CameraUpdate(&application->camera, deltaSeconds,
        false, false, false, false, false,
        mouseDeltaX, mouseDeltaY, 0.0f,
        g_applicationConfiguration.mouseSensitivity);

    PlayerControllerCommand command = {
        .forward =
            (InputIsKeyDown(application->input, INPUT_KEY_W) ? 1.0f : 0.0f)
            - (InputIsKeyDown(application->input, INPUT_KEY_S) ? 1.0f : 0.0f),
        .right =
            (InputIsKeyDown(application->input, INPUT_KEY_D) ? 1.0f : 0.0f)
            - (InputIsKeyDown(application->input, INPUT_KEY_A) ? 1.0f : 0.0f),
        .jumpPressed =
            InputWasKeyPressed(application->input, INPUT_KEY_SPACE),
        .crouchHeld =
            InputIsKeyDown(application->input, INPUT_KEY_SHIFT),
    };

    PlayerCollisionSource collision =
        CreatePlayerCollisionSource(application->world);
    if (PlayerControllerUpdate(&application->player,
            &collision, &application->camera, &command,
            application->camera.yaw, deltaSeconds))
    {
        application->overlayDirty = true;
    }
}

static void OnFrame(void* userData)
{
    ApplicationState* application = userData;

    if (InputWasKeyPressed(application->input, INPUT_KEY_ESCAPE))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(application->window,
            !InputIsKeyDown(application->input, INPUT_KEY_SHIFT));
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_V))
    {
        RendererSetVerticalSync(application->renderer,
            !RendererIsVerticalSyncEnabled(application->renderer));
    }

    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_G))
    {
        ToggleGameMode(application);
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_T)
        && !SquareAbsoluteX(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (WindowConsumeResize(application->window))
    {
        int32_t clientWidth;
        int32_t clientHeight;
        WindowGetClientSize(application->window,
            &clientWidth, &clientHeight);
        if (clientWidth > 0 && clientHeight > 0)
        {
            application->windowWidth = clientWidth;
            application->windowHeight = clientHeight;
            RendererResize(application->renderer,
                clientWidth, clientHeight);
        }
    }

    double currentTimeSeconds = PlatformTimeSeconds();
    float deltaSeconds = (float)(
        currentTimeSeconds - application->previousTimeSeconds);
    application->previousTimeSeconds = currentTimeSeconds;
    if (deltaSeconds > 0.1f)
    {
        deltaSeconds = 0.1f;
    }

    bool mouseLookEnabled =
        WindowIsMouseLookEnabled(application->window);
    int32_t mouseDeltaX = 0;
    int32_t mouseDeltaY = 0;
    if (mouseLookEnabled)
    {
        InputGetMouseDelta(application->input,
            &mouseDeltaX, &mouseDeltaY);
    }

    UpdatePlayer(application,
        deltaSeconds, mouseDeltaX, mouseDeltaY);

    if (!RebaseWorldIfNeeded(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (mouseLookEnabled)
    {
        HandleBlockEditing(application);
    }

    int64_t cameraBlockPosition[3];
    float relativeEyePosition[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cameraBlockPosition[axis] =
            FloorDoubleToInt64(application->camera.position[axis]);
        relativeEyePosition[axis] = (float)(
            application->camera.position[axis]
            - (double)cameraBlockPosition[axis]);
    }

    UpdateOverlay(application, cameraBlockPosition);

    ChunkStreamingSetCenter(application->chunkStreaming,
        cameraBlockPosition[0] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[1] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[2] >> CHUNK_SIZE_LOG2);
    ChunkStreamingPump(application->chunkStreaming);

    float viewMatrix[16];
    float projectionMatrix[16];
    float viewProjectionMatrix[16];
    CameraGetViewMatrix(&application->camera,
        relativeEyePosition, viewMatrix);
    CameraGetProjectionMatrix(
        (float)application->windowWidth
            / (float)application->windowHeight,
        g_applicationConfiguration.fieldOfViewRadians,
        g_applicationConfiguration.nearPlane,
        g_applicationConfiguration.farPlane,
        projectionMatrix);
    Matrix4Multiply(viewMatrix,
        projectionMatrix, viewProjectionMatrix);

    if (RendererBeginFrame(
            application->renderer, viewProjectionMatrix))
    {
        ChunkStreamingDraw(application->chunkStreaming,
            viewProjectionMatrix, cameraBlockPosition);
        if (!RendererEndFrame(application->renderer))
        {
            WindowRequestClose(application->window);
            return;
        }
        RecordPresentedFrame(application);
    }

    InputEndFrame(application->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title = g_applicationConfiguration.windowTitle,
        .width = g_applicationConfiguration.windowWidth,
        .height = g_applicationConfiguration.windowHeight,
    };

    Window* window = WindowCreate(&windowConfiguration);
    if (window == NULL)
    {
        return;
    }

    Input* input = InputCreate(WindowGetNativeHandle(window));
    if (input == NULL)
    {
        WindowDestroy(window);
        return;
    }

    World* world = WorldCreate(g_applicationConfiguration.worldSeed);
    if (world == NULL)
    {
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    int32_t clientWidth;
    int32_t clientHeight;
    WindowGetClientSize(window, &clientWidth, &clientHeight);

    Renderer* renderer = RendererCreate(
        WindowGetNativeHandle(window), clientWidth, clientHeight);
    if (renderer == NULL)
    {
        WorldDestroy(world);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    ChunkStreaming* chunkStreaming = ChunkStreamingCreate(
        world, renderer, g_applicationConfiguration.viewRadiusChunks);
    if (chunkStreaming == NULL)
    {
        RendererDestroy(renderer);
        WorldDestroy(world);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    double startTimeSeconds = PlatformTimeSeconds();
    ApplicationState application = {
        .window = window,
        .input = input,
        .world = world,
        .renderer = renderer,
        .chunkStreaming = chunkStreaming,
        .gameMode = GAME_MODE_FLY,
        .windowWidth = clientWidth,
        .windowHeight = clientHeight,
        .previousTimeSeconds = startTimeSeconds,
        .fpsSampleStartSeconds = startTimeSeconds,
        .overlayDirty = true,
    };
    PlayerControllerInit(&application.player,
        &g_applicationConfiguration.player);

    CameraInit(&application.camera,
        0.0, 0.0, g_applicationConfiguration.spawnHeight,
        0.0f, -0.4f);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, &application);

    ChunkStreamingDestroy(application.chunkStreaming);
    RendererDestroy(renderer);
    WorldDestroy(world);
    InputDestroy(input);
    WindowDestroy(window);
}
