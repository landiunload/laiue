// Декодер JPEG против скалярного эталона. Обратное ДКП, увеличение цветности
// и перевод цвета написаны на векторных регистрах отдельно для SSE2 и NEON,
// поэтому тест идёт от известных коэффициентов: он сам кодирует их в файл, а
// отсчёты и пиксели считает по формулам libjpeg, по одному значению.
//
// Ширины охватывают края векторных шагов, а значения доходят до насыщения и
// переполнения 16 бит: на таких файлах пиксели тоже обязаны быть одинаковыми
// на любом процессоре. Эталон поэтому воспроизводит и разрядность дорожек.
#include "media/image.h"

#include "test_runtime.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_WIDTH 80u
#define MAX_HEIGHT 26u
#define BLOCK_SAMPLES 8u
#define MAX_COMPONENTS 3u
#define MAX_SAMPLING 2u
#define COEFFICIENTS 64u
#define PLANE_WIDTH (MAX_WIDTH + 2u * BLOCK_SAMPLES * MAX_SAMPLING)
#define PLANE_HEIGHT (MAX_HEIGHT + 2u * BLOCK_SAMPLES * MAX_SAMPLING)
#define MAX_BLOCKS (PLANE_WIDTH * PLANE_HEIGHT / (BLOCK_SAMPLES * BLOCK_SAMPLES))
#define MAX_PIXEL_BYTES (MAX_WIDTH * MAX_HEIGHT * 4u)
#define FILE_CAPACITY 262144u
#define SCRATCH_CAPACITY 1048576u
#define DC_SYMBOLS 16u
#define AC_SYMBOLS 256u
#define DC_CODE_BITS 5u
#define AC_CODE_BITS 8u
#define QUANT_VALUE_LIMIT 256u
#define RESTART_INTERVAL 3u
#define MARKER_SOF0 0xC0u
#define MARKER_DHT 0xC4u
#define MARKER_RST0 0xD0u
#define MARKER_SOI 0xD8u
#define MARKER_EOI 0xD9u
#define MARKER_SOS 0xDAu
#define MARKER_DQT 0xDBu
#define MARKER_DRI 0xDDu

// Кратности прореживания: первый компонент и цветность.
typedef struct Sampling
{
    uint32_t horizontal;
    uint32_t vertical;
} Sampling;

static const Sampling SAMPLINGS[] = {{1u, 1u}, {2u, 1u}, {1u, 2u}, {2u, 2u}};

static const uint32_t WIDTHS[] = {1u,  2u,  3u,  4u,  5u,  7u,  8u,  9u,  15u, 16u, 17u,
                                  18u, 19u, 20u, 31u, 32u, 33u, 34u, 47u, 48u, 49u, MAX_WIDTH};
static const uint32_t HEIGHTS[] = {1u, 2u, 3u, 9u, 17u, MAX_HEIGHT};

static const uint8_t ZIGZAG[COEFFICIENTS] = {
    0u,  1u,  8u,  16u, 9u,  2u,  3u,  10u, 17u, 24u, 32u, 25u, 18u, 11u, 4u,  5u,
    12u, 19u, 26u, 33u, 40u, 48u, 41u, 34u, 27u, 20u, 13u, 6u,  7u,  14u, 21u, 28u,
    35u, 42u, 49u, 56u, 57u, 50u, 43u, 36u, 29u, 22u, 15u, 23u, 30u, 37u, 44u, 51u,
    58u, 59u, 52u, 45u, 38u, 31u, 39u, 46u, 53u, 60u, 61u, 54u, 47u, 55u, 62u, 63u,
};

typedef enum ValueRange
{
    // Как у снимка: все промежуточные значения умещаются.
    RANGE_PHOTO,
    // Крайние значения и большой квантователь: дорожки насыщаются.
    RANGE_EXTREME,
    RANGE_COUNT
} ValueRange;

typedef struct Plane
{
    uint32_t blocksWide;
    uint32_t blocksHigh;
    uint32_t plainWidth;
    uint32_t plainHeight;
    uint32_t horizontal;
    uint32_t vertical;
} Plane;

