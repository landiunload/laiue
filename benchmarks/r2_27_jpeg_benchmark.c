// Ручной A/B-стенд декодера JPEG (IDCT, де-квантование, разбор Хаффмана,
// цветовое преобразование и восстановление прореженной цветности).
//
// В ALL и CTest не входит: запускается руками через LAIUE_BUILD_BENCHMARKS=ON
// и читается A/B-скриптом. Стенд детерминирован: вход либо собирается в
// памяти синтетическим энкодером с фиксированным seed, либо берётся из
// tests/texc_fixtures.h. Контрольная сумма кадра печатается, поэтому baseline
// и candidate обязаны дать одну и ту же сумму, иначе «ускорение» — это другая
// картинка.
//
// Формат строки на выборку:
//   R2JPEG case=<name> reps=<n> ms=<fixed> checksum=<hex>
// Проверка усечений (статусы + частичные пиксели) печатает:
//   R2JPEG malformed=<name> checksum=<hex>
//
// Число повторов масштабируется LAIUE_R2_27_JPEG_SCALE (проценты, 100 по
// умолчанию), число выборок на случай — LAIUE_R2_27_JPEG_SAMPLES (1).

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

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0u; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
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

static void BufBytes(BenchBuffer *buffer, const uint8_t *bytes, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index) BufByte(buffer, bytes[index]);
}

// === Синтетический baseline JPEG ===
//
// Таблицы квантования — из единиц. DC кодируется ровно категорией (четыре
// бита, коды 0..15), AC — восемью битами (код равен индексу символа в
// канонической таблице). При таких counts канонический Хаффман совпадает с
// разбором декодера без дополнительных структур. Кодировщик служит только
// для того, чтобы получить валидный и детерминированный вход; достоверность
// пикселей не требуется, важна воспроизводимость и покрытие путей декодера.

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
    if (symbol == 0u) return 0u;      // EOB
    if (symbol == 0xF0u) return 1u;   // ZRL
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

typedef struct BenchComponentSpec
{
    uint32_t horizontal;
    uint32_t vertical;
    uint32_t density;    // вероятность ненулевого AC в тысячных
    uint32_t amplitude;  // предел модуля AC
} BenchComponentSpec;

// Кодирует один блок: DC-разность с предсказанием, затем AC. Порядок
// обхода коэффициентов — зигзаг, как в потоке.
static void EncodeBlock(BenchEntropy *entropy, uint32_t *random, int32_t *prediction,
                        uint32_t blockIndex, uint32_t density, uint32_t amplitude, uint32_t tag)
{
    int32_t dcValue = (int32_t)((blockIndex * 13u + tag) % 97u) - 48;
    int32_t difference = dcValue - *prediction;
    *prediction = dcValue;
    uint32_t category = BitCategory(difference);
    EntropyBits(entropy, category, 4u);
    if (category != 0u)
    {
        int32_t magnitude =
            difference > 0 ? difference : difference + (int32_t)(1u << category) - 1;
        EntropyBits(entropy, (uint32_t)magnitude, category);
    }

    uint32_t run = 0u;
    for (uint32_t zig = 1u; zig < 64u; ++zig)
    {
        bool nonzero = density != 0u && (NextRandom(random) % 1000u) < density;
        if (!nonzero)
        {
            ++run;
            continue;
        }
        int32_t value =
            (int32_t)(NextRandom(random) % (2u * amplitude + 1u)) - (int32_t)amplitude;
        if (value == 0) value = 1;
        uint32_t size = BitCategory(value);
        while (run > 15u)
        {
            EntropyBits(entropy, BenchJpegAcCode(0xF0u), 8u);
            run -= 16u;
        }
        EntropyBits(entropy, BenchJpegAcCode((run << 4u) | size), 8u);
        int32_t magnitude = value > 0 ? value : value + (int32_t)(1u << size) - 1;
        EntropyBits(entropy, (uint32_t)magnitude, size);
        run = 0u;
    }
    if (run > 0u) EntropyBits(entropy, BenchJpegAcCode(0u), 8u);   // EOB
}

