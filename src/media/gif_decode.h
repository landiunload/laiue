#pragma once

#include "media/image.h"

#include <stdint.h>

// GIF87a и GIF89a в RGBA8. Берётся первый кадр: текстура — неподвижная
// картинка, а анимацию движок всё равно не показывает. Прозрачность из
// расширения управления графикой сохраняется — ради неё GIF обычно и
// оказывается в наборе.
//
// Память выделяет вызывающая сторона, как и у остальных декодеров.
// Рабочий буфер занимает словарь LZW: держать шестнадцать килобайт на
// стеке в сборке без CRT нельзя.

ImageStatus GifInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo);

ImageStatus GifDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                      uint32_t pixelBytes, void *scratch, uint32_t scratchBytes);
