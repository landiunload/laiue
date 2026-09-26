// Ручной benchmark точечных и пакетных правок мира. В ALL не входит и в CTest
// не регистрируется: его запускают осознанно и читают глазами.
//
// Предмет — WorldApplyBlockBatch: проверка дублей, группировка мутаций по
// чанкам, резервирование буфера дельт и публикация. Сценарии подобраны по
// форме нагрузки, а не по правдоподобию:
//
//   single_toggle — одиночный WorldTrySetBlock, создание и откат правки;
//   batch_small   — 8 мутаций на 2 чанка (накладные расходы мелкого пакета);
//   batch_few     — 4096 мутаций на 4 чанка (1024 дельты на чанк);
//   batch_wide    — 4096 мутаций на 4096 разных чанков (по одной дельте);
//   batch_cancel  — 4096 мутаций на 64 чанка, полный откат и повторная правка.
//
// Мутации заранее разложены в два массива (A и B), поэтому время внутри
// цикла — это только вызовы движка, без подготовки данных. Значения дельт
// переключаются 1 <-> 2 (база 0), так что каждый пакет реально работает и
// не является no-op.
//
// Режим одиночного сценария включается переменной окружения
// LAIUE_WORLD_BATCH_ONLY=<имя>: тогда мерится только он, а в конце печатается
// пиковая память процесса (commit/working set) — так транзиент одного пакета
// виден без примеси остальных сценариев.

#include "platform/system.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
// PROCESS_MEMORY_COUNTERS и K32GetProcessMemoryInfo: kernel32 уже слинкован
// обёрткой standalone-исполняемого файла, psapi.lib не требуется.
typedef struct WorldBenchMemoryCounters
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
} WorldBenchMemoryCounters;

__declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(
    void* process, WorldBenchMemoryCounters* counters, uint32_t size);
#endif

#define WORLD_BENCH_SAMPLES 5u
#define WORLD_BENCH_MAX_MUTATIONS WORLD_MAX_ATOMIC_BLOCK_MUTATIONS

static volatile uint64_t worldBenchSink;

// === Вывод без CRT ===

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

static void WriteMilliseconds(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.0000");
        return;
    }
    uint64_t scaled = (uint64_t)(value * 10000.0 + 0.5);
    WriteUnsigned(scaled / 10000u);
    WriteText(".");
    uint64_t fraction = scaled % 10000u;
    if (fraction < 1000u)
    {
        WriteText("0");
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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[15u - index] = digits[(value >> (index * 4u)) & 0xfu];
    }
    text[16] = '\0';
    WriteText(text);
}

static bool TextEquals(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static const char* ReadSingleScenario(void)
{
#if defined(_WIN32)
    static char buffer[64];
    DWORD length = GetEnvironmentVariableA(
        "LAIUE_WORLD_BATCH_ONLY", buffer, (DWORD)sizeof(buffer));
    if (length == 0u || length >= (DWORD)sizeof(buffer))
    {
        return NULL;
    }
    return buffer;
#else
    return NULL;
#endif
}

static void PrintPeakMemory(void)
{
#if defined(_WIN32)
    WorldBenchMemoryCounters counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = (uint32_t)sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters,
            (uint32_t)sizeof(counters)) != 0)
    {
        WriteText("MEM commit=");
        WriteUnsigned(counters.pagefileUsage);
        WriteText(" peak_commit=");
        WriteUnsigned(counters.peakPagefileUsage);
        WriteText(" peak_working=");
        WriteUnsigned(counters.peakWorkingSetSize);
        WriteText("\n");
        return;
    }
#endif
    WriteText("MEM unavailable\n");
}

// === Общие помощники ===

static void SortSamples(double* samples, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && samples[insertion - 1u] > value)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
}

static void ReportScenario(const char* name, const double* samples,
    uint32_t sampleCount, uint64_t operations, uint64_t checksum)
{
    double sorted[WORLD_BENCH_SAMPLES];
    for (uint32_t index = 0u; index < sampleCount; ++index)
    {
        sorted[index] = samples[index];
    }
    SortSamples(sorted, sampleCount);

    WriteText("RESULT ");
    WriteText(name);
    WriteText(" ops=");
    WriteUnsigned(operations);
    WriteText(" samples=");
    WriteUnsigned(sampleCount);
    WriteText(" min=");
    WriteMilliseconds(sorted[0]);
    WriteText(" median=");
    WriteMilliseconds(sorted[sampleCount / 2u]);
    WriteText(" max=");
    WriteMilliseconds(sorted[sampleCount - 1u]);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText("\n");
}

