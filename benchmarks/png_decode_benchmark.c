// Ручной A/B-стенд декодера PNG (inflate + разфильтровка + расширение).
//
// В ALL и CTest не входит: его запускают осознанно через
// LAIUE_BUILD_BENCHMARKS=ON. Читает детерминированные фикстуры из каталога
// LAIUE_PNG_BENCH_DIR, гоняет PngDecode фиксированное число раз
// (LAIUE_PNG_BENCH_REPS, по умолчанию 30) и печатает машиночитаемую строку
// на каждый файл:
//
//   BENCH <name> reps <n> avg_ns <int> min_ns <int> checksum <hex>
//
// Контрольная сумма FNV-1a64 по всему кадру печатается, чтобы компилятор не
// выбросил декодирование, а скрипт A/B мог сверить выход baseline и candidate.

#include "media/image.h"
#include "media/png_decode.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static const char *const FIXTURES[] = {
    "rgb_grad_1024",
    "rgb_noise_1024",
    "rgba_grad_1024",
    "rgba_photo_1024",
    "gray16_grad_1024",
    "palette8_1024",
    "palette4_1024",
    "interlaced_rgba_512",
    "stored_rgb_512",
    "fixed_rgb_512",
};
#define FIXTURE_COUNT (sizeof(FIXTURES) / sizeof(FIXTURES[0]))

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
    if (at + 4u < sizeof(path))
    {
        path[at++] = '.';
        path[at++] = 'p';
        path[at++] = 'n';
        path[at++] = 'g';
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

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static void RunFixture(const char *directory, uint32_t directoryLength, const char *name,
                       uint32_t reps)
{
    uint8_t *file = NULL;
    uint32_t fileBytes = 0u;
    if (!ReadFixture(directory, directoryLength, name, &file, &fileBytes))
    {
        WriteText("BENCH ");
        WriteText(name);
        WriteText(" MISSING\n");
        return;
    }

    ImageInfo info = {0};
    ImageStatus status = PngInspect(file, fileBytes, &info);
    if (status != IMAGE_OK)
    {
        WriteText("BENCH ");
        WriteText(name);
        WriteText(" INSPECT_FAIL\n");
        PlatformFree(file);
        return;
    }

    uint8_t *pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    void *scratch = PlatformAllocate(info.scratchBytes, false);
    if (pixels == NULL || scratch == NULL)
    {
        WriteText("BENCH ");
        WriteText(name);
        WriteText(" ALLOC_FAIL\n");
        PlatformFree(scratch);
        PlatformFree(pixels);
        PlatformFree(file);
        return;
    }

    // Прогрев: страницы и таблицы уже горячие к первому измеренному прогону.
    for (uint32_t warm = 0; warm < 3u; ++warm)
    {
        if (PngDecode(file, fileBytes, &info, pixels, info.pixelBytes, scratch,
                      info.scratchBytes) != IMAGE_OK)
        {
            WriteText("BENCH ");
            WriteText(name);
            WriteText(" DECODE_FAIL\n");
            PlatformFree(scratch);
            PlatformFree(pixels);
            PlatformFree(file);
            return;
        }
    }

    uint64_t checksum = 0xcbf29ce484222325ull;
    uint64_t minNs = UINT64_MAX;
    double totalSeconds = 0.0;
    for (uint32_t run = 0; run < reps; ++run)
    {
        double started = PlatformMonotonicSeconds();
        status = PngDecode(file, fileBytes, &info, pixels, info.pixelBytes, scratch,
                           info.scratchBytes);
        double finished = PlatformMonotonicSeconds();
        if (status != IMAGE_OK)
        {
            WriteText("BENCH ");
            WriteText(name);
            WriteText(" DECODE_FAIL\n");
            PlatformFree(scratch);
            PlatformFree(pixels);
            PlatformFree(file);
            return;
        }
        uint64_t ns = (uint64_t)((finished - started) * 1000000000.0);
        if (ns < minNs) minNs = ns;
        totalSeconds += finished - started;
        checksum = HashBytes(checksum, pixels, info.pixelBytes);
    }

    uint64_t avgNs = (uint64_t)((totalSeconds / (double)reps) * 1000000000.0);
    WriteText("BENCH ");
    WriteText(name);
    WriteText(" reps ");
    WriteU64(reps);
    WriteText(" avg_ns ");
    WriteU64(avgNs);
    WriteText(" min_ns ");
    WriteU64(minNs);
    WriteText(" checksum ");
    WriteHex(checksum);
    WriteText("\n");

    PlatformFree(scratch);
    PlatformFree(pixels);
    PlatformFree(file);
}

LAIUE_TEST_ENTRY(PngDecodeBenchmarkEntryPoint)
{
    char directory[512];
    uint32_t directoryLength = PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_DIR", directory,
                                                          (uint32_t)sizeof(directory));
    char repsText[32];
    uint32_t repsLength = PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_REPS", repsText,
                                                     (uint32_t)sizeof(repsText));
    uint32_t reps = 30u;
    if (repsLength != 0u)
    {
        uint32_t parsed = 0u;
        for (uint32_t index = 0; index < repsLength; ++index)
        {
            if (repsText[index] < '0' || repsText[index] > '9') break;
            parsed = parsed * 10u + (uint32_t)(repsText[index] - '0');
        }
        if (parsed != 0u) reps = parsed;
    }

    char filter[256];
    uint32_t filterLength = PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_ONLY", filter,
                                                       (uint32_t)sizeof(filter));

    if (directoryLength == 0u)
    {
        WriteText("BENCH ERROR no LAIUE_PNG_BENCH_DIR\n");
        LAIUE_TEST_SUCCESS();
    }

    for (uint32_t index = 0; index < FIXTURE_COUNT; ++index)
    {
        if (!NameMatches(FIXTURES[index], filter, filterLength)) continue;
        RunFixture(directory, directoryLength, FIXTURES[index], reps);
    }
    LAIUE_TEST_SUCCESS();
}
