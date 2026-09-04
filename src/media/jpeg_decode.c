#include "media/jpeg_decode.h"

#include <stddef.h>

#define JPEG_MAX_COMPONENTS 4u
#define JPEG_MAX_SAMPLING 4u
#define JPEG_HUFFMAN_TABLES 8u
#define JPEG_QUANT_TABLES 4u

// Порядок обхода коэффициентов внутри блока. Коэффициенты хранятся
// именно в нём, а не в естественном порядке: прогрессивный режим задаёт
// спектральный диапазон номерами зигзага, и переводить их туда-сюда на
// каждом блоке незачем. Естественный порядок нужен один раз, в обратном
// ДКП.
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// IDCT_BASIS[u][x] = C(u)/2 * cos((2x+1) u pi / 16), C(0) = 1/sqrt(2).
// Обратное ДКП здесь разделимое и прямое: два прохода по восемь
// умножений вместо быстрой бабочки. Текстура распаковывается один раз
// при загрузке, и понятность формулы важнее пары миллисекунд.
static const float IDCT_BASIS[8][8] = {
    {0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f, 0.353553391f,
     0.353553391f, 0.353553391f},
    {0.490392640f, 0.415734806f, 0.277785117f, 0.097545161f, -0.097545161f, -0.277785117f,
     -0.415734806f, -0.490392640f},
    {0.461939766f, 0.191341716f, -0.191341716f, -0.461939766f, -0.461939766f, -0.191341716f,
     0.191341716f, 0.461939766f},
    {0.415734806f, -0.097545161f, -0.490392640f, -0.277785117f, 0.277785117f, 0.490392640f,
     0.097545161f, -0.415734806f},
    {0.353553391f, -0.353553391f, -0.353553391f, 0.353553391f, 0.353553391f, -0.353553391f,
     -0.353553391f, 0.353553391f},
    {0.277785117f, -0.490392640f, 0.097545161f, 0.415734806f, -0.415734806f, -0.097545161f,
     0.490392640f, -0.277785117f},
    {0.191341716f, -0.461939766f, 0.461939766f, -0.191341716f, -0.191341716f, 0.461939766f,
     -0.461939766f, 0.191341716f},
    {0.097545161f, -0.277785117f, 0.415734806f, -0.490392640f, 0.490392640f, -0.415734806f,
     0.277785117f, -0.097545161f},
};

typedef struct JpegHuffman
{
    bool present;
    // Каноническая таблица по T.81 F.15: коды длины length лежат между
    // minCode[length] и maxCode[length], а их значения начинаются с
    // valuePointer[length]. maxCode = -1 означает, что кодов такой длины
    // в таблице нет.
    int32_t minCode[17];
    int32_t maxCode[17];
    int32_t valuePointer[17];
    uint8_t values[256];
} JpegHuffman;

typedef struct JpegComponent
{
    uint8_t id;
    uint8_t horizontal;
    uint8_t vertical;
    uint8_t quantTable;
    uint8_t dcTable;
    uint8_t acTable;
    int32_t dcPrediction;
    // Блоков в буфере: округлено вверх до целого MCU, потому что
    // чередующийся проход пишет блоки и за краем картинки.
    uint32_t blocksPerLine;
    uint32_t blocksPerColumn;
    // Блоков в самой картинке: одиночный проход обходит только их.
    uint32_t scanBlocksPerLine;
    uint32_t scanBlocksPerColumn;
    // Отсчётов в самом компоненте: за их пределами в буфере лежит
    // продолжение края, которое кодировщик дописал до целого блока.
    uint32_t plainWidth;
    uint32_t plainHeight;
    uint32_t sampleWidth;
    uint8_t *samples;
    int16_t *coefficients;
} JpegComponent;

typedef struct JpegFrame
{
    uint32_t width;
    uint32_t height;
    uint32_t componentCount;
    uint32_t maxHorizontal;
    uint32_t maxVertical;
    uint32_t mcusPerLine;
    uint32_t mcusPerColumn;
    bool progressive;
    // Adobe APP14 отличает RGB, записанный без преобразования, от
    // обычного YCbCr. Без маркера три компонента всегда YCbCr.
    bool adobe;
    uint8_t adobeTransform;
    JpegComponent components[JPEG_MAX_COMPONENTS];
} JpegFrame;

typedef struct JpegLayout
{
    uint32_t huffman;
    uint32_t quant;
    uint32_t samples[JPEG_MAX_COMPONENTS];
    uint32_t coefficients[JPEG_MAX_COMPONENTS];
    uint32_t total;
} JpegLayout;

typedef struct JpegReader
{
    const uint8_t *bytes;
    uint32_t sizeBytes;
    uint32_t position;
    uint32_t bitBuffer;
    uint32_t bitCount;
    // Встретился маркер: энтропийные данные кончились, и дальше
    // выдаются нули. Так повреждённый файл не уходит за конец буфера.
    bool markerReached;
} JpegReader;

typedef struct JpegDecoder
{
    JpegFrame frame;
    JpegReader reader;
    JpegHuffman *huffman;
    uint16_t *quant;
    bool quantPresent[JPEG_QUANT_TABLES];
    uint32_t restartInterval;
    uint32_t eobRun;
} JpegDecoder;

static uint32_t ReadU16Be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 8) | (uint32_t)bytes[1];
}

static uint32_t AlignUp8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
{
    return (value + divisor - 1u) / divisor;
}

