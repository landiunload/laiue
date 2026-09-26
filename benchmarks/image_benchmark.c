// Ручной benchmark декодеров JPEG и GIF. В ALL не входит и в CTest не
// регистрируется: его запускают осознанно и читают глазами.
//
// Стенд меряет именно декодирование картинки в RGBA8. Фикстуры из
// tests/texc_fixtures.h маленькие, поэтому рядом с ними строятся
// синтетические входы: GIF собирается несжатым LZW (столько же кодов,
// сколько пикселей, — видно цену внутреннего цикла на пиксель), а
// baseline JPEG собирается прямо из квантованных коэффициентов, без
// цветового преобразования и без IDCT на стороне стенда. Так размер
// кадра и плотность ненулевых коэффициентов задаются явно, а не
// зависят от набора образцов.
//
// Каждый случай перед замером декодируется один раз и сверяется
// контрольная сумма кадра: она печатается, и A/B-скрипт обязан увидеть
// одну и ту же сумму у baseline и candidate. Иначе «ускорение» могло бы
// оказаться другой картинкой.
//
// Число повторов на выборку задаётся базовым значением случая и
// масштабируется переменной окружения LAIUE_IMAGE_BENCH_SCALE (в
// процентах, по умолчанию 100), число выборок — LAIUE_IMAGE_BENCH_SAMPLES
// (по умолчанию 1). Сам таймер — PlatformMonotonicSeconds.

#include "media/image.h"

#include "platform/system.h"
#include "test_runtime.h"
#include "texc_fixtures.h"

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

// Три знака после запятой: разница между вариантами здесь измеряется
// десятыми долями миллисекунды, и целые их стирают.
static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
    if (value > 1000000000.0)
    {
        value = 1000000000.0;
    }
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

// === Сборка байтов ===

typedef struct BenchBuffer
{
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    bool overflow;
} BenchBuffer;

static void BufByte(BenchBuffer *buffer, uint8_t value)
{
    if (buffer->size < buffer->capacity)
    {
        buffer->data[buffer->size++] = value;
    }
    else
    {
        buffer->overflow = true;
    }
}

static void BufU16Be(BenchBuffer *buffer, uint32_t value)
{
    BufByte(buffer, (uint8_t)(value >> 8));
    BufByte(buffer, (uint8_t)(value & 0xFFu));
}

static void BufU16Le(BenchBuffer *buffer, uint32_t value)
{
    BufByte(buffer, (uint8_t)(value & 0xFFu));
    BufByte(buffer, (uint8_t)(value >> 8));
}

static void BufBytes(BenchBuffer *buffer, const uint8_t *bytes, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index) BufByte(buffer, bytes[index]);
}

// === Синтетический baseline JPEG ===
//
// Один компонент, таблица квантования из единиц, категории кодируются
// ровно длиной: DC — четыре бита (коды 0..15), AC — восемь бит (код равен
// индексу символа в таблице). Канонический Хаффман из таких counts даёт
// коды именно в порядке значений, поэтому таблица кодирования совпадает
// с таблицей разбора декодера без дополнительных структур.

#define BENCH_JPEG_AC_VALUES 242u

static uint32_t BitCategory(int32_t value)
{
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    uint32_t category = 0u;
    while (magnitude != 0u)
    {
        ++category;
        magnitude >>= 1u;
    }
    return category;
}

static uint32_t BenchJpegAcCode(uint32_t symbol)
{
    if (symbol == 0u) return 0u;         // EOB
    if (symbol == 0xF0u) return 1u;      // ZRL
    uint32_t run = symbol >> 4u;
    uint32_t size = symbol & 15u;
    return 2u + run * 15u + (size - 1u);
}

typedef struct BenchEntropy
{
    BenchBuffer *out;
    uint32_t accumulator;
    uint32_t count;
} BenchEntropy;

