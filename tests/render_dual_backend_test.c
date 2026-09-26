// Два живых бэкенда рендера в одном процессе. Это проверка не столько
// отрисовки (её покрывает offscreen-тест), сколько диспетчера: реестр
// «указатель → бэкенд» и его одноэлементный кэш быстрого пути должны
// выдерживать чередование вызовов между D3D12 и Vulkan и уничтожение в
// любом порядке. D3D12 нужен настоящий HWND, поэтому окно создаётся
// голым Win32 и не показывается; Vulkan на этом этапе рисует offscreen.

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

#define TEST_WIDTH 320
#define TEST_HEIGHT 180
#define TEST_PIXEL_BYTES (TEST_WIDTH * TEST_HEIGHT * 4u)
// Драйвера может не быть даже когда бэкенд слинкован: тогда тест
// сообщает о пропуске, а не о ложном провале.
#define SKIP_EXIT_CODE 125
#define INTERLEAVED_FRAMES 8u

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Dual backend check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ZeroBytes(void *destination, size_t count)
{
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t index = 0; index < count; ++index) bytes[index] = 0u;
}

// Единичная матрица: мировые координаты попадают прямо в clip space,
// поэтому положение квада не зависит ни от камеры, ни от проекции.
static void SetIdentity(float matrix[16])
{
    for (uint32_t index = 0; index < 16u; ++index) matrix[index] = 0.0f;
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static void BuildFrameSetup(RendererFrameSetup *setup)
{
    ZeroBytes(setup, sizeof(*setup));
    setup->gamma = 1.0f;
    // Небо чёрное: центральный пиксель кадра будет светлым только за счёт
    // нарисованного меша, и проверка «не чёрный» становится осмысленной.
    setup->skyColor[0] = 0.0f;
    setup->skyColor[1] = 0.0f;
    setup->skyColor[2] = 0.0f;
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
    setup->passes[0].rectMaxX = TEST_WIDTH;
    setup->passes[0].rectMaxY = TEST_HEIGHT;
}

static ChunkQuad MakeQuad(void)
{
    // Одна грань +Z единичного вокселя. С единичной матрицей и смещением
    // -0.5 по X и Y квад накрывает центр кадра.
    return PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
}

static void DrawFrame(Renderer *renderer, const RendererMesh *mesh,
                      const RendererFrameSetup *setup, const char *reason)
{
    const float origin[3] = { -0.5f, -0.5f, -0.5f };
    Expect(RendererBeginFrame(renderer, setup), reason);
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, origin);
    Expect(RendererEndFrame(renderer), "the dual backend frame could not end");
}

#if defined(_WIN32)
static HWND CreateHiddenWindow(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"LaiueDualBackendTestWindow";

    WNDCLASSEXW windowClass;
    ZeroBytes(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return NULL;
    }

    // Окно намеренно не показывается: swapchain создаётся и без показа.
    return CreateWindowExW(0, className, L"laiue dual backend",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           TEST_WIDTH, TEST_HEIGHT, NULL, NULL, instance, NULL);
}
#endif

