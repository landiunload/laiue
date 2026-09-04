#include "media/png_decode.h"

#include "media/inflate.h"

#include <stddef.h>

#define PNG_SIGNATURE_BYTES 8u
#define PNG_MAX_IDAT_CHUNKS 65536u

// CRC проверяется у служебных чанков и не проверяется у IDAT: данные
// пикселей и так покрыты Adler-32 в конце zlib-потока, а служебные
// чанки маленькие, и повреждение палитры иначе прошло бы молча,
// испортив цвета без единого сообщения.

typedef struct PngHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t bitDepth;
    uint32_t colorType;
    bool interlaced;

    uint32_t channels;       // каналов в исходном пикселе
    uint32_t bitsPerPixel;
    uint32_t filterUnit;     // байт на пиксель для фильтра, не меньше 1
} PngHeader;

typedef struct Adam7Pass
{
    uint32_t xOrigin;
    uint32_t yOrigin;
    uint32_t xStep;
    uint32_t yStep;
} Adam7Pass;

static const Adam7Pass ADAM7[7] = {
    {0u, 0u, 8u, 8u}, {4u, 0u, 8u, 8u}, {0u, 4u, 4u, 8u}, {2u, 0u, 4u, 4u},
    {0u, 2u, 2u, 4u}, {1u, 0u, 2u, 2u}, {0u, 1u, 1u, 2u},
};

static const uint8_t PNG_SIGNATURE[PNG_SIGNATURE_BYTES] = {137u, 80u, 78u, 71u,
                                                           13u,  10u, 26u, 10u};

static uint32_t ReadU32Be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static bool TagEquals(const uint8_t *bytes, const char *tag)
{
    return bytes[0] == (uint8_t)tag[0] && bytes[1] == (uint8_t)tag[1] &&
           bytes[2] == (uint8_t)tag[2] && bytes[3] == (uint8_t)tag[3];
}

