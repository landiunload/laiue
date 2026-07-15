#include "api.h"
#include "core/application_config.h"
#include "core/chunk_streaming.h"
#include "core/game_hud.h"
#include "core/game_time.h"
#include "core/math.h"
#include "core/numeric.h"
#include "core/panorama.h"
#include "core/pause_menu.h"
#include "core/player_command_mapper.h"
#include "core/ui.h"
#include "game/camera.h"
#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "interaction/voxel_interaction.h"
#include "input/input.h"
#include "platform/time.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "world/block_properties.h"
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

    GameSettings settings;
    PanoramaCache panoramaCache;
    PauseMenu menu;
    UiContext ui;
    GameHud hud;
    bool mouseLookBeforeMenu;
} ApplicationState;

static void QueryWorldBlockPhysics(
    void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    BlockType type = WorldGetBlock((World*)context, x, y, z);
    BlockProperties properties = BlockGetProperties(type);
    outBlock->flags = properties.solid
        ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = properties.friction;
}

static PlayerCollisionSource CreatePlayerCollisionSource(World* world)
{
    PlayerCollisionSource source = {
        .context = world,
        .queryBlockPhysics = QueryWorldBlockPhysics,
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

    application->framesPerSecond = (uint32_t)(
        (double)application->fpsFrameCount / elapsedSeconds + 0.5);
    application->fpsFrameCount = 0;
    application->fpsSampleStartSeconds = currentTimeSeconds;
}

static void HandleBlockEditing(ApplicationState* application)
{
    VoxelEdit edit;
    bool breakPressed = InputWasMouseButtonPressed(
        application->input, INPUT_MOUSE_BUTTON_LEFT);
    bool placePressed = InputWasMouseButtonPressed(
        application->input, INPUT_MOUSE_BUTTON_RIGHT);
    if (!breakPressed && !placePressed)
    {
        return;
    }

    float direction[3];
    CameraGetForwardVector(&application->camera, direction);

    VoxelBodyShape bodyShape;
    const VoxelBodyShape* blockingShape = NULL;
    const double* blockingPosition = NULL;
    if (application->gameMode == GAME_MODE_WALK)
    {
        PlayerControllerGetBodyShape(&application->player, &bodyShape);
        blockingShape = &bodyShape;
        blockingPosition = application->camera.position;
    }

    if (!VoxelInteractionTryCreateEdit(application->world,
            application->camera.position, direction,
            blockingPosition, blockingShape,
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
}

static void UpdatePlayer(ApplicationState* application,
    float deltaSeconds, int32_t mouseDeltaX, int32_t mouseDeltaY)
{
    // Настройки раздела «Управление» применяются вживую.
    float sensitivity = g_applicationConfiguration.mouseSensitivity
        * (float)application->settings.mouseSensitivityPercent * 0.01f;
    float flySpeed = (float)application->settings.flySpeedBlocks;

    if (application->gameMode == GAME_MODE_FLY)
    {
        while (InputConsumeKeyPress(
                application->input, INPUT_KEY_SPACE))
        {
        }
        CameraUpdate(&application->camera, deltaSeconds,
            InputIsKeyDown(application->input, INPUT_KEY_W),
            InputIsKeyDown(application->input, INPUT_KEY_A),
            InputIsKeyDown(application->input, INPUT_KEY_S),
            InputIsKeyDown(application->input, INPUT_KEY_D),
            InputIsKeyDown(application->input, INPUT_KEY_SPACE),
            mouseDeltaX, mouseDeltaY,
            flySpeed, sensitivity);
        return;
    }

    CameraUpdate(&application->camera, deltaSeconds,
        false, false, false, false, false,
        mouseDeltaX, mouseDeltaY, 0.0f,
        sensitivity);

    PlayerControllerCommand command;
    PlayerCommandMapperBuild(application->input,
        &application->camera, &command);

    PlayerCollisionSource collision =
        CreatePlayerCollisionSource(application->world);
    PlayerControllerUpdate(&application->player,
        &collision, &application->camera, &command,
        deltaSeconds);
}

// Закрывает меню и возвращает игру: восстанавливает режим взгляда,
// сбрасывает накопленный за паузу ввод.
static void ResumeGame(ApplicationState* application)
{
    application->menu.screen = PAUSE_MENU_CLOSED;
    WindowSetMouseLook(application->window, application->mouseLookBeforeMenu);
    InputResetState(application->input);
}

static void OnFrame(void* userData)
{
    ApplicationState* application = userData;

    bool escapePressed =
        InputConsumeKeyPress(application->input, INPUT_KEY_ESCAPE);
    bool menuOpen = application->menu.screen != PAUSE_MENU_CLOSED;

    if (escapePressed && !menuOpen)
    {
        // Открытие меню: курсор освобождается, режим взгляда запоминается.
        PauseMenuOpen(&application->menu);
        application->mouseLookBeforeMenu =
            WindowIsMouseLookEnabled(application->window);
        WindowSetMouseLook(application->window, false);
        menuOpen = true;
        escapePressed = false;
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(application->window,
            !InputIsKeyDown(application->input, INPUT_KEY_SHIFT));
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_V))
    {
        RendererSetVerticalSync(application->renderer,
            !RendererIsVerticalSyncEnabled(application->renderer));
    }

    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_G))
    {
        ToggleGameMode(application);
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_T)
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

    if (!menuOpen)
    {
        UpdatePlayer(application,
            deltaSeconds, mouseDeltaX, mouseDeltaY);
        // Игровое время идёт, пока не открыто меню (пауза).
        application->settings.timeOfDayHours = GameTimeAdvance(
            application->settings.timeOfDayHours,
            application->settings.timeSpeed,
            g_applicationConfiguration.dayLengthMinutes,
            deltaSeconds);
    }

    if (!RebaseWorldIfNeeded(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (mouseLookEnabled && !menuOpen)
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

    ChunkStreamingSetCenter(application->chunkStreaming,
        cameraBlockPosition[0] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[1] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[2] >> CHUNK_SIZE_LOG2);
    ChunkStreamingPump(application->chunkStreaming);

    // HUD и меню собираются одним UI-контекстом. При закрытом меню ввод
    // виджетам не передаётся, но шрифт и HUD всё равно доступны с первого кадра.
    int32_t cursorX = -1;
    int32_t cursorY = -1;
    bool mouseDown = false;
    bool mousePressed = false;
    if (menuOpen)
    {
        WindowGetCursorClientPosition(application->window,
            &cursorX, &cursorY);
        mouseDown = InputIsMouseButtonDown(
            application->input, INPUT_MOUSE_BUTTON_LEFT);
        mousePressed = InputWasMouseButtonPressed(
            application->input, INPUT_MOUSE_BUTTON_LEFT);
    }

    bool uiReady = UiBegin(&application->ui,
        application->windowWidth, application->windowHeight,
        (float)cursorX, (float)cursorY,
        mouseDown, mousePressed, deltaSeconds);
    if (uiReady)
    {
        if (application->ui.fontDirty
            && RendererUiSetFontAtlas(application->renderer,
                application->ui.font.atlas,
                application->ui.font.atlasWidth,
                application->ui.font.atlasHeight))
        {
            application->ui.fontDirty = false;
        }

        if (menuOpen)
        {
            PauseMenuAction action = PauseMenuUpdate(&application->menu,
                &application->ui, &application->settings,
                application->renderer,
                g_applicationConfiguration.dayLengthMinutes,
                application->windowWidth, application->windowHeight,
                escapePressed);
            if (action == PAUSE_MENU_ACTION_QUIT)
            {
                WindowRequestClose(application->window);
                return;
            }
            if (action == PAUSE_MENU_ACTION_RESUME)
            {
                ResumeGame(application);
                application->ui.quadCount = 0;
            }
        }
        else
        {
            uint32_t timeMinutes = (uint32_t)(
                application->settings.timeOfDayHours * 60.0f) % 1440u;
            GameHudDraw(&application->hud, &application->ui,
                application->world, &application->player,
                application->gameMode, application->framesPerSecond,
                timeMinutes, cameraBlockPosition,
                application->windowWidth, application->windowHeight);
        }
    }
    else if (menuOpen)
    {
        // Шрифт недоступен — меню нарисовать нечем, не запираем игрока.
        ResumeGame(application);
    }

    // Описание кадра: обычная перспектива или панорама по граням кубмапы.
    float viewMatrix[16];
    CameraGetViewMatrix(&application->camera,
        relativeEyePosition, viewMatrix);

    RendererFrameSetup frameSetup;
    PanoramaBuildFrameSetup(&application->panoramaCache,
        application->settings.projection,
        (float)application->settings.fovDegrees,
        application->windowWidth, application->windowHeight,
        g_applicationConfiguration.nearPlane,
        g_applicationConfiguration.farPlane,
        viewMatrix, &frameSetup);

    // Свет и небо от времени суток.
    DayLighting lighting;
    GameTimeGetLighting(application->settings.timeOfDayHours, &lighting);
    for (int32_t channel = 0; channel < 3; ++channel)
    {
        frameSetup.sunDirection[channel] = lighting.sunDirection[channel];
        frameSetup.sunColor[channel] = lighting.sunColor[channel];
        frameSetup.ambientColor[channel] = lighting.ambientColor[channel];
        frameSetup.skyColor[channel] = lighting.skyColor[channel];
    }

    if (RendererBeginFrame(application->renderer, &frameSetup))
    {
        for (uint32_t pass = 0; pass < frameSetup.passCount; ++pass)
        {
            RendererBeginScenePass(application->renderer, pass);
            ChunkStreamingDraw(application->chunkStreaming,
                frameSetup.passes[pass].viewProjection,
                cameraBlockPosition);
        }

        if (uiReady && application->ui.quadCount > 0)
        {
            RendererUiQueue(application->renderer,
                application->ui.quads, application->ui.quadCount);
        }

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

    // Состояние приложения крупное (квады интерфейса, кеш панорамы) —
    // живёт на куче: стек без CRT не имеет проб роста (__chkstk),
    // поэтому кадры функций обязаны оставаться меньше страницы.
    ApplicationState* application = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*application));
    if (application == NULL)
    {
        ChunkStreamingDestroy(chunkStreaming);
        RendererDestroy(renderer);
        WorldDestroy(world);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    double startTimeSeconds = PlatformTimeSeconds();
    application->window = window;
    application->input = input;
    application->world = world;
    application->renderer = renderer;
    application->chunkStreaming = chunkStreaming;
    application->gameMode = GAME_MODE_FLY;
    application->windowWidth = clientWidth;
    application->windowHeight = clientHeight;
    application->previousTimeSeconds = startTimeSeconds;
    application->fpsSampleStartSeconds = startTimeSeconds;
    application->settings.fovDegrees =
        g_applicationConfiguration.defaultFieldOfViewDegrees;
    application->settings.projection = RENDER_PROJECTION_AUTO;
    application->settings.timeOfDayHours =
        g_applicationConfiguration.startTimeOfDayHours;
    application->settings.timeSpeed = TIME_SPEED_NORMAL;
    application->settings.mouseSensitivityPercent = 100;
    application->settings.flySpeedBlocks =
        (int32_t)g_applicationConfiguration.cameraSpeed;

    GameHudInit(&application->hud);
    PlayerControllerInit(&application->player,
        &g_applicationConfiguration.player);

    CameraInit(&application->camera,
        0.0, 0.0, g_applicationConfiguration.spawnHeight,
        0.0f, -0.4f);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, application);

    UiRelease(&application->ui);
    ChunkStreamingDestroy(application->chunkStreaming);
    RendererDestroy(renderer);
    WorldDestroy(world);
    InputDestroy(input);
    WindowDestroy(window);
    HeapFree(GetProcessHeap(), 0, application);
}
