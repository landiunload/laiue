// ROUND 2 harness агента 21-vulkan: CPU-времена постановки работы в
// Vulkan-бэкенде рендерера (offscreen, без окна и swapchain). Измеряется
// только время записи команд на CPU; ожидание фенсов и submit остаются
// внутри кадра, но их вклад виден отдельной стадией idle_frame.
//
// Измеряемые прогоны нарочно длинные (десятки миллисекунд на sample), иначе
// доли микросекунды тонут в шуме планировщика. Для draw-стадий время
// копится только по внутренним циклам отрисовки, без BeginFrame/EndFrame.
//
// Стенд не входит ни в ALL, ни в CTest: включается LAIUE_BUILD_BENCHMARKS
// и запускается осознанно. Вывод машинно-читаемый:
//   stage=<name> sample=<i> us=<целые микросекунды>
// затем totals=... checksum=<hex> verify=<ok|fail> и LAIUE_TEST_SUCCESS.
//
// Без Vulkan-бэкенда или драйвера стенд пишет SKIP и завершается кодом 125.
//
// Стенд обязан быть одинаковым для baseline и candidate: тот же вход, тот же
// порядок операций, те же счётчики и та же контрольная сумма пикселей.

#include "platform/system.h"
#include "render/chunk_geometry.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_COUNT 5u
#define SKIP_EXIT_CODE 125

#define BENCH_WIDTH 320
#define BENCH_HEIGHT 180
#define PIXEL_BYTES ((uint32_t)BENCH_WIDTH * (uint32_t)BENCH_HEIGHT * 4u)

// Обычный draw: доминирует запись команд (константное кольцо + bind + draw).
#define DRAW_COUNT 8192u
#define DRAW_FRAMES 300u
// Инстансный draw: тот же per-draw overhead плюс запись данных инстансов.
#define INST_FRAMES 400u
#define INST_DRAW_COUNT 4096u
#define INST_BATCH 32u
#define INST32_FRAMES 200u
// Пустой кадр: базовая цена reset/begin/barrier/submit/fence.
#define IDLE_FRAMES 2000u
// Пачка отложенных загрузок: MAX_PENDING_UPLOADS=64, больше production
// между кадрами не примет.
#define MESH_BATCH 64u
#define UPLOAD_FRAMES 1000u
#define CREATE_ROUNDS 1000u

static volatile uint64_t benchmarkSink;
static RendererFrameSetup sinkSetup;

static const float kOrigin[3] = { 0.0f, 0.0f, 0.0f };

// Накопленная работа за прогон: baseline и candidate обязаны получить
// одинаковые числа, иначе speedup недоказан.
static uint64_t benchDrawCalls;
static uint64_t benchUploadedBytes;
static uint64_t benchDrawnQuads;

static void AccumulateStats(const Renderer *renderer)
{
    RendererStats stats;
    RendererGetStats(renderer, &stats);
    benchDrawCalls += stats.drawCalls;
    benchUploadedBytes += stats.uploadedBytes;
    benchDrawnQuads += stats.drawnQuads;
}

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

static void WriteHex64(uint64_t value)
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

// Один проход сцены во всю цель, единичная матрица: положение квада не
// зависит ни от камеры, ни от проекции, и кадр детерминирован.
static void BuildFrameSetup(RendererFrameSetup *setup)
{
    ZeroBytes(setup, sizeof(*setup));
    setup->gamma = 1.0f;
    setup->skyColor[0] = 0.0f;
    setup->skyColor[1] = 0.0f;
    setup->skyColor[2] = 1.0f;
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
    setup->passes[0].rectMinX = 0u;
    setup->passes[0].rectMinY = 0u;
    setup->passes[0].rectMaxX = (uint32_t)BENCH_WIDTH;
    setup->passes[0].rectMaxY = (uint32_t)BENCH_HEIGHT;
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

static bool IdleFrame(Renderer *renderer)
{
    bool ok = RendererBeginFrame(renderer, &sinkSetup) && RendererEndFrame(renderer);
    if (ok) AccumulateStats(renderer);
    return ok;
}

// N кадров по draws обычных draw-вызовов; время копится только по внутренним
// циклам, поэтому BeginFrame/EndFrame из замера исключены.
static bool RunPlainDrawSample(Renderer *renderer, const RendererMesh *mesh, uint32_t frames,
                               uint32_t draws, uint64_t *outMicroseconds)
{
    uint64_t total = 0u;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        if (!RendererBeginFrame(renderer, &sinkSetup)) return false;
        RendererBeginScenePass(renderer, 0u);
        uint64_t start = NowMicroseconds();
        for (uint32_t draw = 0; draw < draws; ++draw)
        {
            const float origin[3] = { (float)(draw & 0x3FFu) * 0.001f, 0.0f, 0.0f };
            RendererDrawMesh(renderer, mesh, origin);
        }
        total += NowMicroseconds() - start;
        if (!RendererEndFrame(renderer)) return false;
        AccumulateStats(renderer);
    }
    *outMicroseconds = total;
    return true;
}

