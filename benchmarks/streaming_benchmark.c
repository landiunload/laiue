// Ручной benchmark главного потока стриминга чанков. В ALL не входит и в
// CTest не регистрируется: его запускают осознанно и читают глазами.
//
// Меряется только главный поток: смена центра (поиск в таблице, вытеснение,
// вставка, постановка заявок), разбор очереди результатов (Pump) и
// инвалидация блоков. Мир пуст, поэтому рабочие потоки строят ноль квадов и
// ни один вызов Renderer* не происходит: рендерер фиктивный. Это позволяет
// мерить протокол очередей и таблицу без GPU и без зависимости от картинки.
//
// Про сценарии:
//   firstload <R>            — один первый SetCenter и последующая разгрузка;
//   walk <R> <steps> <frames> <sleep_ms> — движение на соседний чанк шагами,
//                              между шагами <frames> вызовов Pump; <sleep_ms>
//                              моделирует паузу кадра и даёт рабочим успевать;
//   teleport <R> <jumps>     — редкие прыжки на много чанков с разгрузкой;
//   invalidate <R> <count>   — инвалидация блоков в устоявшейся таблице.
//
// Все времена — микросекунды, минимум/медиана/максимум по шагам сценария.
// Это микробенчмарк одной подсистемы; о FPS по нему судить нельзя.

#include "platform/system.h"
#include "voxel_render/chunk_streaming.h"
#include "mesh/mesher_service.h"
#include "world/numeric_provider.h"
#include "world/world.h"
#include "world/world_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define STREAM_BENCH_MAX_SAMPLES 4096u
#define STREAM_BENCH_SETTLE_TIMEOUT 60.0

static double benchSamples[STREAM_BENCH_MAX_SAMPLES];
static double benchScratch[STREAM_BENCH_MAX_SAMPLES];
static uint32_t benchSampleCount;
static volatile uint64_t benchSink;

// Фиктивный рендерер: мир пуст, меши не создаются, ни один Renderer*-вызов
// не исполняется. Указатель только хранится стримингом; нули читаются
// диспетчером рендера как AUTO.
static uint64_t benchRendererPlaceholder;

static void WriteText(const char* text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

// Три знака после запятой: здесь разница измеряется микросекундами.
static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction > 999u)
    {
        fraction = 999u;
    }
    if (fraction < 100u)
    {
        WriteText("0");
    }
    if (fraction < 10u)
    {
        WriteText("0");
    }
    WriteUnsigned(fraction);
}

static uint32_t ReadUnsignedEnv(const char* name, uint32_t fallback)
{
    char text[32];
    uint32_t length = PlatformGetEnvironmentUtf8(name, text, (uint32_t)sizeof(text));
    if (length == 0u || length >= sizeof(text))
    {
        return fallback;
    }
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return fallback;
        }
        value = value * 10u + (uint32_t)(text[index] - '0');
    }
    return value;
}

static const char* ReadStringEnv(const char* name, char* buffer, uint32_t capacity,
    const char* fallback)
{
    uint32_t length = PlatformGetEnvironmentUtf8(name, buffer, capacity);
    if (length == 0u || length >= capacity)
    {
        return fallback;
    }
    return buffer;
}

static void ResetSamples(void)
{
    benchSampleCount = 0u;
}

static void AddSample(double microseconds)
{
    if (benchSampleCount < STREAM_BENCH_MAX_SAMPLES)
    {
        benchSamples[benchSampleCount++] = microseconds;
    }
}

static double SampleTotal(void)
{
    double total = 0.0;
    for (uint32_t index = 0u; index < benchSampleCount; ++index)
    {
        total += benchSamples[index];
    }
    return total;
}

static double SampleMin(void)
{
    if (benchSampleCount == 0u)
    {
        return 0.0;
    }
    double best = benchSamples[0];
    for (uint32_t index = 1u; index < benchSampleCount; ++index)
    {
        if (benchSamples[index] < best)
        {
            best = benchSamples[index];
        }
    }
    return best;
}