static uint32_t ReadBit(JpegReader *reader)
{
    if (reader->bitCount == 0u)
    {
        if (reader->markerReached || reader->position >= reader->sizeBytes)
        {
            reader->markerReached = true;
            return 0u;
        }
        uint8_t byte = reader->bytes[reader->position];
        reader->position += 1u;
        if (byte == 0xFFu)
        {
            // Заполняющих 0xFF перед маркером может быть сколько угодно.
            uint32_t look = reader->position;
            while (look < reader->sizeBytes && reader->bytes[look] == 0xFFu) look += 1u;
            uint8_t next = look < reader->sizeBytes ? reader->bytes[look] : 0xD9u;
            if (next == 0x00u)
            {
                reader->position = look + 1u;
            }
            else
            {
                // Маркер: возвращаем позицию на него, чтобы рестарт и
                // разбор следующего сегмента нашли его на месте.
                reader->position -= 1u;
                reader->markerReached = true;
                return 0u;
            }
        }
        reader->bitBuffer = byte;
        reader->bitCount = 8u;
    }
    reader->bitCount -= 1u;
    return (reader->bitBuffer >> reader->bitCount) & 1u;
}

static uint32_t ReadBits(JpegReader *reader, uint32_t count)
{
    uint32_t value = 0u;
    for (uint32_t index = 0; index < count; ++index)
    {
        value = (value << 1) | ReadBit(reader);
    }
    return value;
}

static int32_t DecodeHuffman(JpegReader *reader, const JpegHuffman *table)
{
    if (!table->present) return -1;
    int32_t code = (int32_t)ReadBit(reader);
    for (uint32_t length = 1u; length <= 16u; ++length)
    {
        if (table->maxCode[length] >= 0 && code <= table->maxCode[length])
        {
            int32_t index = table->valuePointer[length] + code - table->minCode[length];
            if (index < 0 || index >= 256) return -1;
            return (int32_t)table->values[index];
        }
        if (length == 16u) break;
        code = (code << 1) | (int32_t)ReadBit(reader);
    }
    return -1;
}

// T.81 F.12: длина и прочитанные биты дают знаковое значение.
static int32_t Extend(uint32_t value, uint32_t length)
{
    if (length == 0u) return 0;
    int32_t threshold = (int32_t)1 << (length - 1u);
    if ((int32_t)value < threshold) return (int32_t)value - ((int32_t)1 << length) + 1;
    return (int32_t)value;
}

static ImageStatus BuildHuffman(JpegHuffman *table, const uint8_t *counts, const uint8_t *values,
                                uint32_t valueCount)
{
    int32_t code = 0;
    uint32_t taken = 0u;
    for (uint32_t length = 1u; length <= 16u; ++length)
    {
        uint32_t count = counts[length - 1u];
        table->valuePointer[length] = (int32_t)taken;
        table->minCode[length] = code;
        code += (int32_t)count;
        taken += count;
        if (count != 0u)
        {
            table->maxCode[length] = code - 1;
            // Кодов длины length не бывает больше, чем 2 в степени length.
            if (code > ((int32_t)1 << length)) return IMAGE_CORRUPT;
        }
        else
        {
            table->maxCode[length] = -1;
        }
        code <<= 1;
    }
    if (taken != valueCount) return IMAGE_CORRUPT;
    for (uint32_t index = 0; index < valueCount; ++index) table->values[index] = values[index];
    table->present = true;
    return IMAGE_OK;
}

// Обратное ДКП блока: коэффициенты лежат в порядке зигзага и уже
// умножены на таблицу квантования.
static void InverseTransform(const float *coefficients, uint8_t *destination, uint32_t stride)
{
    float natural[64];
    for (uint32_t index = 0; index < 64u; ++index) natural[index] = 0.0f;
    for (uint32_t index = 0; index < 64u; ++index) natural[ZIGZAG[index]] = coefficients[index];

    float rows[64];
    for (uint32_t v = 0; v < 8u; ++v)
    {
        for (uint32_t x = 0; x < 8u; ++x)
        {
            float sum = 0.0f;
            for (uint32_t u = 0; u < 8u; ++u) sum += natural[v * 8u + u] * IDCT_BASIS[u][x];
            rows[v * 8u + x] = sum;
        }
    }
    for (uint32_t y = 0; y < 8u; ++y)
    {
        uint8_t *line = destination + (size_t)y * stride;
        for (uint32_t x = 0; x < 8u; ++x)
        {
            float sum = 0.0f;
            for (uint32_t v = 0; v < 8u; ++v) sum += rows[v * 8u + x] * IDCT_BASIS[v][y];
            float level = sum + 128.5f;
            int32_t sample = 0;
            if (level >= 256.0f) sample = 255;
            else if (level > 0.0f) sample = (int32_t)level;
            line[x] = (uint8_t)sample;
        }
    }
}

// Между сегментами допустимы заполняющие 0xFF, а некоторые кодировщики
// оставляют там же мусор. Ищем ближайший настоящий маркер.
static ImageStatus NextMarker(const uint8_t *bytes, uint32_t sizeBytes, uint32_t *position,
                              uint8_t *outMarker)
{
    uint32_t at = *position;
    while (at < sizeBytes && bytes[at] != 0xFFu) at += 1u;
    while (at < sizeBytes && bytes[at] == 0xFFu) at += 1u;
    if (at >= sizeBytes) return IMAGE_TRUNCATED;
    *outMarker = bytes[at];
    *position = at + 1u;
    return IMAGE_OK;
}

static bool MarkerIsStandalone(uint8_t marker)
{
    // SOI, EOI, RSTn и TEM идут без длины.
    return marker == 0xD8u || marker == 0xD9u || marker == 0x01u ||
           (marker >= 0xD0u && marker <= 0xD7u);
}

