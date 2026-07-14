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
    float walkingSpeed;
    float gravity;
    float jumpSpeed;
    float maximumFallSpeed;
    double playerRadius;
    double playerHeight;
    double playerEyeHeight;
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
    .walkingSpeed = 7.0f,
    .gravity = 26.0f,
    .jumpSpeed = 9.0f,
    .maximumFallSpeed = 55.0f,
    .playerRadius = 0.30,
    .playerHeight = 1.80,
    .playerEyeHeight = 1.62,
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

typedef enum GameMode
{
    GAME_MODE_FLY,
    GAME_MODE_WALK,
} GameMode;

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
    double fpsSampleStartSeconds;
    uint32_t fpsFrameCount;
    uint32_t framesPerSecond;
    int64_t coordinateOverlayBlock[3];
    bool overlayDirty;
    GameMode gameMode;
    float verticalVelocity;
    bool grounded;
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

#define COLLISION_EPSILON 0.001

static void GetPlayerBounds(const Camera* camera,
    double minimum[3], double maximum[3])
{
    double feet = camera->position[2] - g_configuration.playerEyeHeight;

    minimum[0] = camera->position[0] - g_configuration.playerRadius;
    maximum[0] = camera->position[0] + g_configuration.playerRadius;
    minimum[1] = camera->position[1] - g_configuration.playerRadius;
    maximum[1] = camera->position[1] + g_configuration.playerRadius;
    minimum[2] = feet;
    maximum[2] = feet + g_configuration.playerHeight;
}

