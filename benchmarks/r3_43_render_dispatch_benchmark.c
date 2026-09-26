// ROUND 3, задача 43-render-dispatch: CPU-цена диспетчера рендера и реестра
// живых Renderer-объектов, отделённая от бэкенда.
//
// Замеряются три вещи:
//   1. Чистый dispatch без работы бэкенда — циклы RendererGetBackend;
//   2. Реестр при нескольких живых рендерах — промах одноэлементного
//      кэша быстрого пути уходит под мьютекс и линейный скан;
//   3. Обычный покадровый путь (draw_mesh/draw_inst1) и create/destroy
//      мешей, где диспетчер стоит на каждом вызове рядом с бэкендом.
//
// Стенд EXCLUDE_FROM_ALL, в CTest не регистрируется. Запускается руками под
// bench-lock. Печатает машинно-читаемые строки
//   stage=<name> sample=<i> us=<микросекунды> ops=<единицы работы>
// затем итоговые строки count/checksum/verify и LAIUE_TEST_SUCCESS.
//
// GPU-времена не измеряются: берётся только CPU-время постановки на CPU,
// поэтому Present/фенсы остаются за границей замера. Без адаптера D3D12 стенд
// печатает SKIP и завершается кодом 125.

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
// Столько итераций в dispatch-only циклах. Работа бэкенда не выполняется,
// поэтому это чистая цена вызова + LookupBackend.
#define DISPATCH_ITERS 2000000u
#define DRAWS_PER_FRAME 20000u
#define MESH_BATCH 64u
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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0; index < 16u; ++index)
    {
        text[index] = digits[(value >> ((15u - index) * 4u)) & 0xFu];
    }
    text[16] = '\0';
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

static void ReportSample(const char *stage, uint32_t sample, uint64_t microseconds,
                         uint64_t ops)
{
    WriteText("stage=");
    WriteText(stage);
    WriteText(" sample=");
    WriteUnsigned(sample);
    WriteText(" us=");
    WriteUnsigned(microseconds);
    WriteText(" ops=");
    WriteUnsigned(ops);
    WriteText("\n");
}

