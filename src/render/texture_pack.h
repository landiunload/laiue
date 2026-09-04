#pragma once

#include "api.h"
#include "content/content_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#define TEXTURE_PACK_NAME_MAX LAIUE_CONTENT_NAME_CAPACITY
#define TEXTURE_PACK_MIN_LAYERS 1u
// Материалов столько же, сколько было всегда; слоёв в массиве больше,
// потому что анимированный материал занимает по слою на кадр.
#define TEXTURE_PACK_MAX_LAYERS 64u
#define TEXTURE_PACK_MAX_SLICES 256u

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

// LTP описывает от одного до 64 материалов. Material id N берёт описание
// N - 1; id сверх имеющихся зажимается к последнему. Материал занимает
// один слой массива, а анимированный — по слою на кадр подряд.
//
// Кадр выбирается по времени, которое приложение передаёт в
// RendererFrameSetup: расписание лежит в паке, а часы принадлежат
// приложению — ровно как со светом. Никакого игрового смысла слою
// рендерер не приписывает.
LAIUE_RENDER_API bool TexturePackEnumerateFrom(LaiueContentCatalog *catalog,
                                               TexturePackList *outList);
LAIUE_RENDER_API bool TexturePackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name);

// Compatibility wrappers use the executable-root default catalog.
LAIUE_RENDER_API bool TexturePackEnumerate(TexturePackList* outList);
LAIUE_RENDER_API void TexturePackListRelease(TexturePackList* list);
LAIUE_RENDER_API bool TexturePackActivate(const wchar_t* name);