static double SampleMedian(void)
{
    if (benchSampleCount == 0u)
    {
        return 0.0;
    }
    double* scratch = benchScratch;
    for (uint32_t index = 0u; index < benchSampleCount; ++index)
    {
        scratch[index] = benchSamples[index];
    }
    for (uint32_t i = 1u; i < benchSampleCount; ++i)
    {
        double value = scratch[i];
        uint32_t j = i;
        while (j > 0u && scratch[j - 1u] > value)
        {
            scratch[j] = scratch[j - 1u];
            --j;
        }
        scratch[j] = value;
    }
    return scratch[benchSampleCount / 2u];
}

static double SampleMax(void)
{
    if (benchSampleCount == 0u)
    {
        return 0.0;
    }
    double worst = benchSamples[0];
    for (uint32_t index = 1u; index < benchSampleCount; ++index)
    {
        if (benchSamples[index] > worst)
        {
            worst = benchSamples[index];
        }
    }
    return worst;
}

static void WriteSampleStats(const char* label)
{
    WriteText(label);
    WriteText("_min_us=");
    WriteFixed(SampleMin());
    WriteText(" ");
    WriteText(label);
    WriteText("_med_us=");
    WriteFixed(SampleMedian());
    WriteText(" ");
    WriteText(label);
    WriteText("_max_us=");
    WriteFixed(SampleMax());
    WriteText(" ");
    WriteText(label);
    WriteText("_total_us=");
    WriteFixed(SampleTotal());
    WriteText(" ");
    WriteText(label);
    WriteText("_n=");
    WriteUnsigned(benchSampleCount);
}

// Разгрузка очереди: Pump, пока не останется ни заявок, ни результатов.
// Возвращает время в миллисекундах и число вызовов Pump.
static double Settle(ChunkStreaming* streaming, uint32_t* outPumps)
{
    double begin = PlatformMonotonicSeconds();
    uint32_t pumps = 0u;
    for (;;)
    {
        ChunkStreamingPump(streaming);
        ++pumps;
        ChunkStreamingStats stats;
        ChunkStreamingGetStats(streaming, &stats);
        if (stats.pendingRequests == 0u && stats.pendingResults == 0u)
        {
            break;
        }
        if (PlatformMonotonicSeconds() - begin > STREAM_BENCH_SETTLE_TIMEOUT)
        {
            break;
        }
        PlatformSleepMilliseconds(0u);
    }
    if (outPumps != NULL)
    {
        *outPumps = pumps;
    }
    return (PlatformMonotonicSeconds() - begin) * 1000.0;
}

static ChunkStreaming* CreateStreaming(World** outWorld, int32_t radius)
{
    World* world = WorldCreate(NULL);
    if (world == NULL)
    {
        WriteText("streaming benchmark world creation failed\n");
        LaiueTestRuntimeExit(1);
    }
    ChunkStreaming* streaming = ChunkStreamingCreate(
        world, (Renderer*)&benchRendererPlaceholder, radius);
    if (streaming == NULL)
    {
        WriteText("streaming benchmark streaming creation failed\n");
        LaiueTestRuntimeExit(1);
    }
    *outWorld = world;
    return streaming;
}

static void WriteReportHeader(const char* scenario, uint32_t radius)
{
    WriteText("streaming_bench scenario=");
    WriteText(scenario);
    WriteText(" radius=");
    WriteUnsigned(radius);
    WriteText(" ");
}

static void WriteCounters(ChunkStreaming* streaming)
{
    ChunkStreamingStats stats;
    ChunkStreamingGetStats(streaming, &stats);
    WriteText("queued=");
    WriteUnsigned(stats.queuedRequests);
    WriteText(" built=");
    WriteUnsigned(stats.completedBuilds);
    WriteText(" uploaded=");
    WriteUnsigned(stats.uploadedMeshes);
    WriteText(" cancelled=");
    WriteUnsigned(stats.cancelledBuilds);
    WriteText(" discarded=");
    WriteUnsigned(stats.discardedBuilds);
    WriteText(" peak_unfinished=");
    WriteUnsigned(stats.peakUnfinishedWork);
}

