// A/B-стенд общего пути ImageResample (ROUND 2, агент 32-image).
//
// Первая волна уже ускорила копию 1:1, точное уменьшение вдвое и
// увеличение. Этот стенд целится в оставшийся общий путь: уменьшение с
// произвольным коэффициентом, смешанный масштаб (одна ось вниз, другая
// вверх) и мелкие картинки. Увеличение и копия оставлены контролем.
//
// Каждая нагрузка печатает `RESULT <name> <median_ms>` и
// `CHECKSUM <name> <hex>` выхода. Одинаковая сумма у baseline и candidate
// означает, что мерялась одна и та же работа.
//
// В ALL и CTest не входит: EXCLUDE_FROM_ALL, запускается A/B-скриптом
// внутри одного bench-lock. Число выборок и повторов масштабируются
// R2_IMAGE_SAMPLES / R2_IMAGE_SCALE (проценты).

#include "media/image.h"
#include "platform/system.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2_MAX_SAMPLES 16u

typedef struct R2Workload
{
    const char *name;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t destinationWidth;
    uint32_t destinationHeight;
    uint32_t iterations;
} R2Workload;

// total work roughly balanced per workload; tuned after a first timing pass.
static const R2Workload R2_WORKLOADS[] = {
    { "down_1024_to_256", 1024u, 1024u, 256u, 256u, 40u },
    { "down_1000_to_333", 1000u, 1000u, 333u, 333u, 40u },
    { "mixed_512x256_to_128x512", 512u, 256u, 128u, 512u, 120u },
    { "mixed_256x512_to_512x128", 256u, 512u, 512u, 128u, 120u },
    { "tiny_8_to_3", 8u, 8u, 3u, 3u, 450000u },
    { "tiny_5_to_2", 5u, 5u, 2u, 2u, 900000u },
    { "odd_33x17_to_7x64", 33u, 17u, 7u, 64u, 30000u },
    { "row_1000x1_to_3x1", 1000u, 1u, 3u, 1u, 60000u },
    { "down_4096_to_64", 4096u, 4096u, 64u, 64u, 6u },
    { "copy_512", 512u, 512u, 512u, 512u, 5000u },
    { "up_100_to_512", 100u, 100u, 512u, 512u, 200u },
};

static volatile uint64_t g_sink;
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
    for (uint32_t index = 0; index < length; ++index) text[index] = digits[length - 1u - index];
    text[length] = '\0';
    WriteText(text);
}

static void WriteFixed(double value)
{
    if (!(value > 0.0)) { WriteText("0.000"); return; }
    if (value > 1000000000.0) value = 1000000000.0;
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction > 999u) fraction = 999u;
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

static uint32_t ReadEnvUnsigned(const char *name, uint32_t fallback, uint32_t maximum)
{
    char text[32];
    uint32_t length = PlatformGetEnvironmentUtf8(name, text, (uint32_t)sizeof(text));
    if (length == 0u) return fallback;
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9') return fallback;
        value = value * 10u + (uint32_t)(text[index] - '0');
        if (value > maximum) return maximum;
    }
    return value == 0u ? fallback : value;
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
    for (size_t index = 0; index < count; ++index) bytes[index] = (uint8_t)(XorShift(state) & 0xFFu);
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

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
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

static void RunOnce(const R2Workload *workload)
{
    ImageResample(g_source, workload->sourceWidth, workload->sourceHeight, g_destination,
                  workload->destinationWidth, workload->destinationHeight);
}

static size_t ResultBytes(const R2Workload *workload)
{
    return (size_t)workload->destinationWidth * workload->destinationHeight * 4u;
}

static double Measure(const R2Workload *workload)
{
    uint32_t samples = ReadEnvUnsigned("R2_IMAGE_SAMPLES", 3u, R2_MAX_SAMPLES);
    double times[R2_MAX_SAMPLES];
    // Прогрев: страницы, ветвления и кеш вне статистики.
    RunOnce(workload);
    RunOnce(workload);
    for (uint32_t sample = 0u; sample < samples; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0u; iteration < workload->iterations; ++iteration)
        {
            RunOnce(workload);
            g_sink += g_destination[(iteration * 131u) % ResultBytes(workload)];
        }
        times[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    return Median(times, samples);
}

LAIUE_TEST_ENTRY(R2ImageResampleBenchmarkEntryPoint)
{
    uint32_t scalePercent = ReadEnvUnsigned("R2_IMAGE_SCALE", 100u, 100000u);

    size_t maxSource = 0u;
    size_t maxDestination = 0u;
    for (uint32_t index = 0u; index < sizeof(R2_WORKLOADS) / sizeof(R2_WORKLOADS[0]); ++index)
    {
        size_t sourceBytes =
            (size_t)R2_WORKLOADS[index].sourceWidth * R2_WORKLOADS[index].sourceHeight * 4u;
        size_t destinationBytes =
            (size_t)R2_WORKLOADS[index].destinationWidth * R2_WORKLOADS[index].destinationHeight * 4u;
        if (sourceBytes > maxSource) maxSource = sourceBytes;
        if (destinationBytes > maxDestination) maxDestination = destinationBytes;
    }
    g_source = (uint8_t *)PlatformAllocate(maxSource, false);
    g_destination = (uint8_t *)PlatformAllocate(maxDestination, false);
    if (g_source == NULL || g_destination == NULL)
    {
        WriteText("r2 image resample benchmark could not allocate its buffers\n");
        LaiueTestRuntimeExit(1);
    }

    WriteText("laiue r2 image resample benchmark\n");
    for (uint32_t index = 0u; index < sizeof(R2_WORKLOADS) / sizeof(R2_WORKLOADS[0]); ++index)
    {
        R2Workload workload = R2_WORKLOADS[index];
        workload.iterations = (workload.iterations * scalePercent) / 100u;
        if (workload.iterations == 0u) workload.iterations = 1u;

        uint32_t state = 0x12345678u ^ (index * 0x9E3779B9u);
        FillRandom(g_source, (size_t)workload.sourceWidth * workload.sourceHeight * 4u, &state);

        double median = Measure(&workload);
        RunOnce(&workload);
        uint64_t checksum =
            HashBytes(0xcbf29ce484222325ull, g_destination, ResultBytes(&workload));

        WriteText("RESULT ");
        WriteText(workload.name);
        WriteText(" ");
        WriteFixed(median);
        WriteText(" ms\nCHECKSUM ");
        WriteText(workload.name);
        WriteText(" ");
        WriteHex64(checksum);
        WriteText("\n");
    }

    if (g_sink == UINT64_MAX) WriteText("");
    PlatformFree(g_source);
    PlatformFree(g_destination);
    WriteText("r2 image resample benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