static void EntropyBits(BenchEntropy *entropy, uint32_t value, uint32_t count)
{
    if (count == 0u) return;
    entropy->accumulator = (entropy->accumulator << count) | (value & ((1u << count) - 1u));
    entropy->count += count;
    while (entropy->count >= 8u)
    {
        entropy->count -= 8u;
        uint8_t byte = (uint8_t)((entropy->accumulator >> entropy->count) & 0xFFu);
        BufByte(entropy->out, byte);
        if (byte == 0xFFu) BufByte(entropy->out, 0x00u);
    }
}

static void EntropyFlush(BenchEntropy *entropy)
{
    while (entropy->count != 0u) EntropyBits(entropy, 1u, 1u);
}

static uint32_t NextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

// density — вероятность ненулевого коэффициента в тысячных (0 — только
// DC), amplitude — предел модуля коэффициента. Возвращает размер файла
// или ноль при переполнении буфера.
static uint32_t BuildSyntheticJpeg(uint8_t *out, uint32_t capacity, uint32_t width, uint32_t height,
                                   uint32_t density, uint32_t amplitude, uint32_t seed)
{
    BenchBuffer buffer = {.data = out, .capacity = capacity};
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xD8u);   // SOI

    // DQT: таблица 0, восемь бит, все делители единицы.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xDBu);
    BufU16Be(&buffer, 67u);
    BufByte(&buffer, 0x00u);
    for (uint32_t index = 0u; index < 64u; ++index) BufByte(&buffer, 1u);

    // SOF0: один компонент, 1x1, таблица квантования 0.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xC0u);
    BufU16Be(&buffer, 11u);
    BufByte(&buffer, 8u);
    BufU16Be(&buffer, height);
    BufU16Be(&buffer, width);
    BufByte(&buffer, 1u);
    BufByte(&buffer, 1u);
    BufByte(&buffer, 0x11u);
    BufByte(&buffer, 0u);

    // DHT DC: шестнадцать кодов длины четыре, значения 0..15.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xC4u);
    BufU16Be(&buffer, 35u);
    BufByte(&buffer, 0x00u);
    for (uint32_t length = 1u; length <= 16u; ++length) BufByte(&buffer, length == 4u ? 16u : 0u);
    for (uint32_t value = 0u; value < 16u; ++value) BufByte(&buffer, (uint8_t)value);

    // DHT AC: 242 кода длины восемь: EOB, ZRL и все пары (run, size).
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xC4u);
    BufU16Be(&buffer, 2u + 1u + 16u + BENCH_JPEG_AC_VALUES);
    BufByte(&buffer, 0x10u);
    for (uint32_t length = 1u; length <= 16u; ++length)
    {
        BufByte(&buffer, length == 8u ? (uint8_t)BENCH_JPEG_AC_VALUES : 0u);
    }
    BufByte(&buffer, 0x00u);
    BufByte(&buffer, 0xF0u);
    for (uint32_t run = 0u; run < 16u; ++run)
    {
        for (uint32_t size = 1u; size <= 15u; ++size)
        {
            BufByte(&buffer, (uint8_t)((run << 4u) | size));
        }
    }

    // SOS: один компонент, таблицы 0/0, весь спектр.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xDAu);
    BufU16Be(&buffer, 8u);
    BufByte(&buffer, 1u);
    BufByte(&buffer, 1u);
    BufByte(&buffer, 0x00u);
    BufByte(&buffer, 0u);
    BufByte(&buffer, 63u);
    BufByte(&buffer, 0u);

    BenchEntropy entropy = {.out = &buffer};
    int32_t previousDc = 0;
    uint32_t random = seed;
    uint32_t blockCount = (width / 8u) * (height / 8u);
    for (uint32_t block = 0u; block < blockCount; ++block)
    {
        int32_t dcValue = (int32_t)((block * 13u + seed) % 97u) - 48;
        int32_t difference = dcValue - previousDc;
        previousDc = dcValue;
        uint32_t category = BitCategory(difference);
        EntropyBits(&entropy, category, 4u);
        if (category != 0u)
        {
            int32_t magnitude =
                difference > 0 ? difference : difference + (int32_t)(1u << category) - 1;
            EntropyBits(&entropy, (uint32_t)magnitude, category);
        }

        uint32_t run = 0u;
        for (uint32_t zig = 1u; zig < 64u; ++zig)
        {
            bool nonzero = density != 0u && (NextRandom(&random) % 1000u) < density;
            if (!nonzero)
            {
                ++run;
                continue;
            }
            int32_t value =
                (int32_t)(NextRandom(&random) % (2u * amplitude + 1u)) - (int32_t)amplitude;
            if (value == 0) value = 1;
            uint32_t size = BitCategory(value);
            while (run > 15u)
            {
                EntropyBits(&entropy, BenchJpegAcCode(0xF0u), 8u);
                run -= 16u;
            }
            EntropyBits(&entropy, BenchJpegAcCode((run << 4u) | size), 8u);
            int32_t magnitude = value > 0 ? value : value + (int32_t)(1u << size) - 1;
            EntropyBits(&entropy, (uint32_t)magnitude, size);
            run = 0u;
        }
        // EOB нужен только если после последнего ненулевого коэффициента
        // остались нули: дойдя до позиции 63, декодер выходит из цикла
        // сам, и лишний EOB был бы прочитан как начало следующего блока.
        if (run > 0u) EntropyBits(&entropy, BenchJpegAcCode(0u), 8u);   // EOB
    }
    EntropyFlush(&entropy);

    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xD9u);   // EOI
    return buffer.overflow ? 0u : buffer.size;
}

