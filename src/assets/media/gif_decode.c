#include "media/gif_decode.h"

#include <stddef.h>

#define GIF_MAX_CODES 4096u
#define GIF_HEADER_BYTES 13u
#define GIF_MAX_FRAMES 256u
#define GIF_DEFAULT_DELAY_MS 100u

// Словарь LZW: префикс, суффикс и стек для разворачивания цепочки.
// Вместе они занимают шестнадцать килобайт и потому живут в рабочем
// буфере вызывающей стороны, а не на стеке.
typedef struct GifDictionary
{
    uint16_t prefix[GIF_MAX_CODES];
    uint8_t suffix[GIF_MAX_CODES];
    uint8_t stack[GIF_MAX_CODES];
} GifDictionary;

typedef struct GifFrame
{
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
    bool interlaced;

    uint32_t paletteOffset;
    uint32_t paletteEntries;
    int32_t transparentIndex;    // -1, если прозрачности нет
    uint32_t delayMilliseconds;
    uint32_t disposal;           // 0/1 — оставить, 2 — очистить, 3 — вернуть

    uint32_t lzwOffset;          // байт минимального размера кода
} GifFrame;

typedef struct GifReader
{
    const uint8_t *file;
    uint32_t sizeBytes;
    uint32_t cursor;
    uint32_t canvasWidth;
    uint32_t canvasHeight;
    uint32_t globalPaletteOffset;
    uint32_t globalPaletteEntries;
} GifReader;

static uint32_t ReadU16Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

// Пропускает цепочку подблоков: длина, данные, ..., нулевая длина.
static bool SkipSubBlocks(const uint8_t *file, uint32_t sizeBytes, uint32_t *cursor)
{
    for (;;)
    {
        if (*cursor >= sizeBytes) return false;
        uint32_t blockSize = file[(*cursor)++];
        if (blockSize == 0u) return true;
        if (blockSize > sizeBytes - *cursor) return false;
        *cursor += blockSize;
    }
}

static ImageStatus GifOpen(const uint8_t *file, uint32_t sizeBytes, GifReader *outReader)
{
    if (sizeBytes < GIF_HEADER_BYTES) return IMAGE_TRUNCATED;
    if (file[0] != 'G' || file[1] != 'I' || file[2] != 'F') return IMAGE_NOT_RECOGNISED;

    GifReader reader = {.file = file, .sizeBytes = sizeBytes};
    reader.canvasWidth = ReadU16Le(file + 6);
    reader.canvasHeight = ReadU16Le(file + 8);
    uint32_t packed = file[10];

    if (reader.canvasWidth == 0u || reader.canvasHeight == 0u) return IMAGE_CORRUPT;
    if (reader.canvasWidth > IMAGE_MAX_DIMENSION || reader.canvasHeight > IMAGE_MAX_DIMENSION)
        return IMAGE_TOO_LARGE;

    reader.cursor = GIF_HEADER_BYTES;
    if ((packed & 0x80u) != 0u)
    {
        reader.globalPaletteEntries = 2u << (packed & 0x07u);
        if (reader.globalPaletteEntries * 3u > sizeBytes - reader.cursor) return IMAGE_TRUNCATED;
        reader.globalPaletteOffset = reader.cursor;
        reader.cursor += reader.globalPaletteEntries * 3u;
    }

    *outReader = reader;
    return IMAGE_OK;
}

