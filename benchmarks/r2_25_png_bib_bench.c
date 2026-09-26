// Внутрибинарный A/B PNG-декодера: базовая (закоммиченная) реализация
// вкомпилирована рядом с текущей production в ОДИН процесс. Поэтому inflate,
// платформенный слой и раскладка бинарника общие, и измеряется именно эффект
// правок png_decode.c, а не сдвиг кода между разными exe.
//
// Фикстуры те же имена, что у laiue_png_decode_benchmark, каталог задаёт
// LAIUE_PNG_BENCH_DIR. Печатает на файл:
//
//   BIB <name> reps <n> base_avg_ns <int> cand_avg_ns <int> base_min_ns <int>
//       cand_min_ns <int> checksum <hex>
//
// Контрольные суммы обеих реализаций обязаны совпасть.

#include "media/image.h"
#include "media/png_decode.h"

#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

ImageStatus PngInspectBaseline(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo);
ImageStatus PngDecodeBaseline(const void *bytes, uint32_t sizeBytes, const ImageInfo *info,
                              void *pixels, uint32_t pixelBytes, void *scratch,
                              uint32_t scratchBytes);

#define PngInspect PngInspectBaseline
#define PngDecode PngDecodeBaseline
#include "r2_25_png_baseline.inc"
#undef PngInspect
#undef PngDecode

static const char *const FIXTURES[] = {
    "rgb_grad_1024",        "rgb_noise_1024",       "rgba_grad_1024",
    "rgba_photo_1024",      "gray16_grad_1024",     "palette8_1024",
    "palette4_1024",        "interlaced_rgba_512",  "stored_rgb_512",
    "fixed_rgb_512",
};
#define FIXTURE_COUNT (sizeof(FIXTURES) / sizeof(FIXTURES[0]))