// === Синтетический GIF ===
//
// Несжатый LZW: каждый пиксель — отдельный литеральный код, поэтому
// декодер проходит полный цикл на каждый пиксель. Раз в тысячу пикселей
// выдаётся clear: словарь не переполняется, а ширина кода остаётся
// предсказуемой. Кадр на весь холст, без прозрачности и без
// чересстрочности; disposal 1 — следующий кадр ложится поверх.

typedef struct GifBlockWriter
{
    BenchBuffer *out;
    uint8_t block[255];
    uint32_t count;
} GifBlockWriter;

static void GifBlockByte(GifBlockWriter *writer, uint8_t value)
{
    writer->block[writer->count++] = value;
    if (writer->count == 255u)
    {
        BufByte(writer->out, 255u);
        BufBytes(writer->out, writer->block, 255u);
        writer->count = 0u;
    }
}

static void GifBlockEnd(GifBlockWriter *writer)
{
    if (writer->count != 0u)
    {
        BufByte(writer->out, (uint8_t)writer->count);
        BufBytes(writer->out, writer->block, writer->count);
        writer->count = 0u;
    }
    BufByte(writer->out, 0u);
}

static void GifCode(GifBlockWriter *writer, uint32_t *accumulator, uint32_t *count, uint32_t value,
                    uint32_t codeBits)
{
    *accumulator |= (value & ((1u << codeBits) - 1u)) << *count;
    *count += codeBits;
    while (*count >= 8u)
    {
        GifBlockByte(writer, (uint8_t)(*accumulator & 0xFFu));
        *accumulator >>= 8u;
        *count -= 8u;
    }
}

static void BuildGifFrame(GifBlockWriter *writer, uint32_t width, uint32_t height,
                          uint32_t frameIndex)
{
    uint32_t accumulator = 0u;
    uint32_t count = 0u;
    uint32_t codeBits = 9u;
    uint32_t nextCode = 258u;
    uint32_t previous = 0xFFFFFFFFu;
    uint32_t sinceClear = 0u;
    uint32_t pixels = width * height;

    GifCode(writer, &accumulator, &count, 256u, codeBits);   // clear
    for (uint32_t index = 0u; index < pixels; ++index)
    {
        uint32_t value = (index * 7u + frameIndex * 53u + (index / width) * 13u) & 0xFFu;
        GifCode(writer, &accumulator, &count, value, codeBits);
        if (previous != 0xFFFFFFFFu)
        {
            ++nextCode;
            if (nextCode == (1u << codeBits) && codeBits < 12u) ++codeBits;
        }
        previous = value;
        if (++sinceClear == 1000u)
        {
            GifCode(writer, &accumulator, &count, 256u, codeBits);
            previous = 0xFFFFFFFFu;
            nextCode = 258u;
            codeBits = 9u;
            sinceClear = 0u;
        }
    }
    GifCode(writer, &accumulator, &count, 257u, codeBits);   // end
    if (count != 0u)
    {
        GifBlockByte(writer, (uint8_t)(accumulator & 0xFFu));
    }
}

