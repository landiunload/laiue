#include "api.h"
#include "platform/window.h"
#include "platform/time.h"
#include "input/input.h"
#include "world/world.h"
#include "render/renderer.h"
#include "game/camera.h"
#include "core/math.h"
#include "core/chunk_streaming.h"

#include <stddef.h>
#include <windows.h>

// Вся конфигурация приложения — в одном месте.
typedef struct ApplicationConfiguration
{
    const wchar_t* windowTitle;
    int32_t windowWidth;
    int32_t windowHeight;
    float cameraSpeed;
    float mouseSensitivity;
    float fieldOfViewRadians;
    float nearPlane;
    float farPlane;
    float editReachDistance;
    int32_t viewRadiusChunks;
    int64_t worldSeed;
    int64_t rebaseThresholdBlocks;
    double spawnHeight;
} ApplicationConfiguration;

static const ApplicationConfiguration g_configuration = {
    .windowTitle = L"laiue",
    .windowWidth = 640,
    .windowHeight = 360,
    .cameraSpeed = 80.0f,
    .mouseSensitivity = 0.0025f,
    .fieldOfViewRadians = 1.047197f,
    .nearPlane = 0.1f,
    .farPlane = 1000.0f,
    .editReachDistance = 8.0f,
    .viewRadiusChunks = 5,
    .worldSeed = 42,
    .rebaseThresholdBlocks = 1048576,
    .spawnHeight = 100.0,
};

typedef struct ApplicationState
{
    Window* window;
    Input* input;
    World* world;
    Renderer* renderer;
    ChunkStreaming* chunkStreaming;
    Camera camera;
    int32_t windowWidth;
    int32_t windowHeight;
    double previousTimeSeconds;
    int64_t coordinateOverlayBlock[3];
    bool coordinateOverlayDirty;
} ApplicationState;

// Адаптер сигнатуры: окно передаёт userData + HRAWINPUT,
// модуль ввода принимает типизированный Input*.
static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static int64_t FloorToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}

static int64_t CalculateChunkAlignedShift(double position)
{
    int64_t block = FloorToInt64(position);
    int64_t chunk = block / CHUNK_SIZE;
    if (block < 0 && block % CHUNK_SIZE != 0)
    {
        --chunk;
    }
    return chunk * CHUNK_SIZE;
}

static int64_t CalculateRebaseShift(double position)
{
    double threshold = (double)g_configuration.rebaseThresholdBlocks;
    if (position >= -threshold && position <= threshold)
    {
        return 0;
    }

    return CalculateChunkAlignedShift(position);
}

// Камера всегда остаётся около локального нуля. Когда она удаляется слишком
// далеко, локальный кеш чанков останавливается, абсолютный bigint-origin мира
// сдвигается, а камера возвращается в точный диапазон double.
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

    ChunkStreamingDestroy(application->chunkStreaming);
    application->chunkStreaming = NULL;

    if (!WorldRebase(application->world, shift[0], shift[1], shift[2]))
    {
        return false;
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        application->camera.position[axis] -= (double)shift[axis];
    }
    application->coordinateOverlayDirty = true;

    application->chunkStreaming = ChunkStreamingCreate(
        application->world, application->renderer, g_configuration.viewRadiusChunks);
    return application->chunkStreaming != NULL;
}

static bool SquareAbsoluteX(ApplicationState* application)
{
    int64_t currentBlockX = FloorToInt64(application->camera.position[0]);

    // Для целых координат только 0²=0 и 1²=1. В этих случаях мир и кеш
    // вообще не трогаем — повторной загрузки чанков не будет.
    if (WorldAbsoluteBlockCoordinateEqualsInt64(
            application->world, 0, currentBlockX, 0)
        || WorldAbsoluteBlockCoordinateEqualsInt64(
            application->world, 0, currentBlockX, 1))
    {
        return true;
    }

    double fractionalX =
        application->camera.position[0] - (double)currentBlockX;

    ChunkStreamingDestroy(application->chunkStreaming);
    application->chunkStreaming = NULL;

    int64_t squaredLocalBlockX;
    if (!WorldSquareAbsoluteX(
            application->world, currentBlockX, &squaredLocalBlockX))
    {
        return false;
    }

    application->camera.position[0] =
        (double)squaredLocalBlockX + fractionalX;
    application->coordinateOverlayDirty = true;
    application->chunkStreaming = ChunkStreamingCreate(
        application->world, application->renderer, g_configuration.viewRadiusChunks);
    return application->chunkStreaming != NULL;
}

static void AppendWideText(wchar_t* destination, uint32_t capacity,
    uint32_t* length, const wchar_t* source)
{
    while (*source != L'\0' && *length + 1 < capacity)
    {
        destination[(*length)++] = *source++;
    }
    if (capacity > 0)
    {
        destination[*length] = L'\0';
    }
}

