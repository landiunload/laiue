// Ручной A/B-стенд декодера MPEG-1 Layer III (агент 29-mp3, round 2).
//
// В ALL и CTest не входит. Намеренно компилирует production-файл
// src/media/mp3_decode.c прямо в исполняемый файл (как broadphase и
// texture_build): так baseline и candidate — это два отдельных exe, и
// сравнение не зависит от подмены DLL.
//
// Входы читаются из каталога LAIUE_MP3_BENCH_DIR (файлы собирает
// reports/29-mp3/gen_fixtures.py). На каждый вход печатается одна
// машиночитаемая строка:
//
//   BENCH <name> frames <n> samples <n> iters <n> total_ns <int>
//               checksum <hex> status ok
//
// total_ns — суммарное время прогонов без прогрева, оно и есть выборка
// A/B. checksum считается по финальному выходу, чтобы baseline и
// candidate сравнивались побитово.
//
// Переменные окружения:
//   LAIUE_MP3_BENCH_DIR   каталог с .mp3 (обязательна)
//   LAIUE_MP3_BENCH_ONLY  список имён через запятую (необязательно)
//   LAIUE_MP3_BENCH_MULT  целый множитель числа прогонов (по умолчанию 1)

#include "media/mp3_decode.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define R2_MP3_OUTPUT_SAMPLES 800000u
#define R2_MP3_SCRATCH_BYTES 131072u

typedef struct R2Mp3Workload
{
    const char *name;
    const char *file;
    uint32_t iterations;
} R2Mp3Workload;

static const R2Mp3Workload WORKLOADS[] = {
    {"real", "real.mp3", 240u},
    {"short", "short.mp3", 480u},
    {"long", "long.mp3", 48u},
    {"malformed", "malformed.mp3", 160u},
    {"truncated", "truncated.mp3", 320u},
    {"rate_alias", "rate_alias.mp3", 160u},
};
#define WORKLOAD_COUNT (sizeof(WORKLOADS) / sizeof(WORKLOADS[0]))

static int16_t g_output[R2_MP3_OUTPUT_SAMPLES];
static uint64_t g_scratch[(R2_MP3_SCRATCH_BYTES + 7u) / 8u];
static volatile uint64_t g_sink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteU64(uint64_t value)
{
    char buffer[24];
    uint32_t pos = sizeof(buffer);
    buffer[--pos] = '\0';
    if (value == 0u)
    {
        buffer[--pos] = '0';
    }
    while (value != 0u)
    {
        buffer[--pos] = (char)('0' + (uint32_t)(value % 10u));
        value /= 10u;
    }
    WriteText(&buffer[pos]);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (uint32_t index = 0; index < 16u; ++index)
    {
        buffer[2u + index] = digits[(value >> (60u - index * 4u)) & 0xFu];
    }
    buffer[18] = '\0';
    WriteText(buffer);
}

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static bool NameMatches(const char *name, const char *filter, uint32_t filterLength)
{
    if (filterLength == 0u) return true;
    for (uint32_t start = 0; start < filterLength; ++start)
    {
        uint32_t end = start;
        while (end < filterLength && filter[end] != ',') ++end;
        uint32_t length = end - start;
        bool same = true;
        for (uint32_t index = 0; index < length; ++index)
        {
            if (name[index] == '\0' || name[index] != filter[start + index])
            {
                same = false;
                break;
            }
        }
        if (same && name[length] == '\0') return true;
        start = end;
    }
    return false;
}

static bool ReadFixture(const char *directory, uint32_t directoryLength, const char *name,
                        uint8_t **outBytes, uint32_t *outSize)
{
    char path[600];
    if (directoryLength + 1u + 64u >= sizeof(path)) return false;
    uint32_t at = 0u;
    for (uint32_t index = 0; index < directoryLength; ++index) path[at++] = directory[index];
    if (at != 0u && path[at - 1u] != '/' && path[at - 1u] != '\\') path[at++] = '/';
    for (uint32_t index = 0; name[index] != '\0' && at + 1u < sizeof(path); ++index)
    {
        path[at++] = name[index];
    }
    path[at] = '\0';

    wchar_t wide[600];
    if (!PlatformUtf8ToWide(path, at, wide, (uint32_t)(sizeof(wide) / sizeof(wide[0])), NULL))
        return false;

    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!PlatformReadEntireFile(wide, 64u * 1024u * 1024u, &bytes, &size)) return false;
    if (size > 0xFFFFFFFFull)
    {
        PlatformFree(bytes);
        return false;
    }
    *outBytes = bytes;
    *outSize = (uint32_t)size;
    return true;
}

