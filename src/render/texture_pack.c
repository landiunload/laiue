#include "render/texture_pack.h"
#include "render/texture_pack_internal.h"
#include "content/content_catalog.h"
#include "platform/system.h"

#include <string.h>



// Material-agnostic single-layer fallback. It keeps the renderer usable
// without assigning any game-specific meaning or palette to material ids.
static const uint8_t g_fallbackPixels[4] = {160, 160, 160, 255};

static void SetStaticAnimation(TexturePackData *pack)
{
    pack->materialCount = pack->sliceCount;
    for (uint32_t material = 0; material < pack->materialCount; ++material)
    {
        pack->animation[material].firstSlice = (uint16_t)material;
        pack->animation[material].frameCount = 1u;
        pack->animation[material].cycleMilliseconds = 0u;
        pack->sliceMilliseconds[material] = 0u;
    }
}

static void SetFallback(TexturePackData *pack)
{
    pack->width = 1;
    pack->height = 1;
    pack->sliceCount = TEXTURE_PACK_MIN_LAYERS;
    pack->mipCount = 1;
    pack->pixels = g_fallbackPixels;
    pack->pixelBytes = sizeof(g_fallbackPixels);
    pack->normalPixels = NULL;
    pack->allocation = NULL;
    SetStaticAnimation(pack);
}

bool TexturePackMaterialNamesSet(TexturePackMaterialNames *names, const wchar_t *const *source,
                                 uint32_t count)
{
    if (names == NULL) return false;
    if (count > TEXTURE_PACK_MAX_LAYERS) return false;
    if (count != 0u && source == NULL) return false;

    for (uint32_t index = 0; index < count; ++index)
    {
        const wchar_t *name = source[index];
        // Имя проверяется здесь, а не при загрузке: пустое или опасное
        // имя лучше отвергнуть там, где приложение его задало.
        if (name == NULL || !LaiueContentPathIsSafe(name)) return false;

        uint32_t length = 0;
        while (name[length] != 0)
        {
            if (length + 1u >= LAIUE_CONTENT_NAME_CAPACITY) return false;
            names->storage[index][length] = name[length];
            ++length;
        }
        names->storage[index][length] = 0;
        names->pointers[index] = names->storage[index];
    }
    names->count = count;
    return true;
}

TexturePackLoadStatus TexturePackLoadActiveFrom(LaiueContentCatalog *catalog,
                                                const wchar_t *const *materialNames,
                                                uint32_t materialCount, TexturePackData *outPack)
{
    if (outPack == NULL)
    {
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }
    // Нейтральный слой ставится сразу: неудачная загрузка обязана
    // оставить рендерер с рабочим паком, а не с пустотой.
    SetFallback(outPack);
    if (catalog == NULL || materialNames == NULL || materialCount == 0u)
    {
        return TEXTURE_PACK_LOAD_NO_ACTIVE_PACK;
    }

    TexturePackData built;
    TexturePackLoadStatus status =
        TexturePackBuildFrom(catalog, materialNames, materialCount, &built);
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE)
    {
        return status;
    }
    *outPack = built;
    return status;
}


static bool GetSubresourceFrom(const TexturePackData *pack, const uint8_t *base, uint32_t slice,
                               uint32_t mip, TexturePackSubresource *outSubresource)
{
    if (pack == NULL || outSubresource == NULL || base == NULL || slice >= pack->sliceCount ||
        mip >= pack->mipCount)
    {
        return false;
    }

    uint64_t bytesPerLayer = 0;
    uint32_t width = pack->width;
    uint32_t height = pack->height;
    for (uint32_t level = 0; level < pack->mipCount; ++level)
    {
        bytesPerLayer += (uint64_t)width * height * 4u;
        if (width > 1u)
            width >>= 1;
        if (height > 1u)
            height >>= 1;
    }

    uint64_t offset = bytesPerLayer * slice;
    width = pack->width;
    height = pack->height;
    for (uint32_t level = 0; level < mip; ++level)
    {
        offset += (uint64_t)width * height * 4u;
        if (width > 1u)
            width >>= 1;
        if (height > 1u)
            height >>= 1;
    }

    uint64_t byteCount = (uint64_t)width * height * 4u;
    if (offset + byteCount > pack->pixelBytes || byteCount > UINT32_MAX)
    {
        return false;
    }

    outSubresource->pixels = base + (size_t)offset;
    outSubresource->width = width;
    outSubresource->height = height;
    outSubresource->rowBytes = width * 4u;
    outSubresource->byteCount = (uint32_t)byteCount;
    return true;
}

bool TexturePackGetSubresource(const TexturePackData *pack, uint32_t slice, uint32_t mip,
                               TexturePackSubresource *outSubresource)
{
    return GetSubresourceFrom(pack, pack != NULL ? pack->pixels : NULL, slice, mip, outSubresource);
}

bool TexturePackGetNormalSubresource(const TexturePackData *pack, uint32_t slice, uint32_t mip,
                                     TexturePackSubresource *outSubresource)
{
    return GetSubresourceFrom(pack, pack != NULL ? pack->normalPixels : NULL, slice, mip,
                              outSubresource);
}