static bool PlayerCollidesAt(World* world, const Camera* camera)
{
    double minimum[3];
    double maximum[3];
    GetPlayerBounds(camera, minimum, maximum);

    int64_t minimumBlock[3];
    int64_t maximumBlock[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        minimumBlock[axis] = FloorToInt64(minimum[axis] + COLLISION_EPSILON);
        maximumBlock[axis] = FloorToInt64(maximum[axis] - COLLISION_EPSILON);
    }

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (WorldGetBlock(world, x, y, z) != BLOCK_AIR)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool BlockPlaneCollides(World* world, int32_t axis, int64_t plane,
    const double minimum[3], const double maximum[3])
{
    int64_t minimumBlock[3];
    int64_t maximumBlock[3];
    for (int32_t currentAxis = 0; currentAxis < 3; ++currentAxis)
    {
        minimumBlock[currentAxis] =
            FloorToInt64(minimum[currentAxis] + COLLISION_EPSILON);
        maximumBlock[currentAxis] =
            FloorToInt64(maximum[currentAxis] - COLLISION_EPSILON);
    }
    minimumBlock[axis] = plane;
    maximumBlock[axis] = plane;

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (WorldGetBlock(world, x, y, z) != BLOCK_AIR)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Движение AABB отдельно по каждой оси. Возвращает true при столкновении.
// Проверяются все пересечённые плоскости блоков, поэтому большой deltaTime
// не позволяет пролететь сквозь тонкую стену или пол.
static bool MoveWalkingPlayerAxis(
    ApplicationState* application, int32_t axis, double distance)
{
    if (distance == 0.0)
    {
        return false;
    }

    double oldMinimum[3];
    double oldMaximum[3];
    GetPlayerBounds(&application->camera, oldMinimum, oldMaximum);

    Camera target = application->camera;
    target.position[axis] += distance;

    double newMinimum[3];
    double newMaximum[3];
    GetPlayerBounds(&target, newMinimum, newMaximum);

    double negativeExtent = axis == 2
        ? g_configuration.playerEyeHeight
        : g_configuration.playerRadius;
    double positiveExtent = axis == 2
        ? g_configuration.playerHeight - g_configuration.playerEyeHeight
        : g_configuration.playerRadius;

    if (distance > 0.0)
    {
        int64_t firstPlane =
            FloorToInt64(oldMaximum[axis] - COLLISION_EPSILON) + 1;
        int64_t lastPlane =
            FloorToInt64(newMaximum[axis] - COLLISION_EPSILON);

        for (int64_t plane = firstPlane; plane <= lastPlane; ++plane)
        {
            if (BlockPlaneCollides(
                    application->world, axis, plane, newMinimum, newMaximum))
            {
                application->camera.position[axis] =
                    (double)plane - positiveExtent - COLLISION_EPSILON;
                return true;
            }
        }
    }
    else
    {
        int64_t firstPlane =
            FloorToInt64(oldMinimum[axis] + COLLISION_EPSILON) - 1;
        int64_t lastPlane =
            FloorToInt64(newMinimum[axis] + COLLISION_EPSILON);

        for (int64_t plane = firstPlane; plane >= lastPlane; --plane)
        {
            if (BlockPlaneCollides(
                    application->world, axis, plane, newMinimum, newMaximum))
            {
                application->camera.position[axis] =
                    (double)plane + 1.0 + negativeExtent + COLLISION_EPSILON;
                return true;
            }
        }
    }

    application->camera.position[axis] = target.position[axis];
    return false;
}

static bool ResolveWalkingPlayerPenetration(ApplicationState* application)
{
    // После переключения режима или телепорта новая область может оказаться
    // внутри рельефа. Поднимаем игрока до первого свободного положения.
    for (uint32_t step = 0; step < CHUNK_SIZE * 4u; ++step)
    {
        if (!PlayerCollidesAt(application->world, &application->camera))
        {
            return true;
        }
        application->camera.position[2] += 1.0;
    }
    return !PlayerCollidesAt(application->world, &application->camera);
}

static bool PlayerOverlapsBlock(
    const ApplicationState* application, const int64_t block[3])
{
    if (application->gameMode != GAME_MODE_WALK)
    {
        return false;
    }

    double minimum[3];
    double maximum[3];
    GetPlayerBounds(&application->camera, minimum, maximum);

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double blockMinimum = (double)block[axis];
        double blockMaximum = blockMinimum + 1.0;
        if (maximum[axis] <= blockMinimum + COLLISION_EPSILON
            || minimum[axis] >= blockMaximum - COLLISION_EPSILON)
        {
            return false;
        }
    }
    return true;
}

static void UpdateWalkingPlayer(ApplicationState* application,
    float deltaSeconds, int32_t mouseDeltaX, int32_t mouseDeltaY)
{
    // CameraUpdate используется только для yaw/pitch; перемещение выполняет
    // контроллер игрока с коллизиями.
    CameraUpdate(&application->camera, deltaSeconds,
        false, false, false, false, false,
        mouseDeltaX, mouseDeltaY, 0.0f, g_configuration.mouseSensitivity);

    float forwardInput =
        (InputIsKeyDown(application->input, INPUT_KEY_W) ? 1.0f : 0.0f)
        - (InputIsKeyDown(application->input, INPUT_KEY_S) ? 1.0f : 0.0f);
    float rightInput =
        (InputIsKeyDown(application->input, INPUT_KEY_D) ? 1.0f : 0.0f)
        - (InputIsKeyDown(application->input, INPUT_KEY_A) ? 1.0f : 0.0f);

    if (forwardInput != 0.0f && rightInput != 0.0f)
    {
        forwardInput *= 0.70710678f;
        rightInput *= 0.70710678f;
    }

    float sinYaw = ScalarSin(application->camera.yaw);
    float cosYaw = ScalarCos(application->camera.yaw);
    float movement = g_configuration.walkingSpeed * deltaSeconds;

    double movementX = (double)(
        (sinYaw * forwardInput + cosYaw * rightInput) * movement);
    double movementY = (double)(
        (cosYaw * forwardInput - sinYaw * rightInput) * movement);

    MoveWalkingPlayerAxis(application, 0, movementX);
    MoveWalkingPlayerAxis(application, 1, movementY);

    if (application->grounded
        && InputWasKeyPressed(application->input, INPUT_KEY_SPACE))
    {
        application->verticalVelocity = g_configuration.jumpSpeed;
        application->grounded = false;
    }

    application->verticalVelocity -= g_configuration.gravity * deltaSeconds;
    if (application->verticalVelocity < -g_configuration.maximumFallSpeed)
    {
        application->verticalVelocity = -g_configuration.maximumFallSpeed;
    }

    float previousVelocity = application->verticalVelocity;
    bool verticalCollision = MoveWalkingPlayerAxis(application, 2,
        (double)(application->verticalVelocity * deltaSeconds));
    if (verticalCollision)
    {
        application->grounded = previousVelocity < 0.0f;
        application->verticalVelocity = 0.0f;
    }
    else
    {
        application->grounded = false;
    }
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

    int64_t centerX = FloorToInt64(application->camera.position[0]) >> CHUNK_SIZE_LOG2;
    int64_t centerY = FloorToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ = FloorToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, true,
        shift[0] / CHUNK_SIZE, shift[1] / CHUNK_SIZE, shift[2] / CHUNK_SIZE,
        centerX, centerY, centerZ);
}

static bool SquareAbsoluteX(ApplicationState* application)
{
    int64_t currentBlockX = FloorToInt64(application->camera.position[0]);
    double fractionalX =
        application->camera.position[0] - (double)currentBlockX;

    if (!ChunkStreamingPause(application->chunkStreaming))
    {
        return false;
    }

    int64_t squaredLocalBlockX;
    bool chunkOriginDeltaFits;
    int64_t chunkOriginDeltaX;
    if (!WorldSquareAbsoluteX(
            application->world, currentBlockX, &squaredLocalBlockX,
            &chunkOriginDeltaFits, &chunkOriginDeltaX))
    {
        return false;
    }

    application->camera.position[0] =
        (double)squaredLocalBlockX + fractionalX;
    if (application->gameMode == GAME_MODE_WALK)
    {
        application->verticalVelocity = 0.0f;
        application->grounded = false;
        if (!ResolveWalkingPlayerPenetration(application))
        {
            // Не оставляем камеру запертой внутри блоков.
            application->gameMode = GAME_MODE_FLY;
        }
    }
    application->overlayDirty = true;

    int64_t centerX = squaredLocalBlockX >> CHUNK_SIZE_LOG2;
    int64_t centerY = FloorToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ = FloorToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, chunkOriginDeltaFits,
        chunkOriginDeltaX, 0, 0, centerX, centerY, centerZ);
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