// Собирает baseline JPEG. width/height должны быть кратны 16. Возвращает
// размер файла или ноль при переполнении буфера.
static uint32_t BuildSyntheticJpeg(uint8_t *out, uint32_t capacity, uint32_t width, uint32_t height,
                                   const BenchComponentSpec *specs, uint32_t componentCount,
                                   uint32_t seed)
{
    BenchBuffer buffer = {.data = out, .capacity = capacity};
    uint32_t maxHorizontal = 1u;
    uint32_t maxVertical = 1u;
    for (uint32_t index = 0u; index < componentCount; ++index)
    {
        if (specs[index].horizontal > maxHorizontal) maxHorizontal = specs[index].horizontal;
        if (specs[index].vertical > maxVertical) maxVertical = specs[index].vertical;
    }

    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xD8u);   // SOI

    // DQT: таблица 0, восемь бит, все делители единицы.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xDBu);
    BufU16Be(&buffer, 67u);
    BufByte(&buffer, 0x00u);
    for (uint32_t index = 0u; index < 64u; ++index) BufByte(&buffer, 1u);

    // SOF0: до четырёх компонентов, заданная дискретизация, таблица 0.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xC0u);
    BufU16Be(&buffer, 8u + 3u * componentCount);
    BufByte(&buffer, 8u);
    BufU16Be(&buffer, height);
    BufU16Be(&buffer, width);
    BufByte(&buffer, (uint8_t)componentCount);
    for (uint32_t index = 0u; index < componentCount; ++index)
    {
        BufByte(&buffer, (uint8_t)(index + 1u));
        BufByte(&buffer,
                (uint8_t)((specs[index].horizontal << 4u) | specs[index].vertical));
        BufByte(&buffer, 0u);
    }

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

    // SOS: все компоненты, таблицы 0/0, весь спектр.
    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xDAu);
    BufU16Be(&buffer, 6u + 2u * componentCount);
    BufByte(&buffer, (uint8_t)componentCount);
    for (uint32_t index = 0u; index < componentCount; ++index)
    {
        BufByte(&buffer, (uint8_t)(index + 1u));
        BufByte(&buffer, 0x00u);
    }
    BufByte(&buffer, 0u);
    BufByte(&buffer, 63u);
    BufByte(&buffer, 0u);

    uint32_t mcusPerLine = (width + 8u * maxHorizontal - 1u) / (8u * maxHorizontal);
    uint32_t mcusPerColumn = (height + 8u * maxVertical - 1u) / (8u * maxVertical);
    BenchEntropy entropy = {.out = &buffer};
    uint32_t random = seed;
    int32_t prediction[4] = {0, 0, 0, 0};
    uint32_t blockIndex[4] = {0u, 0u, 0u, 0u};
    for (uint32_t mcuRow = 0u; mcuRow < mcusPerColumn; ++mcuRow)
    {
        for (uint32_t mcuColumn = 0u; mcuColumn < mcusPerLine; ++mcuColumn)
        {
            for (uint32_t component = 0u; component < componentCount; ++component)
            {
                for (uint32_t vertical = 0u; vertical < specs[component].vertical; ++vertical)
                {
                    for (uint32_t horizontal = 0u; horizontal < specs[component].horizontal;
                         ++horizontal)
                    {
                        EncodeBlock(&entropy, &random, &prediction[component],
                                    blockIndex[component]++, specs[component].density,
                                    specs[component].amplitude, seed + component * 131u);
                    }
                }
            }
        }
    }
    EntropyFlush(&entropy);

    BufByte(&buffer, 0xFFu);
    BufByte(&buffer, 0xD9u);   // EOI
    return buffer.overflow ? 0u : buffer.size;
}

// === Случаи ===