// Читает очередной кадр и оставляет курсор за его данными, чтобы
// следующий вызов продолжил с того же места.
static ImageStatus GifNextFrame(GifReader *reader, GifFrame *outFrame, bool *outDone)
{
    const uint8_t *file = reader->file;
    uint32_t sizeBytes = reader->sizeBytes;

    GifFrame frame = {.transparentIndex = -1, .delayMilliseconds = 0u};
    *outDone = false;

    while (reader->cursor < sizeBytes)
    {
        uint32_t marker = file[reader->cursor++];
        if (marker == 0x3Bu) break;   // конец файла
        if (marker == 0x00u) continue;   // мусорный байт между блоками

        if (marker == 0x21u)
        {
            if (reader->cursor >= sizeBytes) return IMAGE_TRUNCATED;
            uint32_t label = file[reader->cursor++];
            if (label == 0xF9u)
            {
                // Управление графикой относится к следующему кадру.
                if (reader->cursor >= sizeBytes) return IMAGE_TRUNCATED;
                uint32_t blockSize = file[reader->cursor];
                if (blockSize < 4u || blockSize + 1u > sizeBytes - reader->cursor)
                    return IMAGE_TRUNCATED;
                uint32_t packed = file[reader->cursor + 1u];
                frame.disposal = (packed >> 2) & 0x07u;
                // Задержка задана в сотых долях секунды.
                frame.delayMilliseconds = ReadU16Le(file + reader->cursor + 2u) * 10u;
                if ((packed & 0x01u) != 0u)
                {
                    frame.transparentIndex = (int32_t)file[reader->cursor + 4u];
                }
            }
            if (!SkipSubBlocks(file, sizeBytes, &reader->cursor)) return IMAGE_TRUNCATED;
            continue;
        }

        if (marker != 0x2Cu) return IMAGE_CORRUPT;

        if (sizeBytes - reader->cursor < 9u) return IMAGE_TRUNCATED;
        frame.left = ReadU16Le(file + reader->cursor);
        frame.top = ReadU16Le(file + reader->cursor + 2);
        frame.width = ReadU16Le(file + reader->cursor + 4);
        frame.height = ReadU16Le(file + reader->cursor + 6);
        uint32_t framePacked = file[reader->cursor + 8];
        reader->cursor += 9u;

        frame.interlaced = (framePacked & 0x40u) != 0u;
        frame.paletteOffset = reader->globalPaletteOffset;
        frame.paletteEntries = reader->globalPaletteEntries;
        if ((framePacked & 0x80u) != 0u)
        {
            uint32_t localEntries = 2u << (framePacked & 0x07u);
            if (localEntries * 3u > sizeBytes - reader->cursor) return IMAGE_TRUNCATED;
            frame.paletteOffset = reader->cursor;
            frame.paletteEntries = localEntries;
            reader->cursor += localEntries * 3u;
        }

        if (frame.paletteEntries == 0u) return IMAGE_CORRUPT;
        if (frame.width == 0u || frame.height == 0u) return IMAGE_CORRUPT;
        if (frame.left + frame.width > reader->canvasWidth ||
            frame.top + frame.height > reader->canvasHeight)
        {
            return IMAGE_CORRUPT;
        }
        if (reader->cursor >= sizeBytes) return IMAGE_TRUNCATED;

        frame.lzwOffset = reader->cursor;
        ++reader->cursor;   // минимальный размер кода
        if (!SkipSubBlocks(file, sizeBytes, &reader->cursor)) return IMAGE_TRUNCATED;

        *outFrame = frame;
        return IMAGE_OK;
    }

    *outDone = true;
    return IMAGE_OK;
}

ImageStatus GifInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;

    GifReader reader;
    ImageStatus status = GifOpen((const uint8_t *)bytes, sizeBytes, &reader);
    if (status != IMAGE_OK) return status;

    uint32_t frameCount = 0u;
    bool hasAlpha = false;
    for (uint32_t index = 0; index < IMAGE_MAX_FRAMES; ++index) outInfo->frameMilliseconds[index] = 0u;
    for (;;)
    {
        GifFrame frame;
        bool done = false;
        status = GifNextFrame(&reader, &frame, &done);
        if (status != IMAGE_OK) return status;
        if (done) break;

        // Длительность запоминается у каждого кадра отдельно: в GIF они
        // вправе различаться, и один общий интервал менял бы анимацию.
        // Нулевая задержка означает «как можно быстрее»; берём то же,
        // что и просмотрщики, иначе цикл получился бы нулевой длины.
        if (frameCount < IMAGE_MAX_FRAMES)
        {
            outInfo->frameMilliseconds[frameCount] =
                (uint16_t)(frame.delayMilliseconds != 0u ? frame.delayMilliseconds
                                                         : GIF_DEFAULT_DELAY_MS);
        }
        if (frame.transparentIndex >= 0 || frame.width != reader.canvasWidth ||
            frame.height != reader.canvasHeight)
        {
            hasAlpha = true;
        }
        if (++frameCount >= GIF_MAX_FRAMES) break;
    }
    if (frameCount == 0u) return IMAGE_CORRUPT;

    uint64_t frameBytes = (uint64_t)reader.canvasWidth * reader.canvasHeight * 4u;
    uint64_t pixelBytes = frameBytes * frameCount;
    if (pixelBytes > 0xFFFFFFFFull) return IMAGE_TOO_LARGE;

    outInfo->width = reader.canvasWidth;
    outInfo->height = reader.canvasHeight;
    outInfo->frameCount = frameCount;
    // Неподвижная картинка расписания не имеет.
    if (frameCount == 1u) outInfo->frameMilliseconds[0] = 0u;
    outInfo->frameBytes = (uint32_t)frameBytes;
    outInfo->pixelBytes = (uint32_t)pixelBytes;
    // Рабочая память: словарь и копия холста для режима «вернуть как было».
    outInfo->scratchBytes = (uint32_t)sizeof(GifDictionary) + (uint32_t)frameBytes;
    outInfo->hasAlpha = hasAlpha;
    return IMAGE_OK;
}

