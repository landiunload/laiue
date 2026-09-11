// Эквивалентность декодеров изображений: побайтовое совпадение выхода и
// совпадение статусов на усечённых потоках до и после ускорения
// Хаффмана.
//
// Эталонные контрольные суммы сняты с исходной, побитовой реализации
// (см. build/peer и docs/image_decoders_parallel_work.md) и зафиксированы
// здесь константами. Тест сверяет с ними новый разбор, а не сам с собой.
//
// Полная сумма считается по всему кадру RGBA и статусу. Сумма по
// префиксам обходит все длины усечения входного файла: для каждой
// длины копится статус Inspect, статус Decode и пиксели. Так ловится
// любое расхождение в том, сколько байт успел выдать декодер до отказа.

#include "media/image.h"

#include "platform/system.h"
#include "test_runtime.h"
#include "texc_fixtures.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("image decode test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

#define FNV_SEED 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t Mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    hash *= FNV_PRIME;
    return hash;
}

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= FNV_PRIME;
    }
    return hash;
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[20];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (uint32_t index = 0; index < 16u; ++index)
    {
        buffer[2u + index] = digits[(value >> (60u - index * 4u)) & 0xFu];
    }
    buffer[18] = '\0';
    LaiueTestRuntimeWrite(buffer);
}

static void Report(const char *label, uint64_t full, uint64_t prefix)
{
    LaiueTestRuntimeWrite("image decode ");
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(" full=");
    WriteHex(full);
    LaiueTestRuntimeWrite(" prefix=");
    WriteHex(prefix);
    LaiueTestRuntimeWrite("\n");
}

// Полный разбор: статус и все байты кадра.
static uint64_t FullHash(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *info)
{
    uint8_t *pixels = PlatformAllocate(info->pixelBytes, true);
    void *scratch = info->scratchBytes != 0u ? PlatformAllocate(info->scratchBytes, true) : NULL;
    Expect(pixels != NULL && (info->scratchBytes == 0u || scratch != NULL),
           "decode buffers could not be allocated");

    ImageStatus status =
        ImageDecode(file, sizeBytes, info, pixels, info->pixelBytes, scratch, info->scratchBytes);
    uint64_t hash = Mix(FNV_SEED, (uint64_t)status);
    if (status == IMAGE_OK) hash = HashBytes(hash, pixels, info->pixelBytes);

    PlatformFree(scratch);
    PlatformFree(pixels);
    return hash;
}

// Обход всех усечений входа. Буферы берутся под полный файл: у
// успешного префикса требования не больше.
static uint64_t PrefixHash(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *full)
{
    uint8_t *pixels = PlatformAllocate(full->pixelBytes, true);
    void *scratch = full->scratchBytes != 0u ? PlatformAllocate(full->scratchBytes, true) : NULL;
    Expect(pixels != NULL && (full->scratchBytes == 0u || scratch != NULL),
           "prefix buffers could not be allocated");

    uint64_t hash = FNV_SEED;
    for (uint32_t cut = 0u; cut <= sizeBytes; ++cut)
    {
        ImageInfo info = {0};
        ImageStatus inspected = ImageInspect(file, cut, &info);
        hash = Mix(hash, (uint64_t)inspected);
        if (inspected != IMAGE_OK) continue;

        for (uint32_t index = 0; index < full->pixelBytes; ++index) pixels[index] = 0u;
        ImageStatus decoded =
            ImageDecode(file, cut, &info, pixels, full->pixelBytes, scratch, full->scratchBytes);
        hash = Mix(hash, (uint64_t)decoded);
        if (decoded == IMAGE_OK) hash = HashBytes(hash, pixels, info.pixelBytes);
    }

    PlatformFree(scratch);
    PlatformFree(pixels);
    return hash;
}

typedef struct DecodeCase
{
    const char *label;
    const uint8_t *file;
    uint32_t sizeBytes;
    uint64_t expectedFull;
    uint64_t expectedPrefix;
} DecodeCase;