// === Dispatch-only циклы ===
static uint64_t RunGetBackendLoop(const Renderer *renderer, uint32_t iterations)
{
    benchmarkSink = 0u;
    uint64_t start = NowMicroseconds();
    uint64_t accumulator = 0u;
    for (uint32_t index = 0; index < iterations; ++index)
    {
        accumulator += (uint64_t)RendererGetBackend(renderer);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    benchmarkSink += accumulator;
    return elapsed;
}

static uint64_t RunGetBackendAlternating(const Renderer *first, const Renderer *second,
                                         uint32_t pairs)
{
    benchmarkSink = 0u;
    uint64_t start = NowMicroseconds();
    uint64_t accumulator = 0u;
    for (uint32_t index = 0; index < pairs; ++index)
    {
        accumulator += (uint64_t)RendererGetBackend(first);
        accumulator += (uint64_t)RendererGetBackend(second);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    benchmarkSink += accumulator;
    return elapsed;
}

// === Покадровые стадии ===
static bool RunPlainDrawFrame(Renderer *renderer, const RendererMesh *mesh, uint32_t draws,
                              uint64_t *outMicroseconds)
{
    if (!RendererBeginFrame(renderer, &sinkSetup)) return false;
    RendererBeginScenePass(renderer, 0u);
    uint64_t start = NowMicroseconds();
    for (uint32_t draw = 0; draw < draws; ++draw)
    {
        const float origin[3] = {(float)draw * 0.001f, 0.0f, 0.0f};
        RendererDrawMesh(renderer, mesh, origin);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    *outMicroseconds = elapsed;
    if (!RendererEndFrame(renderer)) return false;
    return true;
}

static bool RunInstanceDrawFrame(Renderer *renderer, const RendererMesh *mesh,
                                 const RendererMeshInstance *instances, uint32_t instanceCount,
                                 uint32_t draws, uint64_t *outMicroseconds)
{
    if (!RendererBeginFrame(renderer, &sinkSetup)) return false;
    RendererBeginScenePass(renderer, 0u);
    uint64_t start = NowMicroseconds();
    for (uint32_t draw = 0; draw < draws; ++draw)
    {
        RendererDrawMeshInstances(renderer, mesh, instances, instanceCount);
    }
    uint64_t elapsed = NowMicroseconds() - start;
    *outMicroseconds = elapsed;
    if (!RendererEndFrame(renderer)) return false;
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
        benchmarkSink += 1u;
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

#if defined(_WIN32)
typedef struct R3ProcessMemoryCounters
{
    uint32_t cb;
    uint32_t pageFaultCount;
    uint64_t peakWorkingSetSize;
    uint64_t workingSetSize;
    uint64_t quotaPeakPagedPoolUsage;
    uint64_t quotaPagedPoolUsage;
    uint64_t quotaPeakNonPagedPoolUsage;
    uint64_t quotaNonPagedPoolUsage;
    uint64_t pagefileUsage;
    uint64_t peakPagefileUsage;
} R3ProcessMemoryCounters;

__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall GetProcessMemoryInfo(void *process,
                                                       R3ProcessMemoryCounters *counters,
                                                       uint32_t size);

static void ReportPeakMemory(void)
{
    R3ProcessMemoryCounters counters;
    ZeroBytes(&counters, sizeof(counters));
    counters.cb = (uint32_t)sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, (uint32_t)sizeof(counters)))
    {
        WriteText("peak_commit_bytes=");
        WriteUnsigned(counters.peakPagefileUsage);
        WriteText(" peak_working_set_bytes=");
        WriteUnsigned(counters.peakWorkingSetSize);
        WriteText("\n");
    }
    else
    {
        WriteText("peak_commit_bytes=unknown peak_working_set_bytes=unknown\n");
    }
}

static HWND CreateHiddenWindow(const wchar_t *className, const wchar_t *title)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
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
    return CreateWindowExW(0, className, title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 320, 180, NULL, NULL, instance, NULL);
}
#endif

LAIUE_TEST_ENTRY(R3RenderDispatchBenchmarkEntryPoint)
{
#if !defined(_WIN32)
    WriteText("The render dispatch benchmark needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        WriteText("D3D12 is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    HWND windowA = CreateHiddenWindow(L"LaiueR3DispatchBenchA", L"laiue r3 dispatch bench A");
    if (windowA == NULL)
    {
        WriteText("window A creation failed\n");
        LaiueTestRuntimeExit(1);
    }

    Renderer *rendererA =
        RendererCreateWithBackend(windowA, 320, 180, RENDERER_BACKEND_D3D12);
    if (rendererA == NULL)
    {
        WriteText("No D3D12 adapter available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    RendererSetVerticalSync(rendererA, false);
    if (!RendererPrepareWorld(rendererA))
    {
        WriteText("world prepare failed\n");
        LaiueTestRuntimeExit(1);
    }

    // Второй живой рендер — это и есть неблагоприятная нагрузка для реестра:
    // одноэлементный кэш быстрого пути держит только первый созданный
    // рендер, поэтому любой вызов на втором уходит под мьютекс диспетчера.
    HWND windowB = CreateHiddenWindow(L"LaiueR3DispatchBenchB", L"laiue r3 dispatch bench B");
    Renderer *rendererB = NULL;
    if (windowB != NULL)
    {
        rendererB = RendererCreateWithBackend(windowB, 320, 180, RENDERER_BACKEND_D3D12);
        if (rendererB != NULL)
        {
            RendererSetVerticalSync(rendererB, false);
            if (!RendererPrepareWorld(rendererB))
            {
                RendererDestroy(rendererB);
                rendererB = NULL;
            }
        }
    }
    WriteText(rendererB != NULL ? "secondary=ok\n" : "secondary=unavailable\n");

    const ChunkQuad quad = MakeQuad();
    RendererMesh *mesh = RendererCreateMesh(rendererA, &quad, 1u);
    if (mesh == NULL)
    {
        WriteText("mesh creation failed\n");
        LaiueTestRuntimeExit(1);
    }
    RendererMesh *meshB = rendererB != NULL ? RendererCreateMesh(rendererB, &quad, 1u) : NULL;
    if (rendererB != NULL && meshB == NULL) LaiueTestRuntimeExit(1);
    BuildFrameSetup(&sinkSetup);
    if (!IdleFrame(rendererA)) { WriteText("warmup frame failed\n"); LaiueTestRuntimeExit(1); }
    if (rendererB != NULL && !IdleFrame(rendererB))
    {
        WriteText("secondary warmup frame failed\n");
        LaiueTestRuntimeExit(1);
    }

    RendererMeshInstance instance;
    ZeroBytes(&instance, sizeof(instance));
    instance.scale = 0.001f;

    RendererMesh *meshes[MESH_BATCH];
    for (uint32_t index = 0; index < MESH_BATCH; ++index) meshes[index] = NULL;

    bool ok = true;

    // Прогрев dispatch-only циклов вне выборок.
    (void)RunGetBackendLoop(rendererA, DISPATCH_ITERS / 4u);
    (void)RunGetBackendLoop(NULL, DISPATCH_ITERS / 4u);
    if (rendererB != NULL)
    {
        (void)RunGetBackendLoop(rendererB, DISPATCH_ITERS / 4u);
        (void)RunGetBackendAlternating(rendererA, rendererB, DISPATCH_ITERS / 8u);
    }

    // 0. Контроль цены вызова: NULL завершается до любого кэша.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = RunGetBackendLoop(NULL, DISPATCH_ITERS);
        ReportSample("get_backend_null", sample, elapsed, DISPATCH_ITERS);
    }

    // 1. Typical: одиночный живой рендер, попадание в быстрый путь.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = RunGetBackendLoop(rendererA, DISPATCH_ITERS);
        ReportSample("get_backend", sample, elapsed, DISPATCH_ITERS);
    }

    // 2. Adverse: повторные вызовы на "втором" рендере — промах быстрого кэша.
    if (rendererB != NULL)
    {
        for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
        {
            uint64_t elapsed = RunGetBackendLoop(rendererB, DISPATCH_ITERS);
            ReportSample("get_backend_second", sample, elapsed, DISPATCH_ITERS);
        }

        // 3. Adverse: чередование двух живых рендеров.
        for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
        {
            uint64_t elapsed = RunGetBackendAlternating(rendererA, rendererB, DISPATCH_ITERS);
            ReportSample("get_backend_alt", sample, elapsed, DISPATCH_ITERS * 2u);
        }
    }

    // 4. Typical: обычный вызов на меш (диспетчер + бэкенд).
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunPlainDrawFrame(rendererA, mesh, DRAWS_PER_FRAME, &elapsed);
        if (ok) ReportSample("draw_mesh", sample, elapsed, DRAWS_PER_FRAME);
    }

    // 5. Typical: инстансный вызов с одним инстансом — минимальная работа
    //    бэкенда, поэтому доля диспетчера максимальна.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunInstanceDrawFrame(rendererA, mesh, &instance, 1u, DRAWS_PER_FRAME, &elapsed);
        if (ok) ReportSample("draw_inst1", sample, elapsed, DRAWS_PER_FRAME);
    }

    // 6. Typical: создание/удаление мешей.
    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunCreateBatch(rendererA, meshes, MESH_BATCH, &elapsed);
        if (ok) ReportSample("create_mesh", sample, elapsed, MESH_BATCH);
        if (ok) ok = IdleFrame(rendererA);
        if (ok)
        {
            ok = RunDestroyBatch(rendererA, meshes, MESH_BATCH, &elapsed);
            if (ok) ReportSample("destroy_mesh", sample, elapsed, MESH_BATCH);
        }
        if (ok) ok = IdleFrame(rendererA);
        if (ok) ok = IdleFrame(rendererA);
    }

    // 7. Проверка работы: статистика последнего показанного кадра совпадает с
    //    числом постановок. Отдельный кадр, чтобы не влиять на замеры.
    bool drawCheck = true;
    {
        uint64_t ignored = 0u;
        if (!RunPlainDrawFrame(rendererA, mesh, 128u, &ignored)) drawCheck = false;
        RendererStats stats;
        RendererGetStats(rendererA, &stats);
        if (stats.drawCalls != 128u) drawCheck = false;
        benchmarkSink += stats.drawnQuads;
    }
    if (rendererB != NULL)
    {
        // Статистика второго рендера тоже обязана быть независимой.
        uint64_t ignored = 0u;
        if (!RunPlainDrawFrame(rendererB, meshB, 64u, &ignored)) drawCheck = false;
        RendererStats stats;
        RendererGetStats(rendererB, &stats);
        if (stats.drawCalls != 64u) drawCheck = false;
        benchmarkSink += stats.drawnQuads;
    }

    WriteText("draw_check=");
    WriteText(drawCheck ? "ok" : "fail");
    WriteText("\n");
    WriteText("checksum=");
    WriteHex(benchmarkSink);
    WriteText("\n");
    WriteText("verify=");
    WriteText((ok && drawCheck) ? "ok" : "fail");
    WriteText("\n");

    ReportPeakMemory();

    RendererDestroyMesh(rendererA, mesh);
    RendererDestroy(rendererA);
    if (rendererB != NULL)
    {
        RendererDestroyMesh(rendererB, meshB);
        RendererDestroy(rendererB);
    }
    if (windowA != NULL) DestroyWindow(windowA);
    if (windowB != NULL) DestroyWindow(windowB);

    if (!ok || !drawCheck) LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
#endif
}
