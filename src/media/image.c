#include "media/image.h"

#include "media/gif_decode.h"
#include "media/jpeg_decode.h"
#include "media/png_decode.h"

#include <stddef.h>

ImageFormat ImageProbe(const void *bytes, uint32_t sizeBytes)
{
    if (bytes == NULL) return IMAGE_FORMAT_UNKNOWN;
    const uint8_t *file = (const uint8_t *)bytes;

    if (sizeBytes >= 8u && file[0] == 137u && file[1] == 80u && file[2] == 78u &&
        file[3] == 71u && file[4] == 13u && file[5] == 10u && file[6] == 26u && file[7] == 10u)
    {
        return IMAGE_FORMAT_PNG;
    }
    if (sizeBytes >= 6u && file[0] == 'G' && file[1] == 'I' && file[2] == 'F' && file[3] == '8' &&
        (file[4] == '7' || file[4] == '9') && file[5] == 'a')
    {
        return IMAGE_FORMAT_GIF;
    }
    // JPEG начинается с маркера SOI, за которым сразу идёт следующий
    // маркер: два байта различают его слишком слабо.
    if (sizeBytes >= 3u && file[0] == 0xFFu && file[1] == 0xD8u && file[2] == 0xFFu)
    {
        return IMAGE_FORMAT_JPEG;
    }
    return IMAGE_FORMAT_UNKNOWN;
}

ImageStatus ImageInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;
    switch (ImageProbe(bytes, sizeBytes))
    {
    case IMAGE_FORMAT_PNG: return PngInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_GIF: return GifInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_JPEG: return JpegInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return IMAGE_NOT_RECOGNISED;
}

ImageStatus ImageDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                        uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL) return IMAGE_INVALID_ARGUMENT;
    switch (ImageProbe(bytes, sizeBytes))
    {
    case IMAGE_FORMAT_PNG:
        return PngDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_GIF:
        return GifDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_JPEG:
        return JpegDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return IMAGE_NOT_RECOGNISED;
}

const char *ImageStatusText(ImageStatus status)
{
    switch (status)
    {
    case IMAGE_OK: return "ok";
    case IMAGE_INVALID_ARGUMENT: return "invalid argument";
    case IMAGE_NOT_RECOGNISED: return "the file is not an image this tool reads";
    case IMAGE_TRUNCATED: return "the file ends before the image does";
    case IMAGE_CORRUPT: return "the image data is damaged";
    case IMAGE_UNSUPPORTED_FEATURE: return "the image uses a feature this decoder does not read";
    case IMAGE_TOO_LARGE: return "the image is larger than a texture layer may be";
    case IMAGE_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}

const char *ImageFormatName(ImageFormat format)
{
    switch (format)
    {
    case IMAGE_FORMAT_PNG: return "PNG";
    case IMAGE_FORMAT_GIF: return "GIF";
    case IMAGE_FORMAT_JPEG: return "JPEG";
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return "unknown";
}

void ImageResample(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
                 uint8_t *destination, uint32_t destinationWidth, uint32_t destinationHeight)
{
    if (source == NULL || destination == NULL) return;
    if (sourceWidth == 0u || sourceHeight == 0u) return;
    if (destinationWidth == 0u || destinationHeight == 0u) return;

    for (uint32_t row = 0; row < destinationHeight; ++row)
    {
        // Границы исходного прямоугольника считаются в целых числах:
        // одна и та же формула даёт и усреднение при уменьшении, и
        // ближайший пиксель при увеличении.
        uint32_t firstRow = (uint32_t)((uint64_t)row * sourceHeight / destinationHeight);
        uint32_t lastRow = (uint32_t)(((uint64_t)row + 1u) * sourceHeight / destinationHeight);
        if (lastRow <= firstRow) lastRow = firstRow + 1u;

        for (uint32_t column = 0; column < destinationWidth; ++column)
        {
            uint32_t firstColumn =
                (uint32_t)((uint64_t)column * sourceWidth / destinationWidth);
            uint32_t lastColumn =
                (uint32_t)(((uint64_t)column + 1u) * sourceWidth / destinationWidth);
            if (lastColumn <= firstColumn) lastColumn = firstColumn + 1u;

            uint64_t weightedRed = 0u;
            uint64_t weightedGreen = 0u;
            uint64_t weightedBlue = 0u;
            uint64_t plainRed = 0u;
            uint64_t plainGreen = 0u;
            uint64_t plainBlue = 0u;
            uint64_t alphaSum = 0u;
            uint64_t samples = 0u;

            for (uint32_t sourceRow = firstRow; sourceRow < lastRow; ++sourceRow)
            {
                const uint8_t *line = source + (size_t)sourceRow * sourceWidth * 4u;
                for (uint32_t sourceColumn = firstColumn; sourceColumn < lastColumn;
                     ++sourceColumn)
                {
                    const uint8_t *texel = line + (size_t)sourceColumn * 4u;
                    uint32_t alpha = texel[3];
                    weightedRed += (uint64_t)texel[0] * alpha;
                    weightedGreen += (uint64_t)texel[1] * alpha;
                    weightedBlue += (uint64_t)texel[2] * alpha;
                    plainRed += texel[0];
                    plainGreen += texel[1];
                    plainBlue += texel[2];
                    alphaSum += alpha;
                    ++samples;
                }
            }

            uint8_t *out = destination + ((size_t)row * destinationWidth + column) * 4u;
            if (alphaSum != 0u)
            {
                out[0] = (uint8_t)((weightedRed + alphaSum / 2u) / alphaSum);
                out[1] = (uint8_t)((weightedGreen + alphaSum / 2u) / alphaSum);
                out[2] = (uint8_t)((weightedBlue + alphaSum / 2u) / alphaSum);
            }
            else
            {
                // Полностью прозрачный блок: веса нет, поэтому цвет
                // усредняется обычным образом. Терять его нельзя —
                // фильтрация текстуры вытащит его на границе.
                out[0] = (uint8_t)((plainRed + samples / 2u) / samples);
                out[1] = (uint8_t)((plainGreen + samples / 2u) / samples);
                out[2] = (uint8_t)((plainBlue + samples / 2u) / samples);
            }
            out[3] = (uint8_t)((alphaSum + samples / 2u) / samples);
        }
    }
}