typedef struct GifBitReader
{
    const uint8_t *file;
    uint32_t sizeBytes;
    uint32_t cursor;
    uint32_t blockRemaining;
    uint32_t bitBuffer;
    uint32_t bitCount;
    bool exhausted;
} GifBitReader;

static uint32_t ReadCode(GifBitReader *reader, uint32_t codeBits)
{
    while (reader->bitCount < codeBits)
    {
        if (reader->blockRemaining == 0u)
        {
            if (reader->cursor >= reader->sizeBytes)
            {
                reader->exhausted = true;
                return 0u;
            }
            reader->blockRemaining = reader->file[reader->cursor++];
            if (reader->blockRemaining == 0u ||
                reader->blockRemaining > reader->sizeBytes - reader->cursor)
            {
                reader->exhausted = true;
                return 0u;
            }
        }
        reader->bitBuffer |= (uint32_t)reader->file[reader->cursor++] << reader->bitCount;
        reader->bitCount += 8u;
        --reader->blockRemaining;
    }

    uint32_t code = reader->bitBuffer & ((1u << codeBits) - 1u);
    reader->bitBuffer >>= codeBits;
    reader->bitCount -= codeBits;
    return code;
}

// Порядок строк чересстрочного GIF: четыре прохода с разным началом и
// шагом. Возвращает строку кадра для очередной выданной строки.
static uint32_t InterlacedRow(uint32_t row, uint32_t height)
{
    uint32_t pass1 = (height + 7u) / 8u;
    uint32_t pass2 = (height + 3u) / 8u;
    uint32_t pass3 = (height + 1u) / 4u;

    if (row < pass1) return row * 8u;
    row -= pass1;
    if (row < pass2) return row * 8u + 4u;
    row -= pass2;
    if (row < pass3) return row * 4u + 2u;
    row -= pass3;
    return row * 2u + 1u;
}

// Рисует кадр поверх холста. Прозрачный индекс не пишет ничего: по
// стандарту он оставляет то, что уже нарисовано, и на этом держится вся
// анимация с частичным обновлением.
static ImageStatus DrawFrame(const uint8_t *file, uint32_t sizeBytes, const GifFrame *frame,
                             uint32_t canvasWidth, GifDictionary *dictionary, uint8_t *canvas)
{
    uint32_t minimumCodeBits = file[frame->lzwOffset];
    if (minimumCodeBits < 2u || minimumCodeBits > 11u) return IMAGE_CORRUPT;

    GifBitReader reader = {
        .file = file,
        .sizeBytes = sizeBytes,
        .cursor = frame->lzwOffset + 1u,
    };

    const uint8_t *palette = file + frame->paletteOffset;
    uint32_t clearCode = 1u << minimumCodeBits;
    uint32_t endCode = clearCode + 1u;
    uint32_t nextCode = clearCode + 2u;
    uint32_t codeBits = minimumCodeBits + 1u;
    int32_t previousCode = -1;
    uint8_t firstByte = 0u;

    for (uint32_t code = 0; code < clearCode; ++code)
    {
        dictionary->prefix[code] = 0xFFFFu;
        dictionary->suffix[code] = (uint8_t)code;
    }

    uint32_t writtenPixels = 0u;
    uint32_t total = frame->width * frame->height;
    while (writtenPixels < total)
    {
        uint32_t code = ReadCode(&reader, codeBits);
        if (reader.exhausted) return IMAGE_TRUNCATED;

        if (code == clearCode)
        {
            nextCode = clearCode + 2u;
            codeBits = minimumCodeBits + 1u;
            previousCode = -1;
            continue;
        }
        if (code == endCode) break;

        uint32_t stackDepth = 0u;
        uint32_t current = code;
        if (code >= nextCode)
        {
            // Код, которого ещё нет в словаре, допустим ровно в одном
            // случае: он описывает предыдущую цепочку с её же первым
            // байтом в конце.
            if (previousCode < 0 || code > nextCode) return IMAGE_CORRUPT;
            dictionary->stack[stackDepth++] = firstByte;
            current = (uint32_t)previousCode;
        }

        while (current >= clearCode)
        {
            if (stackDepth >= GIF_MAX_CODES || current >= GIF_MAX_CODES) return IMAGE_CORRUPT;
            dictionary->stack[stackDepth++] = dictionary->suffix[current];
            uint32_t parent = dictionary->prefix[current];
            if (parent == 0xFFFFu || parent >= current) return IMAGE_CORRUPT;
            current = parent;
        }
        if (stackDepth >= GIF_MAX_CODES) return IMAGE_CORRUPT;
        dictionary->stack[stackDepth++] = dictionary->suffix[current];
        firstByte = dictionary->suffix[current];

        while (stackDepth > 0u && writtenPixels < total)
        {
            uint8_t index = dictionary->stack[--stackDepth];
            uint32_t frameRow = writtenPixels / frame->width;
            uint32_t frameColumn = writtenPixels % frame->width;
            ++writtenPixels;

            if ((int32_t)index == frame->transparentIndex) continue;

            uint32_t canvasRow =
                frame->top + (frame->interlaced ? InterlacedRow(frameRow, frame->height) : frameRow);
            uint8_t *texel =
                canvas + ((size_t)canvasRow * canvasWidth + frame->left + frameColumn) * 4u;
            if (index < frame->paletteEntries)
            {
                texel[0] = palette[index * 3u];
                texel[1] = palette[index * 3u + 1u];
                texel[2] = palette[index * 3u + 2u];
            }
            texel[3] = 255u;
        }

        if (previousCode >= 0 && nextCode < GIF_MAX_CODES)
        {
            dictionary->prefix[nextCode] = (uint16_t)previousCode;
            dictionary->suffix[nextCode] = firstByte;
            ++nextCode;
            if (nextCode == (1u << codeBits) && codeBits < 12u) ++codeBits;
        }
        previousCode = (int32_t)code;
    }

    return writtenPixels == total ? IMAGE_OK : IMAGE_TRUNCATED;
}

