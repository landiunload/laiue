// Узкий стенд ресемплера изображений. В обычную сборку и в CTest не
// входит: включается опцией LAIUE_BUILD_BENCHMARKS и запускается руками.
//
// Измеряются три вида вызовов, какие делает сборка текстурного пака
// (`texture_build.c`, `WriteSliceChain`): приведение источника к общему
// размеру пака (1:1, увеличение и точное уменьшение вдвое) и цепочка
// мипов. Стенд печатает по строке `RESULT` на нагрузку и контрольную
// сумму выхода, чтобы baseline и candidate можно было сверить побайтово.
//
// Baseline и candidate — это два exe, собранные из одного текста до и
// после правки `ImageResample`; внешний скрипт чередует их запуски.

#include "media/image.h"
#include "platform/system.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_COUNT 9u
#define ITERATIONS 10u

#define CHAIN_SIDE 256u
#define SOURCE_BYTES (1024u * 1024u * 4u)
#define DESTINATION_BYTES (2u * 1024u * 1024u)

static volatile uint64_t benchmarkSink;
static volatile uint64_t benchmarkChecksum;

static uint8_t *g_source;
static uint8_t *g_destination;

// === Вывод без CRT ===

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

static void WriteMilliseconds(double value)
{
    if (value < 0.0) value = 0.0;
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static void WriteHex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0; index < 16u; ++index)
    {
        text[15u - index] = digits[value & 0xFu];
        value >>= 4u;
    }
    text[16] = '\0';
    WriteText(text);
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && CompareDouble(&samples[insertion - 1u], &value) > 0)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

// === Данные ===

static uint32_t XorShift(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void FillRandom(uint8_t *bytes, size_t count, uint32_t *state)
{
    for (size_t index = 0; index < count; ++index)
    {
        bytes[index] = (uint8_t)(XorShift(state) & 0xFFu);
    }
}

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

// === Нагрузки ===

typedef struct Workload
{
    const char *name;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t destinationWidth;
    uint32_t destinationHeight;
    bool chain;
} Workload;

static const Workload WORKLOADS[] = {
    { "up_16_to_256", 16u, 16u, 256u, 256u, false },
    { "up_64_to_256", 64u, 64u, 256u, 256u, false },
    { "up_100_to_256", 100u, 100u, 256u, 256u, false },
    { "up_128_to_512", 128u, 128u, 512u, 512u, false },
    { "copy_256", 256u, 256u, 256u, 256u, false },
    { "down_half_256_to_128", 256u, 256u, 128u, 128u, false },
    { "down_1024_to_256", 1024u, 1024u, 256u, 256u, false },
    { "down_100_to_64", 100u, 100u, 64u, 64u, false },
    { "chain_256", CHAIN_SIDE, CHAIN_SIDE, 0u, 0u, true },
};

static size_t ChainBytes(uint32_t side)
{
    size_t total = 0u;
    for (;;)
    {
        total += (size_t)side * side * 4u;
        if (side <= 1u) break;
        side >>= 1u;
    }
    return total;
}

// Один прогон цепочки мипов: уровень 0 приводится к 256×256 (копия 1:1),
// дальше каждый уровень считается из предыдущего.
static void RunChainOnce(void)
{
    uint8_t *write = g_destination;
    ImageResample(g_source, CHAIN_SIDE, CHAIN_SIDE, write, CHAIN_SIDE, CHAIN_SIDE);
    const uint8_t *read = write;
    uint32_t readSize = CHAIN_SIDE;
    write += (size_t)CHAIN_SIDE * CHAIN_SIDE * 4u;

    while (readSize > 1u)
    {
        uint32_t nextSize = readSize >> 1u;
        ImageResample(read, readSize, readSize, write, nextSize, nextSize);
        read = write;
        readSize = nextSize;
        write += (size_t)nextSize * nextSize * 4u;
    }
}

static void RunOnce(const Workload *workload)
{
    if (workload->chain)
    {
        RunChainOnce();
        return;
    }
    ImageResample(g_source, workload->sourceWidth, workload->sourceHeight, g_destination,
                  workload->destinationWidth, workload->destinationHeight);
}

static size_t ResultBytes(const Workload *workload)
{
    if (workload->chain) return ChainBytes(CHAIN_SIDE);
    return (size_t)workload->destinationWidth * workload->destinationHeight * 4u;
}

static double Measure(const Workload *workload)
{
    double samples[SAMPLE_COUNT];
    RunOnce(workload);   // прогрев: страницы и кеш
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0; iteration < ITERATIONS; ++iteration)
        {
            RunOnce(workload);
            benchmarkSink += g_destination[iteration];
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    return Median(samples, SAMPLE_COUNT);
}

LAIUE_TEST_ENTRY(ImageResampleBenchmarkEntryPoint)
{
    g_source = (uint8_t *)PlatformAllocate(SOURCE_BYTES, false);
    g_destination = (uint8_t *)PlatformAllocate(DESTINATION_BYTES, false);
    if (g_source == NULL || g_destination == NULL)
    {
        WriteText("image resample benchmark could not allocate its buffers\n");
        LaiueTestRuntimeExit(1);
    }

    WriteText("laiue image resample benchmark\n");
    WriteText("samples=");
    WriteUnsigned(SAMPLE_COUNT);
    WriteText(" iterations=");
    WriteUnsigned(ITERATIONS);
    WriteText("\n\n");

    for (uint32_t index = 0; index < sizeof(WORKLOADS) / sizeof(WORKLOADS[0]); ++index)
    {
        const Workload *workload = &WORKLOADS[index];
        uint32_t state = 0x12345678u ^ (index * 0x9E3779B9u);

        if (workload->chain)
        {
            FillRandom(g_source, (size_t)CHAIN_SIDE * CHAIN_SIDE * 4u, &state);
        }
        else
        {
            FillRandom(g_source, (size_t)workload->sourceWidth * workload->sourceHeight * 4u,
                       &state);
        }

        double median = Measure(workload);
        RunOnce(workload);
        benchmarkChecksum = HashBytes(0xcbf29ce484222325ull, g_destination,
                                      ResultBytes(workload));

        WriteText("RESULT ");
        WriteText(workload->name);
        WriteText(" ");
        WriteMilliseconds(median);
        WriteText(" ms\nCHECKSUM ");
        WriteText(workload->name);
        WriteText(" ");
        WriteHex64(benchmarkChecksum);
        WriteText("\n");
    }

    // Ссылки на sink не дают компилятору выбросить измеряемую работу.
    if (benchmarkSink == UINT64_MAX) WriteText("");
    if (benchmarkChecksum == UINT64_MAX) WriteText("");

    PlatformFree(g_source);
    PlatformFree(g_destination);
    LAIUE_TEST_SUCCESS();
}
