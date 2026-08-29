#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/texture_pack.h"

typedef enum TexturePackLoadStatus
{
    TEXTURE_PACK_LOAD_NOT_ATTEMPTED = 0,
    TEXTURE_PACK_LOAD_OK,
    TEXTURE_PACK_LOAD_NO_ACTIVE_PACK,
    TEXTURE_PACK_LOAD_INVALID,
    TEXTURE_PACK_LOAD_IO_ERROR,
} TexturePackLoadStatus;

// LTP хранит по одному слою на материал. Material id 1
// в меше соответствует слою 0; лишние id зажимаются к последнему слою.
#define TEXTURE_PACK_MAX_MIP_COUNT 13u
#define TEXTURE_PACK_MAX_SUBRESOURCES \
    (TEXTURE_PACK_MAX_LAYERS * TEXTURE_PACK_MAX_MIP_COUNT)

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
