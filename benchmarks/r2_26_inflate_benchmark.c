// Ручной A/B-стенд декодера DEFLATE/zlib (src/media/inflate.c) для волны R2.
//
// В ALL и CTest не входит: включается LAIUE_BUILD_BENCHMARKS и запускается
// A/B-скриптом. Внутренний inflate.c компилируется прямо в этот
// исполняемый файл (как texture_build.c в texture_build_benchmark): так один
// и тот же harness сравнивает baseline и candidate .obj без пересборки DLL.
//
// Фикстуры — детерминированные zlib-потоки из каталога LAIUE_INFLATE_BENCH_DIR
// (<name>.zlib) вместе с ожидаемым распакованным содержимым (<name>.raw).
// Каждая фикстура прогоняется в двух режимах:
//   single — один отрезок;
//   split  — поток режется на четыре отрезка (границы IDAT), чтобы
//            задеть копирование через границы сегментов.
//
// Формат строки на измерение:
//   BENCH name=<n> mode=<m> reps=<r> status=<s> written=<w>
//         ns_total=<t> ns_min=<mn> checksum=<hex>
//
// Печатается суммарное время по reps и минимальное время одного прогона.
// checksum — FNV-1a64 по всему выходу, чтобы компилятор не выбросил работу, а
// A/B-скрипт сверил baseline/candidate побайтово. Первый прогон каждой версии
// — прогрев вне статистики.

#include "media/inflate.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Сценарии: name, reps, число отрезков.
typedef struct BenchCase
{
    const char *name;
    uint32_t reps;
} BenchCase;

static const BenchCase CASES[] = {
    {"stored_1m", 40u},
    {"fixed_text_1m", 60u},
    {"dynamic_text_1m", 60u},
    {"dynamic_photo_1m", 60u},
    {"png_like_1m", 60u},
    {"rle_dist1_1m", 40u},
    {"near_window_1m", 60u},
    {"near3_1m", 60u},
    {"mid_window_1m", 60u},
    {"mixed_1m", 200u},
    {"tiny_dynamic_256", 30000u},
    {"tiny_fixed_256", 30000u},
};
#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))

#define SPLIT_SEGMENTS 4u
#define MAX_OUTPUT (4u * 1024u * 1024u)

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

