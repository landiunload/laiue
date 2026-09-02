#include "render/texture_pack.h"
#include "render/texture_pack_internal.h"
#include "content/content_catalog.h"
#include "platform/system.h"

#include <string.h>

#define LTP_MAGIC 0x3150544Cu
#define LTP_VERSION 1u
#define LTP_VERSION_NORMALS 2u
#define LTP_HEADER_SIZE 24u
#define LTP_FORMAT_RGBA8 1u
#define LTP_FORMAT_RGBA8_NORMALS 2u

#define TEXTURE_MAX_DIMENSION 4096u

// Material-agnostic single-layer fallback. It keeps the renderer usable
// without assigning any game-specific meaning or palette to material ids.
static const uint8_t g_fallbackPixels[4] = {160, 160, 160, 255};

static uint16_t ReadU16Le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void SetFallback(TexturePackData *pack)
{
    pack->width = 1;
    pack->height = 1;
    pack->layerCount = TEXTURE_PACK_MIN_LAYERS;
    pack->mipCount = 1;
    pack->pixels = g_fallbackPixels;
    pack->pixelBytes = sizeof(g_fallbackPixels);
    pack->normalPixels = NULL;
    pack->allocation = NULL;
}

static uint16_t FullMipCount(uint32_t width, uint32_t height)
{
    uint16_t count = 1;
    while (width > 1u || height > 1u)
    {
        if (width > 1u)
            width >>= 1;
        if (height > 1u)
            height >>= 1;
        ++count;
    }
    return count;
}

static bool CalculatePayloadBytes(uint32_t width, uint32_t height, uint32_t layerCount,
                                  uint32_t mipCount, uint32_t *outBytes)
{
    uint64_t bytesPerLayer = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        bytesPerLayer += (uint64_t)width * height * 4u;
        if (width > 1u)
            width >>= 1;
        if (height > 1u)
            height >>= 1;
    }

    uint64_t total = bytesPerLayer * layerCount;
    if (total == 0 || total > UINT32_MAX)
    {
        return false;
    }
    *outBytes = (uint32_t)total;
    return true;
}

static bool LoadLtp(const wchar_t *path, TexturePackData *outPack)
{
    PlatformPathInformation information;
    if (!PlatformGetPathInformation(path, &information) || !information.exists ||
        information.isDirectory || information.isSymbolicLink ||
        information.size < LTP_HEADER_SIZE ||
        information.size > (uint64_t)LTP_HEADER_SIZE + UINT32_MAX)
    {
        return false;
    }

    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0;
    if (!PlatformReadEntireFile(path, (uint64_t)LTP_HEADER_SIZE + UINT32_MAX, &fileBytes,
                                &fileSize) ||
        fileSize != information.size)
    {
        PlatformFree(fileBytes);
        return false;
    }
    const uint8_t *header = fileBytes;

    uint32_t magic = ReadU32Le(header + 0);
    uint16_t version = ReadU16Le(header + 4);
    uint16_t headerSize = ReadU16Le(header + 6);
    uint16_t width = ReadU16Le(header + 8);
    uint16_t height = ReadU16Le(header + 10);
    uint16_t layerCount = ReadU16Le(header + 12);
    uint16_t mipCount = ReadU16Le(header + 14);
    uint32_t format = ReadU32Le(header + 16);
    uint32_t dataBytes = ReadU32Le(header + 20);

    // Версия 1 — только albedo; версия 2 — albedo + идентичный по
    // раскладке блок карт нормалей сразу за ним.
    bool withNormals = version == LTP_VERSION_NORMALS && format == LTP_FORMAT_RGBA8_NORMALS;
    bool versionValid = withNormals || (version == LTP_VERSION && format == LTP_FORMAT_RGBA8);

    uint32_t albedoBytes = 0;
    bool valid = magic == LTP_MAGIC && versionValid && headerSize == LTP_HEADER_SIZE && width > 0 &&
                 width <= TEXTURE_MAX_DIMENSION && height > 0 && height <= TEXTURE_MAX_DIMENSION &&
                 layerCount >= TEXTURE_PACK_MIN_LAYERS && layerCount <= TEXTURE_PACK_MAX_LAYERS &&
                 mipCount == FullMipCount(width, height) &&
                 CalculatePayloadBytes(width, height, layerCount, mipCount, &albedoBytes) &&
                 albedoBytes <= UINT32_MAX / 2u &&
                 dataBytes == (withNormals ? albedoBytes * 2u : albedoBytes) &&
                 fileSize == (uint64_t)LTP_HEADER_SIZE + dataBytes;
    if (!valid)
    {
        PlatformFree(fileBytes);
        return false;
    }

    uint8_t *pixels = fileBytes + LTP_HEADER_SIZE;

    outPack->width = width;
    outPack->height = height;
    outPack->layerCount = layerCount;
    outPack->mipCount = mipCount;
    outPack->pixels = pixels;
    outPack->pixelBytes = albedoBytes;
    outPack->normalPixels = withNormals ? pixels + albedoBytes : NULL;
    outPack->allocation = fileBytes;
    return true;
}

TexturePackLoadStatus TexturePackLoadActiveFrom(LaiueContentCatalog *catalog,
                                                TexturePackData *outPack)
{
    if (catalog == NULL || outPack == NULL)
    {
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }
    SetFallback(outPack);

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
    {
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (!LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, activeName,
                                          LAIUE_CONTENT_NAME_CAPACITY))
    {
        bool activeFileExists =
            LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_TEXTURE_PACK, NULL, L"active.txt",
                                         path, LAIUE_CONTENT_PATH_CAPACITY) &&
            PlatformPathExists(path);
        PlatformFree(path);
        return activeFileExists ? TEXTURE_PACK_LOAD_INVALID : TEXTURE_PACK_LOAD_NO_ACTIVE_PACK;
    }

    if (!LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_TEXTURE_PACK, activeName, NULL, path,
                                      LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(path);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    TexturePackData loaded;
    if (!LoadLtp(path, &loaded))
    {
        PlatformFree(path);
        return TEXTURE_PACK_LOAD_INVALID;
    }
    *outPack = loaded;
    PlatformFree(path);
    return TEXTURE_PACK_LOAD_OK;
}

TexturePackLoadStatus TexturePackLoadActive(TexturePackData *outPack)
{
    return TexturePackLoadActiveFrom(LaiueContentCatalogDefault(), outPack);
}

static bool GetSubresourceFrom(const TexturePackData *pack, const uint8_t *base, uint32_t layer,
                               uint32_t mip, TexturePackSubresource *outSubresource)
{
    if (pack == NULL || outSubresource == NULL || base == NULL || layer >= pack->layerCount ||
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

    uint64_t offset = bytesPerLayer * layer;
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

bool TexturePackGetSubresource(const TexturePackData *pack, uint32_t layer, uint32_t mip,
                               TexturePackSubresource *outSubresource)
{
    return GetSubresourceFrom(pack, pack != NULL ? pack->pixels : NULL, layer, mip, outSubresource);
}

bool TexturePackGetNormalSubresource(const TexturePackData *pack, uint32_t layer, uint32_t mip,
                                     TexturePackSubresource *outSubresource)
{
    return GetSubresourceFrom(pack, pack != NULL ? pack->normalPixels : NULL, layer, mip,
                              outSubresource);
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
    pack->layerCount = 0;
    pack->mipCount = 0;
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
