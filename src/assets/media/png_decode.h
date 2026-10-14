#pragma once

#include "media/image.h"

#include <stdint.h>

// PNG (ISO/IEC 15948) в RGBA8. Поддержаны все пять типов цвета, глубины
// 1, 2, 4, 8 и 16 бит, палитра, прозрачность через tRNS и чересстрочная
// развёртка Adam7 — то есть всё, что стандарт называет PNG, а не
// удобное подмножество.
//
// Память выделяет вызывающая сторона: PngInspect сообщает размер
// картинки и объём рабочего буфера, PngDecode пишет в оба. Рабочий
// буфер обязан быть выровнен под указатель — в его начале лежит список
// отрезков IDAT.

ImageStatus PngInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo);

ImageStatus PngDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                      uint32_t pixelBytes, void *scratch, uint32_t scratchBytes);