// Возвращает true, если оба рендера подняты. При отсутствии адаптера или
// драйвера печатает причину и завершает процесс кодом пропуска.
static bool CreateBoth(void *windowHandle, Renderer **outD3D12, Renderer **outVulkan)
{
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12) ||
        !RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        LaiueTestRuntimeWrite("Both render backends are not available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    Renderer *d3d12 = RendererCreateWithBackend(windowHandle, TEST_WIDTH,
                                                TEST_HEIGHT,
                                                RENDERER_BACKEND_D3D12);
    if (d3d12 == NULL)
    {
        LaiueTestRuntimeWrite("No D3D12 adapter available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    Renderer *vulkan = RendererCreateWithBackend(NULL, TEST_WIDTH, TEST_HEIGHT,
                                                 RENDERER_BACKEND_VULKAN);
    if (vulkan == NULL)
    {
        RendererDestroy(d3d12);
        LaiueTestRuntimeWrite("No Vulkan driver available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    *outD3D12 = d3d12;
    *outVulkan = vulkan;
    return true;
}

LAIUE_TEST_ENTRY(RenderDualBackendTestEntryPoint)
{
    RendererSetContentService(LaiueContentGetStaticServiceV1());
#if !defined(_WIN32)
    LaiueTestRuntimeWrite("The dual backend test needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    HWND window = CreateHiddenWindow();
    Expect(window != NULL, "the hidden Win32 window could not be created");

    RendererFrameSetup setup;
    BuildFrameSetup(&setup);
    const ChunkQuad quad = MakeQuad();

    // === Прогон 1: D3D12 умирает первым ===
    Renderer *d3d12 = NULL;
    Renderer *vulkan = NULL;
    CreateBoth(window, &d3d12, &vulkan);
    Expect(RendererGetBackend(d3d12) == RENDERER_BACKEND_D3D12,
           "the D3D12 renderer must report the D3D12 backend");
    Expect(RendererGetBackend(vulkan) == RENDERER_BACKEND_VULKAN,
           "the Vulkan renderer must report the Vulkan backend");
    if (RendererGetBackend(d3d12) == RENDERER_BACKEND_D3D12)
    {
        RendererSetVerticalSync(d3d12, false);
    }

    Expect(RendererPrepareWorld(d3d12), "the D3D12 world could not be prepared");
    Expect(RendererPrepareWorld(vulkan), "the Vulkan world could not be prepared");
    RendererMesh *d3d12Mesh = RendererCreateMesh(d3d12, &quad, 1u);
    RendererMesh *vulkanMesh = RendererCreateMesh(vulkan, &quad, 1u);
    Expect(d3d12Mesh != NULL, "the D3D12 mesh could not be created");
    Expect(vulkanMesh != NULL, "the Vulkan mesh could not be created");

    // Чередование бьёт по одноэлементному кэшу быстрого пути: один из
    // рендеров каждый раз промахивается и уходит в реестр под мьютексом.
    for (uint32_t frame = 0; frame < INTERLEAVED_FRAMES; ++frame)
    {
        DrawFrame(d3d12, d3d12Mesh, &setup, "the D3D12 frame could not begin");
        DrawFrame(vulkan, vulkanMesh, &setup, "the Vulkan frame could not begin");
    }

    RendererStats stats;
    RendererGetStats(d3d12, &stats);
    Expect(stats.geometryPoolCapacityBytes > 0u,
           "the D3D12 geometry pool must report its capacity");
    RendererGetStats(vulkan, &stats);
    Expect(stats.geometryPoolCapacityBytes > 0u,
           "the Vulkan geometry pool must report its capacity");

    uint8_t *pixels = PlatformAllocate(TEST_PIXEL_BYTES, true);
    Expect(pixels != NULL, "the readback buffer could not be allocated");
    uint32_t width = 0u;
    uint32_t height = 0u;
    Expect(RendererCaptureFrame(vulkan, pixels, TEST_PIXEL_BYTES, &width, &height),
           "the Vulkan frame could not be captured");
    Expect(width == TEST_WIDTH && height == TEST_HEIGHT,
           "the captured Vulkan frame has the wrong size");
    const uint8_t *centre =
        pixels + ((size_t)(TEST_HEIGHT / 2u) * TEST_WIDTH + TEST_WIDTH / 2u) * 4u;
    Expect((uint32_t)centre[0] + (uint32_t)centre[1] + (uint32_t)centre[2] > 30u,
           "the centre of the Vulkan frame must show the drawn mesh, not black sky");

    // Уничтожаем D3D12 первым: Vulkan обязан продолжить рисовать кадры,
    // а быстрый путь — пережить исчезновение закэшированного указателя.
    RendererDestroyMesh(d3d12, d3d12Mesh);
    RendererDestroy(d3d12);
    DrawFrame(vulkan, vulkanMesh, &setup, "the surviving Vulkan frame could not begin");
    RendererDestroyMesh(vulkan, vulkanMesh);
    RendererDestroy(vulkan);

    // === Прогон 2: Vulkan умирает первым ===
    CreateBoth(window, &d3d12, &vulkan);
    RendererSetVerticalSync(d3d12, false);
    Expect(RendererPrepareWorld(d3d12), "the D3D12 world could not be prepared again");
    Expect(RendererPrepareWorld(vulkan), "the Vulkan world could not be prepared again");
    d3d12Mesh = RendererCreateMesh(d3d12, &quad, 1u);
    vulkanMesh = RendererCreateMesh(vulkan, &quad, 1u);
    Expect(d3d12Mesh != NULL, "the second D3D12 mesh could not be created");
    Expect(vulkanMesh != NULL, "the second Vulkan mesh could not be created");

    RendererDestroyMesh(vulkan, vulkanMesh);
    RendererDestroy(vulkan);
    // D3D12 — единственный живой рендер: его кадр обязан пройти и после
    // того, как диспетчер потерял Vulkan-сосед по реестру.
    DrawFrame(d3d12, d3d12Mesh, &setup, "the surviving D3D12 frame could not begin");
    RendererGetStats(d3d12, &stats);
    Expect(stats.geometryPoolCapacityBytes > 0u,
           "the surviving D3D12 geometry pool must report its capacity");
    RendererDestroyMesh(d3d12, d3d12Mesh);
    RendererDestroy(d3d12);

    // === AUTO на Windows разрешается в D3D12 ===
    Renderer *automatic = RendererCreateWithBackend(window, TEST_WIDTH, TEST_HEIGHT,
                                                    RENDERER_BACKEND_AUTO);
    Expect(automatic != NULL, "the AUTO renderer could not be created");
    Expect(RendererGetBackend(automatic) == RENDERER_BACKEND_D3D12,
           "AUTO must resolve to the D3D12 default on Windows");
    RendererDestroy(automatic);

    Expect(RendererGetBackend(NULL) == RENDERER_BACKEND_AUTO, "a null renderer must report AUTO");
    RendererDestroy(NULL);

    // More than the old global registry's eight slots. Each live object must
    // retain its backend even when other renderers are destroyed out of order.
    Renderer *many[9] = {0};
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        many[index] = RendererCreateWithBackend(NULL, 16, 16, RENDERER_BACKEND_VULKAN);
        Expect(many[index] != NULL, "the independent offscreen renderer could not be created");
        Expect(RendererGetBackend(many[index]) == RENDERER_BACKEND_VULKAN,
               "all nine renderers must retain their backend");
    }
    for (uint32_t parity = 0u; parity < 2u; ++parity)
    {
        for (uint32_t index = parity; index < 9u; index += 2u)
        {
            Expect(RendererGetBackend(many[index]) == RENDERER_BACKEND_VULKAN,
                   "destroying another renderer must not change the backend");
            RendererDestroy(many[index]);
        }
    }

    PlatformFree(pixels);
    DestroyWindow(window);
    LaiueTestRuntimeWrite("Dual backend checks passed\n");
    LAIUE_TEST_SUCCESS();
#endif
}