static int16_t g_blocks[MAX_COMPONENTS][MAX_BLOCKS][COEFFICIENTS];
static uint8_t g_samples[MAX_COMPONENTS][PLANE_HEIGHT][PLANE_WIDTH];
static uint8_t g_rows[MAX_COMPONENTS][MAX_WIDTH + 1u];
static uint16_t g_quant[COEFFICIENTS];
static uint8_t g_file[FILE_CAPACITY];
static uint8_t g_pixels[MAX_PIXEL_BYTES];
static uint8_t g_reference[MAX_PIXEL_BYTES];
static alignas(16) uint8_t g_scratch[SCRATCH_CAPACITY];
static uint32_t g_random;

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("jpeg reference test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t Random(uint32_t limit)
{
    g_random = g_random * 1664525u + 1013904223u;
    return (g_random >> 8) % limit;
}

// Запись файла ---------------------------------------------------------------

typedef struct FileWriter
{
    uint32_t size;
    uint32_t bitBuffer;
    uint32_t bitCount;
} FileWriter;

static void WriteByte(FileWriter *writer, uint32_t value)
{
    Expect(writer->size < FILE_CAPACITY, "the file must fit its buffer");
    g_file[writer->size] = (uint8_t)value;
    writer->size += 1u;
}

static void WriteU16(FileWriter *writer, uint32_t value)
{
    WriteByte(writer, value >> 8);
    WriteByte(writer, value & 0xFFu);
}

static void WriteMarker(FileWriter *writer, uint32_t marker)
{
    WriteByte(writer, 0xFFu);
    WriteByte(writer, marker);
}

static void WriteBits(FileWriter *writer, uint32_t value, uint32_t count)
{
    for (uint32_t bit = count; bit > 0u; --bit)
    {
        writer->bitBuffer = (writer->bitBuffer << 1) | ((value >> (bit - 1u)) & 1u);
        writer->bitCount += 1u;
        if (writer->bitCount == 8u)
        {
            WriteByte(writer, writer->bitBuffer & 0xFFu);
            if ((writer->bitBuffer & 0xFFu) == 0xFFu) WriteByte(writer, 0u);
            writer->bitBuffer = 0u;
            writer->bitCount = 0u;
        }
    }
}

// Хвост байта заполняется единицами, как у обычных кодировщиков.
static void FlushBits(FileWriter *writer)
{
    while (writer->bitCount != 0u) WriteBits(writer, 1u, 1u);
}

// Коды одной длины: символ и есть код, поэтому кодировщику не нужна таблица.
static void WriteHuffmanTable(FileWriter *writer, uint32_t classAndId, uint32_t codeBits,
                              uint32_t symbols)
{
    WriteMarker(writer, MARKER_DHT);
    WriteU16(writer, 2u + 1u + 16u + symbols);
    WriteByte(writer, classAndId);
    for (uint32_t length = 1u; length <= 16u; ++length)
    {
        WriteByte(writer, length == codeBits ? symbols & 0xFFu : 0u);
    }
    for (uint32_t symbol = 0u; symbol < symbols; ++symbol) WriteByte(writer, symbol);
}

static uint32_t BitSize(int32_t value)
{
    uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
    uint32_t size = 0u;
    while (magnitude != 0u)
    {
        size += 1u;
        magnitude >>= 1;
    }
    return size;
}

// T.81 F.1.2.1: отрицательное значение пишется как value - 1 в size битах.
static void WriteValue(FileWriter *writer, int32_t value, uint32_t size)
{
    if (size == 0u) return;
    const uint32_t mask = ((uint32_t)1 << size) - 1u;
    WriteBits(writer, (uint32_t)(value < 0 ? value - 1 : value) & mask, size);
}

static void WriteBlock(FileWriter *writer, const int16_t *block, int32_t *prediction)
{
    const int32_t difference = (int32_t)block[0] - *prediction;
    *prediction = block[0];
    const uint32_t dcSize = BitSize(difference);
    WriteBits(writer, dcSize, DC_CODE_BITS);
    WriteValue(writer, difference, dcSize);

    uint32_t run = 0u;
    for (uint32_t index = 1u; index < COEFFICIENTS; ++index)
    {
        const int32_t value = block[ZIGZAG[index]];
        if (value == 0)
        {
            run += 1u;
            continue;
        }
        while (run >= 16u)
        {
            WriteBits(writer, 0xF0u, AC_CODE_BITS);
            run -= 16u;
        }
        const uint32_t size = BitSize(value);
        WriteBits(writer, (run << 4) | size, AC_CODE_BITS);
        WriteValue(writer, value, size);
        run = 0u;
    }
    if (run != 0u) WriteBits(writer, 0u, AC_CODE_BITS);
}

static void WriteHeaders(FileWriter *writer, uint32_t width, uint32_t height,
                         uint32_t componentCount, const Sampling *sampling)
{
    WriteMarker(writer, MARKER_SOI);

    WriteMarker(writer, MARKER_DQT);
    WriteU16(writer, 2u + 1u + COEFFICIENTS);
    WriteByte(writer, 0u);
    for (uint32_t index = 0u; index < COEFFICIENTS; ++index)
    {
        WriteByte(writer, g_quant[ZIGZAG[index]]);
    }

    WriteMarker(writer, MARKER_SOF0);
    WriteU16(writer, 8u + 3u * componentCount);
    WriteByte(writer, 8u);
    WriteU16(writer, height);
    WriteU16(writer, width);
    WriteByte(writer, componentCount);
    for (uint32_t index = 0u; index < componentCount; ++index)
    {
        WriteByte(writer, index + 1u);
        WriteByte(writer, index == 0u ? (sampling->horizontal << 4) | sampling->vertical : 0x11u);
        WriteByte(writer, 0u);
    }

    WriteHuffmanTable(writer, 0x00u, DC_CODE_BITS, DC_SYMBOLS);
    WriteHuffmanTable(writer, 0x10u, AC_CODE_BITS, AC_SYMBOLS - 1u);
}

static void WriteScanHeader(FileWriter *writer, uint32_t componentCount)
{
    WriteMarker(writer, MARKER_SOS);
    WriteU16(writer, 6u + 2u * componentCount);
    WriteByte(writer, componentCount);
    for (uint32_t index = 0u; index < componentCount; ++index)
    {
        WriteByte(writer, index + 1u);
        WriteByte(writer, 0u);
    }
    WriteByte(writer, 0u);
    WriteByte(writer, 63u);
    WriteByte(writer, 0u);
}

// Эталон: обратное ДКП ---------------------------------------------------------

static int16_t Wrap16(int32_t value)
{
    return (int16_t)(uint16_t)(uint32_t)value;
}

static int32_t Wrap32(int64_t value)
{
    return (int32_t)(uint32_t)(uint64_t)value;
}

// Округление в 32-битной дорожке, сдвиг и насыщение до 16 бит.
static int16_t Narrow(int32_t value, uint32_t bits)
{
    const int32_t rounded = Wrap32((int64_t)value + ((int64_t)1 << (bits - 1u)));
    const int32_t shifted = rounded >> bits;
    if (shifted < -32768) return -32768;
    if (shifted > 32767) return 32767;
    return (int16_t)shifted;
}

static int32_t Pair(int32_t first, int32_t firstFactor, int32_t second, int32_t secondFactor)
{
    return Wrap32((int64_t)first * firstFactor + (int64_t)second * secondFactor);
}

// islow из libjpeg в той разрядности, в какой его считают дорожки.
static void Transform1D(const int16_t *in, size_t inStep, int16_t *out, size_t outStep,
                        uint32_t descale)
{
    const int32_t in0 = in[0u * inStep];
    const int32_t in1 = in[1u * inStep];
    const int32_t in2 = in[2u * inStep];
    const int32_t in3 = in[3u * inStep];
    const int32_t in4 = in[4u * inStep];
    const int32_t in5 = in[5u * inStep];
    const int32_t in6 = in[6u * inStep];
    const int32_t in7 = in[7u * inStep];

    const int32_t even2 = Pair(in2, 4433, in6, 4433 - 15137);
    const int32_t even3 = Pair(in2, 4433 + 6270, in6, 4433);
    const int32_t even0 = Wrap32((int64_t)Wrap16(in0 + in4) * 8192);
    const int32_t even1 = Wrap32((int64_t)Wrap16(in0 - in4) * 8192);
    const int32_t sum10 = Wrap32((int64_t)even0 + even3);
    const int32_t sum13 = Wrap32((int64_t)even0 - even3);
    const int32_t sum11 = Wrap32((int64_t)even1 + even2);
    const int32_t sum12 = Wrap32((int64_t)even1 - even2);

    const int32_t z3 = Wrap16(in7 + in3);
    const int32_t z4 = Wrap16(in5 + in1);
    const int32_t rotated3 = Pair(z3, 9633 - 16069, z4, 9633);
    const int32_t rotated4 = Pair(z3, 9633, z4, 9633 - 3196);
    const int32_t odd0 = Wrap32((int64_t)Pair(in7, 2446 - 7373, in1, -7373) + rotated3);
    const int32_t odd1 = Wrap32((int64_t)Pair(in5, 16819 - 20995, in3, -20995) + rotated4);
    const int32_t odd2 = Wrap32((int64_t)Pair(in5, -20995, in3, 25172 - 20995) + rotated3);
    const int32_t odd3 = Wrap32((int64_t)Pair(in7, -7373, in1, 12299 - 7373) + rotated4);

    out[0u * outStep] = Narrow(Wrap32((int64_t)sum10 + odd3), descale);
    out[7u * outStep] = Narrow(Wrap32((int64_t)sum10 - odd3), descale);
    out[1u * outStep] = Narrow(Wrap32((int64_t)sum11 + odd2), descale);
    out[6u * outStep] = Narrow(Wrap32((int64_t)sum11 - odd2), descale);
    out[2u * outStep] = Narrow(Wrap32((int64_t)sum12 + odd1), descale);
    out[5u * outStep] = Narrow(Wrap32((int64_t)sum12 - odd1), descale);
    out[3u * outStep] = Narrow(Wrap32((int64_t)sum13 + odd0), descale);
    out[4u * outStep] = Narrow(Wrap32((int64_t)sum13 - odd0), descale);
}

static uint8_t SampleFromLane(int16_t value)
{
    int32_t centred = value;
    if (centred < -128) centred = -128;
    if (centred > 127) centred = 127;
    return (uint8_t)(centred + 128);
}

// Блок только с DC декодер считает отдельно: в полной разрядности и с
// насыщением одного значения. У настоящего файла разницы нет.
static uint8_t FlatSample(int32_t dc, uint32_t quant)
{
    const int64_t scaled = (int64_t)dc * quant * 4;
    int64_t level = ((scaled + 16) >> 5) + 128;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    return (uint8_t)level;
}

static void TransformBlock(const int16_t *block, uint8_t *samples, size_t stride)
{
    bool acPresent = false;
    for (uint32_t index = 1u; index < COEFFICIENTS; ++index) acPresent |= block[index] != 0;
    if (!acPresent)
    {
        const uint8_t level = FlatSample(block[0], g_quant[0]);
        for (uint32_t y = 0u; y < BLOCK_SAMPLES; ++y)
        {
            memset(samples + y * stride, level, BLOCK_SAMPLES);
        }
        return;
    }

    int16_t product[COEFFICIENTS];
    int16_t work[COEFFICIENTS];
    int16_t result[COEFFICIENTS];
    for (uint32_t index = 0u; index < COEFFICIENTS; ++index)
    {
        product[index] = Wrap16((int32_t)block[index] * (int32_t)g_quant[index]);
    }
    for (uint32_t column = 0u; column < BLOCK_SAMPLES; ++column)
    {
        Transform1D(product + column, 8u, work + column, 8u, 11u);
    }
    for (size_t row = 0u; row < BLOCK_SAMPLES; ++row)
    {
        Transform1D(work + row * 8u, 1u, result + row * 8u, 1u, 18u);
    }
    for (uint32_t y = 0u; y < BLOCK_SAMPLES; ++y)
    {
        for (uint32_t x = 0u; x < BLOCK_SAMPLES; ++x)
        {
            samples[y * stride + x] = SampleFromLane(result[y * 8u + x]);
        }
    }
}

// Эталон: увеличение и перевод цвета ---------------------------------------------

static const uint8_t *UpsampledRow(uint32_t component, const Plane *plane, uint32_t width,
                                   uint32_t y)
{
    uint8_t *row = g_rows[component];
    uint8_t (*samples)[PLANE_WIDTH] = g_samples[component];
    const size_t plainWidth = plane->plainWidth;
    const bool halfWidth = plane->horizontal == 2u && plainWidth > 2u;
    const bool halfHeight = plane->vertical == 2u;
    if (plane->horizontal == 1u && plane->vertical == 1u) return samples[y];

    uint32_t nearRow = y;
    uint32_t farRow = y;
    if (halfHeight)
    {
        nearRow = y >> 1;
        farRow = nearRow;
        if ((y & 1u) != 0u)
        {
            if (nearRow + 1u < plane->plainHeight) farRow = nearRow + 1u;
        }
        else if (nearRow != 0u)
        {
            farRow = nearRow - 1u;
        }
    }

    if (plane->horizontal == 1u)
    {
        const uint32_t bias = (y & 1u) != 0u ? 2u : 1u;
        for (uint32_t x = 0u; x < width; ++x)
        {
            row[x] = (uint8_t)((3u * samples[nearRow][x] + samples[farRow][x] + bias) >> 2);
        }
        return row;
    }
    if (!halfWidth)
    {
        for (uint32_t x = 0u; x < width; ++x) row[x] = samples[y >> (plane->vertical - 1u)][x >> 1];
        return row;
    }

    // Строка значений: у половинной высоты суммы столбцов, иначе отсчёты.
    uint32_t values[PLANE_WIDTH];
    for (size_t x = 0u; x < plainWidth; ++x)
    {
        values[x] = halfHeight ? 3u * samples[nearRow][x] + samples[farRow][x] : samples[y][x];
    }
    const uint32_t bits = halfHeight ? 4u : 2u;
    const uint32_t evenBias = halfHeight ? 8u : 1u;
    const uint32_t oddBias = halfHeight ? 7u : 2u;
    for (uint32_t x = 0u; x < width; ++x)
    {
        const size_t index = x >> 1;
        const bool odd = (x & 1u) != 0u;
        const size_t other = odd ? index + 1u : index - 1u;
        const bool edge = odd ? other >= plainWidth : index == 0u;
        if (!edge)
        {
            row[x] = (uint8_t)((3u * values[index] + values[other] + (odd ? oddBias : evenBias)) >>
                               bits);
        }
        else if (halfHeight)
        {
            row[x] = (uint8_t)((4u * values[index] + (odd ? 7u : 8u)) >> 4);
        }
        else
        {
            row[x] = (uint8_t)values[index];
        }
    }
    return row;
}

static uint8_t Clamp(int32_t value)
{
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static void ColorPixel(const uint8_t *luma, const uint8_t *blue, const uint8_t *red, size_t x,
                       uint8_t *out)
{
    const int32_t brightness = luma[x];
    const int32_t cb = (int32_t)blue[x] - 128;
    const int32_t cr = (int32_t)red[x] - 128;
    out[0] = Clamp(brightness + ((91881 * cr + 32768) >> 16));
    out[1] = Clamp(brightness + ((-22554 * cb - 46802 * cr + 32768) >> 16));
    out[2] = Clamp(brightness + ((116130 * cb + 32768) >> 16));
    out[3] = 255u;
}

// Коэффициенты -------------------------------------------------------------------

static int16_t RandomCoefficient(ValueRange range, uint32_t index)
{
    if (range == RANGE_EXTREME)
    {
        const uint32_t kind = Random(6u);
        if (kind == 0u) return (int16_t)((int32_t)Random(2047u) - 1023);
        if (kind == 1u) return (int16_t)((index & 1u) != 0u ? 1023 : -1023);
        if (kind == 2u) return (int16_t)((int32_t)Random(255u) - 127);
        return 0;
    }
    // Чем выше частота, тем реже коэффициент и тем он меньше.
    const uint32_t scale = 2u + 256u / (index + 2u);
    if (Random(index / 4u + 2u) != 0u) return 0;
    return (int16_t)((int32_t)Random(2u * scale + 1u) - (int32_t)scale);
}

static void FillQuant(ValueRange range)
{
    for (uint32_t index = 0u; index < COEFFICIENTS; ++index)
    {
        const uint32_t limit = range == RANGE_EXTREME ? QUANT_VALUE_LIMIT - 1u : 8u;
        g_quant[index] = (uint16_t)(1u + Random(limit));
    }
}

static void FillBlocks(const Plane *planes, uint32_t componentCount, ValueRange range)
{
    for (uint32_t component = 0u; component < componentCount; ++component)
    {
        const uint32_t blocks = planes[component].blocksWide * planes[component].blocksHigh;
        Expect(blocks <= MAX_BLOCKS, "the blocks must fit their buffer");
        for (uint32_t block = 0u; block < blocks; ++block)
        {
            int16_t *values = g_blocks[component][block];
            memset(values, 0, COEFFICIENTS * sizeof(values[0]));
            values[0] = (int16_t)((int32_t)Random(1601u) - 800);
            // Плоские блоки идут отдельным путём и в декодере, и здесь.
            if (Random(5u) == 0u) continue;
            for (uint32_t index = 1u; index < COEFFICIENTS; ++index)
            {
                values[ZIGZAG[index]] = RandomCoefficient(range, index);
            }
        }
    }
}

// Сценарий -----------------------------------------------------------------------

static void BuildPlanes(uint32_t width, uint32_t height, uint32_t componentCount,
                        const Sampling *sampling, Plane *planes, uint32_t *outMcusWide,
                        uint32_t *outMcusHigh)
{
    const uint32_t mcuWidth = BLOCK_SAMPLES * sampling->horizontal;
    const uint32_t mcuHeight = BLOCK_SAMPLES * sampling->vertical;
    *outMcusWide = componentCount == 1u ? (width + 7u) / 8u : (width + mcuWidth - 1u) / mcuWidth;
    *outMcusHigh = componentCount == 1u ? (height + 7u) / 8u : (height + mcuHeight - 1u) / mcuHeight;
    for (uint32_t component = 0u; component < componentCount; ++component)
    {
        Plane *plane = &planes[component];
        const uint32_t horizontal = component == 0u ? sampling->horizontal : 1u;
        const uint32_t vertical = component == 0u ? sampling->vertical : 1u;
        plane->blocksWide = *outMcusWide * (componentCount == 1u ? 1u : horizontal);
        plane->blocksHigh = *outMcusHigh * (componentCount == 1u ? 1u : vertical);
        plane->horizontal = sampling->horizontal / horizontal;
        plane->vertical = sampling->vertical / vertical;
        plane->plainWidth = (width * horizontal + sampling->horizontal - 1u) / sampling->horizontal;
        plane->plainHeight = (height * vertical + sampling->vertical - 1u) / sampling->vertical;
        if (componentCount == 1u)
        {
            plane->horizontal = 1u;
            plane->vertical = 1u;
            plane->plainWidth = width;
            plane->plainHeight = height;
        }
    }
}

static uint32_t BuildFile(uint32_t width, uint32_t height, uint32_t componentCount,
                          const Sampling *sampling, const Plane *planes, uint32_t mcusWide,
                          uint32_t mcusHigh, bool restarts)
{
    FileWriter writer = {0u, 0u, 0u};
    WriteHeaders(&writer, width, height, componentCount, sampling);
    if (restarts)
    {
        WriteMarker(&writer, MARKER_DRI);
        WriteU16(&writer, 4u);
        WriteU16(&writer, RESTART_INTERVAL);
    }
    WriteScanHeader(&writer, componentCount);

    int32_t prediction[MAX_COMPONENTS] = {0, 0, 0};
    uint32_t restart = 0u;
    for (uint32_t row = 0u; row < mcusHigh; ++row)
    {
        for (uint32_t column = 0u; column < mcusWide; ++column)
        {
            const uint32_t unit = row * mcusWide + column;
            if (restarts && unit != 0u && unit % RESTART_INTERVAL == 0u)
            {
                FlushBits(&writer);
                WriteMarker(&writer, MARKER_RST0 + restart);
                restart = (restart + 1u) & 7u;
                memset(prediction, 0, sizeof(prediction));
            }
            for (uint32_t component = 0u; component < componentCount; ++component)
            {
                const Plane *plane = &planes[component];
                const uint32_t blocksDown = plane->blocksHigh / mcusHigh;
                const uint32_t blocksAcross = plane->blocksWide / mcusWide;
                for (uint32_t down = 0u; down < blocksDown; ++down)
                {
                    for (uint32_t across = 0u; across < blocksAcross; ++across)
                    {
                        const uint32_t block = (row * blocksDown + down) * plane->blocksWide +
                                               column * blocksAcross + across;
                        WriteBlock(&writer, g_blocks[component][block], &prediction[component]);
                    }
                }
            }
        }
    }
    FlushBits(&writer);
    WriteMarker(&writer, MARKER_EOI);
    return writer.size;
}

static void BuildReference(uint32_t width, uint32_t height, uint32_t componentCount,
                           const Plane *planes)
{
    for (uint32_t component = 0u; component < componentCount; ++component)
    {
        const Plane *plane = &planes[component];
        for (size_t row = 0u; row < plane->blocksHigh; ++row)
        {
            for (size_t column = 0u; column < plane->blocksWide; ++column)
            {
                TransformBlock(g_blocks[component][row * plane->blocksWide + column],
                               &g_samples[component][row * 8u][column * 8u], PLANE_WIDTH);
            }
        }
    }
    for (uint32_t y = 0u; y < height; ++y)
    {
        uint8_t *line = g_reference + (size_t)y * width * 4u;
        if (componentCount == 1u)
        {
            for (size_t x = 0u; x < width; ++x)
            {
                const uint8_t gray = g_samples[0][y][x];
                line[4u * x] = gray;
                line[4u * x + 1u] = gray;
                line[4u * x + 2u] = gray;
                line[4u * x + 3u] = 255u;
            }
            continue;
        }
        const uint8_t *luma = UpsampledRow(0u, &planes[0], width, y);
        const uint8_t *blue = UpsampledRow(1u, &planes[1], width, y);
        const uint8_t *red = UpsampledRow(2u, &planes[2], width, y);
        for (size_t x = 0u; x < width; ++x) ColorPixel(luma, blue, red, x, line + 4u * x);
    }
}

static void ExpectPicture(uint32_t width, uint32_t height, uint32_t componentCount,
                          const Sampling *sampling, ValueRange range, bool restarts)
{
    Plane planes[MAX_COMPONENTS];
    uint32_t mcusWide = 0u;
    uint32_t mcusHigh = 0u;
    BuildPlanes(width, height, componentCount, sampling, planes, &mcusWide, &mcusHigh);
    FillQuant(range);
    FillBlocks(planes, componentCount, range);
    const uint32_t fileBytes = BuildFile(width, height, componentCount, sampling, planes, mcusWide,
                                         mcusHigh, restarts);
    BuildReference(width, height, componentCount, planes);

    ImageInfo info;
    Expect(ImageInspect(g_file, fileBytes, &info) == IMAGE_OK, "the file must be recognised");
    Expect(info.pixelBytes == width * height * 4u, "the size must come from the header");
    Expect(info.scratchBytes <= SCRATCH_CAPACITY, "the scratch must fit its buffer");
    Expect(ImageDecode(g_file, fileBytes, &info, g_pixels, info.pixelBytes, g_scratch,
                       info.scratchBytes) == IMAGE_OK,
           "the picture must decode");
    Expect(memcmp(g_pixels, g_reference, info.pixelBytes) == 0,
           "the pixels must match the reference");
}

LAIUE_TEST_ENTRY(JpegReferenceTestEntryPoint)
{
    g_random = 2026u;
    const uint32_t widths = (uint32_t)(sizeof(WIDTHS) / sizeof(WIDTHS[0]));
    const uint32_t heights = (uint32_t)(sizeof(HEIGHTS) / sizeof(HEIGHTS[0]));
    const uint32_t samplings = (uint32_t)(sizeof(SAMPLINGS) / sizeof(SAMPLINGS[0]));
    for (uint32_t widthIndex = 0u; widthIndex < widths; ++widthIndex)
    {
        for (uint32_t heightIndex = 0u; heightIndex < heights; ++heightIndex)
        {
            const uint32_t width = WIDTHS[widthIndex];
            const uint32_t height = HEIGHTS[heightIndex];
            for (uint32_t range = 0u; range < RANGE_COUNT; ++range)
            {
                ExpectPicture(width, height, 1u, &SAMPLINGS[0], (ValueRange)range, false);
                for (uint32_t sampling = 0u; sampling < samplings; ++sampling)
                {
                    ExpectPicture(width, height, 3u, &SAMPLINGS[sampling], (ValueRange)range,
                                  false);
                    ExpectPicture(width, height, 3u, &SAMPLINGS[sampling], (ValueRange)range,
                                  true);
                }
            }
        }
    }

    LaiueTestRuntimeWrite("jpeg reference test passed\n");
    LAIUE_TEST_SUCCESS();
}
