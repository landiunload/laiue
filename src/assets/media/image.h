#pragma once

#include <stdbool.h>
#include <stdint.h>

// Общий контракт декодеров изображений. Все они отдают RGBA8 без
// выравнивания строк.
//
// Библиотека внутренняя и статическая: её символы не входят в ABI
// модулей, а пользуются ей и рендерер, читающий текстурпак, и
// офлайн-инструмент, готовящий его заранее. Общий код важнее того,
// чтобы рантайм ничего не декодировал: декодеры свои, без сторонних
// зависимостей, и одна реализация лучше двух.
//
// Своей памяти декодеры не выделяют: сначала Inspect сообщает размеры и
// объём рабочей памяти, потом Decode пишет в буферы вызывающей стороны.
// Так у библиотеки нет ни своего мнения об аллокаторе, ни скрытого
// владения, а инструмент решает, откуда брать память для картинки в
// шестнадцать мегабайт.

// Предел стороны совпадает с пределом слоя LTP.
#define IMAGE_MAX_DIMENSION 4096u
// Предел кадров совпадает с пределом контейнера `.lt`.
#define IMAGE_MAX_FRAMES 256u

typedef enum ImageStatus
{
    IMAGE_OK = 0,
    IMAGE_INVALID_ARGUMENT,
    IMAGE_NOT_RECOGNISED,
    IMAGE_TRUNCATED,
    IMAGE_CORRUPT,
    IMAGE_UNSUPPORTED_FEATURE,
    IMAGE_TOO_LARGE,
    IMAGE_BUFFER_TOO_SMALL,
} ImageStatus;

typedef enum ImageFormat
{
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_GIF,
    IMAGE_FORMAT_JPEG,
} ImageFormat;

typedef struct ImageInfo
{
    uint32_t width;
    uint32_t height;
    // Кадров в файле: у PNG и JPEG всегда один, у GIF — сколько есть.
    uint32_t frameCount;
    // Длительность каждого кадра по отдельности: в GIF они вправе
    // различаться, и усреднять их — значит менять анимацию. У
    // неподвижных форматов заполнен только нулевой элемент, и он ноль.
    uint16_t frameMilliseconds[IMAGE_MAX_FRAMES];
    // Ровно width * height * 4 байта: RGBA8, строки вплотную.
    uint32_t frameBytes;
    // frameCount * frameBytes: кадры лежат подряд.
    uint32_t pixelBytes;
    // Рабочая память декодера. Ноль означает, что она не нужна.
    uint32_t scratchBytes;
    bool hasAlpha;
} ImageInfo;

// Определяет формат по сигнатуре, ничего не разбирая.
ImageFormat ImageProbe(const void *bytes, uint32_t sizeBytes);

// Разбор файла любого формата, который читает библиотека: формат
// определяется по сигнатуре, дальше работает нужный декодер. Так
// сведения о наборе форматов лежат в одном месте, а не повторяются у
// каждого, кто открывает картинку. Отдельные PngInspect и остальные
// остаются для того, кто формат уже знает.
ImageStatus ImageInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo);

ImageStatus ImageDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                        uint32_t pixelBytes, void *scratch, uint32_t scratchBytes);

// Усреднение по площади. Уменьшение усредняет попавшие в пиксель
// исходники с весом альфы — иначе прозрачные пиксели, у которых цвет
// обычно чёрный, затемняли бы края. Увеличение повторяет ближайший
// пиксель: для пиксель-арта это нужнее сглаживания.
//
// Тем же вызовом строится mip-цепочка: уровень n+1 — это уровень n,
// уменьшенный вдвое.
void ImageResample(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
                   uint8_t *destination, uint32_t destinationWidth,
                   uint32_t destinationHeight);

const char *ImageStatusText(ImageStatus status);
const char *ImageFormatName(ImageFormat format);
