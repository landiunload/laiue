#pragma once

#include "media/image.h"

#include <stdint.h>

// GIF87a и GIF89a в RGBA8. Текстуре нужен первый кадр, а показать всю анимацию
// можно потоком GifStream в конце файла. Прозрачность из
// расширения управления графикой сохраняется — ради неё GIF обычно и
// оказывается в наборе.
//
// Память выделяет вызывающая сторона, как и у остальных декодеров.
// Рабочий буфер занимает словарь LZW: держать шестнадцать килобайт на
// стеке в сборке без CRT нельзя.

ImageStatus GifInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo);

ImageStatus GifDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                      uint32_t pixelBytes, void *scratch, uint32_t scratchBytes);

// Показ анимации кадр за кадром. В памяти лежат только файл, холст одного кадра и
// рабочий буфер, поэтому память не зависит от длины GIF, а числа кадров показ не
// ограничивает. Файл и буфер остаются у вызывающей стороны на всё время показа.
uint32_t GifStreamScratchBytes(const ImageInfo *info);
ImageStatus GifStreamOpen(const void *bytes, uint32_t sizeBytes, const ImageInfo *info,
                          void *scratch, uint32_t scratchBytes);

// Рисует следующий кадр поверх прежнего холста, размер которого info->frameBytes, и
// сообщает, сколько его показывать. За последним кадром снова идёт первый. Испорченный
// хвост файла считается его концом: ошибку вернёт только нечитаемый первый кадр.
ImageStatus GifStreamNext(void *scratch, void *canvas, uint32_t *outDelayMilliseconds);