void TexturePackCaptureAnimation(TexturePackAnimationSet *outSet, const TexturePackData *pack)
{
    if (outSet == NULL) return;
    memset(outSet, 0, sizeof(*outSet));
    if (pack == NULL) return;

    outSet->sliceCount = pack->sliceCount;
    outSet->materialCount = pack->materialCount;
    for (uint32_t material = 0; material < pack->materialCount; ++material)
    {
        outSet->animation[material] = pack->animation[material];
    }
    for (uint32_t slice = 0; slice < TEXTURE_PACK_MAX_SLICES; ++slice)
    {
        outSet->sliceMilliseconds[slice] = pack->sliceMilliseconds[slice];
    }
}

// Кадры материала идут по своей длительности каждый, поэтому нужный
// ищется накоплением, а не делением: у GIF задержки вправе различаться.
uint32_t TexturePackResolveSlice(const TexturePackAnimationSet *set, uint32_t material,
                                 double animationSeconds)
{
    if (set == NULL || material >= TEXTURE_PACK_MAX_LAYERS) return 0u;
    const TexturePackAnimation *animation = &set->animation[material];
    if (animation->frameCount <= 1u || animation->cycleMilliseconds == 0u)
    {
        return animation->firstSlice;
    }

    // Часы принадлежат приложению и могут идти как угодно; кадр от этого
    // не должен выходить за пределы материала.
    if (!(animationSeconds > 0.0)) animationSeconds = 0.0;
    double cycle = (double)animation->cycleMilliseconds;
    double milliseconds = animationSeconds * 1000.0;
    // Свёртка по длине цикла до перевода в целое: за сутки анимации
    // миллисекунды ещё помещаются в double точно, а прямое приведение
    // большого значения к uint32 не определено.
    if (milliseconds >= cycle)
    {
        milliseconds -= cycle * (double)(uint64_t)(milliseconds / cycle);
    }

    uint32_t elapsed = (uint32_t)milliseconds;
    uint32_t accumulated = 0u;
    for (uint32_t frame = 0; frame < animation->frameCount; ++frame)
    {
        uint32_t slice = (uint32_t)animation->firstSlice + frame;
        if (slice >= TEXTURE_PACK_MAX_SLICES) break;
        accumulated += set->sliceMilliseconds[slice];
        if (elapsed < accumulated) return slice;
    }
    return (uint32_t)animation->firstSlice + animation->frameCount - 1u;
}

void TexturePackFillSliceTable(const TexturePackAnimationSet *set, double animationSeconds,
                               uint32_t *outSlices)
{
    if (outSlices == NULL) return;
    for (uint32_t word = 0; word < 16u; ++word) outSlices[word] = 0u;
    if (set == NULL || set->materialCount == 0u) return;
    uint32_t materialCount = set->materialCount;
    if (materialCount > TEXTURE_PACK_MAX_LAYERS) materialCount = TEXTURE_PACK_MAX_LAYERS;

    for (uint32_t material = 0; material < TEXTURE_PACK_MAX_LAYERS; ++material)
    {
        uint32_t source = material < materialCount ? material : materialCount - 1u;
        uint32_t slice = TexturePackResolveSlice(set, source, animationSeconds);
        if (slice > 255u) slice = 255u;
        outSlices[material >> 2] |= slice << ((material & 3u) * 8u);
    }
}

void TexturePackRelease(TexturePackData *pack)
{
    if (pack == NULL)
    {
        return;
    }
    PlatformFree(pack->allocation);
    pack->width = 0;
    pack->height = 0;
    pack->sliceCount = 0;
    pack->mipCount = 0;
    pack->materialCount = 0;
    pack->pixels = NULL;
    pack->pixelBytes = 0;
    pack->normalPixels = NULL;
    pack->allocation = NULL;
}

bool TexturePackEnumerateFrom(LaiueContentCatalog *catalog, TexturePackList *outList)
{
    if (catalog == NULL || outList == NULL)
    {
        return false;
    }
    outList->entries = NULL;
    outList->count = 0;

    LaiueContentList contentList;
    if (!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_TEXTURE_PACK, &contentList))
    {
        return false;
    }
    if (contentList.count == 0)
    {
        LaiueContentListRelease(&contentList);
        return true;
    }
    outList->entries =
        PlatformAllocate((size_t)contentList.count * sizeof(TexturePackEntry), false);
    if (outList->entries == NULL)
    {
        LaiueContentListRelease(&contentList);
        return false;
    }
    for (uint32_t index = 0; index < contentList.count; ++index)
    {
        uint32_t length = 0;
        while (contentList.entries[index].name[length] != L'\0' &&
               length + 1u < TEXTURE_PACK_NAME_MAX)
        {
            outList->entries[index].name[length] = contentList.entries[index].name[length];
            ++length;
        }
        outList->entries[index].name[length] = L'\0';
        outList->entries[index].active = contentList.entries[index].active;
    }
    outList->count = contentList.count;
    LaiueContentListRelease(&contentList);
    return true;
}

bool TexturePackEnumerate(TexturePackList *outList)
{
    return TexturePackEnumerateFrom(LaiueContentCatalogDefault(), outList);
}

void TexturePackListRelease(TexturePackList *list)
{
    if (list == NULL)
    {
        return;
    }
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0;
}

bool TexturePackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name)
{
    // NULL или пустая строка сбрасывают выбор на нейтральный
    // однослойный fallback при следующей перезагрузке пака.
    if (name == NULL || name[0] == L'\0')
    {
        return LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, NULL);
    }

    // Валидация имени и формата, запись active.txt и проверка существования
    // пака целиком живут в едином каталоге содержимого.
    return LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, name);
}

bool TexturePackActivate(const wchar_t *name)
{
    return TexturePackActivateIn(LaiueContentCatalogDefault(), name);
}