// Координата дельты внутри чанка по её порядковому номеру. perChunk обязан
// делить 4096 нацело — иначе слоты не будут уникальны.
static uint32_t LocalIndexForSlot(uint32_t slot, uint32_t perChunk)
{
    return slot * (4096u / perChunk);
}

static void FillMutations(WorldBlockMutation* mutations, uint32_t count,
    uint32_t perChunk, BlockType expected, BlockType replacement, int64_t chunkStride)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t chunkIndex = index / perChunk;
        uint32_t slot = index % perChunk;
        uint32_t localIndex = LocalIndexForSlot(slot, perChunk);
        int64_t chunkX = (int64_t)chunkIndex * chunkStride + 3;
        int64_t chunkY = (int64_t)chunkIndex * 7 + 2;
        int64_t chunkZ = (int64_t)chunkIndex * 11 + 5;
        int64_t localX = (int64_t)(localIndex / 4096u);
        int64_t localY = (int64_t)((localIndex / CHUNK_SIZE) % CHUNK_SIZE);
        int64_t localZ = (int64_t)(localIndex % CHUNK_SIZE);
        mutations[index].block[0] = chunkX * CHUNK_SIZE + localX;
        mutations[index].block[1] = chunkY * CHUNK_SIZE + localY;
        mutations[index].block[2] = chunkZ * CHUNK_SIZE + localZ;
        mutations[index].expected = expected;
        mutations[index].replacement = replacement;
    }
}

static bool PrecreateMutations(World* world, const WorldBlockMutation* mutations,
    uint32_t count, BlockType value)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!WorldTrySetBlock(world, mutations[index].block[0],
                mutations[index].block[1], mutations[index].block[2], value))
        {
            return false;
        }
    }
    return true;
}

// Время чередующихся пакетов A и B; подготовка данных не входит.
static double TimeAlternating(World* world, const WorldBlockMutation* even,
    const WorldBlockMutation* odd, uint32_t count, uint32_t rounds,
    uint64_t* sink)
{
    double start = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        const WorldBlockMutation* batch = (round & 1u) != 0u ? odd : even;
        if (WorldApplyBlockBatch(world, batch, count))
        {
            ++*sink;
        }
    }
    return (PlatformMonotonicSeconds() - start) * 1000.0;
}

static uint64_t SampleChecksum(World* world, const WorldBlockMutation* mutations,
    uint32_t count)
{
    uint64_t checksum = WorldGetRevision(world);
    uint32_t stride = count > 64u ? count / 64u : 1u;
    for (uint32_t index = 0u; index < count; index += stride)
    {
        checksum = checksum * 1000003u + WorldGetBlock(world,
            mutations[index].block[0], mutations[index].block[1],
            mutations[index].block[2]);
    }
    return checksum;
}

// === Сценарий: одиночные правки ===