// Побитовый CRC-32 без таблицы: служебные чанки занимают сотни байтов,
// и заводить ради них килобайт статических данных незачем.
static uint32_t Crc32(const uint8_t *bytes, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t index = 0; index < size; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0; bit < 8u; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t ScanlineBytes(const PngHeader *header, uint32_t width)
{
    return (width * header->bitsPerPixel + 7u) / 8u;
}

static ImageStatus ParseHeader(const uint8_t *bytes, uint32_t sizeBytes, PngHeader *outHeader)
{
    if (sizeBytes < PNG_SIGNATURE_BYTES + 8u + 13u + 4u) return IMAGE_TRUNCATED;
    for (uint32_t index = 0; index < PNG_SIGNATURE_BYTES; ++index)
    {
        if (bytes[index] != PNG_SIGNATURE[index]) return IMAGE_NOT_RECOGNISED;
    }

    const uint8_t *chunk = bytes + PNG_SIGNATURE_BYTES;
    if (ReadU32Be(chunk) != 13u || !TagEquals(chunk + 4, "IHDR")) return IMAGE_CORRUPT;
    if (Crc32(chunk + 4, 4u + 13u) != ReadU32Be(chunk + 8 + 13)) return IMAGE_CORRUPT;

    const uint8_t *header = chunk + 8;
    uint32_t width = ReadU32Be(header);
    uint32_t height = ReadU32Be(header + 4);
    uint32_t bitDepth = header[8];
    uint32_t colorType = header[9];
    uint32_t compression = header[10];
    uint32_t filter = header[11];
    uint32_t interlace = header[12];

    if (width == 0u || height == 0u) return IMAGE_CORRUPT;
    if (width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION) return IMAGE_TOO_LARGE;
    if (compression != 0u || filter != 0u) return IMAGE_UNSUPPORTED_FEATURE;
    if (interlace > 1u) return IMAGE_UNSUPPORTED_FEATURE;

    uint32_t channels = 0u;
    switch (colorType)
    {
    case 0u: channels = 1u; break;   // серый
    case 2u: channels = 3u; break;   // RGB
    case 3u: channels = 1u; break;   // палитра
    case 4u: channels = 2u; break;   // серый с альфой
    case 6u: channels = 4u; break;   // RGBA
    default: return IMAGE_CORRUPT;
    }

    bool depthAllowed = false;
    if (colorType == 0u)
    {
        depthAllowed = bitDepth == 1u || bitDepth == 2u || bitDepth == 4u || bitDepth == 8u ||
                       bitDepth == 16u;
    }
    else if (colorType == 3u)
    {
        depthAllowed = bitDepth == 1u || bitDepth == 2u || bitDepth == 4u || bitDepth == 8u;
    }
    else
    {
        depthAllowed = bitDepth == 8u || bitDepth == 16u;
    }
    if (!depthAllowed) return IMAGE_CORRUPT;

    outHeader->width = width;
    outHeader->height = height;
    outHeader->bitDepth = bitDepth;
    outHeader->colorType = colorType;
    outHeader->interlaced = interlace == 1u;
    outHeader->channels = channels;
    outHeader->bitsPerPixel = channels * bitDepth;
    outHeader->filterUnit = (outHeader->bitsPerPixel + 7u) / 8u;
    return IMAGE_OK;
}

// Полный размер распакованного потока: у чересстрочного PNG он равен
// сумме по семи проходам, и каждый проход имеет свои строки с фильтром.
static uint64_t RawStreamBytes(const PngHeader *header)
{
    if (!header->interlaced)
    {
        return (uint64_t)header->height * (1u + ScanlineBytes(header, header->width));
    }

    uint64_t total = 0u;
    for (uint32_t pass = 0; pass < 7u; ++pass)
    {
        const Adam7Pass *layout = &ADAM7[pass];
        if (header->width <= layout->xOrigin || header->height <= layout->yOrigin) continue;
        uint32_t passWidth =
            (header->width - layout->xOrigin + layout->xStep - 1u) / layout->xStep;
        uint32_t passHeight =
            (header->height - layout->yOrigin + layout->yStep - 1u) / layout->yStep;
        total += (uint64_t)passHeight * (1u + ScanlineBytes(header, passWidth));
    }
    return total;
}

typedef struct ChunkScan
{
    uint32_t idatCount;
    uint32_t paletteOffset;
    uint32_t paletteSize;
    uint32_t transparencyOffset;
    uint32_t transparencySize;
    bool sawEnd;
} ChunkScan;

static ImageStatus ScanChunks(const uint8_t *bytes, uint32_t sizeBytes, ChunkScan *outScan,
                              InflateSegment *segments, uint32_t segmentCapacity)
{
    ChunkScan scan = {0};
    uint32_t cursor = PNG_SIGNATURE_BYTES;
    while (cursor + 12u <= sizeBytes)
    {
        uint32_t chunkBytes = ReadU32Be(bytes + cursor);
        if (chunkBytes > sizeBytes - cursor - 12u) return IMAGE_TRUNCATED;

        const uint8_t *tag = bytes + cursor + 4u;
        const uint8_t *payload = tag + 4u;
        if (TagEquals(tag, "IDAT"))
        {
            if (scan.idatCount >= PNG_MAX_IDAT_CHUNKS) return IMAGE_TOO_LARGE;
            if (segments != NULL)
            {
                if (scan.idatCount >= segmentCapacity) return IMAGE_INVALID_ARGUMENT;
                segments[scan.idatCount].bytes = payload;
                segments[scan.idatCount].size = chunkBytes;
            }
            ++scan.idatCount;
        }
        else
        {
            if (Crc32(tag, 4u + chunkBytes) != ReadU32Be(payload + chunkBytes))
                return IMAGE_CORRUPT;
            if (TagEquals(tag, "PLTE"))
            {
                if (chunkBytes % 3u != 0u || chunkBytes > 256u * 3u) return IMAGE_CORRUPT;
                scan.paletteOffset = (uint32_t)(payload - bytes);
                scan.paletteSize = chunkBytes;
            }
            else if (TagEquals(tag, "tRNS"))
            {
                scan.transparencyOffset = (uint32_t)(payload - bytes);
                scan.transparencySize = chunkBytes;
            }
            else if (TagEquals(tag, "IEND"))
            {
                scan.sawEnd = true;
                break;
            }
        }

        cursor += 12u + chunkBytes;
    }

    if (!scan.sawEnd) return IMAGE_TRUNCATED;
    if (scan.idatCount == 0u) return IMAGE_CORRUPT;
    *outScan = scan;
    return IMAGE_OK;
}

ImageStatus PngInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;

    PngHeader header;
    ImageStatus status = ParseHeader((const uint8_t *)bytes, sizeBytes, &header);
    if (status != IMAGE_OK) return status;

    ChunkScan scan;
    status = ScanChunks((const uint8_t *)bytes, sizeBytes, &scan, NULL, 0u);
    if (status != IMAGE_OK) return status;
    if (header.colorType == 3u && scan.paletteSize == 0u) return IMAGE_CORRUPT;

    uint64_t rawBytes = RawStreamBytes(&header);
    uint64_t scratch = rawBytes + (uint64_t)scan.idatCount * sizeof(InflateSegment);
    if (scratch > 0xFFFFFFFFull) return IMAGE_TOO_LARGE;

    outInfo->width = header.width;
    outInfo->height = header.height;
    outInfo->frameCount = 1u;
    outInfo->frameMilliseconds[0] = 0u;
    outInfo->frameBytes = header.width * header.height * 4u;
    outInfo->pixelBytes = outInfo->frameBytes;
    outInfo->scratchBytes = (uint32_t)scratch;
    outInfo->hasAlpha =
        header.colorType == 4u || header.colorType == 6u || scan.transparencySize != 0u;
    return IMAGE_OK;
}

