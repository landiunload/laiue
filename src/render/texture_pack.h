#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define TEXTURE_PACK_NAME_MAX 64
#define TEXTURE_PACK_MIN_LAYERS 1u
#define TEXTURE_PACK_MAX_LAYERS 64u

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