static void RunSingleToggle(uint32_t rounds)
{
    const uint32_t coordinateCount = 4096u;
    static int64_t blockX[4096];
    static int64_t blockY[4096];
    static int64_t blockZ[4096];
    World* world = WorldCreate(NULL);
    if (world == NULL)
    {
        WriteText("RESULT single_toggle ERROR world\n");
        return;
    }
    for (uint32_t index = 0u; index < coordinateCount; ++index)
    {
        uint32_t chunkIndex = index & 1u;
        uint32_t localIndex = (index * 37u) & 4095u;
        blockX[index] = (int64_t)chunkIndex * CHUNK_SIZE
            + (int64_t)(localIndex / 4096u);
        blockY[index] = (int64_t)((localIndex / CHUNK_SIZE) % CHUNK_SIZE);
        blockZ[index] = (int64_t)(localIndex % CHUNK_SIZE);
    }

    double samples[WORLD_BENCH_SAMPLES];
    uint64_t checksum = 0u;
    for (uint32_t sample = 0u; sample < WORLD_BENCH_SAMPLES; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t round = 0u; round < rounds; ++round)
        {
            for (uint32_t index = 0u; index < coordinateCount; ++index)
            {
                if (WorldTrySetBlock(world, blockX[index], blockY[index],
                        blockZ[index], (BlockType)5u))
                {
                    ++worldBenchSink;
                }
            }
            for (uint32_t index = 0u; index < coordinateCount; ++index)
            {
                if (WorldTrySetBlock(world, blockX[index], blockY[index],
                        blockZ[index], BLOCK_AIR))
                {
                    ++worldBenchSink;
                }
            }
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    checksum = WorldGetRevision(world);
    checksum = checksum * 1000003u + (uint64_t)WorldGetBlock(world, blockX[0], blockY[0], blockZ[0]);
    ReportScenario("single_toggle", samples, WORLD_BENCH_SAMPLES,
        (uint64_t)rounds * coordinateCount * 2u, checksum);
    WorldDestroy(world);
}

// === Сценарий: пакеты ===

// mode = false: обновление существующих дельт (1<->2);
// mode = true:  полный откат (1->0) и повторная правка (0->1).
static void RunBatch(const char* name, uint32_t perChunk, uint32_t chunkCount,
    uint32_t rounds, bool cancelMode)
{
    uint32_t count = perChunk * chunkCount;
    if (count == 0u || count > WORLD_BENCH_MAX_MUTATIONS
        || (4096u % perChunk) != 0u)
    {
        WriteText("RESULT ");
        WriteText(name);
        WriteText(" SKIP bad parameters\n");
        return;
    }
    WorldBlockMutation* even = PlatformAllocate(
        (size_t)count * sizeof(*even), false);
    WorldBlockMutation* odd = PlatformAllocate(
        (size_t)count * sizeof(*odd), false);
    World* world = WorldCreate(NULL);
    if (even == NULL || odd == NULL || world == NULL)
    {
        PlatformFree(even);
        PlatformFree(odd);
        WorldDestroy(world);
        WriteText("RESULT ");
        WriteText(name);
        WriteText(" ERROR allocation\n");
        return;
    }

    if (cancelMode)
    {
        FillMutations(even, count, perChunk, (BlockType)1u, BLOCK_AIR, 3);
        FillMutations(odd, count, perChunk, BLOCK_AIR, (BlockType)1u, 3);
    }
    else
    {
        FillMutations(even, count, perChunk, (BlockType)1u, (BlockType)2u, 3);
        FillMutations(odd, count, perChunk, (BlockType)2u, (BlockType)1u, 3);
    }

    if (!PrecreateMutations(world, even, count, (BlockType)1u))
    {
        WriteText("RESULT ");
        WriteText(name);
        WriteText(" ERROR precreate\n");
        PlatformFree(even);
        PlatformFree(odd);
        WorldDestroy(world);
        return;
    }

    double samples[WORLD_BENCH_SAMPLES];
    uint64_t checksum = 0u;
    for (uint32_t sample = 0u; sample < WORLD_BENCH_SAMPLES; ++sample)
    {
        samples[sample] = TimeAlternating(world, even, odd, count, rounds,
            &checksum);
    }
    uint64_t finalChecksum = SampleChecksum(world, even, count);
    ReportScenario(name, samples, WORLD_BENCH_SAMPLES, (uint64_t)rounds * count,
        finalChecksum);

    PlatformFree(even);
    PlatformFree(odd);
    WorldDestroy(world);
}

static void RunAll(void)
{
    RunSingleToggle(300u);
    RunBatch("batch_small", 4u, 2u, 20000u, false);
    RunBatch("batch_few", 1024u, 4u, 30u, false);
    RunBatch("batch_wide", 1u, 4096u, 20u, false);
    RunBatch("batch_cancel", 64u, 64u, 20u, true);
}

static void RunOne(const char* name)
{
    if (TextEquals(name, "single_toggle"))
    {
        RunSingleToggle(300u);
    }
    else if (TextEquals(name, "batch_small"))
    {
        RunBatch("batch_small", 4u, 2u, 4000u, false);
    }
    else if (TextEquals(name, "batch_few"))
    {
        RunBatch("batch_few", 1024u, 4u, 8u, false);
    }
    else if (TextEquals(name, "batch_wide"))
    {
        RunBatch("batch_wide", 1u, 4096u, 8u, false);
    }
    else if (TextEquals(name, "batch_cancel"))
    {
        RunBatch("batch_cancel", 64u, 64u, 8u, true);
    }
    else
    {
        WriteText("unknown scenario: ");
        WriteText(name);
        WriteText("\n");
    }
}

LAIUE_TEST_ENTRY(WorldBatchBenchmarkEntryPoint)
{
    WriteText("laiue world batch benchmark\n");

    const char* only = ReadSingleScenario();
    if (only != NULL)
    {
        RunOne(only);
    }
    else
    {
        RunAll();
    }

    if (worldBenchSink == UINT64_MAX)
    {
        WriteText("");
    }
    PrintPeakMemory();
    LAIUE_TEST_SUCCESS();
}