#define BENCH_MAX_CASES 16u
#define BENCH_MAX_SAMPLES 32u

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
        WriteText("r2 27 jpeg benchmark: inspect failed for ");
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
        WriteText("r2 27 jpeg benchmark: allocation failed for ");
        WriteText(name);
        WriteText("\n");
        return false;
    }
    ImageStatus decoded =
        ImageDecode(file, fileBytes, &info, testCase->pixels, info.pixelBytes, testCase->scratch,
                    info.scratchBytes);
    if (decoded != IMAGE_OK)
    {
        WriteText("r2 27 jpeg benchmark: decode failed for ");
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
        accumulator += pixels[(repeat * 97u) % pixelBytes];
        accumulator += pixels[(repeat * 997u) % pixelBytes];
    }
    g_sink += accumulator;
    return PlatformMonotonicSeconds() - begin;
}

// Обход всех усечений: статус Inspect/Decode и частичные пиксели. Ловит
// расхождение в том, сколько байт декодер успел выдать до отказа.
static uint64_t PrefixHash(const uint8_t *file, uint32_t sizeBytes, const ImageInfo *full)
{
    uint8_t *pixels = (uint8_t *)PlatformAllocate(full->pixelBytes, true);
    void *scratch = full->scratchBytes != 0u ? PlatformAllocate(full->scratchBytes, true) : NULL;
    if (pixels == NULL || (full->scratchBytes != 0u && scratch == NULL))
    {
        WriteText("r2 27 jpeg benchmark: prefix allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t cut = 0u; cut <= sizeBytes; ++cut)
    {
        ImageInfo info = {0};
        ImageStatus inspected = ImageInspect(file, cut, &info);
        hash ^= (uint64_t)inspected;
        hash *= 0x100000001b3ull;
        if (inspected != IMAGE_OK) continue;

        for (uint32_t index = 0u; index < full->pixelBytes; ++index) pixels[index] = 0u;
        ImageStatus decoded =
            ImageDecode(file, cut, &info, pixels, full->pixelBytes, scratch, full->scratchBytes);
        hash ^= (uint64_t)decoded;
        hash *= 0x100000001b3ull;
        if (decoded == IMAGE_OK) hash = HashBytes(hash, pixels, info.pixelBytes);
    }

    PlatformFree(scratch);
    PlatformFree(pixels);
    return hash;
}

static void CheckMalformed(const char *name, const uint8_t *file, uint32_t sizeBytes)
{
    ImageInfo info = {0};
    if (ImageInspect(file, sizeBytes, &info) != IMAGE_OK)
    {
        WriteText("r2 27 jpeg benchmark: malformed fixture inspect failed for ");
        WriteText(name);
        WriteText("\n");
        LaiueTestRuntimeExit(1);
    }
    uint64_t hash = PrefixHash(file, sizeBytes, &info);
    WriteText("R2JPEG malformed=");
    WriteText(name);
    WriteText(" checksum=");
    WriteHex(hash);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(R2Jpeg27BenchmarkEntryPoint)
{
    uint32_t samples = ReadEnvUnsigned("LAIUE_R2_27_JPEG_SAMPLES", 1u, BENCH_MAX_SAMPLES);
    uint32_t percent = ReadEnvUnsigned("LAIUE_R2_27_JPEG_SCALE", 100u, 100000u);

    uint32_t capacity = 2u * 1024u * 1024u;
    uint8_t *synthetic = (uint8_t *)PlatformAllocate(capacity, false);
    if (synthetic == NULL)
    {
        WriteText("r2 27 jpeg benchmark: synthetic buffer allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    // 1) Гладкий серый кадр: только DC — проверяет быстрый путь и IDCT.
    BenchComponentSpec grayFlat[1] = {{1u, 1u, 0u, 1u}};
    // 2) Плотный серый: почти все AC ненулевые — горячий Хаффман и IDCT.
    BenchComponentSpec grayNoise[1] = {{1u, 1u, 700u, 80u}};
    // 3) 4:4:4 цвет: полное разрешение цветности, треугольной интерполяции нет.
    BenchComponentSpec color444[3] = {{1u, 1u, 500u, 60u}, {1u, 1u, 300u, 40u}, {1u, 1u, 300u, 40u}};
    // 4) 4:2:0 цвет: половинная цветность, треугольный фильтр на каждый пиксель.
    BenchComponentSpec color420[3] = {{2u, 2u, 500u, 60u}, {1u, 1u, 300u, 40u}, {1u, 1u, 300u, 40u}};
    // 5) Умеренная плотность: типичная фотография с гладкими и детальными
    // участками; часть блоков остаётся только с DC.
    BenchComponentSpec graySmooth[1] = {{1u, 1u, 40u, 20u}};
    // 6) Цветная 4:2:0 умеренной плотности.
    BenchComponentSpec colorPhoto[3] = {{2u, 2u, 40u, 20u}, {1u, 1u, 30u, 16u}, {1u, 1u, 30u, 16u}};

    uint32_t fileBytes;
    // Синтетические входы строятся последовательно в одном буфере.
    fileBytes = BuildSyntheticJpeg(synthetic, capacity, 256u, 256u, grayFlat, 1u, 3u);
    if (fileBytes == 0u)
    {
        WriteText("r2 27 jpeg benchmark: gray flat build failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PrepareCase("jpeg-gray-flat-256", synthetic, fileBytes, 0u, true))
    {
        LaiueTestRuntimeExit(1);
    }

    // Каждому синтетическому случаю нужен собственный вход: PrepareCase
    // держит указатель, поэтому буферы копируются при построении.
    uint8_t *grayNoiseFile = (uint8_t *)PlatformAllocate(capacity, false);
    uint8_t *color444File = (uint8_t *)PlatformAllocate(capacity, false);
    uint8_t *color420File = (uint8_t *)PlatformAllocate(capacity, false);
    uint8_t *large420File = (uint8_t *)PlatformAllocate(capacity, false);
    uint8_t *graySmoothFile = (uint8_t *)PlatformAllocate(capacity, false);
    uint8_t *colorPhotoFile = (uint8_t *)PlatformAllocate(capacity, false);
    if (grayNoiseFile == NULL || color444File == NULL || color420File == NULL ||
        large420File == NULL || graySmoothFile == NULL || colorPhotoFile == NULL)
    {
        WriteText("r2 27 jpeg benchmark: case buffer allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    fileBytes = BuildSyntheticJpeg(graySmoothFile, capacity, 256u, 256u, graySmooth, 1u, 29u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t graySmoothBytes = fileBytes;

    fileBytes = BuildSyntheticJpeg(colorPhotoFile, capacity, 256u, 256u, colorPhoto, 3u, 31u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t colorPhotoBytes = fileBytes;

    fileBytes = BuildSyntheticJpeg(grayNoiseFile, capacity, 256u, 256u, grayNoise, 1u, 7u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t grayNoiseBytes = fileBytes;

    fileBytes = BuildSyntheticJpeg(color444File, capacity, 256u, 256u, color444, 3u, 11u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t color444Bytes = fileBytes;

    fileBytes = BuildSyntheticJpeg(color420File, capacity, 256u, 256u, color420, 3u, 13u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t color420Bytes = fileBytes;

    fileBytes = BuildSyntheticJpeg(large420File, capacity, 512u, 512u, color420, 3u, 17u);
    if (fileBytes == 0u) LaiueTestRuntimeExit(1);
    uint32_t large420Bytes = fileBytes;

    // Базовые повторы подобраны так, чтобы измеренный прогон был заметно
    // длиннее разрешения таймера; масштаб — LAIUE_R2_27_JPEG_SCALE.
    uint32_t flatRepeats = (600u * percent) / 100u;
    uint32_t noiseRepeats = (250u * percent) / 100u;
    uint32_t color444Repeats = (120u * percent) / 100u;
    uint32_t color420Repeats = (100u * percent) / 100u;
    uint32_t largeRepeats = (25u * percent) / 100u;
    uint32_t smoothRepeats = (250u * percent) / 100u;
    uint32_t photoRepeats = (100u * percent) / 100u;
    uint32_t fixtureRepeats = (40000u * percent) / 100u;
    if (flatRepeats == 0u) flatRepeats = 1u;
    if (noiseRepeats == 0u) noiseRepeats = 1u;
    if (color444Repeats == 0u) color444Repeats = 1u;
    if (color420Repeats == 0u) color420Repeats = 1u;
    if (largeRepeats == 0u) largeRepeats = 1u;
    if (smoothRepeats == 0u) smoothRepeats = 1u;
    if (photoRepeats == 0u) photoRepeats = 1u;
    if (fixtureRepeats == 0u) fixtureRepeats = 1u;

    bool prepared =
        PrepareCase("jpeg-gray-smooth-256", graySmoothFile, graySmoothBytes, smoothRepeats, true) &&
        PrepareCase("jpeg-color-420-photo-256", colorPhotoFile, colorPhotoBytes, photoRepeats,
                    true) &&
        PrepareCase("jpeg-gray-noise-256", grayNoiseFile, grayNoiseBytes, noiseRepeats, true) &&
        PrepareCase("jpeg-color-444-256", color444File, color444Bytes, color444Repeats, true) &&
        PrepareCase("jpeg-color-420-256", color420File, color420Bytes, color420Repeats, true) &&
        PrepareCase("jpeg-color-420-512", large420File, large420Bytes, largeRepeats, true) &&
        PrepareCase("jpeg-fixture-baseline-16", JPEG_BASELINE_FILE,
                    (uint32_t)sizeof(JPEG_BASELINE_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-chroma-16", JPEG_CHROMA_FILE,
                    (uint32_t)sizeof(JPEG_CHROMA_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-subsampled-20x12", JPEG_SUBSAMPLED_FILE,
                    (uint32_t)sizeof(JPEG_SUBSAMPLED_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-gray-16", JPEG_GRAY_FILE, (uint32_t)sizeof(JPEG_GRAY_FILE),
                    fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-progressive-16", JPEG_PROGRESSIVE_FILE,
                    (uint32_t)sizeof(JPEG_PROGRESSIVE_FILE), fixtureRepeats, false) &&
        PrepareCase("jpeg-fixture-solid-tiny", JPEG_SOLID_FILE,
                    (uint32_t)sizeof(JPEG_SOLID_FILE), fixtureRepeats, false);
    if (!prepared)
    {
        WriteText("r2 27 jpeg benchmark: a case could not be prepared\n");
        LaiueTestRuntimeExit(1);
    }
    // Гладкий случай повторов задаётся здесь: PrepareCase выше уже добавил
    // его с нулём повторов.
    g_cases[0].repeats = flatRepeats;

    for (uint32_t caseIndex = 0u; caseIndex < g_caseCount; ++caseIndex)
    {
        BenchCase *testCase = &g_cases[caseIndex];
        RunCase(testCase, testCase->repeats);   // прогрев вне статистики
        for (uint32_t sample = 0u; sample < samples; ++sample)
        {
            double seconds = RunCase(testCase, testCase->repeats);
            if (testCase->sampleCount < BENCH_MAX_SAMPLES)
            {
                testCase->samples[testCase->sampleCount++] = seconds * 1000.0;
            }
            WriteText("R2JPEG case=");
            WriteText(testCase->name);
            WriteText(" reps=");
            WriteUnsigned(testCase->repeats);
            WriteText(" ms=");
            WriteFixed(seconds * 1000.0);
            WriteText(" checksum=");
            WriteHex(testCase->checksum);
            WriteText("\n");
        }
    }

    CheckMalformed("baseline", JPEG_BASELINE_FILE, (uint32_t)sizeof(JPEG_BASELINE_FILE));
    CheckMalformed("progressive", JPEG_PROGRESSIVE_FILE, (uint32_t)sizeof(JPEG_PROGRESSIVE_FILE));
    CheckMalformed("chroma", JPEG_CHROMA_FILE, (uint32_t)sizeof(JPEG_CHROMA_FILE));
    CheckMalformed("subsampled", JPEG_SUBSAMPLED_FILE, (uint32_t)sizeof(JPEG_SUBSAMPLED_FILE));

    for (uint32_t index = 0u; index < g_caseCount; ++index)
    {
        if (g_cases[index].ownsFile) PlatformFree((void *)g_cases[index].file);
        ReleaseCase(&g_cases[index]);
    }
    WriteText("r2 27 jpeg benchmark: done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
