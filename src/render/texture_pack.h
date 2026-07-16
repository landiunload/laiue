#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define TEXTURE_PACK_NAME_MAX 64

typedef struct TexturePackEntry
{
    wchar_t name[TEXTURE_PACK_NAME_MAX];
    bool active;
} TexturePackEntry;

typedef struct TexturePackList
{
    TexturePackEntry* entries;
    uint32_t count;
} TexturePackList;

LAIUE_RENDER_API bool TexturePackEnumerate(TexturePackList* outList);
LAIUE_RENDER_API void TexturePackListRelease(TexturePackList* list);
LAIUE_RENDER_API bool TexturePackActivate(const wchar_t* name);

// Stable layer order shared by texture-pack authoring and the chunk shader.
// A pack always contains exactly these three layers.
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
    // LTP2: карты нормалей (RGB — нормаль, A — ambient occlusion),
    // раскладка идентична albedo. NULL у паков версии 1.
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

// Looks for <exe-directory>/textures/active.txt. The file must contain a
// safe ASCII .ltp file name from the same directory. Missing or invalid input
// is deliberately non-fatal: outPack receives a built-in 1x1 fallback.
// Release an already loaded pack before loading another one into the same
// structure.
void TexturePackLoadActive(TexturePackData* outPack);

// Returns a tightly packed RGBA8 subresource from the layer-major, mip-major
// payload. The returned memory is owned by TexturePackData.
bool TexturePackGetSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource);

// То же для карт нормалей; false, если пак их не содержит.
bool TexturePackGetNormalSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource);

void TexturePackRelease(TexturePackData* pack);
