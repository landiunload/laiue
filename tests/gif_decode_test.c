// Регрессия GIF-декодера: побайтовое совпадение выхода и
// совпадение статусов на всех усечениях входа. Эталоны сняты с baseline
// (исходная реализация src/media/gif_decode.c) и зафиксированы
// константами — тест сверяет оптимизированный разбор не сам с собой.
//
// Случаи: tiny, литеральный LZW, сжатый LZW, длинные цепочки, чересстрочный,
// прозрачный с disposal 1/2/3, частые clear-коды и штатные фикстуры
// texc_fixtures.h. Все входы малы, поэтому обходятся все длины усечения.

#include "media/gif_decode.h"
#include "media/image.h"

#include "platform/system.h"
#include "gif_fixtures.h"
#include "test_runtime.h"
#include "texc_fixtures.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("gif decode test failed: ");
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
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        buffer[2u + index] = digits[(value >> (60u - index * 4u)) & 0xFu];
    }
    buffer[18] = '\0';
    LaiueTestRuntimeWrite(buffer);
}

static uint64_t FullHash(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *info)
{
    uint8_t *pixels = (uint8_t *)PlatformAllocate(info->pixelBytes, true);
    void *scratch =
        info->scratchBytes != 0u ? PlatformAllocate(info->scratchBytes, true) : NULL;
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

static uint64_t PrefixHash(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *full)
{
    uint8_t *pixels = (uint8_t *)PlatformAllocate(full->pixelBytes, true);
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

static bool SameBytes(const uint8_t *first, const uint8_t *second, uint32_t size)
{
    for (uint32_t index = 0u; index < size; ++index)
    {
        if (first[index] != second[index])
            return false;
    }
    return true;
}

// Поток обязан выдать те же кадры и задержки, что и распаковка всего файла, а за
// последним кадром снова первый.
static void CheckStream(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *info)
{
    uint8_t *pixels = (uint8_t *)PlatformAllocate(info->pixelBytes, true);
    void *scratch = PlatformAllocate(info->scratchBytes, true);
    const uint32_t streamBytes = GifStreamScratchBytes(info);
    void *stream = PlatformAllocate(streamBytes, true);
    uint8_t *canvas = (uint8_t *)PlatformAllocate(info->frameBytes, true);
    Expect(pixels != NULL && scratch != NULL && stream != NULL && canvas != NULL,
           "stream buffers could not be allocated");
    Expect(ImageDecode(file, sizeBytes, info, pixels, info->pixelBytes, scratch,
                       info->scratchBytes) == IMAGE_OK,
           "a reference fixture must decode");
    Expect(GifStreamOpen(file, sizeBytes, info, stream, streamBytes - 1u) == IMAGE_BUFFER_TOO_SMALL,
           "the stream must refuse a short buffer");
    Expect(GifStreamOpen(file, sizeBytes, info, stream, streamBytes) == IMAGE_OK,
           "the stream must open a fixture");
    for (uint32_t pass = 0u; pass < 3u; ++pass)
    {
        for (uint32_t frame = 0u; frame < info->frameCount; ++frame)
        {
            uint32_t delay = 0u;
            Expect(GifStreamNext(stream, canvas, &delay) == IMAGE_OK,
                   "the stream must give every frame");
            Expect(SameBytes(canvas, pixels + (size_t)frame * info->frameBytes, info->frameBytes),
                   "the stream must give the same frame as the whole decode");
            Expect(info->frameCount == 1u || delay == info->frameMilliseconds[frame],
                   "the stream must give the delay of the frame");
        }
    }
    PlatformFree(canvas);
    PlatformFree(stream);
    PlatformFree(scratch);
    PlatformFree(pixels);
}

// На любом усечении файла поток не выходит за буферы, а файл, который ещё читается,
// не перестаёт показываться из-за испорченного хвоста.
static void CheckStreamTruncation(const uint8_t *file, uint32_t sizeBytes)
{
    for (uint32_t cut = 0u; cut <= sizeBytes; ++cut)
    {
        ImageInfo info = {0};
        if (ImageInspect(file, cut, &info) != IMAGE_OK)
            continue;
        const uint32_t streamBytes = GifStreamScratchBytes(&info);
        void *stream = PlatformAllocate(streamBytes, true);
        uint8_t *canvas = (uint8_t *)PlatformAllocate(info.frameBytes, true);
        Expect(stream != NULL && canvas != NULL, "stream buffers could not be allocated");
        if (GifStreamOpen(file, cut, &info, stream, streamBytes) == IMAGE_OK)
        {
            uint32_t delay = 0u;
            ImageStatus status = GifStreamNext(stream, canvas, &delay);
            for (uint32_t frame = 0u; status == IMAGE_OK && frame < 2u * info.frameCount; ++frame)
            {
                status = GifStreamNext(stream, canvas, &delay);
            }
            Expect(status == IMAGE_OK || delay == 0u,
                   "only an unreadable first frame may stop the stream");
        }
        PlatformFree(canvas);
        PlatformFree(stream);
    }
}

// Длина показа не упирается в предел кадров ImageInfo.
#define LONG_STREAM_FRAMES 300u
static uint8_t s_longFile[65536];
static uint64_t s_longHashes[LONG_STREAM_FRAMES];

static void CheckLongStream(void)
{
    const GenGifParams params = {.width = 4u,
                                 .height = 4u,
                                 .frames = LONG_STREAM_FRAMES,
                                 .pattern = GEN_GIF_NOISE,
                                 .seed = 9u};
    const uint32_t size = GenGifBuild(s_longFile, (uint32_t)sizeof(s_longFile), &params);
    Expect(size != 0u, "a long fixture could not be built");
    ImageInfo info = {0};
    Expect(ImageInspect(s_longFile, size, &info) == IMAGE_OK && info.frameCount == IMAGE_MAX_FRAMES,
           "the whole decode stops at its limit of frames");
    const uint32_t streamBytes = GifStreamScratchBytes(&info);
    void *stream = PlatformAllocate(streamBytes, true);
    uint8_t *canvas = (uint8_t *)PlatformAllocate(info.frameBytes, true);
    Expect(stream != NULL && canvas != NULL, "stream buffers could not be allocated");
    Expect(GifStreamOpen(s_longFile, size, &info, stream, streamBytes) == IMAGE_OK,
           "the stream must open a long fixture");
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        for (uint32_t frame = 0u; frame < LONG_STREAM_FRAMES; ++frame)
        {
            uint32_t delay = 0u;
            Expect(GifStreamNext(stream, canvas, &delay) == IMAGE_OK,
                   "the stream must go on past the limit of frames");
            const uint64_t hash = HashBytes(FNV_SEED, canvas, info.frameBytes);
            if (pass == 0u)
            {
                s_longHashes[frame] = hash;
            }
            else
            {
                Expect(hash == s_longHashes[frame], "the long fixture must repeat from its start");
            }
        }
    }
    Expect(s_longHashes[IMAGE_MAX_FRAMES] != s_longHashes[0],
           "the frame past the limit is its own, not the first one again");
    PlatformFree(canvas);
    PlatformFree(stream);
}

typedef struct GenGifDecodeCase
{
    const char *label;
    const uint8_t *file;
    uint32_t sizeBytes;
    uint64_t expectedFull;
    uint64_t expectedPrefix;
    bool ownsFile;
} GenGifDecodeCase;

// Все входы строятся генератором фикстур; буфер живёт до конца теста.
#define GEN_CASE_CAPACITY 65536u
static uint8_t s_caseBuffers[8][GEN_CASE_CAPACITY];

static void CheckCase(const GenGifDecodeCase *testCase)
{
    ImageInfo info = {0};
    Expect(ImageInspect(testCase->file, testCase->sizeBytes, &info) == IMAGE_OK,
           "a reference fixture must be inspected");

    uint64_t full = FullHash(testCase->file, testCase->sizeBytes, &info);
    uint64_t prefix = PrefixHash(testCase->file, testCase->sizeBytes, &info);
    CheckStream(testCase->file, testCase->sizeBytes, &info);
    CheckStreamTruncation(testCase->file, testCase->sizeBytes);

    LaiueTestRuntimeWrite("gif decode ");
    LaiueTestRuntimeWrite(testCase->label);
    LaiueTestRuntimeWrite(" full=");
    WriteHex(full);
    LaiueTestRuntimeWrite(" prefix=");
    WriteHex(prefix);
    LaiueTestRuntimeWrite("\n");

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

LAIUE_TEST_ENTRY(GifDecodeTestEntryPoint)
{
    const GenGifParams tiny = {.width = 8u, .height = 8u, .frames = 1u, .seed = 1u};
    const GenGifParams literal = {.width = 32u,
                                  .height = 32u,
                                  .frames = 3u,
                                  .clearPeriod = 256u,
                                  .pattern = GEN_GIF_NOISE,
                                  .seed = 2u};
    const GenGifParams compressed = {.width = 32u,
                                     .height = 32u,
                                     .frames = 3u,
                                     .compressed = true,
                                     .pattern = GEN_GIF_NOISE,
                                     .seed = 3u};
    const GenGifParams blocks = {.width = 48u,
                                 .height = 32u,
                                 .frames = 2u,
                                 .compressed = true,
                                 .pattern = GEN_GIF_BLOCKS,
                                 .seed = 4u};
    const GenGifParams interlaced = {.width = 32u,
                                     .height = 32u,
                                     .frames = 2u,
                                     .interlaced = true,
                                     .compressed = true,
                                     .pattern = GEN_GIF_BLOCKS,
                                     .seed = 5u};
    const GenGifParams transparent = {.width = 32u,
                                      .height = 32u,
                                      .frames = 6u,
                                      .compressed = true,
                                      .transparent = true,
                                      .rectWidth = 12u,
                                      .rectHeight = 12u,
                                      .pattern = GEN_GIF_NOISE,
                                      .seed = 6u};
    const GenGifParams reset = {.width = 32u,
                                .height = 32u,
                                .frames = 2u,
                                .compressed = true,
                                .clearPeriod = 16u,
                                .pattern = GEN_GIF_GRADIENT,
                                .seed = 7u};

    const GenGifParams *params[7] = {&tiny,       &literal,     &compressed, &blocks,
                                     &interlaced, &transparent, &reset};
    const char *labels[7] = {"tiny-8x8x1",     "literal-32x32x3", "lzw-noise-32x32x3",
                             "lzw-blocks-48x32", "interlaced-32x32", "transparent-32x32",
                             "reset-32x32"};

    // Эталоны сняты с baseline (исходная реализация gif_decode.c).
    static const uint64_t expectedFull[9] = {
        0x0c80b870af92f1cfull, 0x6be3897385de7b5bull, 0xc6b362f11b3ec652ull,
        0xaa2e1fd4628b03dfull, 0x8be646491d6662dfull, 0xf7487724b192af16ull,
        0x97be306cb00c2bdfull, 0x86a6c339a1439e25ull, 0xb33611372ff8f19full,
    };
    static const uint64_t expectedPrefix[9] = {
        0x81fdd9fae640910cull, 0x32613d3faa124ab9ull, 0x3e010eb401d6094bull,
        0xb7ac4c69abf70c10ull, 0x7cad4e9c0eb8ed78ull, 0x2f89705af2fa948full,
        0x2fd40fd2baf38740ull, 0x1cd9bc081cf44c34ull, 0xafc087d971a3823cull,
    };

    GenGifDecodeCase cases[9];
    uint32_t caseCount = 0u;
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        uint32_t size = GenGifBuild(s_caseBuffers[index], GEN_CASE_CAPACITY, params[index]);
        Expect(size != 0u, "a generated fixture could not be built");
        cases[caseCount].label = labels[index];
        cases[caseCount].file = s_caseBuffers[index];
        cases[caseCount].sizeBytes = size;
        cases[caseCount].expectedFull = expectedFull[caseCount];
        cases[caseCount].expectedPrefix = expectedPrefix[caseCount];
        cases[caseCount].ownsFile = false;
        ++caseCount;
    }

    cases[caseCount].label = "fixture-animated";
    cases[caseCount].file = GIF_ANIMATED_FILE;
    cases[caseCount].sizeBytes = (uint32_t)sizeof(GIF_ANIMATED_FILE);
    cases[caseCount].expectedFull = expectedFull[caseCount];
    cases[caseCount].expectedPrefix = expectedPrefix[caseCount];
    cases[caseCount].ownsFile = false;
    ++caseCount;

    cases[caseCount].label = "fixture-variable";
    cases[caseCount].file = GIF_VARIABLE_FILE;
    cases[caseCount].sizeBytes = (uint32_t)sizeof(GIF_VARIABLE_FILE);
    cases[caseCount].expectedFull = expectedFull[caseCount];
    cases[caseCount].expectedPrefix = expectedPrefix[caseCount];
    cases[caseCount].ownsFile = false;
    ++caseCount;

    for (uint32_t index = 0u; index < caseCount; ++index)
    {
        CheckCase(&cases[index]);
    }
    CheckLongStream();

    LaiueTestRuntimeWrite("gif decode test passed\n");
    LAIUE_TEST_SUCCESS();
}
