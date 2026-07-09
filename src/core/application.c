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

#define CAMERA_SPEED 80.0f
#define FIELD_OF_VIEW_RADIANS 1.047197f
#define NEAR_PLANE 0.1f
#define FAR_PLANE 1000.0f
#define VIEW_RADIUS_CHUNKS 3
#define WORLD_SEED 42

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
} ApplicationState;

// Адаптер сигнатуры: окно передаёт userData + HRAWINPUT,
// модуль ввода принимает типизированный Input*.
static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static int64_t ChunkFromCameraPosition(float position)
{
    int64_t chunk = (int64_t)(position / (float)CHUNK_SIZE);
    return position < 0.0f ? chunk - 1 : chunk;
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

    // Камера вращается только при захваченной мыши.
    int32_t mouseDeltaX = 0;
    int32_t mouseDeltaY = 0;
    if (WindowIsMouseLookEnabled(application->window))
    {
        InputGetMouseDelta(application->input, &mouseDeltaX, &mouseDeltaY);
    }

    CameraUpdate(&application->camera, deltaSeconds,
        InputIsKeyDown(application->input, INPUT_KEY_W),
        InputIsKeyDown(application->input, INPUT_KEY_A),
        InputIsKeyDown(application->input, INPUT_KEY_S),
        InputIsKeyDown(application->input, INPUT_KEY_D),
        mouseDeltaX, mouseDeltaY, CAMERA_SPEED);

    // Стриминг чанков: центр следует за камерой, меши строятся
    // рабочим потоком, готовые забираются каждый кадр.
    ChunkStreamingSetCenter(application->chunkStreaming,
        ChunkFromCameraPosition(application->camera.position[0]),
        ChunkFromCameraPosition(application->camera.position[1]),
        ChunkFromCameraPosition(application->camera.position[2]));
    ChunkStreamingPump(application->chunkStreaming);

    float viewMatrix[16];
    float projectionMatrix[16];
    float viewProjectionMatrix[16];
    CameraGetViewMatrix(&application->camera, viewMatrix);
    CameraGetProjectionMatrix(
        (float)application->windowWidth / (float)application->windowHeight,
        FIELD_OF_VIEW_RADIANS, NEAR_PLANE, FAR_PLANE, projectionMatrix);
    Matrix4Multiply(viewMatrix, projectionMatrix, viewProjectionMatrix);
    RendererSetViewProjection(application->renderer, viewProjectionMatrix);

    RendererBeginFrame(application->renderer);
    ChunkStreamingDraw(application->chunkStreaming, viewProjectionMatrix);
    RendererEndFrame(application->renderer);

    InputEndFrame(application->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title  = L"laiue",
        .width  = 1280,
        .height = 720,
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

    World* world = WorldCreate(WORLD_SEED);
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

    ChunkStreaming* chunkStreaming = ChunkStreamingCreate(world, renderer, VIEW_RADIUS_CHUNKS);
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
    };

    CameraInit(&application.camera, 0.0f, 80.0f, 0.0f, 0.0f, 0.0f);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, &application);

    ChunkStreamingDestroy(chunkStreaming);
    RendererDestroy(renderer);
    WorldDestroy(world);
    InputDestroy(input);
    WindowDestroy(window);
}
