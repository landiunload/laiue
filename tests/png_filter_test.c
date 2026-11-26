// Снятие фильтров PNG на всех размерах пикселя. Sub, Average и Paeth
// написаны на векторных регистрах отдельно для SSE2 и NEON, поэтому тест
// идёт от стандарта: один снимок кодируется без фильтра и с каждым
// фильтром по формулам PNG, и декодер обязан выдать те же пиксели.
//
// Широкие строки проверяют цикл по пикселям, короткие — край строки: чтение
// за её конец на последней строке вышло бы за распакованный поток. Поток
// собран из несжатых блоков: распаковка здесь ни при чём.
#include "media/image.h"

#include "test_runtime.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define IMAGE_HEIGHT 6u
#define MAX_ROW_BYTES 1024u
#define MAX_WIDTH 96u
#define MAX_PIXEL_BYTES (MAX_WIDTH * IMAGE_HEIGHT * 4u)
#define RAW_CAPACITY ((MAX_ROW_BYTES + 1u) * IMAGE_HEIGHT)
#define FILE_CAPACITY (RAW_CAPACITY + 128u)
#define SCRATCH_CAPACITY (RAW_CAPACITY + 65536u)
#define FILTER_MODE_NONE 0u
#define FILTER_MODE_MIXED 5u
#define FILTER_MODES 6u
#define INVALID_FILTER 5u

typedef struct PixelLayout
{
    uint32_t colorType;
    uint32_t bitDepth;
    uint32_t channels;
} PixelLayout;

static const PixelLayout LAYOUTS[] = {
    {0u, 1u, 1u}, {0u, 2u, 1u}, {0u, 4u, 1u}, {0u, 8u, 1u}, {0u, 16u, 1u}, {4u, 8u, 2u},
    {4u, 16u, 2u}, {2u, 8u, 3u}, {2u, 16u, 3u}, {6u, 8u, 4u}, {6u, 16u, 4u},
};

static const uint32_t WIDTHS[] = {1u, 2u, 3u, 4u, 5u, 8u, 17u, MAX_WIDTH};