static bool RunInstanceDrawSample(Renderer *renderer, const RendererMesh *mesh,
                                  const RendererMeshInstance *instances, uint32_t instanceCount,
                                  uint32_t frames, uint32_t draws, uint64_t *outMicroseconds)
{
    uint64_t total = 0u;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        if (!RendererBeginFrame(renderer, &sinkSetup)) return false;
        RendererBeginScenePass(renderer, 0u);
        uint64_t start = NowMicroseconds();
        for (uint32_t draw = 0; draw < draws; ++draw)
        {
            RendererDrawMeshInstances(renderer, mesh, instances, instanceCount);
        }
        total += NowMicroseconds() - start;
        if (!RendererEndFrame(renderer)) return false;
        AccumulateStats(renderer);
    }
    *outMicroseconds = total;
    return true;
}

static bool RunIdleSample(Renderer *renderer, uint32_t frames, uint64_t *outMicroseconds)
{
    uint64_t start = NowMicroseconds();
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        if (!IdleFrame(renderer)) return false;
    }
    *outMicroseconds = NowMicroseconds() - start;
    return true;
}

// Создание пачки мешей: время берётся вокруг RendererCreateMesh — сюда
// входят пул геометрии, выбор кольца загрузки и memcpy данных.
static bool RunCreateBatch(Renderer *renderer, RendererMesh **meshes, uint32_t count)
{
    const ChunkQuad quad = MakeQuad();
    for (uint32_t index = 0; index < count; ++index)
    {
        meshes[index] = RendererCreateMesh(renderer, &quad, 1u);
        if (meshes[index] == NULL) return false;
        benchmarkSink += 1u;
    }
    return true;
}

static void RunDestroyBatch(Renderer *renderer, RendererMesh **meshes, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        RendererDestroyMesh(renderer, meshes[index]);
        meshes[index] = NULL;
    }
}

// Кадр, в начале которого записываются накопленные загрузки мешей. Время
// включает BeginFrame (там живёт RecordPendingUploads), пустой проход и
// EndFrame.
static bool RunUploadSample(Renderer *renderer, RendererMesh **meshes, uint32_t frames,
                            uint64_t *outMicroseconds)
{
    uint64_t total = 0u;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        if (!RunCreateBatch(renderer, meshes, MESH_BATCH)) return false;
        uint64_t start = NowMicroseconds();
        bool ok = RendererBeginFrame(renderer, &sinkSetup);
        if (ok) RendererBeginScenePass(renderer, 0u);
        if (ok) ok = RendererEndFrame(renderer);
        total += NowMicroseconds() - start;
        if (ok) AccumulateStats(renderer);
        RunDestroyBatch(renderer, meshes, MESH_BATCH);
        if (!ok) return false;
        if (!IdleFrame(renderer)) return false;
        if (!IdleFrame(renderer)) return false;
    }
    *outMicroseconds = total;
    return true;
}

// Цена только RendererCreateMesh, без кадра: за кадром остаются загрузка и
// возврат диапазонов.
static bool RunCreateSample(Renderer *renderer, RendererMesh **meshes, uint32_t rounds,
                            uint64_t *outMicroseconds)
{
    const ChunkQuad quad = MakeQuad();
    uint64_t total = 0u;
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        uint64_t start = NowMicroseconds();
        for (uint32_t index = 0; index < MESH_BATCH; ++index)
        {
            meshes[index] = RendererCreateMesh(renderer, &quad, 1u);
            if (meshes[index] == NULL) return false;
            benchmarkSink += 1u;
        }
        total += NowMicroseconds() - start;
        if (!IdleFrame(renderer)) return false;
        RunDestroyBatch(renderer, meshes, MESH_BATCH);
        if (!IdleFrame(renderer)) return false;
        if (!IdleFrame(renderer)) return false;
    }
    *outMicroseconds = total;
    return true;
}