static void UpdateCoordinateOverlay(ApplicationState* application,
    const int64_t cameraBlockPosition[3])
{
    if (!application->coordinateOverlayDirty
        && application->coordinateOverlayBlock[0] == cameraBlockPosition[0]
        && application->coordinateOverlayBlock[1] == cameraBlockPosition[1]
        && application->coordinateOverlayBlock[2] == cameraBlockPosition[2])
    {
        return;
    }

    wchar_t coordinate[3][32];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldFormatAbsoluteBlockCoordinate(application->world, axis,
            cameraBlockPosition[axis], coordinate[axis], 32);
        application->coordinateOverlayBlock[axis] = cameraBlockPosition[axis];
    }

    wchar_t text[128] = L"";
    uint32_t length = 0;
    AppendWideText(text, 128, &length, L"X: ");
    AppendWideText(text, 128, &length, coordinate[0]);
    AppendWideText(text, 128, &length, L"\r\nY: ");
    AppendWideText(text, 128, &length, coordinate[1]);
    AppendWideText(text, 128, &length, L"\r\nZ: ");
    AppendWideText(text, 128, &length, coordinate[2]);

    WindowSetOverlayText(application->window, text);
    application->coordinateOverlayDirty = false;
}

// Трассировка луча по вокселям (DDA, Amanatides & Woo).
// outPreviousBlock — последний воздушный блок перед попаданием
// (куда ставится новый блок).
static bool VoxelRaycast(World* world, const double origin[3], const float direction[3],
    float maximumDistance, int64_t outHitBlock[3], int64_t outPreviousBlock[3])
{
    int64_t block[3];
    int64_t step[3];
    double tMax[3];
    double tDelta[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        block[axis] = FloorToInt64(origin[axis]);
        double axisDirection = (double)direction[axis];

        if (axisDirection > 1e-6)
        {
            step[axis] = 1;
            tDelta[axis] = 1.0 / axisDirection;
            tMax[axis] = ((double)(block[axis] + 1) - origin[axis]) / axisDirection;
        }
        else if (axisDirection < -1e-6)
        {
            step[axis] = -1;
            tDelta[axis] = -1.0 / axisDirection;
            tMax[axis] = ((double)block[axis] - origin[axis]) / axisDirection;
        }
        else
        {
            step[axis] = 0;
            tDelta[axis] = 1e30;
            tMax[axis] = 1e30;
        }
    }

    for (;;)
    {
        int32_t axis = 0;
        if (tMax[1] < tMax[axis]) axis = 1;
        if (tMax[2] < tMax[axis]) axis = 2;

        if (tMax[axis] > (double)maximumDistance)
        {
            return false;
        }

        outPreviousBlock[0] = block[0];
        outPreviousBlock[1] = block[1];
        outPreviousBlock[2] = block[2];

        block[axis] += step[axis];
        tMax[axis] += tDelta[axis];

        if (WorldGetBlock(world, block[0], block[1], block[2]) != BLOCK_AIR)
        {
            outHitBlock[0] = block[0];
            outHitBlock[1] = block[1];
            outHitBlock[2] = block[2];
            return true;
        }
    }
}