static uint32_t BuildSyntheticGif(uint8_t *out, uint32_t capacity, uint32_t width, uint32_t height,
                                  uint32_t frameCount)
{
    BenchBuffer buffer = {.data = out, .capacity = capacity};
    static const char signature[] = "GIF89a";
    for (uint32_t index = 0u; index < 6u; ++index) BufByte(&buffer, (uint8_t)signature[index]);
    BufU16Le(&buffer, width);
    BufU16Le(&buffer, height);
    BufByte(&buffer, 0xF7u);   // глобальная палитра 256, цветовое разрешение 8
    BufByte(&buffer, 0u);
    BufByte(&buffer, 0u);
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        BufByte(&buffer, (uint8_t)index);
        BufByte(&buffer, (uint8_t)(255u - index));
        BufByte(&buffer, (uint8_t)((index * 3u) & 0xFFu));
    }

    for (uint32_t frame = 0u; frame < frameCount; ++frame)
    {
        // Graphic Control Extension: disposal 1 (оставить), без прозрачности.
        BufByte(&buffer, 0x21u);
        BufByte(&buffer, 0xF9u);
        BufByte(&buffer, 0x04u);
        BufByte(&buffer, 0x04u);
        BufU16Le(&buffer, 5u);
        BufByte(&buffer, 0u);
        BufByte(&buffer, 0u);

        // Image Descriptor: полный холст, глобальная палитра, без чересстрочности.
        BufByte(&buffer, 0x2Cu);
        BufU16Le(&buffer, 0u);
        BufU16Le(&buffer, 0u);
        BufU16Le(&buffer, width);
        BufU16Le(&buffer, height);
        BufByte(&buffer, 0u);

        BufByte(&buffer, 8u);   // минимальный размер кода
        GifBlockWriter writer = {.out = &buffer};
        BuildGifFrame(&writer, width, height, frame);
        GifBlockEnd(&writer);
    }
    BufByte(&buffer, 0x3Bu);   // trailer
    return buffer.overflow ? 0u : buffer.size;
}

// === Случаи ===

#define BENCH_MAX_CASES 8u
#define BENCH_MAX_SAMPLES 64u

typedef struct BenchCase
{
    const char *name;
    const uint8_t *file;
    uint32_t fileBytes;
    uint32_t repeats;
    bool ownsFile;
    ImageInfo info;
    uint8_t *pixels;
    void *scratch;
    uint64_t checksum;
    double samples[BENCH_MAX_SAMPLES];
    uint32_t sampleCount;
} BenchCase;

static BenchCase g_cases[BENCH_MAX_CASES];
static uint32_t g_caseCount;

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static bool PrepareCase(const char *name, const uint8_t *file, uint32_t fileBytes,
                        uint32_t repeats, bool ownsFile)
{
    if (g_caseCount >= BENCH_MAX_CASES) return false;
    BenchCase *testCase = &g_cases[g_caseCount];
    testCase->name = name;
    testCase->file = file;
    testCase->fileBytes = fileBytes;
    testCase->repeats = repeats;
    testCase->ownsFile = ownsFile;

    ImageInfo info = {0};
    ImageStatus inspect = ImageInspect(file, fileBytes, &info);
    if (inspect != IMAGE_OK)
    {
        WriteText("image benchmark: inspect failed for ");
        WriteText(name);
        WriteText(" status=");
        WriteUnsigned((uint64_t)inspect);
        WriteText(" bytes=");
        WriteUnsigned(fileBytes);
        WriteText("\n");
        return false;
    }
    testCase->info = info;
    testCase->pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    testCase->scratch =
        info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    if (testCase->pixels == NULL || (info.scratchBytes != 0u && testCase->scratch == NULL))
    {
        WriteText("image benchmark: allocation failed for ");
        WriteText(name);
        WriteText("\n");
        return false;
    }
    ImageStatus decoded =
        ImageDecode(file, fileBytes, &info, testCase->pixels, info.pixelBytes, testCase->scratch,
                    info.scratchBytes);
    if (decoded != IMAGE_OK)
    {
        WriteText("image benchmark: decode failed for ");
        WriteText(name);
        WriteText(" status=");
        WriteUnsigned((uint64_t)decoded);
        WriteText("\n");
        return false;
    }
    testCase->checksum = HashBytes(0xcbf29ce484222325ull, testCase->pixels, info.pixelBytes);
    ++g_caseCount;
    return true;
}