static uint64_t ChecksumPixels(const uint8_t *pixels, uint32_t bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t index = 0; index < bytes; ++index)
    {
        hash ^= pixels[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Детерминированная сцена для контрольной суммы: один квад в известной
// точке. Baseline и candidate обязаны получить одинаковые пиксели.
static bool CaptureChecksum(Renderer *renderer, RendererMesh *mesh, uint8_t *pixels,
                            uint64_t *outChecksum)
{
    if (!RendererBeginFrame(renderer, &sinkSetup)) return false;
    RendererBeginScenePass(renderer, 0u);
    RendererDrawMesh(renderer, mesh, kOrigin);
    if (!RendererEndFrame(renderer)) return false;
    AccumulateStats(renderer);

    uint32_t width = 0u;
    uint32_t height = 0u;
    if (!RendererCaptureFrame(renderer, pixels, PIXEL_BYTES, &width, &height)) return false;
    if (width != (uint32_t)BENCH_WIDTH || height != (uint32_t)BENCH_HEIGHT) return false;
    *outChecksum = ChecksumPixels(pixels, PIXEL_BYTES);
    return true;
}

LAIUE_TEST_ENTRY(R2VulkanSubmitBenchmarkEntryPoint)
{
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        WriteText("Vulkan backend is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    Renderer *renderer = RendererCreateWithBackend(NULL, BENCH_WIDTH, BENCH_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    if (renderer == NULL)
    {
        WriteText("No Vulkan driver available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    if (RendererGetBackend(renderer) != RENDERER_BACKEND_VULKAN)
    {
        WriteText("renderer did not report the Vulkan backend\n");
        LaiueTestRuntimeExit(1);
    }
    if (!RendererPrepareWorld(renderer))
    {
        WriteText("world prepare failed\n");
        LaiueTestRuntimeExit(1);
    }

    uint8_t *pixels = (uint8_t *)PlatformAllocate(PIXEL_BYTES, true);
    RendererMeshInstance *instances = (RendererMeshInstance *)PlatformAllocate(
        (size_t)INST_BATCH * sizeof(RendererMeshInstance), true);
    RendererMesh *meshes[MESH_BATCH];
    for (uint32_t index = 0; index < MESH_BATCH; ++index) meshes[index] = NULL;
    if (pixels == NULL || instances == NULL)
    {
        WriteText("harness allocation failed\n");
        LaiueTestRuntimeExit(1);
    }
    for (uint32_t index = 0; index < INST_BATCH; ++index)
    {
        instances[index].originRelative[0] = (float)(index % 8u) * 0.01f;
        instances[index].originRelative[1] = (float)(index / 8u) * 0.01f;
        instances[index].scale = 0.001f;
    }

    const ChunkQuad quad = MakeQuad();
    RendererMesh *mesh = RendererCreateMesh(renderer, &quad, 1u);
    if (mesh == NULL)
    {
        WriteText("mesh creation failed\n");
        LaiueTestRuntimeExit(1);
    }

    BuildFrameSetup(&sinkSetup);

    // Прогрев: страницы памяти, первые ресурсы, кэши команд драйвера.
    {
        uint64_t ignored = 0u;
        if (!RunPlainDrawSample(renderer, mesh, 8u, 512u, &ignored) ||
            !RunInstanceDrawSample(renderer, mesh, instances, 1u, 8u, 512u, &ignored) ||
            !RunInstanceDrawSample(renderer, mesh, instances, INST_BATCH, 8u, 512u, &ignored) ||
            !RunIdleSample(renderer, 32u, &ignored) ||
            !RunUploadSample(renderer, meshes, 16u, &ignored) ||
            !RunCreateSample(renderer, meshes, 16u, &ignored))
        {
            WriteText("warmup failed\n");
            LaiueTestRuntimeExit(1);
        }
    }

    uint64_t checksum = 0u;
    if (!CaptureChecksum(renderer, mesh, pixels, &checksum))
    {
        WriteText("checksum frame failed\n");
        LaiueTestRuntimeExit(1);
    }

    bool ok = true;

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunPlainDrawSample(renderer, mesh, DRAW_FRAMES, DRAW_COUNT, &elapsed);
        if (ok) ReportSample("draw_mesh", sample, elapsed);
    }

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunInstanceDrawSample(renderer, mesh, instances, 1u, INST_FRAMES, INST_DRAW_COUNT,
                                   &elapsed);
        if (ok) ReportSample("draw_inst1", sample, elapsed);
    }

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunInstanceDrawSample(renderer, mesh, instances, INST_BATCH, INST32_FRAMES,
                                   INST_DRAW_COUNT, &elapsed);
        if (ok) ReportSample("draw_inst32", sample, elapsed);
    }

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunIdleSample(renderer, IDLE_FRAMES, &elapsed);
        if (ok) ReportSample("idle_frame", sample, elapsed);
    }

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunUploadSample(renderer, meshes, UPLOAD_FRAMES, &elapsed);
        if (ok) ReportSample("upload64_frame", sample, elapsed);
    }

    for (uint32_t sample = 0; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunCreateSample(renderer, meshes, CREATE_ROUNDS, &elapsed);
        if (ok) ReportSample("create_mesh64", sample, elapsed);
    }

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

    uint64_t finalChecksum = 0u;
    if (ok && !CaptureChecksum(renderer, mesh, pixels, &finalChecksum)) ok = false;

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    WriteText("totals draws=");
    WriteUnsigned(benchDrawCalls);
    WriteText(" uploaded=");
    WriteUnsigned(benchUploadedBytes);
    WriteText(" quads=");
    WriteUnsigned(benchDrawnQuads);
    WriteText(" pool_used=");
    WriteUnsigned(stats.geometryPoolUsedBytes);
    WriteText(" pool_capacity=");
    WriteUnsigned(stats.geometryPoolCapacityBytes);
    WriteText("\nchecksum=");
    WriteHex64(checksum);
    WriteText("\nchecksum_final=");
    WriteHex64(finalChecksum);
    WriteText("\nverify=");
    WriteText(ok ? "ok" : "fail");
    WriteText("\n");

    RendererDestroyMesh(renderer, mesh);
    RendererDestroy(renderer);
    PlatformFree(instances);
    PlatformFree(pixels);

    if (benchmarkSink == UINT64_MAX) WriteText("");
    if (!ok) LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
