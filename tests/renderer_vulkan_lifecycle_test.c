// Регрессия жизненного цикла окна/swapchain Vulkan: повторное создание и
// уничтожение, resize/minimize/restore, смена vsync, переключение
// D3D12<->Vulkan и несколько кадровых слотов. Окно — скрытый Win32
// WS_POPUP: пользователь не должен видеть интерактивных окон, а swapchain
// создаётся и без показа окна (так же, как в стенде
// docs/vulkan_swapchain_parallel_work.md).
//
// Отсутствие Vulkan-драйвера или несовместимая поверхность — это SKIP
// (код 125), а не PASS: тест, который не смог создать рендер, не
// подтверждает ровно ничего. Проверки идут по пикселям и размерам цели,
// чтобы «успешный» кадр с пустым present отличался от работающего.

#include "render/chunk_geometry.h"
#include "render/content_provider.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define SKIP_EXIT_CODE 125
#define TEST_CLIENT_WIDTH 320
#define TEST_CLIENT_HEIGHT 180
// Самый крупный кадр, который тест когда-либо читает: 640x360.
#define MAX_CAPTURE_WIDTH 640u
#define MAX_CAPTURE_HEIGHT 360u
#define MAX_CAPTURE_BYTES (MAX_CAPTURE_WIDTH * MAX_CAPTURE_HEIGHT * 4u)

// Всё, кроме точки входа, нужно только оконному сценарию: на не-Windows
// хелперы были бы неиспользуемыми, а сборка идёт с warnings-as-errors.
#if defined(_WIN32)

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Vulkan lifecycle check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ZeroBytes(void *destination, size_t count)
{
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t index = 0; index < count; ++index)
        bytes[index] = 0u;
}

// Единичная матрица: мировые координаты попадают прямо в clip space, поэтому
// положение квада не зависит ни от камеры, ни от проекции.
static void SetIdentity(float matrix[16])
{
    for (uint32_t index = 0; index < 16u; ++index)
        matrix[index] = 0.0f;
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

// Кадр только с очисткой: для swapchain-жизненного цикла этого достаточно,
// потому что present копирует и показывает цель независимо от содержимого.
static void BuildClearSetup(RendererFrameSetup *setup)
{
    ZeroBytes(setup, sizeof(*setup));
    setup->gamma = 1.0f;
    setup->skyColor[0] = 0.0f;
    setup->skyColor[1] = 0.0f;
    setup->skyColor[2] = 1.0f;
    setup->passCount = 0u;
}

// Кадр с одной гранью: цель всё ещё читается, но теперь несёт геометрию.
static void BuildSceneSetup(RendererFrameSetup *setup)
{
    BuildClearSetup(setup);
    setup->sunDirection[0] = 0.0f;
    setup->sunDirection[1] = 0.0f;
    setup->sunDirection[2] = -1.0f;
    setup->sunColor[0] = 1.0f;
    setup->sunColor[1] = 1.0f;
    setup->sunColor[2] = 1.0f;
    setup->ambientColor[0] = 1.0f;
    setup->ambientColor[1] = 1.0f;
    setup->ambientColor[2] = 1.0f;
    setup->passCount = 1u;
    SetIdentity(setup->passes[0].viewProjection);
    setup->passes[0].faceIndex = 0u;
    setup->passes[0].rectMinX = 0u;
    setup->passes[0].rectMinY = 0u;
    setup->passes[0].rectMaxX = MAX_CAPTURE_WIDTH;
    setup->passes[0].rectMaxY = MAX_CAPTURE_HEIGHT;
}

static const wchar_t *const kWindowClassName = L"LaiueVulkanLifecycleTestWindow";

static bool EnsureWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass;
    ZeroBytes(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass))
        return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// Окно намеренно не показывается и не активируется: swapchain создаётся по
// HWND скрытого WS_POPUP, а WS_POPUP даёт клиентскую область, равную
// запрошенному размеру (никаких рамок, чьё влияние на currentExtent
// зависело бы от настройки неклиентской области).
static HWND CreateTestWindow(HINSTANCE instance, int32_t width, int32_t height)
{
    RECT rectangle = {0, 0, width, height};
    const DWORD style = WS_POPUP;
    AdjustWindowRect(&rectangle, style, FALSE);
    return CreateWindowExW(0, kWindowClassName, L"laiue vulkan lifecycle", style, CW_USEDEFAULT,
                           CW_USEDEFAULT, rectangle.right - rectangle.left,
                           rectangle.bottom - rectangle.top, NULL, NULL, instance, NULL);
}