// Редактирование мира: ЛКМ — сломать блок, ПКМ — поставить.
static void HandleBlockEditing(ApplicationState* application)
{
    bool breakPressed = InputWasMouseButtonPressed(application->input, INPUT_MOUSE_BUTTON_LEFT);
    bool placePressed = InputWasMouseButtonPressed(application->input, INPUT_MOUSE_BUTTON_RIGHT);
    if (!breakPressed && !placePressed)
    {
        return;
    }

    float lookDirection[3];
    CameraGetForwardVector(&application->camera, lookDirection);

    int64_t hitBlock[3];
    int64_t previousBlock[3];
    if (!VoxelRaycast(application->world, application->camera.position, lookDirection,
            g_configuration.editReachDistance, hitBlock, previousBlock))
    {
        return;
    }

    if (breakPressed)
    {
        WorldSetBlock(application->world, hitBlock[0], hitBlock[1], hitBlock[2], BLOCK_AIR);
        ChunkStreamingInvalidateBlock(application->chunkStreaming, hitBlock[0], hitBlock[1], hitBlock[2]);
    }
    else
    {
        WorldSetBlock(application->world, previousBlock[0], previousBlock[1], previousBlock[2], BLOCK_EARTH);
        ChunkStreamingInvalidateBlock(application->chunkStreaming, previousBlock[0], previousBlock[1], previousBlock[2]);
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

    // F7 — включить mouse look, Shift+F7 — выключить.
    if (InputWasKeyPressed(application->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(application->window, !InputIsKeyDown(application->input, INPUT_KEY_SHIFT));
    }

    // Вне фокуса события отпускания клавиш не доставляются —
    // без сброса клавиши «залипали» бы нажатыми.
    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_T))
    {
        if (!SquareAbsoluteX(application))
        {
            WindowRequestClose(application->window);
            return;
        }
    }

    if (WindowConsumeResize(application->window))
    {
        int32_t clientWidth;
        int32_t clientHeight;
        WindowGetClientSize(application->window, &clientWidth, &clientHeight);
        if (clientWidth > 0 && clientHeight > 0)
        {
            application->windowWidth = clientWidth;
            application->windowHeight = clientHeight;
            RendererResize(application->renderer, clientWidth, clientHeight);
        }
    }

    // Время кадра.
    double currentTimeSeconds = PlatformTimeSeconds();
    float deltaSeconds = (float)(currentTimeSeconds - application->previousTimeSeconds);
    application->previousTimeSeconds = currentTimeSeconds;
    if (deltaSeconds > 0.1f)
    {
        deltaSeconds = 0.1f;
    }

    // Камера вращается и редактирует мир только при захваченной мыши.
    bool mouseLookEnabled = WindowIsMouseLookEnabled(application->window);
    int32_t mouseDeltaX = 0;
    int32_t mouseDeltaY = 0;
    if (mouseLookEnabled)
    {
        InputGetMouseDelta(application->input, &mouseDeltaX, &mouseDeltaY);
    }

    CameraUpdate(&application->camera, deltaSeconds,
        InputIsKeyDown(application->input, INPUT_KEY_W),
        InputIsKeyDown(application->input, INPUT_KEY_A),
        InputIsKeyDown(application->input, INPUT_KEY_S),
        InputIsKeyDown(application->input, INPUT_KEY_D),
        InputIsKeyDown(application->input, INPUT_KEY_SPACE),
        mouseDeltaX, mouseDeltaY,
        g_configuration.cameraSpeed, g_configuration.mouseSensitivity);

    if (!RebaseWorldIfNeeded(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (mouseLookEnabled)
    {
        HandleBlockEditing(application);
    }

    // Origin rebasing: начало координат рендера — блок камеры,
    // все позиции в кадре малы и точны при любых мировых координатах.
    int64_t cameraBlockPosition[3];
    float relativeEyePosition[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cameraBlockPosition[axis] = FloorToInt64(application->camera.position[axis]);
        relativeEyePosition[axis] = (float)(application->camera.position[axis] - (double)cameraBlockPosition[axis]);
    }

    UpdateCoordinateOverlay(application, cameraBlockPosition);

    // Стриминг чанков: центр следует за камерой, меши строятся
    // пулом рабочих потоков, готовые забираются с бюджетом на кадр.
    ChunkStreamingSetCenter(application->chunkStreaming,
        cameraBlockPosition[0] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[1] >> CHUNK_SIZE_LOG2,
        cameraBlockPosition[2] >> CHUNK_SIZE_LOG2);
    ChunkStreamingPump(application->chunkStreaming);

    float viewMatrix[16];
    float projectionMatrix[16];
    float viewProjectionMatrix[16];
    CameraGetViewMatrix(&application->camera, relativeEyePosition, viewMatrix);
    CameraGetProjectionMatrix(
        (float)application->windowWidth / (float)application->windowHeight,
        g_configuration.fieldOfViewRadians, g_configuration.nearPlane, g_configuration.farPlane,
        projectionMatrix);
    Matrix4Multiply(viewMatrix, projectionMatrix, viewProjectionMatrix);

    if (RendererBeginFrame(application->renderer, viewProjectionMatrix))
    {
        ChunkStreamingDraw(application->chunkStreaming, viewProjectionMatrix, cameraBlockPosition);
        RendererEndFrame(application->renderer);
    }

    InputEndFrame(application->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title  = g_configuration.windowTitle,
        .width  = g_configuration.windowWidth,
        .height = g_configuration.windowHeight,
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

    World* world = WorldCreate(g_configuration.worldSeed);
    if (world == NULL)
    {
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    // Рендерер создаётся под фактический размер клиентской области.
    int32_t clientWidth;
    int32_t clientHeight;
    WindowGetClientSize(window, &clientWidth, &clientHeight);

    Renderer* renderer = RendererCreate(WindowGetNativeHandle(window), clientWidth, clientHeight);
    if (renderer == NULL)
    {
        WorldDestroy(world);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    ChunkStreaming* chunkStreaming = ChunkStreamingCreate(world, renderer, g_configuration.viewRadiusChunks);
    if (chunkStreaming == NULL)
    {
        RendererDestroy(renderer);
        WorldDestroy(world);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    ApplicationState application = {
        .window = window,
        .input = input,
        .world = world,
        .renderer = renderer,
        .chunkStreaming = chunkStreaming,
        .windowWidth = clientWidth,
        .windowHeight = clientHeight,
        .previousTimeSeconds = PlatformTimeSeconds(),
        .coordinateOverlayDirty = true,
    };

    // Камера стартует над рельефом и сразу смотрит немного вниз.
    CameraInit(&application.camera, 0.0, 0.0, g_configuration.spawnHeight, 0.0f, -0.4f);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, &application);

    ChunkStreamingDestroy(application.chunkStreaming);
    RendererDestroy(renderer);
    WorldDestroy(world);
    InputDestroy(input);
    WindowDestroy(window);
}