static uint8_t PaethPredictor(int32_t left, int32_t above, int32_t upperLeft)
{
    int32_t estimate = left + above - upperLeft;
    int32_t distanceLeft = estimate > left ? estimate - left : left - estimate;
    int32_t distanceAbove = estimate > above ? estimate - above : above - estimate;
    int32_t distanceUpperLeft =
        estimate > upperLeft ? estimate - upperLeft : upperLeft - estimate;

    if (distanceLeft <= distanceAbove && distanceLeft <= distanceUpperLeft) return (uint8_t)left;
    if (distanceAbove <= distanceUpperLeft) return (uint8_t)above;
    return (uint8_t)upperLeft;
}

// Снимает фильтр со строки на месте. Предыдущая строка уже развёрнута,
// поэтому обратные ссылки читают готовые байты.
static bool Unfilter(uint8_t *row, const uint8_t *previous, uint32_t rowBytes, uint32_t filterUnit,
                     uint32_t filterType)
{
    switch (filterType)
    {
    case 0u: return true;
    case 1u:
        for (uint32_t index = filterUnit; index < rowBytes; ++index)
        {
            row[index] = (uint8_t)(row[index] + row[index - filterUnit]);
        }
        return true;
    case 2u:
        if (previous == NULL) return true;
        for (uint32_t index = 0; index < rowBytes; ++index)
        {
            row[index] = (uint8_t)(row[index] + previous[index]);
        }
        return true;
    case 3u:
        for (uint32_t index = 0; index < rowBytes; ++index)
        {
            uint32_t left = index >= filterUnit ? row[index - filterUnit] : 0u;
            uint32_t above = previous != NULL ? previous[index] : 0u;
            row[index] = (uint8_t)(row[index] + (left + above) / 2u);
        }
        return true;
    case 4u:
        for (uint32_t index = 0; index < rowBytes; ++index)
        {
            int32_t left = index >= filterUnit ? row[index - filterUnit] : 0;
            int32_t above = previous != NULL ? previous[index] : 0;
            int32_t upperLeft =
                (previous != NULL && index >= filterUnit) ? previous[index - filterUnit] : 0;
            row[index] = (uint8_t)(row[index] + PaethPredictor(left, above, upperLeft));
        }
        return true;
    default: return false;
    }
}

// Значение канала, приведённое к 0..255. Глубины 1, 2 и 4 бита
// растягиваются повторением, а не сдвигом: иначе белый перестал бы быть
// белым.
static uint8_t ScaleSample(uint32_t value, uint32_t bitDepth)
{
    switch (bitDepth)
    {
    case 1u: return value != 0u ? 255u : 0u;
    case 2u: return (uint8_t)(value * 85u);
    case 4u: return (uint8_t)(value * 17u);
    default: return (uint8_t)value;
    }
}