static void CheckCase(const DecodeCase *testCase)
{
    ImageInfo info = {0};
    Expect(ImageInspect(testCase->file, testCase->sizeBytes, &info) == IMAGE_OK,
           "a reference fixture must be inspected");

    uint64_t full = FullHash(testCase->file, testCase->sizeBytes, &info);
    uint64_t prefix = PrefixHash(testCase->file, testCase->sizeBytes, &info);
    Report(testCase->label, full, prefix);
    if (testCase->expectedFull != 0u)
    {
        Expect(full == testCase->expectedFull, "the full decode must match the reference");
    }
    if (testCase->expectedPrefix != 0u)
    {
        Expect(prefix == testCase->expectedPrefix,
               "the truncated decodes must match the reference");
    }
}

LAIUE_TEST_ENTRY(ImageDecodeTestEntryPoint)
{
    // PNG: разбор точный, статусы и байты обязаны совпасть. JPEG: обратное
    // ДКП своё, но при неизменной арифметике выход не меняется вовсе, и
    // эталоны сняты с того же кода.
    static const DecodeCase cases[] = {
        {"png-rgba", PNG_RGBA_FILE, (uint32_t)sizeof(PNG_RGBA_FILE), 0x0529eda670a0ad7full,
         0xb13fde1952d9b2aaull},
        {"png-palette", PNG_PALETTE_FILE, (uint32_t)sizeof(PNG_PALETTE_FILE),
         0x59340f128dc0fa2eull, 0x7c011b9cec159fbeull},
        {"png-gray16", PNG_GRAY16_FILE, (uint32_t)sizeof(PNG_GRAY16_FILE), 0x3373fcd40edcfd87ull,
         0x171a7e5fd18aed90ull},
        {"png-interlaced", PNG_INTERLACED_FILE, (uint32_t)sizeof(PNG_INTERLACED_FILE),
         0x147253fd8afa99dfull, 0x6e3d3b04db477549ull},
        {"png-solid", PNG_SOLID_FILE, (uint32_t)sizeof(PNG_SOLID_FILE), 0x5b638429cd93565full,
         0x20613380c942052full},
        {"jpeg-baseline", JPEG_BASELINE_FILE, (uint32_t)sizeof(JPEG_BASELINE_FILE),
         0x5c5a0439d209ee47ull, 0xbe8fd95dc2468784ull},
        {"jpeg-progressive", JPEG_PROGRESSIVE_FILE, (uint32_t)sizeof(JPEG_PROGRESSIVE_FILE),
         0x5c5a0439d209ee47ull, 0xde5b9d197b9d5cc7ull},
        {"jpeg-subsampled", JPEG_SUBSAMPLED_FILE, (uint32_t)sizeof(JPEG_SUBSAMPLED_FILE),
         0x4e5d41515746edb5ull, 0x03cae46207111cdaull},
        {"jpeg-restart", JPEG_RESTART_FILE, (uint32_t)sizeof(JPEG_RESTART_FILE),
         0x4e5d41515746edb5ull, 0x4de85a41cd1e4638ull},
        {"jpeg-chroma", JPEG_CHROMA_FILE, (uint32_t)sizeof(JPEG_CHROMA_FILE),
         0x6009a994666605bfull, 0x607ed3db27d06efaull},
        {"jpeg-solid", JPEG_SOLID_FILE, (uint32_t)sizeof(JPEG_SOLID_FILE), 0x80422d07754a4ddfull,
         0xd41bb96eca661d09ull},
        {"jpeg-gray", JPEG_GRAY_FILE, (uint32_t)sizeof(JPEG_GRAY_FILE), 0x2d9ffcd83d82a660ull,
         0x4834b9c795f53c87ull},
    };

    for (uint32_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        CheckCase(&cases[index]);
    }

    LaiueTestRuntimeWrite("image decode test passed\n");
    LAIUE_TEST_SUCCESS();
}