static void AppendUnsignedDecimal(wchar_t* destination, uint32_t capacity,
    uint32_t* length, uint32_t value)
{
    wchar_t reversedDigits[10];
    uint32_t digitCount = 0;
    do
    {
        reversedDigits[digitCount++] = (wchar_t)(L'0' + value % 10u);
        value /= 10u;
    }
    while (value != 0);

    while (digitCount > 0 && *length + 1 < capacity)
    {
        destination[(*length)++] = reversedDigits[--digitCount];
    }
    if (capacity > 0)
    {
        destination[*length] = L'\0';
    }
}

static void RecordPresentedFrame(ApplicationState* application)
{
    double currentTimeSeconds = PlatformTimeSeconds();
    application->fpsFrameCount++;

    double elapsedSeconds = currentTimeSeconds - application->fpsSampleStartSeconds;
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

    wchar_t coordinate[3][32];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldFormatAbsoluteBlockCoordinate(application->world, axis,
            cameraBlockPosition[axis], coordinate[axis], 32);
        application->coordinateOverlayBlock[axis] = cameraBlockPosition[axis];
    }

    wchar_t text[160] = L"";
    uint32_t length = 0;
    AppendWideText(text, 160, &length, L"X: ");
    AppendWideText(text, 160, &length, coordinate[0]);
    AppendWideText(text, 160, &length, L"\r\nY: ");
    AppendWideText(text, 160, &length, coordinate[1]);
    AppendWideText(text, 160, &length, L"\r\nZ: ");
    AppendWideText(text, 160, &length, coordinate[2]);
    AppendWideText(text, 160, &length, L"\r\nFPS: ");
    AppendUnsignedDecimal(text, 160, &length, application->framesPerSecond);
    AppendWideText(text, 160, &length, L"\r\nMode: ");
    AppendWideText(text, 160, &length,
        application->gameMode == GAME_MODE_WALK ? L"WALK" : L"FLY");

    WindowSetOverlayText(application->window, text);
    application->overlayDirty = false;
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
        if (PlayerOverlapsBlock(application, previousBlock))
        {
            return;
        }

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

    // V — переключить вертикальную синхронизацию.
    if (InputWasKeyPressed(application->input, INPUT_KEY_V))
    {
        RendererSetVerticalSync(application->renderer,
            !RendererIsVerticalSyncEnabled(application->renderer));
    }

    // Вне фокуса события отпускания клавиш не доставляются —
    // без сброса клавиши «залипали» бы нажатыми.
    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }

    if (InputWasKeyPressed(application->input, INPUT_KEY_G))
    {
        if (application->gameMode == GAME_MODE_FLY)
        {
            application->gameMode = GAME_MODE_WALK;
            application->verticalVelocity = 0.0f;
            application->grounded = false;
            if (!ResolveWalkingPlayerPenetration(application))
            {
                application->gameMode = GAME_MODE_FLY;
            }
        }
        else
        {
            application->gameMode = GAME_MODE_FLY;
            application->verticalVelocity = 0.0f;
            application->grounded = false;
        }
        application->overlayDirty = true;
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

    if (application->gameMode == GAME_MODE_WALK)
    {
        UpdateWalkingPlayer(
            application, deltaSeconds, mouseDeltaX, mouseDeltaY);
    }
    else
    {
        CameraUpdate(&application->camera, deltaSeconds,
            InputIsKeyDown(application->input, INPUT_KEY_W),
            InputIsKeyDown(application->input, INPUT_KEY_A),
            InputIsKeyDown(application->input, INPUT_KEY_S),
            InputIsKeyDown(application->input, INPUT_KEY_D),
            InputIsKeyDown(application->input, INPUT_KEY_SPACE),
            mouseDeltaX, mouseDeltaY,
            g_configuration.cameraSpeed, g_configuration.mouseSensitivity);
    }

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

    UpdateOverlay(application, cameraBlockPosition);

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

    double startTimeSeconds = PlatformTimeSeconds();
    ApplicationState application = {
        .window = window,
        .input = input,
        .world = world,
        .renderer = renderer,
        .chunkStreaming = chunkStreaming,
        .windowWidth = clientWidth,
        .windowHeight = clientHeight,
        .previousTimeSeconds = startTimeSeconds,
        .fpsSampleStartSeconds = startTimeSeconds,
        .overlayDirty = true,
        .gameMode = GAME_MODE_FLY,
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