static uint32_t ReadPackedSample(const uint8_t *row, uint32_t index, uint32_t bitDepth)
{
    switch (bitDepth)
    {
    case 8u: return row[index];
    case 16u: return ((uint32_t)row[index * 2u] << 8) | row[index * 2u + 1u];
    default:
    {
        uint32_t perByte = 8u / bitDepth;
        uint32_t shift = (perByte - 1u - (index % perByte)) * bitDepth;
        return (row[index / perByte] >> shift) & ((1u << bitDepth) - 1u);
    }
    }
}

typedef struct DecodeContext
{
    const PngHeader *header;
    const uint8_t *palette;        // PLTE, тройки RGB
    uint32_t paletteEntries;
    const uint8_t *transparency;   // tRNS
    uint32_t transparencySize;
} DecodeContext;

static void ExpandRow(const DecodeContext *context, const uint8_t *row, uint32_t rowWidth,
                      uint8_t *destination, uint32_t destinationStride)
{
    const PngHeader *header = context->header;
    uint32_t depth = header->bitDepth;
    // Глубина 16 бит приводится к 8 старшим битам: LTP хранит RGBA8, а
    // младший байт всё равно потерялся бы при записи.
    uint32_t shift = depth == 16u ? 8u : 0u;

    for (uint32_t pixel = 0; pixel < rowWidth; ++pixel)
    {
        uint8_t *out = destination + (size_t)pixel * destinationStride;
        uint32_t base = pixel * header->channels;

        switch (header->colorType)
        {
        case 0u:
        {
            uint32_t gray = ReadPackedSample(row, base, depth);
            uint8_t level = ScaleSample(gray >> shift, depth == 16u ? 8u : depth);
            out[0] = level;
            out[1] = level;
            out[2] = level;
            out[3] = 255u;
            if (context->transparencySize >= 2u)
            {
                uint32_t key = ((uint32_t)context->transparency[0] << 8) |
                               context->transparency[1];
                if (depth != 16u) key &= (1u << depth) - 1u;
                if (gray == key) out[3] = 0u;
            }
            break;
        }
        case 2u:
        {
            uint32_t red = ReadPackedSample(row, base, depth);
            uint32_t green = ReadPackedSample(row, base + 1u, depth);
            uint32_t blue = ReadPackedSample(row, base + 2u, depth);
            out[0] = (uint8_t)(red >> shift);
            out[1] = (uint8_t)(green >> shift);
            out[2] = (uint8_t)(blue >> shift);
            out[3] = 255u;
            if (context->transparencySize >= 6u)
            {
                uint32_t keyRed = ((uint32_t)context->transparency[0] << 8) |
                                  context->transparency[1];
                uint32_t keyGreen = ((uint32_t)context->transparency[2] << 8) |
                                    context->transparency[3];
                uint32_t keyBlue = ((uint32_t)context->transparency[4] << 8) |
                                   context->transparency[5];
                if (depth != 16u)
                {
                    uint32_t mask = (1u << depth) - 1u;
                    keyRed &= mask;
                    keyGreen &= mask;
                    keyBlue &= mask;
                }
                if (red == keyRed && green == keyGreen && blue == keyBlue) out[3] = 0u;
            }
            break;
        }
        case 3u:
        {
            uint32_t index = ReadPackedSample(row, base, depth);
            if (index >= context->paletteEntries) index = 0u;
            out[0] = context->palette[index * 3u];
            out[1] = context->palette[index * 3u + 1u];
            out[2] = context->palette[index * 3u + 2u];
            out[3] = index < context->transparencySize ? context->transparency[index] : 255u;
            break;
        }
        case 4u:
        {
            uint32_t gray = ReadPackedSample(row, base, depth) >> shift;
            uint32_t alpha = ReadPackedSample(row, base + 1u, depth) >> shift;
            out[0] = (uint8_t)gray;
            out[1] = (uint8_t)gray;
            out[2] = (uint8_t)gray;
            out[3] = (uint8_t)alpha;
            break;
        }
        default:
            out[0] = (uint8_t)(ReadPackedSample(row, base, depth) >> shift);
            out[1] = (uint8_t)(ReadPackedSample(row, base + 1u, depth) >> shift);
            out[2] = (uint8_t)(ReadPackedSample(row, base + 2u, depth) >> shift);
            out[3] = (uint8_t)(ReadPackedSample(row, base + 3u, depth) >> shift);
            break;
        }
    }
}

