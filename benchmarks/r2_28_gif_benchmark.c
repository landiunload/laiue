// Стенд GIF-декодера агента 28-gif: измеряет именно ImageDecode на заранее
// построенных входах. В ALL не входит и в CTest не регистрируется.
//
// Покрывает tiny, литеральный (код на пиксель), сжатый LZW, крупный кадр,
// чересстрочность, прозрачность с disposal 1/2/3 и частые clear-коды.
// Для полнокадровых непрозрачных случаев выход сверяется с ожидаемой
// картинкой по формуле фикстуры: если кодировщик фикстур или декодер
// разошлись, стенд падает до замера. Для всех случаев печатается контрольная
// сумма кадра, и A/B обязан увидеть одинаковые суммы у baseline и candidate.
//
// Число повторов на случай фиксировано (база на случай * R2_28_GIF_SCALE%),
// поэтому baseline и candidate выполняют ровно одну и ту же работу.

#include "media/image.h"

#include "platform/system.h"
#include "r2_28_gif_fixtures.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static volatile uint64_t g_sink;

static void WriteText(const char *text)
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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        buffer[2u + index] = digits[(value >> (60u - index * 4u)) & 0xFu];
    }
    buffer[18] = '\0';
    WriteText(buffer);
}

static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
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

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

#define R2_GIF_MAX_CASES 8u

typedef struct R2GifCase
{
    const char *name;
    R2GifParams params;
    uint32_t baseRepeats;
    uint8_t *file;
    uint32_t fileBytes;
    uint32_t capacity;
    ImageInfo info;
    uint8_t *pixels;
    void *scratch;
    uint64_t checksum;
} R2GifCase;

static R2GifCase g_cases[R2_GIF_MAX_CASES];
static uint32_t g_caseCount;