static ImageStatus ParseFrameSegment(const uint8_t *segment, uint32_t length, bool progressive,
                                     JpegFrame *frame)
{
    if (length < 6u) return IMAGE_CORRUPT;
    uint32_t precision = segment[0];
    // Двенадцать бит на отсчёт стандарт допускает, но наш выход
    // восьмибитный, и молча терять четыре бита хуже, чем отказаться.
    if (precision != 8u) return IMAGE_UNSUPPORTED_FEATURE;

    uint32_t height = ReadU16Be(segment + 1);
    uint32_t width = ReadU16Be(segment + 3);
    uint32_t componentCount = segment[5];
    // Нулевая высота означает маркер DNL после первого прохода: размер
    // становится известен только в конце файла.
    if (width == 0u || height == 0u) return IMAGE_UNSUPPORTED_FEATURE;
    if (width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION) return IMAGE_TOO_LARGE;
    if (componentCount != 1u && componentCount != 3u) return IMAGE_UNSUPPORTED_FEATURE;
    if (length < 6u + componentCount * 3u) return IMAGE_CORRUPT;

    frame->width = width;
    frame->height = height;
    frame->componentCount = componentCount;
    frame->progressive = progressive;
    frame->maxHorizontal = 1u;
    frame->maxVertical = 1u;

    for (uint32_t index = 0; index < componentCount; ++index)
    {
        const uint8_t *entry = segment + 6u + index * 3u;
        JpegComponent *component = &frame->components[index];
        component->id = entry[0];
        component->horizontal = (uint8_t)(entry[1] >> 4);
        component->vertical = (uint8_t)(entry[1] & 15u);
        component->quantTable = entry[2];
        if (component->horizontal == 0u || component->horizontal > JPEG_MAX_SAMPLING) return IMAGE_CORRUPT;
        if (component->vertical == 0u || component->vertical > JPEG_MAX_SAMPLING) return IMAGE_CORRUPT;
        if (component->quantTable >= JPEG_QUANT_TABLES) return IMAGE_CORRUPT;
        if (component->horizontal > frame->maxHorizontal) frame->maxHorizontal = component->horizontal;
        if (component->vertical > frame->maxVertical) frame->maxVertical = component->vertical;
    }

    frame->mcusPerLine = DivideRoundUp(width, 8u * frame->maxHorizontal);
    frame->mcusPerColumn = DivideRoundUp(height, 8u * frame->maxVertical);

    for (uint32_t index = 0; index < componentCount; ++index)
    {
        JpegComponent *component = &frame->components[index];
        uint32_t componentWidth =
            DivideRoundUp(width * component->horizontal, frame->maxHorizontal);
        uint32_t componentHeight = DivideRoundUp(height * component->vertical, frame->maxVertical);
        component->plainWidth = componentWidth;
        component->plainHeight = componentHeight;
        component->scanBlocksPerLine = DivideRoundUp(componentWidth, 8u);
        component->scanBlocksPerColumn = DivideRoundUp(componentHeight, 8u);
        component->blocksPerLine = frame->mcusPerLine * component->horizontal;
        component->blocksPerColumn = frame->mcusPerColumn * component->vertical;
        component->sampleWidth = component->blocksPerLine * 8u;
    }
    return IMAGE_OK;
}

static ImageStatus ComputeLayout(const JpegFrame *frame, JpegLayout *outLayout)
{
    uint64_t offset = 0u;
    outLayout->huffman = 0u;
    offset += (uint64_t)JPEG_HUFFMAN_TABLES * sizeof(JpegHuffman);
    offset = AlignUp8((uint32_t)offset);
    outLayout->quant = (uint32_t)offset;
    offset += (uint64_t)JPEG_QUANT_TABLES * 64u * sizeof(uint16_t);

    for (uint32_t index = 0; index < frame->componentCount; ++index)
    {
        const JpegComponent *component = &frame->components[index];
        offset = (offset + 7u) & ~(uint64_t)7u;
        if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
        outLayout->samples[index] = (uint32_t)offset;
        offset += (uint64_t)component->sampleWidth * component->blocksPerColumn * 8u;
    }
    if (frame->progressive)
    {
        for (uint32_t index = 0; index < frame->componentCount; ++index)
        {
            const JpegComponent *component = &frame->components[index];
            offset = (offset + 7u) & ~(uint64_t)7u;
            if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
            outLayout->coefficients[index] = (uint32_t)offset;
            offset += (uint64_t)component->blocksPerLine * component->blocksPerColumn * 64u *
                      sizeof(int16_t);
        }
    }
    else
    {
        for (uint32_t index = 0; index < JPEG_MAX_COMPONENTS; ++index)
        {
            outLayout->coefficients[index] = 0u;
        }
    }
    if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
    outLayout->total = (uint32_t)offset;
    return IMAGE_OK;
}

// Доходит до заголовка кадра и останавливается: размеры и расклад
// памяти известны уже там, а разбирать таблицы ради Inspect незачем.
static ImageStatus ParseFrameHeader(const uint8_t *bytes, uint32_t sizeBytes, JpegFrame *frame)
{
    if (sizeBytes < 4u) return IMAGE_TRUNCATED;
    if (bytes[0] != 0xFFu || bytes[1] != 0xD8u) return IMAGE_NOT_RECOGNISED;

    for (uint32_t index = 0; index < JPEG_MAX_COMPONENTS; ++index)
    {
        frame->components[index].samples = NULL;
        frame->components[index].coefficients = NULL;
    }
    frame->adobe = false;
    frame->adobeTransform = 0u;

    uint32_t position = 2u;
    for (;;)
    {
        uint8_t marker = 0u;
        ImageStatus status = NextMarker(bytes, sizeBytes, &position, &marker);
        if (status != IMAGE_OK) return status;
        if (marker == 0xD9u) return IMAGE_CORRUPT;   // EOI до заголовка кадра
        if (MarkerIsStandalone(marker)) continue;

        if (position + 2u > sizeBytes) return IMAGE_TRUNCATED;
        uint32_t length = ReadU16Be(bytes + position);
        if (length < 2u) return IMAGE_CORRUPT;
        if (position + length > sizeBytes) return IMAGE_TRUNCATED;
        const uint8_t *segment = bytes + position + 2u;
        uint32_t payload = length - 2u;

        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u)
        {
            return ParseFrameSegment(segment, payload, marker == 0xC2u, frame);
        }
        if ((marker >= 0xC3u && marker <= 0xCFu) && marker != 0xC4u && marker != 0xC8u &&
            marker != 0xCCu)
        {
            // Иерархический режим, потери без ДКП и арифметическое
            // кодирование: разбирать их наполовину хуже, чем сказать
            // об этом прямо.
            return IMAGE_UNSUPPORTED_FEATURE;
        }
        if (marker == 0xEEu && payload >= 12u && segment[0] == 'A' && segment[1] == 'd' &&
            segment[2] == 'o' && segment[3] == 'b' && segment[4] == 'e')
        {
            frame->adobe = true;
            frame->adobeTransform = segment[11];
        }
        if (marker == 0xDAu) return IMAGE_CORRUPT;   // проход до заголовка кадра

        position += length;
    }
}