static void RunWorkload(const char *directory, uint32_t directoryLength,
                        const R2Mp3Workload *workload, uint32_t mult)
{
    uint8_t *file = NULL;
    uint32_t fileBytes = 0u;
    if (!ReadFixture(directory, directoryLength, workload->file, &file, &fileBytes))
    {
        WriteText("BENCH ");
        WriteText(workload->name);
        WriteText(" status missing\n");
        return;
    }

    Mp3Info info = {0};
    Mp3Status status = Mp3Inspect(file, fileBytes, &info);
    if (status != MP3_OK)
    {
        WriteText("BENCH ");
        WriteText(workload->name);
        WriteText(" status inspect_");
        WriteU64((uint64_t)status);
        WriteText("\n");
        PlatformFree(file);
        return;
    }
    if (info.scratchBytes > R2_MP3_SCRATCH_BYTES ||
        (uint64_t)info.frameCount * info.channelCount > R2_MP3_OUTPUT_SAMPLES)
    {
        WriteText("BENCH ");
        WriteText(workload->name);
        WriteText(" status too_big\n");
        PlatformFree(file);
        return;
    }

    uint32_t iterations = workload->iterations * mult;
    if (iterations == 0u) iterations = 1u;
    uint32_t warmup = iterations / 8u;
    if (warmup < 2u) warmup = 2u;

    for (uint32_t run = 0; run < warmup; ++run)
    {
        if (Mp3DecodeSamples(file, fileBytes, &info, g_output, R2_MP3_OUTPUT_SAMPLES,
                             g_scratch, R2_MP3_SCRATCH_BYTES) != MP3_OK)
        {
            WriteText("BENCH ");
            WriteText(workload->name);
            WriteText(" status warm_fail\n");
            PlatformFree(file);
            return;
        }
    }

    double started = PlatformMonotonicSeconds();
    for (uint32_t run = 0; run < iterations; ++run)
    {
        if (Mp3DecodeSamples(file, fileBytes, &info, g_output, R2_MP3_OUTPUT_SAMPLES,
                             g_scratch, R2_MP3_SCRATCH_BYTES) != MP3_OK)
        {
            WriteText("BENCH ");
            WriteText(workload->name);
            WriteText(" status run_fail\n");
            PlatformFree(file);
            return;
        }
    }
    double finished = PlatformMonotonicSeconds();

    if (Mp3DecodeSamples(file, fileBytes, &info, g_output, R2_MP3_OUTPUT_SAMPLES,
                         g_scratch, R2_MP3_SCRATCH_BYTES) != MP3_OK)
    {
        WriteText("BENCH ");
        WriteText(workload->name);
        WriteText(" status final_fail\n");
        PlatformFree(file);
        return;
    }

    uint32_t bytes = info.frameCount * info.channelCount * 2u;
    uint64_t checksum = 0xcbf29ce484222325ull;
    checksum = HashBytes(checksum, (const uint8_t *)g_output, bytes);
    checksum ^= (uint64_t)info.frameCount * 0x9e3779b97f4a7c15ull;
    g_sink = checksum;

    WriteText("BENCH ");
    WriteText(workload->name);
    WriteText(" frames ");
    WriteU64(info.frameCount);
    WriteText(" samples ");
    WriteU64((uint64_t)info.frameCount * info.channelCount);
    WriteText(" scratch ");
    WriteU64(info.scratchBytes);
    WriteText(" iters ");
    WriteU64(iterations);
    WriteText(" total_ns ");
    WriteU64((uint64_t)((finished - started) * 1000000000.0));
    WriteText(" checksum ");
    WriteHex(checksum);
    WriteText(" status ok\n");

    PlatformFree(file);
}

LAIUE_TEST_ENTRY(R2Mp3BenchmarkEntryPoint)
{
    char directory[512];
    uint32_t directoryLength = PlatformGetEnvironmentUtf8("LAIUE_MP3_BENCH_DIR", directory,
                                                          (uint32_t)sizeof(directory));
    char filter[256];
    uint32_t filterLength = PlatformGetEnvironmentUtf8("LAIUE_MP3_BENCH_ONLY", filter,
                                                       (uint32_t)sizeof(filter));
    char multText[32];
    uint32_t multLength = PlatformGetEnvironmentUtf8("LAIUE_MP3_BENCH_MULT", multText,
                                                     (uint32_t)sizeof(multText));
    uint32_t mult = 1u;
    if (multLength != 0u)
    {
        uint32_t parsed = 0u;
        for (uint32_t index = 0; index < multLength; ++index)
        {
            if (multText[index] < '0' || multText[index] > '9') break;
            parsed = parsed * 10u + (uint32_t)(multText[index] - '0');
        }
        if (parsed != 0u) mult = parsed;
    }

    if (directoryLength == 0u)
    {
        WriteText("BENCH ERROR no LAIUE_MP3_BENCH_DIR\n");
        LAIUE_TEST_SUCCESS();
    }

    for (uint32_t index = 0; index < WORKLOAD_COUNT; ++index)
    {
        if (!NameMatches(WORKLOADS[index].name, filter, filterLength)) continue;
        RunWorkload(directory, directoryLength, &WORKLOADS[index], mult);
    }
    LAIUE_TEST_SUCCESS();
}