// Сверяет выход с формулой фикстуры (только полнокадровые непрозрачные случаи).
static bool VerifyPixels(const R2GifCase *testCase)
{
    if (testCase->params.transparent) return true;
    const R2GifParams *params = &testCase->params;
    for (uint32_t frame = 0u; frame < testCase->info.frameCount; ++frame)
    {
        const uint8_t *canvas = testCase->pixels + (size_t)frame * testCase->info.frameBytes;
        for (uint32_t y = 0u; y < params->height; ++y)
        {
            for (uint32_t x = 0u; x < params->width; ++x)
            {
                uint8_t color[3];
                R2GifPalette(R2GifValue(params, frame, x, y), color);
                const uint8_t *texel = canvas + ((size_t)y * params->width + x) * 4u;
                if (texel[0] != color[0] || texel[1] != color[1] || texel[2] != color[2] ||
                    texel[3] != 255u)
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool PrepareCase(const char *name, R2GifParams params, uint32_t baseRepeats,
                        uint32_t scalePercent)
{
    if (g_caseCount >= R2_GIF_MAX_CASES) return false;
    R2GifCase *testCase = &g_cases[g_caseCount];
    testCase->name = name;
    testCase->params = params;
    testCase->baseRepeats = baseRepeats;

    uint32_t capacity = params.width * params.height * params.frames * 2u + 65536u;
    testCase->capacity = capacity;
    testCase->file = (uint8_t *)PlatformAllocate(capacity, false);
    if (testCase->file == NULL) return false;
    testCase->fileBytes = R2GifBuild(testCase->file, capacity, &params);
    if (testCase->fileBytes == 0u)
    {
        WriteText("r2gif: fixture build failed for ");
        WriteText(name);
        WriteText("\n");
        return false;
    }

    ImageStatus inspected = ImageInspect(testCase->file, testCase->fileBytes, &testCase->info);
    if (inspected != IMAGE_OK)
    {
        WriteText("r2gif: inspect failed for ");
        WriteText(name);
        WriteText(" status=");
        WriteUnsigned((uint64_t)inspected);
        WriteText("\n");
        return false;
    }

    testCase->pixels = (uint8_t *)PlatformAllocate(testCase->info.pixelBytes, false);
    testCase->scratch = testCase->info.scratchBytes != 0u
                            ? PlatformAllocate(testCase->info.scratchBytes, false)
                            : NULL;
    if (testCase->pixels == NULL ||
        (testCase->info.scratchBytes != 0u && testCase->scratch == NULL))
    {
        WriteText("r2gif: allocation failed for ");
        WriteText(name);
        WriteText("\n");
        return false;
    }

    ImageStatus decoded = ImageDecode(testCase->file, testCase->fileBytes, &testCase->info,
                                      testCase->pixels, testCase->info.pixelBytes,
                                      testCase->scratch, testCase->info.scratchBytes);
    if (decoded != IMAGE_OK)
    {
        WriteText("r2gif: decode failed for ");
        WriteText(name);
        WriteText(" status=");
        WriteUnsigned((uint64_t)decoded);
        WriteText("\n");
        return false;
    }
    if (!VerifyPixels(testCase))
    {
        WriteText("r2gif: decoded pixels diverge from the fixture for ");
        WriteText(name);
        WriteText("\n");
        return false;
    }
    testCase->checksum = HashBytes(0xcbf29ce484222325ull, testCase->pixels,
                                   testCase->info.pixelBytes);
    (void)scalePercent;
    ++g_caseCount;
    return true;
}

static double RunCase(const R2GifCase *testCase, uint32_t repeats)
{
    const uint8_t *file = testCase->file;
    uint32_t fileBytes = testCase->fileBytes;
    const ImageInfo *info = &testCase->info;
    uint8_t *pixels = testCase->pixels;
    void *scratch = testCase->scratch;
    uint64_t accumulator = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
    {
        ImageDecode(file, fileBytes, info, pixels, info->pixelBytes, scratch, info->scratchBytes);
        accumulator += pixels[(repeat * 97u) % info->pixelBytes];
        accumulator += pixels[(repeat * 997u) % info->pixelBytes];
    }
    g_sink += accumulator;
    return PlatformMonotonicSeconds() - begin;
}

LAIUE_TEST_ENTRY(R2GifBenchmarkEntryPoint)
{
    uint32_t scalePercent = ReadEnvUnsigned("R2_28_GIF_SCALE", 100u, 100000u);

    // Прозрачный случай: движущийся прямоугольник, прозрачный индекс 0,
    // disposal по кругу 1/2/3 — композиция поверх холста.
    static const R2GifParams transparent = {.width = 256u,
                                            .height = 256u,
                                            .frames = 8u,
                                            .interlaced = false,
                                            .compressed = true,
                                            .clearPeriod = 0u,
                                            .transparent = true,
                                            .rectWidth = 96u,
                                            .rectHeight = 96u,
                                            .pattern = R2_GIF_NOISE,
                                            .seed = 7u};

    bool prepared =
        PrepareCase("tiny-8x8x1",
                    (R2GifParams){.width = 8u, .height = 8u, .frames = 1u, .pattern = R2_GIF_NOISE,
                                  .seed = 1u},
                    200000u, scalePercent) &&
        PrepareCase("literal-256x256x6",
                    (R2GifParams){.width = 256u, .height = 256u, .frames = 6u,
                                  .clearPeriod = 1000u, .pattern = R2_GIF_NOISE, .seed = 2u},
                    60u, scalePercent) &&
        PrepareCase("lzw-noise-256x256x6",
                    (R2GifParams){.width = 256u, .height = 256u, .frames = 6u,
                                  .compressed = true, .pattern = R2_GIF_NOISE, .seed = 3u},
                    60u, scalePercent) &&
        PrepareCase("lzw-blocks-512x512x2",
                    (R2GifParams){.width = 512u, .height = 512u, .frames = 2u,
                                  .compressed = true, .pattern = R2_GIF_BLOCKS, .seed = 4u},
                    40u, scalePercent) &&
        PrepareCase("lzw-large-1024x1024x1",
                    (R2GifParams){.width = 1024u, .height = 1024u, .frames = 1u,
                                  .compressed = true, .pattern = R2_GIF_GRADIENT, .seed = 5u},
                    24u, scalePercent) &&
        PrepareCase("interlaced-512x512x4",
                    (R2GifParams){.width = 512u, .height = 512u, .frames = 4u,
                                  .interlaced = true, .compressed = true,
                                  .pattern = R2_GIF_BLOCKS, .seed = 6u},
                    16u, scalePercent) &&
        PrepareCase("transparent-256x256x8", transparent, 80u, scalePercent) &&
        PrepareCase("reset-256x256x2",
                    (R2GifParams){.width = 256u, .height = 256u, .frames = 2u,
                                  .compressed = true, .clearPeriod = 64u,
                                  .pattern = R2_GIF_NOISE, .seed = 8u},
                    120u, scalePercent);
    if (!prepared)
    {
        WriteText("r2gif: a case could not be prepared\n");
        LaiueTestRuntimeExit(1);
    }

    for (uint32_t caseIndex = 0u; caseIndex < g_caseCount; ++caseIndex)
    {
        R2GifCase *testCase = &g_cases[caseIndex];
        uint32_t repeats = (testCase->baseRepeats * scalePercent) / 100u;
        if (repeats == 0u) repeats = 1u;

        WriteText("r2gif info case=");
        WriteText(testCase->name);
        WriteText(" file_bytes=");
        WriteUnsigned(testCase->fileBytes);
        WriteText(" pixel_bytes=");
        WriteUnsigned(testCase->info.pixelBytes);
        WriteText(" scratch_bytes=");
        WriteUnsigned(testCase->info.scratchBytes);
        WriteText(" repeats=");
        WriteUnsigned(repeats);
        WriteText(" checksum=");
        WriteHex(testCase->checksum);
        WriteText("\n");

        RunCase(testCase, repeats);   // прогрев вне статистики
        double seconds = RunCase(testCase, repeats);
        WriteText("r2gif case=");
        WriteText(testCase->name);
        WriteText(" repeats=");
        WriteUnsigned(repeats);
        WriteText(" total_ms=");
        WriteFixed(seconds * 1000.0);
        WriteText(" checksum=");
        WriteHex(testCase->checksum);
        WriteText("\n");
    }

    for (uint32_t index = 0u; index < g_caseCount; ++index)
    {
        PlatformFree(g_cases[index].file);
        PlatformFree(g_cases[index].pixels);
        PlatformFree(g_cases[index].scratch);
    }
    WriteText("r2gif done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
