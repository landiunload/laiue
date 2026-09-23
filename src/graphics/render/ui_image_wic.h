#pragma once

#include <stdbool.h>
#include <stdint.h>

// Декодирует PNG/JPEG через системный Windows Imaging Component в RGBA8.
// Буфер принадлежит вызывающему и освобождается через HeapFree.
bool UiImageLoadRgba(const wchar_t* path, uint8_t** outPixels,
    uint32_t* outWidth, uint32_t* outHeight);