static void RunFirstLoad(uint32_t radius)
{
    World* world = NULL;
    ChunkStreaming* streaming = CreateStreaming(&world, (int32_t)radius);

    double begin = PlatformMonotonicSeconds();
    ChunkStreamingSetCenter(streaming, 0, 0, 0);
    double setCenter = (PlatformMonotonicSeconds() - begin) * 1000000.0;

    uint32_t pumps = 0u;
    double settle = Settle(streaming, &pumps);

    WriteReportHeader("firstload", radius);
    WriteText("setcenter_us=");
    WriteFixed(setCenter);
    WriteText(" settle_ms=");
    WriteFixed(settle);
    WriteText(" settle_pumps=");
    WriteUnsigned(pumps);
    WriteText(" ");
    WriteCounters(streaming);
    WriteText("\n");

    ChunkStreamingDestroy(streaming);
    WorldDestroy(world);
}

static void RunWalk(uint32_t radius, uint32_t steps, uint32_t frames, uint32_t sleepMs)
{
    World* world = NULL;
    ChunkStreaming* streaming = CreateStreaming(&world, (int32_t)radius);

    ChunkStreamingSetCenter(streaming, 0, 0, 0);
    (void)Settle(streaming, NULL);

    ResetSamples();
    double pumpTotal = 0.0;
    for (uint32_t step = 0u; step < steps; ++step)
    {
        double begin = PlatformMonotonicSeconds();
        ChunkStreamingSetCenter(streaming, (int64_t)step + 1, 0, 0);
        double setCenter = (PlatformMonotonicSeconds() - begin) * 1000000.0;
        AddSample(setCenter);

        for (uint32_t frame = 0u; frame < frames; ++frame)
        {
            double pumpBegin = PlatformMonotonicSeconds();
            ChunkStreamingPump(streaming);
            pumpTotal += (PlatformMonotonicSeconds() - pumpBegin) * 1000000.0;
        }
        if (sleepMs != 0u)
        {
            PlatformSleepMilliseconds(sleepMs);
        }
    }

    double pumpsPerStep = (double)(frames == 0u ? 1u : frames);
    uint32_t stopPumps = 0u;
    double stop = Settle(streaming, &stopPumps);

    WriteReportHeader("walk", radius);
    WriteText("steps=");
    WriteUnsigned(steps);
    WriteText(" frames=");
    WriteUnsigned(frames);
    WriteText(" sleep_ms=");
    WriteUnsigned(sleepMs);
    WriteText(" ");
    WriteSampleStats("setcenter");
    WriteText(" pump_total_us=");
    WriteFixed(pumpTotal);
    WriteText(" pump_per_step_us=");
    WriteFixed(pumpTotal / (pumpsPerStep * (double)steps));
    WriteText(" stop_ms=");
    WriteFixed(stop);
    WriteText(" stop_pumps=");
    WriteUnsigned(stopPumps);
    WriteText(" ");
    WriteCounters(streaming);
    WriteText("\n");

    ChunkStreamingDestroy(streaming);
    WorldDestroy(world);
}

static void RunTeleport(uint32_t radius, uint32_t jumps)
{
    World* world = NULL;
    ChunkStreaming* streaming = CreateStreaming(&world, (int32_t)radius);

    ChunkStreamingSetCenter(streaming, 0, 0, 0);
    (void)Settle(streaming, NULL);

    ResetSamples();
    double settleTotal = 0.0;
    for (uint32_t jump = 0u; jump < jumps; ++jump)
    {
        int64_t target = (int64_t)(jump + 1u) * 1000;
        double begin = PlatformMonotonicSeconds();
        ChunkStreamingSetCenter(streaming, target, 0, 0);
        AddSample((PlatformMonotonicSeconds() - begin) * 1000000.0);
        settleTotal += Settle(streaming, NULL);
    }

    WriteReportHeader("teleport", radius);
    WriteText("jumps=");
    WriteUnsigned(jumps);
    WriteText(" ");
    WriteSampleStats("setcenter");
    WriteText(" settle_total_ms=");
    WriteFixed(settleTotal);
    WriteText(" ");
    WriteCounters(streaming);
    WriteText("\n");

    ChunkStreamingDestroy(streaming);
    WorldDestroy(world);
}

