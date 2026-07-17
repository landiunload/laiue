#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum TexturePackLoadStatus
{
    TEXTURE_PACK_LOAD_NOT_ATTEMPTED = 0,
    TEXTURE_PACK_LOAD_OK,
    TEXTURE_PACK_LOAD_NO_ACTIVE_PACK,
    TEXTURE_PACK_LOAD_INVALID,
    TEXTURE_PACK_LOAD_IO_ERROR,
} TexturePackLoadStatus;

// Стабильный порядок слоёв между авторингом LTP и chunk shader.
typedef enum TexturePackLayer
{
    TEXTURE_PACK_LAYER_DIRT = 0,
    TEXTURE_PACK_LAYER_GRASS_TOP = 1,
    TEXTURE_PACK_LAYER_GRASS_SIDE = 2,
    TEXTURE_PACK_LAYER_COUNT = 3
} TexturePackLayer;

typedef struct TexturePackData
{
    uint16_t width;
    uint16_t height;
    uint16_t layerCount;
    uint16_t mipCount;
    const uint8_t* pixels;
    uint32_t pixelBytes;
    const uint8_t* normalPixels;
} TexturePackData;

typedef struct TexturePackSubresource
{
    const uint8_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t rowBytes;
    uint32_t byteCount;
} TexturePackSubresource;

TexturePackLoadStatus TexturePackLoadActive(TexturePackData* outPack);
bool TexturePackGetSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource);
bool TexturePackGetNormalSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource);
void TexturePackRelease(TexturePackData* pack);