static void ClearRect(uint8_t *canvas, uint32_t canvasWidth, const GifFrame *frame)
{
    for (uint32_t row = 0; row < frame->height; ++row)
    {
        uint8_t *line = canvas + ((size_t)(frame->top + row) * canvasWidth + frame->left) * 4u;
        for (uint32_t index = 0; index < frame->width * 4u; ++index) line[index] = 0u;
    }
}

ImageStatus GifDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                      uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL || scratch == NULL)
        return IMAGE_INVALID_ARGUMENT;
    if (pixelBytes < info->pixelBytes || scratchBytes < info->scratchBytes)
        return IMAGE_BUFFER_TOO_SMALL;

    const uint8_t *file = (const uint8_t *)bytes;
    GifReader reader;
    ImageStatus status = GifOpen(file, sizeBytes, &reader);
    if (status != IMAGE_OK) return status;
    if (reader.canvasWidth != info->width || reader.canvasHeight != info->height)
        return IMAGE_INVALID_ARGUMENT;

    GifDictionary *dictionary = (GifDictionary *)scratch;
    uint8_t *saved = (uint8_t *)scratch + sizeof(GifDictionary);
    uint8_t *output = (uint8_t *)pixels;

    // Обнуление ради компилятора: поле читается только при
    // havePrevious, но доказать это анализатору нечем.
    GifFrame previousFrame = {0};
    bool havePrevious = false;
    for (uint32_t frameIndex = 0; frameIndex < info->frameCount; ++frameIndex)
    {
        GifFrame frame;
        bool done = false;
        status = GifNextFrame(&reader, &frame, &done);
        if (status != IMAGE_OK) return status;
        if (done) return IMAGE_CORRUPT;

        uint8_t *canvas = output + (size_t)frameIndex * info->frameBytes;
        if (frameIndex == 0u)
        {
            for (uint32_t index = 0; index < info->frameBytes; ++index) canvas[index] = 0u;
        }
        else
        {
            const uint8_t *earlier = canvas - info->frameBytes;
            for (uint32_t index = 0; index < info->frameBytes; ++index)
            {
                canvas[index] = earlier[index];
            }
            // Способ убирания относится к кадру, который уже показан:
            // 2 очищает его прямоугольник, 3 возвращает то, что было
            // под ним, остальные оставляют картинку как есть.
            if (havePrevious && previousFrame.disposal == 2u)
            {
                ClearRect(canvas, reader.canvasWidth, &previousFrame);
            }
            else if (havePrevious && previousFrame.disposal == 3u)
            {
                for (uint32_t index = 0; index < info->frameBytes; ++index)
                {
                    canvas[index] = saved[index];
                }
            }
        }

        if (frame.disposal == 3u)
        {
            for (uint32_t index = 0; index < info->frameBytes; ++index) saved[index] = canvas[index];
        }

        status = DrawFrame(file, sizeBytes, &frame, reader.canvasWidth, dictionary, canvas);
        if (status != IMAGE_OK) return status;

        previousFrame = frame;
        havePrevious = true;
    }
    return IMAGE_OK;
}
