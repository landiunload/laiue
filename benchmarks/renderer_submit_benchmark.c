// Ручной CPU-бенчмарк D3D12-пути отправки кадра: постановка мешей,
// инстансные вызовы и пул геометрии. GPU-времена намеренно не измеряются —
// берётся только время записи команд на CPU, чтобы правки в затронутых
// участках renderer_d3d12.c сравнивались без шума драйвера.
//
// Стенд не входит ни в ALL, ни в CTest: включается LAIUE_BUILD_BENCHMARKS
// и запускается осознанно. Вывод машинно-читаемый:
//   stage=<name> sample=<i> us=<целые микросекунды>
// затем строка verify=<ok|fail> и LAIUE_TEST_SUCCESS.
//
// Без адаптера D3D12 стенд пишет SKIP и завершается кодом 125.

#include "platform/system.h"
#include "render/chunk_geometry.h"
#include "render/renderer.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define SAMPLE_COUNT 9u
#define DRAWS_PER_FRAME 20000u
#define MESH_BATCH 64u
#define INSTANCE_BATCH 16u
#define INSTANCE_CAPACITY 64u
#define SKIP_EXIT_CODE 125

static volatile uint64_t benchmarkSink;
static RendererFrameSetup sinkSetup;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0; index < length; ++index) text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

static void ZeroBytes(void *destination, size_t count)
{
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t index = 0; index < count; ++index) bytes[index] = 0u;
}

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
    setup->sunDirection[2] = -1.0f;
    setup->sunColor[0] = 1.0f;
    setup->sunColor[1] = 1.0f;
    setup->sunColor[2] = 1.0f;
    setup->ambientColor[0] = 1.0f;
    setup->ambientColor[1] = 1.0f;
    setup->ambientColor[2] = 1.0f;
    setup->passCount = 1u;
    SetIdentity(setup->passes[0].viewProjection);
    setup->passes[0].rectMaxX = 320u;
    setup->passes[0].rectMaxY = 180u;
}

static ChunkQuad MakeQuad(void)
{
    return PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
}

static uint64_t NowMicroseconds(void)
{
    double seconds = PlatformMonotonicSeconds();
    return (uint64_t)(seconds * 1000000.0 + 0.5);
}

static void ReportSample(const char *stage, uint32_t sample, uint64_t microseconds)
{
    WriteText("stage=");
    WriteText(stage);
    WriteText(" sample=");
    WriteUnsigned(sample);
    WriteText(" us=");
    WriteUnsigned(microseconds);
    WriteText("\n");
}

#if defined(_WIN32)
static HWND CreateHiddenWindow(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"LaiueRendererSubmitBenchWindow";

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

    return CreateWindowExW(0, className, L"laiue renderer submit bench", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 320, 180, NULL, NULL, instance, NULL);
}
#endif