static void RunInvalidate(uint32_t radius, uint32_t count)
{
    World* world = NULL;
    ChunkStreaming* streaming = CreateStreaming(&world, (int32_t)radius);

    ChunkStreamingSetCenter(streaming, 0, 0, 0);
    (void)Settle(streaming, NULL);

    ResetSamples();
    int64_t side = (int64_t)radius * CHUNK_SIZE;
    for (uint32_t index = 0u; index < count; ++index)
    {
        int64_t blockX = (int64_t)(index % 61u) * 7 - side;
        int64_t blockY = (int64_t)((index / 61u) % 61u) * 7 - side;
        int64_t blockZ = (int64_t)((index / 3721u) % 61u) * 7 - side;
        double begin = PlatformMonotonicSeconds();
        ChunkStreamingInvalidateBlock(streaming, blockX, blockY, blockZ);
        AddSample((PlatformMonotonicSeconds() - begin) * 1000000.0);
    }

    WriteReportHeader("invalidate", radius);
    WriteText("count=");
    WriteUnsigned(count);
    WriteText(" ");
    WriteSampleStats("invalidate");
    WriteText(" ");
    WriteCounters(streaming);
    WriteText("\n");

    ChunkStreamingDestroy(streaming);
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(StreamingBenchmarkEntryPoint)
{
    // Keep the standalone benchmark's service setup aligned with the stress
    // test; without providers, worker threads cannot drain the request ring.
    WorldSetNumericService(LaiueNumericGetStaticServiceV1());
    ChunkStreamingSetWorldService(LaiueWorldGetStaticServiceV1());
    ChunkStreamingSetMesherService(LaiueMesherGetStaticServiceV1());
    ChunkStreamingSetGraphicsService(NULL);

    char scenarioBuffer[32];
    for (uint32_t index = 0u; index < sizeof(scenarioBuffer); ++index)
    {
        scenarioBuffer[index] = '\0';
    }
    const char* scenario = ReadStringEnv(
        "LAIUE_STREAMING_BENCH_SCENARIO", scenarioBuffer, (uint32_t)sizeof(scenarioBuffer), "all");
    uint32_t radius = ReadUnsignedEnv("LAIUE_STREAMING_BENCH_RADIUS", 12u);
    uint32_t steps = ReadUnsignedEnv("LAIUE_STREAMING_BENCH_STEPS", 200u);
    uint32_t frames = ReadUnsignedEnv("LAIUE_STREAMING_BENCH_FRAMES", 1u);
    uint32_t jumps = ReadUnsignedEnv("LAIUE_STREAMING_BENCH_JUMPS", 20u);
    uint32_t invalidates = ReadUnsignedEnv("LAIUE_STREAMING_BENCH_INVALIDATES", 2000u);

    if (radius < 1u || radius > 16u)
    {
        WriteText("streaming benchmark radius out of range\n");
        LaiueTestRuntimeExit(1);
    }

    WriteText("laiue streaming benchmark scenario=");
    WriteText(scenario);
    WriteText(" radius=");
    WriteUnsigned(radius);
    WriteText("\n");

    bool runAll = scenario[0] == 'a' && scenario[1] == 'l' && scenario[2] == 'l';
    if (runAll || (scenario[0] == 'f' && scenario[1] == 'i'))
    {
        RunFirstLoad(radius);
    }
    if (runAll || (scenario[0] == 'w' && scenario[1] == 'a' && scenario[2] == 'l'))
    {
        RunWalk(radius, steps, frames, 0u);
    }
    if (runAll || (scenario[0] == 's' && scenario[1] == 'l'))
    {
        RunWalk(radius, steps, frames, 8u);
    }
    if (runAll || (scenario[0] == 't' && scenario[1] == 'e'))
    {
        RunTeleport(radius, jumps);
    }
    if (runAll || (scenario[0] == 'i' && scenario[1] == 'n'))
    {
        RunInvalidate(radius, invalidates);
    }

    WriteText("streaming benchmark done sink=");
    benchSink += benchSampleCount;
    WriteUnsigned(benchSink);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
