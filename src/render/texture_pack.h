#pragma once

#include "api.h"
#include "content/content_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#define TEXTURE_PACK_NAME_MAX LAIUE_CONTENT_NAME_CAPACITY
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

// LTP contains one layer per application-defined material.  Material id N
// maps to layer N - 1; ids beyond the available 1..64 layers clamp to the last
// layer.  The renderer assigns no game meaning to any layer.
LAIUE_RENDER_API bool TexturePackEnumerateFrom(LaiueContentCatalog *catalog,
                                               TexturePackList *outList);
LAIUE_RENDER_API bool TexturePackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name);

// Compatibility wrappers use the executable-root default catalog.
LAIUE_RENDER_API bool TexturePackEnumerate(TexturePackList* outList);
LAIUE_RENDER_API void TexturePackListRelease(TexturePackList* list);
LAIUE_RENDER_API bool TexturePackActivate(const wchar_t* name);