ImageStatus JpegInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;

    JpegFrame frame;
    ImageStatus status = ParseFrameHeader((const uint8_t *)bytes, sizeBytes, &frame);
    if (status != IMAGE_OK) return status;

    JpegLayout layout;
    status = ComputeLayout(&frame, &layout);
    if (status != IMAGE_OK) return status;

    uint32_t frameBytes = frame.width * frame.height * 4u;
    outInfo->width = frame.width;
    outInfo->height = frame.height;
    outInfo->frameCount = 1u;
    outInfo->frameMilliseconds[0] = 0u;
    outInfo->frameBytes = frameBytes;
    outInfo->pixelBytes = frameBytes;
    outInfo->scratchBytes = layout.total;
    outInfo->hasAlpha = false;
    return IMAGE_OK;
}

typedef struct JpegScan
{
    uint32_t componentCount;
    uint32_t componentIndex[JPEG_MAX_COMPONENTS];
    uint32_t spectralStart;
    uint32_t spectralEnd;
    uint32_t approximationHigh;
    uint32_t approximationLow;
} JpegScan;

// Коэффициент не выходит за int16 у любого правильного файла. Зажатие
// защищает от испорченного: лучше неверный пиксель, чем переполнение.
static int32_t ClampCoefficient(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

static ImageStatus ParseQuantSegment(JpegDecoder *decoder, const uint8_t *segment, uint32_t length)
{
    uint32_t offset = 0u;
    while (offset < length)
    {
        uint32_t selector = segment[offset];
        offset += 1u;
        uint32_t precision = selector >> 4;
        uint32_t table = selector & 15u;
        if (precision > 1u || table >= JPEG_QUANT_TABLES) return IMAGE_CORRUPT;
        uint32_t needed = precision == 0u ? 64u : 128u;
        if (offset + needed > length) return IMAGE_CORRUPT;

        uint16_t *values = decoder->quant + (size_t)table * 64u;
        for (uint32_t index = 0; index < 64u; ++index)
        {
            values[index] = precision == 0u
                                ? (uint16_t)segment[offset + index]
                                : (uint16_t)ReadU16Be(segment + offset + index * 2u);
        }
        offset += needed;
        decoder->quantPresent[table] = true;
    }
    return IMAGE_OK;
}

static ImageStatus ParseHuffmanSegment(JpegDecoder *decoder, const uint8_t *segment,
                                       uint32_t length)
{
    uint32_t offset = 0u;
    while (offset < length)
    {
        uint32_t selector = segment[offset];
        offset += 1u;
        uint32_t tableClass = selector >> 4;
        uint32_t table = selector & 15u;
        if (tableClass > 1u || table >= 4u) return IMAGE_CORRUPT;
        if (offset + 16u > length) return IMAGE_CORRUPT;

        uint32_t total = 0u;
        for (uint32_t index = 0; index < 16u; ++index) total += segment[offset + index];
        if (total > 256u) return IMAGE_CORRUPT;
        if (offset + 16u + total > length) return IMAGE_CORRUPT;

        ImageStatus status = BuildHuffman(&decoder->huffman[tableClass * 4u + table],
                                          segment + offset, segment + offset + 16u, total);
        if (status != IMAGE_OK) return status;
        offset += 16u + total;
    }
    return IMAGE_OK;
}

static ImageStatus ParseScanSegment(JpegDecoder *decoder, const uint8_t *segment, uint32_t length,
                                    JpegScan *outScan)
{
    if (length < 1u) return IMAGE_CORRUPT;
    uint32_t count = segment[0];
    if (count == 0u || count > decoder->frame.componentCount) return IMAGE_CORRUPT;
    if (length < 1u + count * 2u + 3u) return IMAGE_CORRUPT;

    outScan->componentCount = count;
    for (uint32_t index = 0; index < count; ++index)
    {
        uint32_t id = segment[1u + index * 2u];
        uint32_t tables = segment[2u + index * 2u];
        uint32_t found = JPEG_MAX_COMPONENTS;
        for (uint32_t search = 0; search < decoder->frame.componentCount; ++search)
        {
            if (decoder->frame.components[search].id == id)
            {
                found = search;
                break;
            }
        }
        if (found == JPEG_MAX_COMPONENTS) return IMAGE_CORRUPT;

        JpegComponent *component = &decoder->frame.components[found];
        component->dcTable = (uint8_t)(tables >> 4);
        component->acTable = (uint8_t)(tables & 15u);
        if (component->dcTable >= 4u || component->acTable >= 4u) return IMAGE_CORRUPT;
        // Таблица квантования обязана прийти до прохода: иначе кадр
        // разобрался бы с нулевым делителем и вышел серым.
        if (!decoder->quantPresent[component->quantTable]) return IMAGE_CORRUPT;
        outScan->componentIndex[index] = found;
    }

    const uint8_t *tail = segment + 1u + count * 2u;
    if (decoder->frame.progressive)
    {
        outScan->spectralStart = tail[0];
        outScan->spectralEnd = tail[1];
        outScan->approximationHigh = (uint32_t)(tail[2] >> 4);
        outScan->approximationLow = (uint32_t)(tail[2] & 15u);
        if (outScan->spectralEnd > 63u) return IMAGE_CORRUPT;
        if (outScan->spectralStart > outScan->spectralEnd) return IMAGE_CORRUPT;
        // Проход постоянной составляющей охватывает только её, а проход
        // переменных читает ровно один компонент.
        if (outScan->spectralStart == 0u && outScan->spectralEnd != 0u) return IMAGE_CORRUPT;
        if (outScan->spectralStart != 0u && count != 1u) return IMAGE_CORRUPT;
        if (outScan->approximationLow > 13u) return IMAGE_CORRUPT;
    }
    else
    {
        outScan->spectralStart = 0u;
        outScan->spectralEnd = 63u;
        outScan->approximationHigh = 0u;
        outScan->approximationLow = 0u;
    }
    return IMAGE_OK;
}

static void OutputBlock(const JpegDecoder *decoder, const JpegComponent *component,
                        const int16_t *block, uint32_t blockRow, uint32_t blockColumn)
{
    if (blockRow >= component->blocksPerColumn || blockColumn >= component->blocksPerLine) return;

    const uint16_t *quant = decoder->quant + (size_t)component->quantTable * 64u;
    float values[64];
    for (uint32_t index = 0; index < 64u; ++index)
    {
        values[index] = (float)block[index] * (float)quant[index];
    }
    uint8_t *destination = component->samples +
                           (size_t)blockRow * 8u * component->sampleWidth + (size_t)blockColumn * 8u;
    InverseTransform(values, destination, component->sampleWidth);
}

static ImageStatus DecodeSequentialBlock(JpegDecoder *decoder, JpegComponent *component,
                                         int16_t *block)
{
    int32_t symbol = DecodeHuffman(&decoder->reader, &decoder->huffman[component->dcTable]);
    if (symbol < 0 || symbol > 15) return IMAGE_CORRUPT;
    int32_t difference =
        symbol != 0
            ? Extend(ReadBits(&decoder->reader, (uint32_t)symbol), (uint32_t)symbol)
            : 0;
    component->dcPrediction = ClampCoefficient(component->dcPrediction + difference);
    block[0] = (int16_t)component->dcPrediction;

    uint32_t index = 1u;
    while (index < 64u)
    {
        int32_t pair = DecodeHuffman(&decoder->reader, &decoder->huffman[4u + component->acTable]);
        if (pair < 0) return IMAGE_CORRUPT;
        uint32_t size = (uint32_t)pair & 15u;
        uint32_t run = (uint32_t)pair >> 4;
        if (size == 0u)
        {
            if (run != 15u) break;   // конец блока
            index += 16u;
            continue;
        }
        index += run;
        if (index >= 64u) return IMAGE_CORRUPT;
        block[index] = (int16_t)Extend(ReadBits(&decoder->reader, size), size);
        index += 1u;
    }
    return IMAGE_OK;
}

static ImageStatus DecodeDcFirst(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                 uint32_t approximationLow)
{
    int32_t symbol = DecodeHuffman(&decoder->reader, &decoder->huffman[component->dcTable]);
    if (symbol < 0 || symbol > 15) return IMAGE_CORRUPT;
    int32_t difference =
        symbol != 0
            ? Extend(ReadBits(&decoder->reader, (uint32_t)symbol), (uint32_t)symbol)
            : 0;
    component->dcPrediction = ClampCoefficient(component->dcPrediction + difference);
    block[0] =
        (int16_t)ClampCoefficient(component->dcPrediction * (int32_t)(1u << approximationLow));
    return IMAGE_OK;
}

static void DecodeDcRefine(JpegDecoder *decoder, int16_t *block, uint32_t approximationLow)
{
    if (ReadBit(&decoder->reader) != 0u)
    {
        int32_t value = (int32_t)block[0] | (int32_t)(1u << approximationLow);
        block[0] = (int16_t)ClampCoefficient(value);
    }
}

static ImageStatus DecodeAcFirst(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                 const JpegScan *scan)
{
    if (decoder->eobRun > 0u)
    {
        decoder->eobRun -= 1u;
        return IMAGE_OK;
    }
    uint32_t index = scan->spectralStart;
    while (index <= scan->spectralEnd)
    {
        int32_t pair = DecodeHuffman(&decoder->reader, &decoder->huffman[4u + component->acTable]);
        if (pair < 0) return IMAGE_CORRUPT;
        uint32_t size = (uint32_t)pair & 15u;
        uint32_t run = (uint32_t)pair >> 4;
        if (size == 0u)
        {
            if (run < 15u)
            {
                // Конец блока сразу для целой серии блоков.
                decoder->eobRun = (1u << run) - 1u;
                if (run > 0u) decoder->eobRun += ReadBits(&decoder->reader, run);
                break;
            }
            index += 16u;
            continue;
        }
        index += run;
        if (index > scan->spectralEnd) return IMAGE_CORRUPT;
        int32_t value = Extend(ReadBits(&decoder->reader, size), size);
        block[index] =
            (int16_t)ClampCoefficient(value * (int32_t)(1u << scan->approximationLow));
        index += 1u;
    }
    return IMAGE_OK;
}

// T.81 G.1.2.3. Уточняющий проход не приносит новых значений целиком:
// он добавляет один бит уже известным коэффициентам и вставляет
// единицы там, где раньше был ноль. Порядок битов в потоке задан
// перебором коэффициентов, поэтому пропустить ни одного нельзя.
static ImageStatus DecodeAcRefine(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                  const JpegScan *scan)
{
    int32_t positive = (int32_t)(1u << scan->approximationLow);
    uint32_t index = scan->spectralStart;

    if (decoder->eobRun == 0u)
    {
        while (index <= scan->spectralEnd)
        {
            int32_t pair =
                DecodeHuffman(&decoder->reader, &decoder->huffman[4u + component->acTable]);
            if (pair < 0) return IMAGE_CORRUPT;
            uint32_t size = (uint32_t)pair & 15u;
            uint32_t run = (uint32_t)pair >> 4;
            int32_t value = 0;

            if (size == 0u)
            {
                if (run < 15u)
                {
                    decoder->eobRun = 1u << run;
                    if (run > 0u) decoder->eobRun += ReadBits(&decoder->reader, run);
                    break;
                }
            }
            else
            {
                // Уточняющий проход не кодирует величину: только знак.
                if (size != 1u) return IMAGE_CORRUPT;
                value = ReadBit(&decoder->reader) != 0u ? positive : -positive;
            }

            while (index <= scan->spectralEnd)
            {
                int16_t *coefficient = &block[index];
                if (*coefficient != 0)
                {
                    if (ReadBit(&decoder->reader) != 0u && ((int32_t)*coefficient & positive) == 0)
                    {
                        *coefficient = (int16_t)ClampCoefficient(
                            (int32_t)*coefficient + (*coefficient >= 0 ? positive : -positive));
                    }
                }
                else
                {
                    if (run == 0u)
                    {
                        if (value != 0) *coefficient = (int16_t)value;
                        index += 1u;
                        break;
                    }
                    run -= 1u;
                }
                index += 1u;
            }
        }
    }

    if (decoder->eobRun > 0u)
    {
        // Внутри серии конца блока новых коэффициентов нет, но
        // корректирующие биты у ненулевых всё равно лежат в потоке.
        while (index <= scan->spectralEnd)
        {
            int16_t *coefficient = &block[index];
            if (*coefficient != 0)
            {
                if (ReadBit(&decoder->reader) != 0u && ((int32_t)*coefficient & positive) == 0)
                {
                    *coefficient = (int16_t)ClampCoefficient(
                        (int32_t)*coefficient + (*coefficient >= 0 ? positive : -positive));
                }
            }
            index += 1u;
        }
        decoder->eobRun -= 1u;
    }
    return IMAGE_OK;
}

static ImageStatus DecodeBlock(JpegDecoder *decoder, const JpegScan *scan,
                               JpegComponent *component, uint32_t blockRow, uint32_t blockColumn)
{
    if (decoder->frame.progressive)
    {
        if (blockRow >= component->blocksPerColumn || blockColumn >= component->blocksPerLine)
        {
            return IMAGE_CORRUPT;
        }
        int16_t *block =
            component->coefficients +
            ((size_t)blockRow * component->blocksPerLine + blockColumn) * 64u;
        if (scan->spectralStart == 0u)
        {
            if (scan->approximationHigh == 0u)
            {
                return DecodeDcFirst(decoder, component, block, scan->approximationLow);
            }
            DecodeDcRefine(decoder, block, scan->approximationLow);
            return IMAGE_OK;
        }
        if (scan->approximationHigh == 0u) return DecodeAcFirst(decoder, component, block, scan);
        return DecodeAcRefine(decoder, component, block, scan);
    }

    int16_t block[64];
    for (uint32_t index = 0; index < 64u; ++index) block[index] = 0;
    ImageStatus status = DecodeSequentialBlock(decoder, component, block);
    if (status != IMAGE_OK) return status;
    OutputBlock(decoder, component, block, blockRow, blockColumn);
    return IMAGE_OK;
}

static ImageStatus ReadRestart(JpegDecoder *decoder)
{
    JpegReader *reader = &decoder->reader;
    reader->bitCount = 0u;
    reader->markerReached = false;
    while (reader->position + 1u < reader->sizeBytes)
    {
        if (reader->bytes[reader->position] != 0xFFu)
        {
            reader->position += 1u;
            continue;
        }
        uint8_t marker = reader->bytes[reader->position + 1u];
        if (marker >= 0xD0u && marker <= 0xD7u)
        {
            reader->position += 2u;
            return IMAGE_OK;
        }
        if (marker == 0xFFu)
        {
            reader->position += 1u;
            continue;
        }
        // Другой маркер там, где обязан стоять рестарт: данные прохода
        // кончились раньше, чем обещал заголовок.
        return IMAGE_TRUNCATED;
    }
    return IMAGE_TRUNCATED;
}

static ImageStatus DecodeScan(JpegDecoder *decoder, const JpegScan *scan)
{
    JpegFrame *frame = &decoder->frame;
    decoder->eobRun = 0u;
    for (uint32_t index = 0; index < scan->componentCount; ++index)
    {
        frame->components[scan->componentIndex[index]].dcPrediction = 0;
    }

    uint32_t unitsPerLine = 0u;
    uint32_t unitCount = 0u;
    if (scan->componentCount == 1u)
    {
        // Одиночный проход обходит блоки самого компонента, а не MCU:
        // выравнивание до целого MCU в поток не попадает.
        const JpegComponent *component = &frame->components[scan->componentIndex[0]];
        unitsPerLine = component->scanBlocksPerLine;
        unitCount = component->scanBlocksPerLine * component->scanBlocksPerColumn;
    }
    else
    {
        unitsPerLine = frame->mcusPerLine;
        unitCount = frame->mcusPerLine * frame->mcusPerColumn;
    }
    if (unitsPerLine == 0u || unitCount == 0u) return IMAGE_CORRUPT;

    uint32_t interval = decoder->restartInterval != 0u ? decoder->restartInterval : unitCount;
    uint32_t unit = 0u;
    while (unit < unitCount)
    {
        uint32_t limit = unit + interval;
        if (limit > unitCount || limit < unit) limit = unitCount;
        for (; unit < limit; ++unit)
        {
            uint32_t row = unit / unitsPerLine;
            uint32_t column = unit % unitsPerLine;
            if (scan->componentCount == 1u)
            {
                JpegComponent *component = &frame->components[scan->componentIndex[0]];
                ImageStatus status = DecodeBlock(decoder, scan, component, row, column);
                if (status != IMAGE_OK) return status;
                continue;
            }
            for (uint32_t index = 0; index < scan->componentCount; ++index)
            {
                JpegComponent *component = &frame->components[scan->componentIndex[index]];
                for (uint32_t vertical = 0; vertical < component->vertical; ++vertical)
                {
                    for (uint32_t horizontal = 0; horizontal < component->horizontal; ++horizontal)
                    {
                        ImageStatus status =
                            DecodeBlock(decoder, scan, component,
                                        row * component->vertical + vertical,
                                        column * component->horizontal + horizontal);
                        if (status != IMAGE_OK) return status;
                    }
                }
            }
        }
        if (unit < unitCount)
        {
            ImageStatus status = ReadRestart(decoder);
            if (status != IMAGE_OK) return status;
            decoder->eobRun = 0u;
            for (uint32_t index = 0; index < scan->componentCount; ++index)
            {
                frame->components[scan->componentIndex[index]].dcPrediction = 0;
            }
        }
    }
    return IMAGE_OK;
}

// Прогрессивный кадр собирается целиком только после последнего
// прохода: до него коэффициенты неполные, и обратное ДКП дало бы
// промежуточную картинку.
static void FinishProgressive(const JpegDecoder *decoder)
{
    const JpegFrame *frame = &decoder->frame;
    for (uint32_t index = 0; index < frame->componentCount; ++index)
    {
        const JpegComponent *component = &frame->components[index];
        for (uint32_t row = 0; row < component->blocksPerColumn; ++row)
        {
            for (uint32_t column = 0; column < component->blocksPerLine; ++column)
            {
                const int16_t *block =
                    component->coefficients +
                    ((size_t)row * component->blocksPerLine + column) * 64u;
                OutputBlock(decoder, component, block, row, column);
            }
        }
    }
}

// Целочисленное преобразование с теми же коэффициентами, что у
// эталонных реализаций: расхождение с ними остаётся в пределах
// округления обратного ДКП, а не накапливается на цвете.
#define JPEG_COLOR_HALF 32768
static const int32_t JPEG_CR_TO_RED = 91881;      // 1.40200
static const int32_t JPEG_CB_TO_BLUE = 116130;    // 1.77200
static const int32_t JPEG_CB_TO_GREEN = -22554;   // -0.34414
static const int32_t JPEG_CR_TO_GREEN = -46802;   // -0.71414

static uint8_t ClampByte(int32_t value)
{
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

// Отсчёт прореженного компонента лежит посередине своей пары, а не в
// её начале, поэтому восстановление половинного разрешения — не
// повторение соседа, а взвешивание три к одному. Повторение отличалось
// бы от любого другого декодера на десятки единиц по краям цвета: это
// измерено, а не предположено.
//
// Отношения, отличные от вдвое, встречаются редко (4:1:1 и подобное);
// для них берётся ближайший отсчёт, потому что промежуточные веса там
// уже неочевидны, а картинок таких почти нет.
static void AxisTap(uint32_t coordinate, uint32_t sampling, uint32_t maximum, uint32_t extent,
                    uint32_t *outNear, uint32_t *outFar, uint32_t *outNearWeight)
{
    if (sampling == maximum)
    {
        *outNear = coordinate;
        *outFar = coordinate;
        *outNearWeight = 4u;
        return;
    }
    if (maximum == sampling * 2u)
    {
        uint32_t centre = coordinate >> 1;
        uint32_t neighbour = centre;
        if ((coordinate & 1u) != 0u) neighbour = centre + 1u;
        else if (centre != 0u) neighbour = centre - 1u;
        if (centre >= extent) centre = extent - 1u;
        if (neighbour >= extent) neighbour = extent - 1u;
        *outNear = centre;
        *outFar = neighbour;
        *outNearWeight = 3u;
        return;
    }
    uint32_t nearest = coordinate * sampling / maximum;
    if (nearest >= extent) nearest = extent - 1u;
    *outNear = nearest;
    *outFar = nearest;
    *outNearWeight = 4u;
}

static uint8_t SampleAt(const JpegFrame *frame, const JpegComponent *component, uint32_t x,
                        uint32_t y)
{
    if (component->horizontal == frame->maxHorizontal &&
        component->vertical == frame->maxVertical)
    {
        return component->samples[(size_t)y * component->sampleWidth + x];
    }

    uint32_t nearX = 0u;
    uint32_t farX = 0u;
    uint32_t weightX = 4u;
    AxisTap(x, component->horizontal, frame->maxHorizontal, component->plainWidth, &nearX, &farX,
            &weightX);

    uint32_t nearY = 0u;
    uint32_t farY = 0u;
    uint32_t weightY = 4u;
    AxisTap(y, component->vertical, frame->maxVertical, component->plainHeight, &nearY, &farY,
            &weightY);

    const uint8_t *nearRow = component->samples + (size_t)nearY * component->sampleWidth;
    const uint8_t *farRow = component->samples + (size_t)farY * component->sampleWidth;
    uint32_t restX = 4u - weightX;
    uint32_t restY = 4u - weightY;
    uint32_t total = weightX * weightY * nearRow[nearX] + restX * weightY * nearRow[farX] +
                     weightX * restY * farRow[nearX] + restX * restY * farRow[farX];
    return (uint8_t)((total + 8u) >> 4);
}

static void ConvertToRgba(const JpegFrame *frame, uint8_t *pixels)
{
    // Adobe с transform 0 означает, что три компонента — уже RGB.
    bool convertColor = frame->componentCount == 3u && !(frame->adobe && frame->adobeTransform == 0u);

    for (uint32_t y = 0; y < frame->height; ++y)
    {
        uint8_t *out = pixels + (size_t)y * frame->width * 4u;
        for (uint32_t x = 0; x < frame->width; ++x)
        {
            if (frame->componentCount == 1u)
            {
                uint8_t value = SampleAt(frame, &frame->components[0], x, y);
                out[0] = value;
                out[1] = value;
                out[2] = value;
            }
            else
            {
                int32_t first = (int32_t)SampleAt(frame, &frame->components[0], x, y);
                int32_t second = (int32_t)SampleAt(frame, &frame->components[1], x, y);
                int32_t third = (int32_t)SampleAt(frame, &frame->components[2], x, y);
                if (convertColor)
                {
                    int32_t blueDifference = second - 128;
                    int32_t redDifference = third - 128;
                    // Сдвиг вправо у отрицательного здесь арифметический:
                    // это и требуется, деление округляло бы к нулю.
                    out[0] = ClampByte(
                        first + ((JPEG_CR_TO_RED * redDifference + JPEG_COLOR_HALF) >> 16));
                    out[1] = ClampByte(first + ((JPEG_CB_TO_GREEN * blueDifference +
                                                 JPEG_CR_TO_GREEN * redDifference +
                                                 JPEG_COLOR_HALF) >> 16));
                    out[2] = ClampByte(
                        first + ((JPEG_CB_TO_BLUE * blueDifference + JPEG_COLOR_HALF) >> 16));
                }
                else
                {
                    out[0] = (uint8_t)first;
                    out[1] = (uint8_t)second;
                    out[2] = (uint8_t)third;
                }
            }
            out[3] = 255u;
            out += 4;
        }
    }
}

ImageStatus JpegDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                       uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL) return IMAGE_INVALID_ARGUMENT;
    if (pixelBytes < info->pixelBytes) return IMAGE_BUFFER_TOO_SMALL;

    const uint8_t *file = (const uint8_t *)bytes;
    JpegDecoder decoder;
    ImageStatus status = ParseFrameHeader(file, sizeBytes, &decoder.frame);
    if (status != IMAGE_OK) return status;
    if (decoder.frame.width != info->width || decoder.frame.height != info->height)
    {
        return IMAGE_CORRUPT;
    }

    JpegLayout layout;
    status = ComputeLayout(&decoder.frame, &layout);
    if (status != IMAGE_OK) return status;
    if (scratch == NULL || scratchBytes < layout.total) return IMAGE_BUFFER_TOO_SMALL;

    uint8_t *base = (uint8_t *)scratch;
    decoder.huffman = (JpegHuffman *)(void *)(base + layout.huffman);
    decoder.quant = (uint16_t *)(void *)(base + layout.quant);
    decoder.restartInterval = 0u;
    decoder.eobRun = 0u;
    for (uint32_t index = 0; index < JPEG_HUFFMAN_TABLES; ++index)
    {
        decoder.huffman[index].present = false;
    }
    for (uint32_t index = 0; index < JPEG_QUANT_TABLES; ++index) decoder.quantPresent[index] = false;

    for (uint32_t index = 0; index < decoder.frame.componentCount; ++index)
    {
        JpegComponent *component = &decoder.frame.components[index];
        component->samples = base + layout.samples[index];
        component->dcPrediction = 0;
        // Нейтральный уровень: у обрезанного файла недошедшие блоки
        // останутся серыми, а не чёрными, и цветность не поедет.
        size_t sampleCount = (size_t)component->sampleWidth * component->blocksPerColumn * 8u;
        for (size_t offset = 0; offset < sampleCount; ++offset) component->samples[offset] = 128u;

        if (decoder.frame.progressive)
        {
            component->coefficients = (int16_t *)(void *)(base + layout.coefficients[index]);
            size_t coefficientCount =
                (size_t)component->blocksPerLine * component->blocksPerColumn * 64u;
            for (size_t offset = 0; offset < coefficientCount; ++offset)
            {
                component->coefficients[offset] = 0;
            }
        }
        else
        {
            component->coefficients = NULL;
        }
    }

    uint32_t position = 2u;
    bool decodedScan = false;
    for (;;)
    {
        uint8_t marker = 0u;
        status = NextMarker(file, sizeBytes, &position, &marker);
        if (status != IMAGE_OK)
        {
            // Конец файла без EOI: проходы уже прочитаны, и требовать
            // маркер ради маркера незачем.
            if (decodedScan) break;
            return status;
        }
        if (marker == 0xD9u) break;
        if (MarkerIsStandalone(marker)) continue;

        if (position + 2u > sizeBytes) return IMAGE_TRUNCATED;
        uint32_t length = ReadU16Be(file + position);
        if (length < 2u) return IMAGE_CORRUPT;
        if (position + length > sizeBytes) return IMAGE_TRUNCATED;
        const uint8_t *segment = file + position + 2u;
        uint32_t payload = length - 2u;

        if (marker == 0xDBu)
        {
            status = ParseQuantSegment(&decoder, segment, payload);
            if (status != IMAGE_OK) return status;
        }
        else if (marker == 0xC4u)
        {
            status = ParseHuffmanSegment(&decoder, segment, payload);
            if (status != IMAGE_OK) return status;
        }
        else if (marker == 0xDDu)
        {
            if (payload < 2u) return IMAGE_CORRUPT;
            decoder.restartInterval = ReadU16Be(segment);
        }
        else if (marker == 0xDAu)
        {
            JpegScan scan;
            status = ParseScanSegment(&decoder, segment, payload, &scan);
            if (status != IMAGE_OK) return status;

            decoder.reader.bytes = file;
            decoder.reader.sizeBytes = sizeBytes;
            decoder.reader.position = position + length;
            decoder.reader.bitBuffer = 0u;
            decoder.reader.bitCount = 0u;
            decoder.reader.markerReached = false;

            status = DecodeScan(&decoder, &scan);
            if (status != IMAGE_OK) return status;
            decodedScan = true;
            position = decoder.reader.position;
            continue;
        }

        position += length;
    }

    if (!decodedScan) return IMAGE_CORRUPT;
    if (decoder.frame.progressive) FinishProgressive(&decoder);
    ConvertToRgba(&decoder.frame, (uint8_t *)pixels);
    return IMAGE_OK;
}