static ImageStatus DecodePass(const DecodeContext *context, uint8_t *raw, uint32_t passWidth,
                              uint32_t passHeight, uint32_t xOrigin, uint32_t yOrigin,
                              uint32_t xStep, uint32_t yStep, uint8_t *pixels)
{
    const PngHeader *header = context->header;
    uint32_t rowBytes = ScanlineBytes(header, passWidth);
    uint8_t *previous = NULL;

    for (uint32_t row = 0; row < passHeight; ++row)
    {
        uint8_t *line = raw + (size_t)row * (rowBytes + 1u);
        uint32_t filterType = line[0];
        uint8_t *data = line + 1u;
        if (!Unfilter(data, previous, rowBytes, header->filterUnit, filterType))
            return IMAGE_CORRUPT;

        uint32_t targetRow = yOrigin + row * yStep;
        uint8_t *destination = pixels + ((size_t)targetRow * header->width + xOrigin) * 4u;
        ExpandRow(context, data, passWidth, destination, xStep * 4u);
        previous = data;
    }
    return IMAGE_OK;
}

ImageStatus PngDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                      uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL || scratch == NULL)
        return IMAGE_INVALID_ARGUMENT;
    if (pixelBytes < info->pixelBytes || scratchBytes < info->scratchBytes)
        return IMAGE_BUFFER_TOO_SMALL;

    const uint8_t *file = (const uint8_t *)bytes;
    PngHeader header;
    ImageStatus status = ParseHeader(file, sizeBytes, &header);
    if (status != IMAGE_OK) return status;
    if (header.width != info->width || header.height != info->height)
        return IMAGE_INVALID_ARGUMENT;

    // Список отрезков живёт в начале рабочего буфера: он короткий, а
    // выравнивание у него строже, чем у байтов потока.
    InflateSegment *segments = (InflateSegment *)scratch;
    ChunkScan scan;
    status = ScanChunks(file, sizeBytes, &scan, segments,
                        scratchBytes / (uint32_t)sizeof(InflateSegment));
    if (status != IMAGE_OK) return status;

    uint32_t segmentBytes = scan.idatCount * (uint32_t)sizeof(InflateSegment);
    uint8_t *raw = (uint8_t *)scratch + segmentBytes;
    uint32_t rawBytes = info->scratchBytes - segmentBytes;

    uint32_t written = 0u;
    status = InflateZlib(segments, scan.idatCount, raw, rawBytes, &written);
    if (status != IMAGE_OK) return status;
    if (written != rawBytes) return IMAGE_CORRUPT;

    uint8_t defaultPalette[3] = {0u, 0u, 0u};
    DecodeContext context = {
        .header = &header,
        .palette = scan.paletteSize != 0u ? file + scan.paletteOffset : defaultPalette,
        .paletteEntries = scan.paletteSize / 3u,
        .transparency = scan.transparencySize != 0u ? file + scan.transparencyOffset : NULL,
        .transparencySize = scan.transparencySize,
    };
    if (header.colorType == 3u && context.paletteEntries == 0u) return IMAGE_CORRUPT;

    uint8_t *output = (uint8_t *)pixels;
    if (!header.interlaced)
    {
        return DecodePass(&context, raw, header.width, header.height, 0u, 0u, 1u, 1u, output);
    }

    uint8_t *cursor = raw;
    for (uint32_t pass = 0; pass < 7u; ++pass)
    {
        const Adam7Pass *layout = &ADAM7[pass];
        if (header.width <= layout->xOrigin || header.height <= layout->yOrigin) continue;
        uint32_t passWidth = (header.width - layout->xOrigin + layout->xStep - 1u) / layout->xStep;
        uint32_t passHeight =
            (header.height - layout->yOrigin + layout->yStep - 1u) / layout->yStep;

        status = DecodePass(&context, cursor, passWidth, passHeight, layout->xOrigin,
                            layout->yOrigin, layout->xStep, layout->yStep, output);
        if (status != IMAGE_OK) return status;
        cursor += (size_t)passHeight * (ScanlineBytes(&header, passWidth) + 1u);
    }
    return IMAGE_OK;
}