static uint8_t g_image[IMAGE_HEIGHT][MAX_ROW_BYTES];
static uint8_t g_raw[RAW_CAPACITY];
static uint8_t g_file[FILE_CAPACITY];
static uint8_t g_pixels[MAX_PIXEL_BYTES];
static uint8_t g_reference[MAX_PIXEL_BYTES];
static alignas(16) uint8_t g_scratch[SCRATCH_CAPACITY];
static uint32_t g_random;

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("png filter test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t Random(uint32_t limit)
{
    g_random = g_random * 1664525u + 1013904223u;
    return (g_random >> 8) % limit;
}

static int32_t Distance(int32_t first, int32_t second)
{
    return first > second ? first - second : second - first;
}

static int32_t Paeth(int32_t left, int32_t above, int32_t upperLeft)
{
    const int32_t estimate = left + above - upperLeft;
    const int32_t distanceLeft = Distance(estimate, left);
    const int32_t distanceAbove = Distance(estimate, above);
    const int32_t distanceUpperLeft = Distance(estimate, upperLeft);
    if (distanceLeft <= distanceAbove && distanceLeft <= distanceUpperLeft) return left;
    return distanceAbove <= distanceUpperLeft ? above : upperLeft;
}

static uint32_t Crc32(const uint8_t *bytes, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t index = 0u; index < size; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t Adler32(const uint8_t *bytes, uint32_t size)
{
    uint32_t low = 1u;
    uint32_t high = 0u;
    for (uint32_t index = 0u; index < size; ++index)
    {
        low = (low + bytes[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

static void PutU32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

// Открывает чанк: длина и тег. Возвращает начало данных.
static uint8_t *BeginChunk(uint32_t *cursor, const char *tag, uint32_t size)
{
    PutU32(g_file + *cursor, size);
    memcpy(g_file + *cursor + 4u, tag, 4u);
    return g_file + *cursor + 8u;
}

static void EndChunk(uint32_t *cursor, uint32_t size)
{
    Expect(size <= FILE_CAPACITY - 12u && *cursor <= FILE_CAPACITY - 12u - size,
           "the chunk must fit the file");
    PutU32(g_file + *cursor + 8u + size, Crc32(g_file + *cursor + 4u, size + 4u));
    *cursor += size + 12u;
}

static uint32_t FilterByte(uint32_t mode, uint32_t row)
{
    return mode == FILTER_MODE_MIXED ? row % FILTER_MODE_MIXED : mode;
}

static void FilterRow(uint8_t *destination, uint32_t row, uint32_t rowBytes, uint32_t unit,
                      uint32_t filter)
{
    const uint8_t *current = g_image[row];
    const uint8_t *upper = row > 0u ? g_image[row - 1u] : NULL;
    for (uint32_t index = 0u; index < rowBytes; ++index)
    {
        const int32_t left = index >= unit ? current[index - unit] : 0;
        const int32_t above = upper != NULL ? upper[index] : 0;
        const int32_t upperLeft = upper != NULL && index >= unit ? upper[index - unit] : 0;
        int32_t prediction = 0;
        if (filter == 1u) prediction = left;
        if (filter == 2u) prediction = above;
        if (filter == 3u) prediction = (left + above) / 2;
        if (filter == 4u) prediction = Paeth(left, above, upperLeft);
        destination[index] = (uint8_t)(current[index] - prediction);
    }
}

// Собирает файл PNG: IHDR, один IDAT с несжатым блоком и IEND.
static uint32_t BuildFile(const PixelLayout *layout, uint32_t width, uint32_t rowBytes,
                          uint32_t unit, uint32_t mode)
{
    uint32_t rawBytes = 0u;
    for (uint32_t row = 0u; row < IMAGE_HEIGHT; ++row)
    {
        const uint32_t filter = FilterByte(mode, row);
        g_raw[rawBytes++] = (uint8_t)filter;
        FilterRow(g_raw + rawBytes, row, rowBytes, unit, filter);
        rawBytes += rowBytes;
    }

    static const uint8_t SIGNATURE[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
    memcpy(g_file, SIGNATURE, sizeof(SIGNATURE));
    uint32_t cursor = (uint32_t)sizeof(SIGNATURE);

    uint8_t *header = BeginChunk(&cursor, "IHDR", 13u);
    PutU32(header, width);
    PutU32(header + 4u, IMAGE_HEIGHT);
    header[8] = (uint8_t)layout->bitDepth;
    header[9] = (uint8_t)layout->colorType;
    header[10] = 0u;
    header[11] = 0u;
    header[12] = 0u;
    EndChunk(&cursor, 13u);

    const uint32_t streamBytes = 2u + 5u + rawBytes + 4u;
    uint8_t *stream = BeginChunk(&cursor, "IDAT", streamBytes);
    stream[0] = 0x78u;
    stream[1] = 0x01u;
    stream[2] = 0x01u;
    stream[3] = (uint8_t)rawBytes;
    stream[4] = (uint8_t)(rawBytes >> 8);
    stream[5] = (uint8_t)~rawBytes;
    stream[6] = (uint8_t)(~rawBytes >> 8);
    memcpy(stream + 7u, g_raw, rawBytes);
    PutU32(stream + 7u + rawBytes, Adler32(g_raw, rawBytes));
    EndChunk(&cursor, streamBytes);

    (void)BeginChunk(&cursor, "IEND", 0u);
    EndChunk(&cursor, 0u);
    return cursor;
}

static ImageStatus DecodeFile(uint32_t fileBytes, uint8_t *pixels, uint32_t *outPixelBytes)
{
    ImageInfo info;
    Expect(ImageInspect(g_file, fileBytes, &info) == IMAGE_OK, "the file must be recognised");
    Expect(info.pixelBytes <= MAX_PIXEL_BYTES, "the pixels must fit their buffer");
    Expect(info.scratchBytes <= SCRATCH_CAPACITY, "the scratch must fit its buffer");
    *outPixelBytes = info.pixelBytes;
    return ImageDecode(g_file, fileBytes, &info, pixels, info.pixelBytes, g_scratch,
                       info.scratchBytes);
}

static void ExpectLayout(const PixelLayout *layout, uint32_t width)
{
    const uint32_t bitsPerPixel = layout->channels * layout->bitDepth;
    const uint32_t rowBytes = (width * bitsPerPixel + 7u) / 8u;
    const uint32_t unit = bitsPerPixel >= 8u ? bitsPerPixel / 8u : 1u;
    Expect(rowBytes <= MAX_ROW_BYTES, "the row must fit its buffer");

    // Соседние значения близки, как на снимке, а редкие скачки меняют
    // победителя Paeth: на одном шуме его ветви встречались бы реже.
    for (uint32_t row = 0u; row < IMAGE_HEIGHT; ++row)
    {
        for (uint32_t index = 0u; index < rowBytes; ++index)
        {
            const uint32_t base = index >= unit ? g_image[row][index - unit] : Random(256u);
            g_image[row][index] = Random(16u) == 0u ? (uint8_t)Random(256u)
                                                    : (uint8_t)(base + Random(9u) - 4u);
        }
    }

    uint32_t pixelBytes = 0u;
    uint32_t fileBytes = BuildFile(layout, width, rowBytes, unit, FILTER_MODE_NONE);
    Expect(DecodeFile(fileBytes, g_reference, &pixelBytes) == IMAGE_OK,
           "an unfiltered picture must decode");

    for (uint32_t mode = 1u; mode < FILTER_MODES; ++mode)
    {
        fileBytes = BuildFile(layout, width, rowBytes, unit, mode);
        Expect(DecodeFile(fileBytes, g_pixels, &pixelBytes) == IMAGE_OK,
               "a filtered picture must decode");
        Expect(memcmp(g_pixels, g_reference, pixelBytes) == 0,
               "a filter must restore the same pixels");
    }
}

static void ExpectInvalidFilter(void)
{
    const PixelLayout layout = {6u, 8u, 4u};
    memset(g_image, 0, sizeof(g_image));
    uint32_t fileBytes = BuildFile(&layout, 3u, 12u, 4u, FILTER_MODE_NONE);

    // Строка 0 начинается сразу за заголовками: подпись, IHDR, длина и тег
    // IDAT, два байта zlib и пять байт несжатого блока.
    const uint32_t firstFilter = 8u + 25u + 8u + 2u + 5u;
    g_file[firstFilter] = INVALID_FILTER;
    uint32_t pixelBytes = 0u;
    Expect(DecodeFile(fileBytes, g_pixels, &pixelBytes) == IMAGE_CORRUPT,
           "an unknown filter must fail");
}

LAIUE_TEST_ENTRY(PngFilterTestEntryPoint)
{
    g_random = 2024u;
    const uint32_t layouts = (uint32_t)(sizeof(LAYOUTS) / sizeof(LAYOUTS[0]));
    const uint32_t widths = (uint32_t)(sizeof(WIDTHS) / sizeof(WIDTHS[0]));
    for (uint32_t layout = 0u; layout < layouts; ++layout)
    {
        for (uint32_t width = 0u; width < widths; ++width)
        {
            ExpectLayout(&LAYOUTS[layout], WIDTHS[width]);
        }
    }
    ExpectInvalidFilter();

    LaiueTestRuntimeWrite("png filter test passed\n");
    LAIUE_TEST_SUCCESS();
}