typedef ImageStatus (*InspectFn)(const void *, uint32_t, ImageInfo *);
typedef ImageStatus (*DecodeFn)(const void *, uint32_t, const ImageInfo *, void *, uint32_t, void *,
                                uint32_t);

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
        path[at++] = name[index];
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
        WriteText("BIB ");
        WriteText(name);
        WriteText(" MISSING\n");
        return;
    }

    ImageInfo info = {0};
    if (PngInspect(file, fileBytes, &info) != IMAGE_OK)
    {
        WriteText("BIB ");
        WriteText(name);
        WriteText(" INSPECT_FAIL\n");
        PlatformFree(file);
        return;
    }

    uint8_t *pixelsBase = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    void *scratchBase = PlatformAllocate(info.scratchBytes, false);
    uint8_t *pixelsCand = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    void *scratchCand = PlatformAllocate(info.scratchBytes, false);
    if (pixelsBase == NULL || scratchBase == NULL || pixelsCand == NULL || scratchCand == NULL)
    {
        WriteText("BIB ");
        WriteText(name);
        WriteText(" ALLOC_FAIL\n");
        PlatformFree(scratchCand);
        PlatformFree(pixelsCand);
        PlatformFree(scratchBase);
        PlatformFree(pixelsBase);
        PlatformFree(file);
        return;
    }

    volatile DecodeFn decodeBase = PngDecodeBaseline;
    volatile DecodeFn decodeCand = PngDecode;

    for (uint32_t warm = 0; warm < 3u; ++warm)
    {
        if (decodeBase(file, fileBytes, &info, pixelsBase, info.pixelBytes, scratchBase,
                       info.scratchBytes) != IMAGE_OK ||
            decodeCand(file, fileBytes, &info, pixelsCand, info.pixelBytes, scratchCand,
                       info.scratchBytes) != IMAGE_OK)
        {
            WriteText("BIB ");
            WriteText(name);
            WriteText(" DECODE_FAIL\n");
            PlatformFree(scratchCand);
            PlatformFree(pixelsCand);
            PlatformFree(scratchBase);
            PlatformFree(pixelsBase);
            PlatformFree(file);
            return;
        }
    }

    uint64_t checksumBase = 0xcbf29ce484222325ull;
    uint64_t checksumCand = 0xcbf29ce484222325ull;
    uint64_t baseMin = UINT64_MAX;
    uint64_t candMin = UINT64_MAX;
    double baseTotal = 0.0;
    double candTotal = 0.0;

    for (uint32_t run = 0; run < reps; ++run)
    {
        bool baseFirst = (run & 1u) == 0u;
        for (uint32_t stage = 0; stage < 2u; ++stage)
        {
            bool doBase = baseFirst ? (stage == 0u) : (stage == 1u);
            double started = PlatformMonotonicSeconds();
            ImageStatus status =
                doBase ? decodeBase(file, fileBytes, &info, pixelsBase, info.pixelBytes,
                                    scratchBase, info.scratchBytes)
                       : decodeCand(file, fileBytes, &info, pixelsCand, info.pixelBytes,
                                    scratchCand, info.scratchBytes);
            double finished = PlatformMonotonicSeconds();
            if (status != IMAGE_OK)
            {
                WriteText("BIB ");
                WriteText(name);
                WriteText(" DECODE_FAIL\n");
                PlatformFree(scratchCand);
                PlatformFree(pixelsCand);
                PlatformFree(scratchBase);
                PlatformFree(pixelsBase);
                PlatformFree(file);
                return;
            }
            uint64_t ns = (uint64_t)((finished - started) * 1000000000.0);
            if (doBase)
            {
                if (ns < baseMin) baseMin = ns;
                baseTotal += finished - started;
            }
            else
            {
                if (ns < candMin) candMin = ns;
                candTotal += finished - started;
            }
        }
        checksumBase = HashBytes(checksumBase, pixelsBase, info.pixelBytes);
        checksumCand = HashBytes(checksumCand, pixelsCand, info.pixelBytes);
    }

    uint64_t baseAvg = (uint64_t)((baseTotal / (double)reps) * 1000000000.0);
    uint64_t candAvg = (uint64_t)((candTotal / (double)reps) * 1000000000.0);

    WriteText("BIB ");
    WriteText(name);
    WriteText(" reps ");
    WriteU64(reps);
    WriteText(" base_avg_ns ");
    WriteU64(baseAvg);
    WriteText(" cand_avg_ns ");
    WriteU64(candAvg);
    WriteText(" base_min_ns ");
    WriteU64(baseMin);
    WriteText(" cand_min_ns ");
    WriteU64(candMin);
    WriteText(" checksum ");
    WriteHex(checksumBase);
    WriteText(checksumBase == checksumCand ? " equal\n" : " MISMATCH\n");

    PlatformFree(scratchCand);
    PlatformFree(pixelsCand);
    PlatformFree(scratchBase);
    PlatformFree(pixelsBase);
    PlatformFree(file);
}

LAIUE_TEST_ENTRY(R2PngBibBenchmarkEntryPoint)
{
    char directory[512];
    uint32_t directoryLength =
        PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_DIR", directory, (uint32_t)sizeof(directory));
    char repsText[32];
    uint32_t repsLength =
        PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_REPS", repsText, (uint32_t)sizeof(repsText));
    uint32_t reps = 120u;
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
    uint32_t filterLength =
        PlatformGetEnvironmentUtf8("LAIUE_PNG_BENCH_ONLY", filter, (uint32_t)sizeof(filter));

    if (directoryLength == 0u)
    {
        WriteText("BIB ERROR no LAIUE_PNG_BENCH_DIR\n");
        LAIUE_TEST_SUCCESS();
    }

    for (uint32_t index = 0; index < FIXTURE_COUNT; ++index)
    {
        if (!NameMatches(FIXTURES[index], filter, filterLength)) continue;
        RunFixture(directory, directoryLength, FIXTURES[index], reps);
    }
    LAIUE_TEST_SUCCESS();
}