static void ReleaseCase(BenchCase *testCase)
{
    PlatformFree(testCase->pixels);
    PlatformFree(testCase->scratch);
}

static double RunCase(const BenchCase *testCase, uint32_t repeats)
{
    const uint8_t *file = testCase->file;
    uint32_t fileBytes = testCase->fileBytes;
    const ImageInfo *info = &testCase->info;
    uint8_t *pixels = testCase->pixels;
    void *scratch = testCase->scratch;
    uint32_t pixelBytes = info->pixelBytes;
    uint64_t accumulator = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
    {
        ImageDecode(file, fileBytes, info, pixels, pixelBytes, scratch, info->scratchBytes);
        // Чтение выборочных байтов не даёт оптимизатору выбросить запись
        // кадра: без него decode стал бы мёртвым кодом.
        accumulator += pixels[(repeat * 97u) % pixelBytes];
        accumulator += pixels[(repeat * 997u) % pixelBytes];
    }
    g_sink += accumulator;
    return PlatformMonotonicSeconds() - begin;
}

static double MedianOf(double *values, uint32_t count)
{
    if (count == 0u) return 0.0;
    for (uint32_t index = 1u; index < count; ++index)
    {
        double key = values[index];
        uint32_t position = index;
        while (position > 0u && values[position - 1u] > key)
        {
            values[position] = values[position - 1u];
            --position;
        }
        values[position] = key;
    }
    return values[count / 2u];
}