static bool ReadFixture(const char *directory, uint32_t directoryLength, const char *name,
                        const char *extension, uint8_t **outBytes, uint32_t *outSize)
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
    for (uint32_t index = 0; extension[index] != '\0' && at + 1u < sizeof(path); ++index)
    {
        path[at++] = extension[index];
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

static void Report(const char *name, const char *mode, uint32_t reps, int32_t status,
                   uint32_t written, uint64_t totalNs, uint64_t minNs, uint64_t checksum)
{
    WriteText("BENCH name=");
    WriteText(name);
    WriteText(" mode=");
    WriteText(mode);
    WriteText(" reps=");
    WriteU64(reps);
    WriteText(" status=");
    WriteU64((uint64_t)status);
    WriteText(" written=");
    WriteU64(written);
    WriteText(" ns_total=");
    WriteU64(totalNs);
    WriteText(" ns_min=");
    WriteU64(minNs);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText("\n");
}

// Возвращает false только при несоответствии выхода ожидаемому.
static bool RunCase(const char *directory, uint32_t directoryLength, const BenchCase *bench)
{
    uint8_t *stream = NULL;
    uint32_t streamBytes = 0u;
    uint8_t *expected = NULL;
    uint32_t expectedBytes = 0u;
    if (!ReadFixture(directory, directoryLength, bench->name, ".zlib", &stream, &streamBytes) ||
        !ReadFixture(directory, directoryLength, bench->name, ".raw", &expected, &expectedBytes))
    {
        WriteText("BENCH name=");
        WriteText(bench->name);
        WriteText(" MISSING\n");
        PlatformFree(stream);
        PlatformFree(expected);
        return true;
    }

    // Буферы как у реального вызывающего: выход ровно по распакованному
    // размеру, рабочая память — размер InflateWork.
    uint8_t *output = (uint8_t *)PlatformAllocate(MAX_OUTPUT, false);
    InflateWork *work = (InflateWork *)PlatformAllocate(sizeof(InflateWork), false);
    if (output == NULL || work == NULL)
    {
        WriteText("BENCH name=");
        WriteText(bench->name);
        WriteText(" ALLOC_FAIL\n");
        PlatformFree(output);
        PlatformFree(work);
        PlatformFree(stream);
        PlatformFree(expected);
        return true;
    }

    // Проверка корректности: один прогон в каждом режиме должен дать байт-в-байт
    // ожидаемое содержимое и тот же written.
    InflateSegment single = {stream, streamBytes};
    uint32_t written = 0u;
    ImageStatus status = InflateZlib(&single, 1u, output, MAX_OUTPUT, work, sizeof(InflateWork),
                                     &written);
    if (status != IMAGE_OK || written != expectedBytes ||
        memcmp(output, expected, expectedBytes) != 0)
    {
        WriteText("BENCH name=");
        WriteText(bench->name);
        WriteText(" SINGLE_MISMATCH status=");
        WriteU64((uint64_t)status);
        WriteText(" written=");
        WriteU64(written);
        WriteText(" expected=");
        WriteU64(expectedBytes);
        WriteText("\n");
        PlatformFree(output);
        PlatformFree(work);
        PlatformFree(stream);
        PlatformFree(expected);
        return false;
    }

    InflateSegment split[SPLIT_SEGMENTS];
    uint32_t cuts[SPLIT_SEGMENTS + 1u];
    cuts[0] = 0u;
    for (uint32_t index = 1u; index < SPLIT_SEGMENTS; ++index)
        cuts[index] = (uint32_t)((uint64_t)streamBytes * index / SPLIT_SEGMENTS);
    cuts[SPLIT_SEGMENTS] = streamBytes;
    for (uint32_t index = 0u; index < SPLIT_SEGMENTS; ++index)
    {
        split[index].bytes = stream + cuts[index];
        split[index].size = cuts[index + 1u] - cuts[index];
    }
    written = 0u;
    status = InflateZlib(split, SPLIT_SEGMENTS, output, MAX_OUTPUT, work, sizeof(InflateWork),
                         &written);
    if (status != IMAGE_OK || written != expectedBytes ||
        memcmp(output, expected, expectedBytes) != 0)
    {
        WriteText("BENCH name=");
        WriteText(bench->name);
        WriteText(" SPLIT_MISMATCH\n");
        PlatformFree(output);
        PlatformFree(work);
        PlatformFree(stream);
        PlatformFree(expected);
        return false;
    }

    // Прогрев обеих конфигураций: страницы, таблицы, кэш инструкций.
    for (uint32_t warm = 0u; warm < 2u; ++warm)
    {
        written = 0u;
        (void)InflateZlib(&single, 1u, output, MAX_OUTPUT, work, sizeof(InflateWork), &written);
        written = 0u;
        (void)InflateZlib(split, SPLIT_SEGMENTS, output, MAX_OUTPUT, work, sizeof(InflateWork),
                          &written);
    }

    for (uint32_t modeIndex = 0u; modeIndex < 2u; ++modeIndex)
    {
        const bool isSplit = modeIndex == 1u;
        const InflateSegment *segments = isSplit ? split : &single;
        uint32_t segmentCount = isSplit ? SPLIT_SEGMENTS : 1u;
        uint64_t totalNs = 0u;
        uint64_t minNs = UINT64_MAX;
        uint64_t checksum = 0xcbf29ce484222325ull;
        uint32_t lastWritten = 0u;
        int32_t lastStatus = 0;
        for (uint32_t run = 0u; run < bench->reps; ++run)
        {
            double started = PlatformMonotonicSeconds();
            uint32_t runWritten = 0u;
            ImageStatus runStatus = InflateZlib(segments, segmentCount, output, MAX_OUTPUT, work,
                                                sizeof(InflateWork), &runWritten);
            double finished = PlatformMonotonicSeconds();
            uint64_t ns = (uint64_t)((finished - started) * 1000000000.0);
            if (ns < minNs) minNs = ns;
            totalNs += ns;
            checksum = HashBytes(checksum, output, runWritten < 4096u ? runWritten : 4096u);
            checksum ^= ((uint64_t)(uint32_t)runStatus << 32) ^ runWritten;
            lastWritten = runWritten;
            lastStatus = (int32_t)runStatus;
        }
        Report(bench->name, isSplit ? "split" : "single", bench->reps, lastStatus, lastWritten,
               totalNs, minNs, checksum);
    }

    PlatformFree(output);
    PlatformFree(work);
    PlatformFree(stream);
    PlatformFree(expected);
    return true;
}

LAIUE_TEST_ENTRY(R2InflateBenchmarkEntryPoint)
{
    char directory[512];
    uint32_t directoryLength = PlatformGetEnvironmentUtf8("LAIUE_INFLATE_BENCH_DIR", directory,
                                                          (uint32_t)sizeof(directory));
    if (directoryLength == 0u)
    {
        WriteText("BENCH ERROR no LAIUE_INFLATE_BENCH_DIR\n");
        LaiueTestRuntimeExit(1);
    }

    for (uint32_t index = 0u; index < CASE_COUNT; ++index)
    {
        if (!RunCase(directory, directoryLength, &CASES[index]))
        {
            WriteText("r2 inflate benchmark: output mismatch\n");
            LaiueTestRuntimeExit(1);
        }
    }
    WriteText("r2 inflate benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