// Меняет именно клиентскую область: без показа окна SetWindowPos всё равно
// пересчитывает клиентский прямоугольник.
static void ResizeTestWindowClient(HWND window, int32_t width, int32_t height)
{
    RECT rectangle = {0, 0, width, height};
    const DWORD style = (DWORD)(uintptr_t)GetWindowLongPtrW(window, GWL_STYLE);
    AdjustWindowRect(&rectangle, style, FALSE);
    SetWindowPos(window, NULL, 0, 0, rectangle.right - rectangle.left,
                 rectangle.bottom - rectangle.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ReadTestWindowClient(HWND window, int32_t *outWidth, int32_t *outHeight)
{
    RECT rectangle;
    GetClientRect(window, &rectangle);
    *outWidth = rectangle.right - rectangle.left;
    *outHeight = rectangle.bottom - rectangle.top;
}

// Один кадр, который обязан пройти целиком. Данные кадра задаёт вызывающий.
static void DrawFrame(Renderer *renderer, const RendererFrameSetup *setup, const char *beginReason,
                      const char *endReason)
{
    Expect(RendererBeginFrame(renderer, setup), beginReason);
    if (setup->passCount != 0u)
        RendererBeginScenePass(renderer, 0u);
    Expect(RendererEndFrame(renderer), endReason);
}

// Кадр с одной гранью и настоящим мешем: проверяет, что мир и swapchain
// уживаются в одном рендере.
static void DrawMeshFrame(Renderer *renderer, RendererFrameSetup *setup, RendererMesh *mesh)
{
    static const float origin[3] = {-0.5f, -0.5f, -0.5f};
    setup->passCount = 1u;
    Expect(RendererBeginFrame(renderer, setup), "a mesh frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    Expect(RendererEndFrame(renderer), "a mesh frame could not end");
}

// Первый пиксель кадра только с очисткой обязан быть цветом неба (0,0,1).
// Это отличает по-настоящему записанный кадр от цели, которую «успешно»
// прочитали, но вовсе не рисовали.
static void ExpectSkyColour(const void *pixels, const char *reason)
{
    const uint8_t *pixel = (const uint8_t *)pixels;
    Expect(pixel[2] > 200u && pixel[0] < 30u && pixel[1] < 30u, reason);
}

// Читает цель и сверяет её размер с ожидаемым: смена размера, которая не
// дошла до цели кадра, иначе осталась бы незаменченной.
static void CaptureWithSize(Renderer *renderer, void *pixels, uint32_t expectedWidth,
                            uint32_t expectedHeight, const char *reason)
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    Expect(RendererCaptureFrame(renderer, pixels, MAX_CAPTURE_BYTES, &width, &height), reason);
    Expect(width == expectedWidth && height == expectedHeight,
           "the captured frame must follow the requested size");
}

static RendererMesh *CreateUnitMesh(Renderer *renderer)
{
    ChunkQuad quad = PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
    RendererMesh *mesh = RendererCreateMesh(renderer, &quad, 1u);
    Expect(mesh != NULL, "the unit mesh could not be created");
    return mesh;
}

// === 1. Повторное создание и уничтожение с честным окном на цикл ===
static void RunCreateDestroyStress(HINSTANCE instance, void *pixels)
{
    RendererFrameSetup setup;
    BuildSceneSetup(&setup);

    for (uint32_t cycle = 0u; cycle < 3u; ++cycle)
    {
        HWND window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
        Expect(window != NULL, "the stress window could not be created");
        int32_t clientWidth = 0;
        int32_t clientHeight = 0;
        ReadTestWindowClient(window, &clientWidth, &clientHeight);

        Renderer *renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH,
                                                       TEST_CLIENT_HEIGHT, RENDERER_BACKEND_VULKAN);
        Expect(renderer != NULL, "the windowed Vulkan renderer could not be created");
        Expect(RendererGetBackend(renderer) == RENDERER_BACKEND_VULKAN,
               "the renderer must report the Vulkan backend");
        Expect(RendererPrepareWorld(renderer), "the stress world could not be prepared");

        RendererMesh *mesh = CreateUnitMesh(renderer);
        const uint32_t frames = cycle == 0u ? 2u : 1u;
        for (uint32_t frame = 0u; frame < frames; ++frame)
            DrawMeshFrame(renderer, &setup, mesh);

        uint32_t width = 0u;
        uint32_t height = 0u;
        Expect(RendererCaptureFrame(renderer, pixels, MAX_CAPTURE_BYTES, &width, &height),
               "the stress frame could not be captured");
        Expect(width == (uint32_t)clientWidth && height == (uint32_t)clientHeight,
               "the stress frame must follow the client area");

        RendererDestroyMesh(renderer, mesh);
        RendererDestroy(renderer);
        DestroyWindow(window);
    }
}

// === 2. Resize, minimize и restore ===
static void RunResizeMinimize(HINSTANCE instance, void *pixels)
{
    HWND window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(window != NULL, "the resize window could not be created");
    Renderer *renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    Expect(renderer != NULL, "the resize renderer could not be created");
    Expect(RendererPrepareWorld(renderer), "the resize world could not be prepared");

    RendererFrameSetup setup;
    BuildClearSetup(&setup);

    int32_t clientWidth = 0;
    int32_t clientHeight = 0;
    ReadTestWindowClient(window, &clientWidth, &clientHeight);
    Expect(clientWidth > 0 && clientHeight > 0, "the initial client area must be non-empty");

    // Стартовый размер берётся из текущей клиентской области поверхности.
    DrawFrame(renderer, &setup, "the initial frame could not begin",
              "the initial frame could not end");
    CaptureWithSize(renderer, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                    "the initial frame could not be captured");
    ExpectSkyColour(pixels, "the initial frame must show the sky clear colour");

    // Цель кадра меняется без изменения окна: swapchain остаётся прежним
    // размером, поэтому present проходит через путь blit.
    RendererResize(renderer, 200, 120);
    DrawFrame(renderer, &setup, "the target-resize frame could not begin",
              "the target-resize frame could not end");
    CaptureWithSize(renderer, pixels, 200u, 120u, "the target-resize frame could not be captured");

    // Шторм resize до кадра: применяется последний запрос, а промежуточные
    // схлопываются. Утечка цели на каждое промежуточное значение здесь была
    // бы видна как рост памяти, а неверное применение — как чужой размер.
    for (uint32_t step = 0u; step < 10u; ++step)
    {
        RendererResize(renderer, 300 + (int32_t)step, 200 + (int32_t)step);
    }
    DrawFrame(renderer, &setup, "the resize-storm frame could not begin",
              "the resize-storm frame could not end");
    CaptureWithSize(renderer, pixels, 309u, 209u, "the resize-storm frame could not be captured");

    // Теперь меняется и окно, и цель: размеры обязаны сойтись.
    ResizeTestWindowClient(window, 480, 270);
    ReadTestWindowClient(window, &clientWidth, &clientHeight);
    RendererResize(renderer, clientWidth, clientHeight);
    DrawFrame(renderer, &setup, "the window-resize frame could not begin",
              "the window-resize frame could not end");
    CaptureWithSize(renderer, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                    "the window-resize frame could not be captured");

    // Свёрнутое окно: нулевой запрошенный размер. BeginFrame обязан
    // вернуть false, не блокируясь и не роняя рендер; EndFrame без
    // начатого кадра — тоже false.
    RendererResize(renderer, 0, 0);
    for (uint32_t frame = 0u; frame < 4u; ++frame)
    {
        Expect(!RendererBeginFrame(renderer, &setup),
               "a minimised window must skip frames, not begin one");
        Expect(!RendererEndFrame(renderer), "EndFrame without a started frame must report failure");
    }

    // Смена vsync и отрицательный размер, пока окно свёрнуто: значения
    // копятся, но кадры по-прежнему не начинаются.
    RendererSetVerticalSync(renderer, false);
    Expect(!RendererIsVerticalSyncEnabled(renderer), "vsync must be reported off while minimised");
    RendererResize(renderer, -17, -23);
    Expect(!RendererBeginFrame(renderer, &setup),
           "a negative size must behave like a minimised window");

    // Восстановление: окно и цель возвращаются, кадры и чтение снова живы.
    ResizeTestWindowClient(window, 480, 270);
    ReadTestWindowClient(window, &clientWidth, &clientHeight);
    RendererResize(renderer, clientWidth, clientHeight);
    DrawFrame(renderer, &setup, "the restored frame could not begin",
              "the restored frame could not end");
    CaptureWithSize(renderer, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                    "the restored frame could not be captured");
    ExpectSkyColour(pixels, "the restored frame must draw again after a minimise");

    RendererSetVerticalSync(renderer, true);
    DrawFrame(renderer, &setup, "the frame after vsync restore could not begin",
              "the frame after vsync restore could not end");

    RendererDestroy(renderer);
    DestroyWindow(window);
}

// === 3. Смена vsync между кадрами ===
static void RunVerticalSyncToggle(HINSTANCE instance, void *pixels)
{
    HWND window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(window != NULL, "the vsync window could not be created");
    int32_t clientWidth = 0;
    int32_t clientHeight = 0;
    ReadTestWindowClient(window, &clientWidth, &clientHeight);
    Renderer *renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    Expect(renderer != NULL, "the vsync renderer could not be created");
    Expect(RendererPrepareWorld(renderer), "the vsync world could not be prepared");

    RendererFrameSetup setup;
    BuildClearSetup(&setup);

    // Смена present-mode живёт в swapchain, поэтому каждое переключение
    // обязано пересобрать swapchain в начале следующего кадра.
    for (uint32_t step = 0u; step < 6u; ++step)
    {
        const bool enabled = (step % 2u) == 0u;
        RendererSetVerticalSync(renderer, enabled);
        Expect(RendererIsVerticalSyncEnabled(renderer) == enabled,
               "the vsync flag must follow the last request");

        for (uint32_t frame = 0u; frame < 2u; ++frame)
        {
            DrawFrame(renderer, &setup, "a vsync-toggle frame could not begin",
                      "a vsync-toggle frame could not end");
        }
        CaptureWithSize(renderer, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                        "a vsync-toggle frame could not be captured");
    }

    // Повторная установка того же значения не должна ломать кадры: значение
    // не меняется, swapchain не пересобирается.
    RendererSetVerticalSync(renderer, false);
    RendererSetVerticalSync(renderer, false);
    DrawFrame(renderer, &setup, "the idempotent vsync frame could not begin",
              "the idempotent vsync frame could not end");

    RendererDestroy(renderer);
    DestroyWindow(window);
}

// === 4. Несколько кадровых слотов подряд ===
static void RunFrameSlots(HINSTANCE instance, void *pixels)
{
    HWND window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(window != NULL, "the frame-slot window could not be created");
    int32_t clientWidth = 0;
    int32_t clientHeight = 0;
    ReadTestWindowClient(window, &clientWidth, &clientHeight);
    Renderer *renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    Expect(renderer != NULL, "the frame-slot renderer could not be created");
    Expect(RendererPrepareWorld(renderer), "the frame-slot world could not be prepared");

    RendererFrameSetup setup;
    BuildSceneSetup(&setup);

    // 7 кадров больше двух кадровых слотов: фенсы и семафоры обязаны
    // переиспользоваться по кругу. Между кадрами меняются vsync и меш,
    // чтобы слоты не оказались в одинаковом состоянии.
    RendererMesh *mesh = CreateUnitMesh(renderer);
    for (uint32_t frame = 0u; frame < 7u; ++frame)
    {
        if (frame == 3u)
            RendererSetVerticalSync(renderer, false);
        if (frame == 5u)
        {
            RendererDestroyMesh(renderer, mesh);
            mesh = CreateUnitMesh(renderer);
        }
        DrawMeshFrame(renderer, &setup, mesh);
    }

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    Expect(stats.scenePasses == 1u, "the last frame must report its single scene pass");
    Expect(stats.drawCalls == 1u, "the last frame must report its single draw call");
    CaptureWithSize(renderer, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                    "the frame-slot frame could not be captured");

    RendererDestroyMesh(renderer, mesh);
    RendererDestroy(renderer);
    DestroyWindow(window);
}

// === 5. Брошенный кадр и уничтожение в свёрнутом состоянии ===
// Рендер нельзя требовать завершать: приложение вправе начать кадр и
// решить закрыться. Уничтожение обязано пережить запись команды и
// свёрнутое окно без падения.
static void RunAbandonedFrameDestroy(HINSTANCE instance)
{
    RendererFrameSetup setup;
    BuildSceneSetup(&setup);

    // Кадр начат, но не закончен: меш и проходы уже записаны в команду.
    HWND window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(window != NULL, "the abandoned-frame window could not be created");
    Renderer *renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    Expect(renderer != NULL, "the abandoned-frame renderer could not be created");
    Expect(RendererPrepareWorld(renderer), "the abandoned-frame world could not be prepared");
    static const float origin[3] = {-0.5f, -0.5f, -0.5f};
    RendererMesh *mesh = CreateUnitMesh(renderer);
    Expect(RendererBeginFrame(renderer, &setup), "the abandoned frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    RendererDestroy(renderer);
    DestroyWindow(window);

    // Свёрнутое окно: уничтожение между пропущенными кадрами.
    window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(window != NULL, "the minimised-destroy window could not be created");
    renderer = RendererCreateWithBackend(window, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                         RENDERER_BACKEND_VULKAN);
    Expect(renderer != NULL, "the minimised-destroy renderer could not be created");
    Expect(RendererPrepareWorld(renderer), "the minimised-destroy world could not be prepared");
    RendererResize(renderer, 0, 0);
    Expect(!RendererBeginFrame(renderer, &setup),
           "a minimised renderer must not begin a frame before it is destroyed");
    RendererDestroy(renderer);
    DestroyWindow(window);
}

// === 6. D3D12 и Vulkan рядом: окно у каждого, уничтожение в обоих порядках ===
static void RunBackendSwitch(HINSTANCE instance, void *pixels)
{
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        LaiueTestRuntimeWrite("D3D12 is not compiled in; backend switching is limited to Vulkan\n");
        return;
    }

    RendererFrameSetup setup;
    BuildSceneSetup(&setup);

    // Уничтожаем D3D12 первым, затем наоборот: диспетчер обязан пережить
    // исчезновение любого из живых рендеров, а Vulkan — продолжить кадры.
    for (uint32_t order = 0u; order < 2u; ++order)
    {
        HWND d3d12Window = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
        HWND vulkanWindow = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
        Expect(d3d12Window != NULL && vulkanWindow != NULL,
               "the backend-switch windows could not be created");
        int32_t clientWidth = 0;
        int32_t clientHeight = 0;
        ReadTestWindowClient(vulkanWindow, &clientWidth, &clientHeight);

        Renderer *d3d12 = RendererCreateWithBackend(d3d12Window, TEST_CLIENT_WIDTH,
                                                    TEST_CLIENT_HEIGHT, RENDERER_BACKEND_D3D12);
        if (d3d12 == NULL)
        {
            DestroyWindow(d3d12Window);
            DestroyWindow(vulkanWindow);
            LaiueTestRuntimeWrite("No D3D12 adapter available; backend switching skipped\n");
            return;
        }
        Renderer *vulkan = RendererCreateWithBackend(vulkanWindow, TEST_CLIENT_WIDTH,
                                                     TEST_CLIENT_HEIGHT, RENDERER_BACKEND_VULKAN);
        Expect(vulkan != NULL, "the Vulkan half of the switch could not be created");
        Expect(RendererGetBackend(d3d12) == RENDERER_BACKEND_D3D12,
               "the switch must keep the D3D12 backend identity");
        Expect(RendererGetBackend(vulkan) == RENDERER_BACKEND_VULKAN,
               "the switch must keep the Vulkan backend identity");

        Expect(RendererPrepareWorld(d3d12), "the D3D12 switch world could not be prepared");
        Expect(RendererPrepareWorld(vulkan), "the Vulkan switch world could not be prepared");
        RendererTexture *d3d12Texture = RendererCreateTexture(
            d3d12, 4u, 4u, 1u, LAIUE_GRAPHICS_FORMAT_RGBA8_UNORM);
        RendererTexture *vulkanTexture = RendererCreateTexture(
            vulkan, 4u, 4u, 1u, LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB);
        Expect(d3d12Texture != NULL && vulkanTexture != NULL,
               "both backends must create native texture resources");
        uint8_t texturePixels[4u * 4u * 4u] = {0};
        Expect(RendererUploadTexture(d3d12, d3d12Texture, texturePixels,
                                     sizeof(texturePixels), 4u * 4u) &&
                   RendererUploadTexture(vulkan, vulkanTexture, texturePixels,
                                         sizeof(texturePixels), 4u * 4u),
               "both backends must upload native texture pixels");
        RendererSampler *d3d12Sampler = RendererCreateSampler(
            d3d12, LAIUE_GRAPHICS_FILTER_LINEAR, LAIUE_GRAPHICS_FILTER_NEAREST,
            LAIUE_GRAPHICS_ADDRESS_REPEAT, LAIUE_GRAPHICS_ADDRESS_CLAMP,
            LAIUE_GRAPHICS_ADDRESS_MIRROR);
        RendererSampler *vulkanSampler = RendererCreateSampler(
            vulkan, LAIUE_GRAPHICS_FILTER_NEAREST, LAIUE_GRAPHICS_FILTER_LINEAR,
            LAIUE_GRAPHICS_ADDRESS_REPEAT, LAIUE_GRAPHICS_ADDRESS_CLAMP,
            LAIUE_GRAPHICS_ADDRESS_MIRROR);
        Expect(d3d12Sampler != NULL && vulkanSampler != NULL,
               "both backends must create native sampler resources");
        RendererMesh *d3d12Mesh = CreateUnitMesh(d3d12);
        RendererMesh *vulkanMesh = CreateUnitMesh(vulkan);

        for (uint32_t frame = 0u; frame < 4u; ++frame)
        {
            DrawMeshFrame(d3d12, &setup, d3d12Mesh);
            DrawMeshFrame(vulkan, &setup, vulkanMesh);
        }
        CaptureWithSize(vulkan, pixels, (uint32_t)clientWidth, (uint32_t)clientHeight,
                        "the alternating Vulkan frame could not be captured");

        RendererDestroyMesh(d3d12, d3d12Mesh);
        RendererDestroyMesh(vulkan, vulkanMesh);
        RendererDestroyTexture(d3d12, d3d12Texture);
        RendererDestroyTexture(vulkan, vulkanTexture);
        RendererDestroySampler(d3d12, d3d12Sampler);
        RendererDestroySampler(vulkan, vulkanSampler);
        if (order == 0u)
        {
            // D3D12 умирает первым — Vulkan обязан пережить.
            RendererDestroy(d3d12);
            RendererMesh *survivor = CreateUnitMesh(vulkan);
            DrawMeshFrame(vulkan, &setup, survivor);
            RendererDestroyMesh(vulkan, survivor);
            RendererDestroy(vulkan);
        }
        else
        {
            // Vulkan умирает первым — D3D12 обязан пережить.
            RendererDestroy(vulkan);
            RendererMesh *survivor = CreateUnitMesh(d3d12);
            DrawMeshFrame(d3d12, &setup, survivor);
            RendererDestroyMesh(d3d12, survivor);
            RendererDestroy(d3d12);
        }
        DestroyWindow(d3d12Window);
        DestroyWindow(vulkanWindow);
    }
}
#endif

LAIUE_TEST_ENTRY(RendererVulkanLifecycleTestEntryPoint)
{
    RendererSetContentService(LaiueContentGetStaticServiceV1());
#if !defined(_WIN32)
    LaiueTestRuntimeWrite("The Vulkan lifecycle test needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        LaiueTestRuntimeWrite("No Vulkan backend compiled in; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    HINSTANCE instance = GetModuleHandleW(NULL);
    Expect(EnsureWindowClass(instance), "the hidden window class could not be registered");

    // Разведка драйвера и поверхности до сценариев: если окно не даёт
    // рабочую Vulkan-поверхность, это SKIP, а не серия ложных провалов.
    HWND probeWindow = CreateTestWindow(instance, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    Expect(probeWindow != NULL, "the probe window could not be created");
    Renderer *probe = RendererCreateWithBackend(probeWindow, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                                                RENDERER_BACKEND_VULKAN);
    if (probe == NULL)
    {
        DestroyWindow(probeWindow);
        LaiueTestRuntimeWrite("No windowed Vulkan surface available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    RendererDestroy(probe);
    DestroyWindow(probeWindow);

    void *pixels = PlatformAllocate(MAX_CAPTURE_BYTES, true);
    Expect(pixels != NULL, "the readback buffer could not be allocated");

    RunCreateDestroyStress(instance, pixels);
    RunResizeMinimize(instance, pixels);
    RunVerticalSyncToggle(instance, pixels);
    RunFrameSlots(instance, pixels);
    RunAbandonedFrameDestroy(instance);
    RunBackendSwitch(instance, pixels);

    PlatformFree(pixels);
    UnregisterClassW(kWindowClassName, instance);
    LaiueTestRuntimeWrite("Vulkan window/swapchain lifecycle checks passed\n");
    LAIUE_TEST_SUCCESS();
#endif
}