LAIUE_TEST_ENTRY(ImageBenchmarkEntryPoint)
{
    uint32_t samples = ReadEnvUnsigned("LAIUE_IMAGE_BENCH_SAMPLES", 1u, BENCH_MAX_SAMPLES);
    uint32_t scalePercent = ReadEnvUnsigned("LAIUE_IMAGE_BENCH_SCALE", 100u, 100000u);

    // GIF: 256x256, шесть кадров. Полный холст на кадр, поэтому копия
    // предыдущего кадра и цена нулевого прямоугольника видны как есть.
    const uint32_t gifWidth = 256u;
    const uint32_t gifHeight = 256u;
    const uint32_t gifFrames = 6u;
    uint32_t gifCapacity = 4u * 1024u * 1024u;
    uint8_t *gif = (uint8_t *)PlatformAllocate(gifCapacity, false);
    uint32_t gifBytes =
        gif != NULL ? BuildSyntheticGif(gif, gifCapacity, gifWidth, gifHeight, gifFrames) : 0u;

    // JPEG: 256x256, один компонент. Плотный случай — почти все
    // коэффициенты ненулевые (горячий Хаффман и ReadBits), плоский —
    // только DC (горячее обратное ДКП).
    const uint32_t jpegWidth = 256u;
    const uint32_t jpegHeight = 256u;
    uint32_t jpegCapacity = jpegWidth * jpegHeight * 8u + 65536u;
    uint8_t *jpegNoise = (uint8_t *)PlatformAllocate(jpegCapacity, false);
    uint8_t *jpegFlat = (uint8_t *)PlatformAllocate(jpegCapacity, false);
    uint32_t jpegNoiseBytes =
        jpegNoise != NULL
            ? BuildSyntheticJpeg(jpegNoise, jpegCapacity, jpegWidth, jpegHeight, 700u, 80u, 7u)
            : 0u;
    uint32_t jpegFlatBytes =
        jpegFlat != NULL
            ? BuildSyntheticJpeg(jpegFlat, jpegCapacity, jpegWidth, jpegHeight, 0u, 1u, 11u)
            : 0u;

    if (gifBytes == 0u || jpegNoiseBytes == 0u || jpegFlatBytes == 0u)
    {
        WriteText("image benchmark: synthetic input could not be built\n");
        LaiueTestRuntimeExit(1);
    }

    uint32_t gifRepeats = (40u * scalePercent) / 100u;
    uint32_t noiseRepeats = (300u * scalePercent) / 100u;
    uint32_t flatRepeats = (400u * scalePercent) / 100u;
    uint32_t fixtureRepeats = (20000u * scalePercent) / 100u;
    if (gifRepeats == 0u) gifRepeats = 1u;
    if (noiseRepeats == 0u) noiseRepeats = 1u;
    if (flatRepeats == 0u) flatRepeats = 1u;
    if (fixtureRepeats == 0u) fixtureRepeats = 1u;

    bool prepared =
        PrepareCase("gif-anim-256x256x6", gif, gifBytes, gifRepeats, true) &&
        PrepareCase("jpeg-synth-noise-256", jpegNoise, jpegNoiseBytes, noiseRepeats, true) &&
        PrepareCase("jpeg-synth-flat-256", jpegFlat, jpegFlatBytes, flatRepeats, true) &&
        PrepareCase("jpeg-fixture-baseline-16", JPEG_BASELINE_FILE,
                    (uint32_t)sizeof(JPEG_BASELINE_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-chroma-16", JPEG_CHROMA_FILE,
                    (uint32_t)sizeof(JPEG_CHROMA_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-subsampled-20x12", JPEG_SUBSAMPLED_FILE,
                    (uint32_t)sizeof(JPEG_SUBSAMPLED_FILE), fixtureRepeats, false);
    if (!prepared)
    {
        WriteText("image benchmark: a case could not be prepared\n");
        LaiueTestRuntimeExit(1);
    }

    for (uint32_t caseIndex = 0u; caseIndex < g_caseCount; ++caseIndex)
    {
        BenchCase *testCase = &g_cases[caseIndex];
        // Прогрев: страницы касаются, ветвления прогреваются.
        RunCase(testCase, testCase->repeats);
        for (uint32_t sample = 0u; sample < samples; ++sample)
        {
            double seconds = RunCase(testCase, testCase->repeats);
            testCase->samples[testCase->sampleCount++] = seconds * 1000.0;
            WriteText("imagebench case=");
            WriteText(testCase->name);
            WriteText(" sample=");
            WriteUnsigned(sample);
            WriteText(" ms=");
            WriteFixed(seconds * 1000.0);
            WriteText(" checksum=");
            WriteHex(testCase->checksum);
            WriteText("\n");
        }

        uint64_t after =
            HashBytes(0xcbf29ce484222325ull, testCase->pixels, testCase->info.pixelBytes);
        if (after != testCase->checksum)
        {
            WriteText("image benchmark: checksum changed during the run\n");
            LaiueTestRuntimeExit(1);
        }

        double sorted[BENCH_MAX_SAMPLES];
        for (uint32_t index = 0u; index < testCase->sampleCount; ++index)
        {
            sorted[index] = testCase->samples[index];
        }
        double median = MedianOf(sorted, testCase->sampleCount);
        WriteText("imagesummary case=");
        WriteText(testCase->name);
        WriteText(" samples=");
        WriteUnsigned(testCase->sampleCount);
        WriteText(" min_ms=");
        WriteFixed(sorted[0]);
        WriteText(" median_ms=");
        WriteFixed(median);
        WriteText(" max_ms=");
        WriteFixed(sorted[testCase->sampleCount - 1u]);
        WriteText(" checksum=");
        WriteHex(testCase->checksum);
        WriteText("\n");
    }

    for (uint32_t index = 0u; index < g_caseCount; ++index)
    {
        if (g_cases[index].ownsFile)
        {
            PlatformFree((void *)g_cases[index].file);
        }
        ReleaseCase(&g_cases[index]);
    }
    WriteText("image benchmark: done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