// Один кадр с переданным числом инстансных вызовов; время берётся только
// вокруг цикла отрисовки, а не вокруг Present.
static bool RunInstanceDrawFrame(Renderer *renderer, const RendererMesh *mesh,
                                 const RendererMeshInstance *instances, uint32_t instanceCount,
                                 uint32_t draws, uint64_t *outMicroseconds)
{
    if (!RendererBeginFrame(renderer, &sinkSetup))
    {
        return false;
    }
    RendererBeginScenePass(renderer, 0u);
    uint64_t start = NowMicroseconds();
    for (uint32_t draw = 0; draw < draws; ++draw)
    {
        RendererDrawMeshInstances(renderer, mesh, instances, instanceCount);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    *outMicroseconds = elapsed;
    if (!RendererEndFrame(renderer))
    {
        return false;
    }
    return true;
}

static bool RunPlainDrawFrame(Renderer *renderer, const RendererMesh *mesh, uint32_t draws,
                              uint64_t *outMicroseconds)
{
    if (!RendererBeginFrame(renderer, &sinkSetup))
    {
        return false;
    }
    RendererBeginScenePass(renderer, 0u);
    uint64_t start = NowMicroseconds();
    for (uint32_t draw = 0; draw < draws; ++draw)
    {
        const float origin[3] = {(float)draw * 0.001f, 0.0f, 0.0f};
        RendererDrawMesh(renderer, mesh, origin);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    *outMicroseconds = elapsed;
    if (!RendererEndFrame(renderer))
    {
        return false;
    }
    return true;
}

static bool RunCreateBatch(Renderer *renderer, RendererMesh **meshes, uint32_t count,
                           uint64_t *outMicroseconds)
{
    const ChunkQuad quad = MakeQuad();
    uint64_t start = NowMicroseconds();
    for (uint32_t index = 0; index < count; ++index)
    {
        meshes[index] = RendererCreateMesh(renderer, &quad, 1u);
    }
    *outMicroseconds = NowMicroseconds() - start;

    for (uint32_t index = 0; index < count; ++index)
    {
        if (meshes[index] == NULL) return false;
        benchmarkSink += meshes[index] != NULL ? 1u : 0u;
    }
    return true;
}

static bool RunDestroyBatch(Renderer *renderer, RendererMesh **meshes, uint32_t count,
                            uint64_t *outMicroseconds)
{
    uint64_t start = NowMicroseconds();
    for (uint32_t index = 0; index < count; ++index)
    {
        RendererDestroyMesh(renderer, meshes[index]);
    }
    *outMicroseconds = NowMicroseconds() - start;
    return true;
}

static bool IdleFrame(Renderer *renderer)
{
    return RendererBeginFrame(renderer, &sinkSetup) && RendererEndFrame(renderer);
}

LAIUE_TEST_ENTRY(RendererSubmitBenchmarkEntryPoint)
{
#if !defined(_WIN32)
    WriteText("The renderer submit benchmark needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        WriteText("D3D12 is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    HWND window = CreateHiddenWindow();
    if (window == NULL)
    {
        WriteText("window creation failed\n");
        LaiueTestRuntimeExit(1);
    }

    Renderer *renderer =
        RendererCreateWithBackend(window, 320, 180, RENDERER_BACKEND_D3D12);
    if (renderer == NULL)
    {
        WriteText("No D3D12 adapter available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    RendererSetVerticalSync(renderer, false);
    if (!RendererPrepareWorld(renderer))
    {
        WriteText("world prepare failed\n");
        LaiueTestRuntimeExit(1);
    }

    const ChunkQuad quad = MakeQuad();
    RendererMesh *mesh = RendererCreateMesh(renderer, &quad, 1u);
    if (mesh == NULL)
    {
        WriteText("mesh creation failed\n");
        LaiueTestRuntimeExit(1);
    }
    BuildFrameSetup(&sinkSetup);
    if (!IdleFrame(renderer)) { WriteText("warmup frame failed\n"); LaiueTestRuntimeExit(1); }

    RendererMeshInstance *instances = (RendererMeshInstance *)PlatformAllocate(
        (size_t)INSTANCE_CAPACITY * sizeof(RendererMeshInstance), true);
    if (instances == NULL)
    {
        WriteText("instance allocation failed\n");
        LaiueTestRuntimeExit(1);
    }
    for (uint32_t index = 0; index < INSTANCE_CAPACITY; ++index)
    {
        instances[index].originRelative[0] = (float)(index % 8u) * 0.01f;
        instances[index].originRelative[1] = (float)(index / 8u) * 0.01f;
        instances[index].scale = 0.001f;
    }

    RendererMesh *meshes[MESH_BATCH];
    for (uint32_t index = 0; index < MESH_BATCH; ++index) meshes[index] = NULL;

    // Прогрев всех стадий: страницы, первые ресурсы, кэши команд.
    {
        uint64_t ignored = 0u;
        if (!RunPlainDrawFrame(renderer, mesh, 256u, &ignored) ||
            !RunInstanceDrawFrame(renderer, mesh, instances, 1u, 256u, &ignored) ||
            !RunInstanceDrawFrame(renderer, mesh, instances, INSTANCE_BATCH, 256u, &ignored) ||
            !RunCreateBatch(renderer, meshes, MESH_BATCH, &ignored) ||
            !IdleFrame(renderer) ||
            !RunDestroyBatch(renderer, meshes, MESH_BATCH, &ignored) ||
            !IdleFrame(renderer) || !IdleFrame(renderer))
        {
            WriteText("warmup failed\n");
            LaiueTestRuntimeExit(1);
        }
    }

    bool ok = true;
    // 1. Обычный вызов на меш: доминирует запись команд.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunPlainDrawFrame(renderer, mesh, DRAWS_PER_FRAME, &elapsed);
        if (ok) ReportSample("draw_mesh", sample, elapsed);
    }

    // 2. Инстансный вызов с одним инстансом: чистый per-draw overhead.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunInstanceDrawFrame(renderer, mesh, instances, 1u, DRAWS_PER_FRAME, &elapsed);
        if (ok) ReportSample("draw_inst1", sample, elapsed);
    }

    // 3. Инстансный вызов с 16 инстансами: запись данных плюс per-draw overhead.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunInstanceDrawFrame(renderer, mesh, instances, INSTANCE_BATCH, DRAWS_PER_FRAME,
                                  &elapsed);
        if (ok) ReportSample("draw_inst16", sample, elapsed);
    }

    // 4. Создание/удаление мешей через пул геометрии.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunCreateBatch(renderer, meshes, MESH_BATCH, &elapsed);
        if (ok) ReportSample("create_mesh64", sample, elapsed);
        if (ok) ok = IdleFrame(renderer);
        if (ok)
        {
            ok = RunDestroyBatch(renderer, meshes, MESH_BATCH, &elapsed);
            if (ok) ReportSample("destroy_mesh64", sample, elapsed);
        }
        // Даём фенсам пройти, чтобы отложенные диапазоны вернулись в пул.
        if (ok) ok = IdleFrame(renderer);
        if (ok) ok = IdleFrame(renderer);
    }

    // 5. RendererGetStats: O(1) на слитом пуле.
    if (ok)
    {
        RendererStats stats;
        uint64_t start = NowMicroseconds();
        for (uint32_t index = 0; index < 1000000u; ++index)
        {
            RendererGetStats(renderer, &stats);
            benchmarkSink += stats.geometryPoolUsedBytes;
        }
        ReportSample("get_stats", 0u, NowMicroseconds() - start);
    }

    WriteText("verify=");
    WriteText(ok ? "ok" : "fail");
    WriteText("\n");

    RendererDestroyMesh(renderer, mesh);
    RendererDestroy(renderer);
    PlatformFree(instances);
    DestroyWindow(window);

    if (benchmarkSink == UINT64_MAX) WriteText("");
    if (!ok) LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
#endif
}
