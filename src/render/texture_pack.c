#include "render/texture_pack.h"
#include "render/texture_pack_internal.h"
#include "content/content_catalog.h"

#include <windows.h>
#include <string.h>

#define LTP_MAGIC       0x3150544Cu
#define LTP_VERSION     1u
#define LTP_VERSION_NORMALS 2u
#define LTP_HEADER_SIZE 24u
#define LTP_FORMAT_RGBA8 1u
#define LTP_FORMAT_RGBA8_NORMALS 2u

#define ACTIVE_NAME_MAX_BYTES 128u
#define PATH_CAPACITY_CHARS   32768u
#define TEXTURE_MAX_DIMENSION 4096u

// Layer-major fallback: dirt, grass top, grass side.
static const uint8_t g_fallbackPixels[TEXTURE_PACK_LAYER_COUNT * 4] = {
    118, 90, 66, 255,
    117, 173, 85, 255,
    102, 121, 63, 255,
};

static uint16_t ReadU16Le(const uint8_t* bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t* bytes)
{
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
}

static void SetFallback(TexturePackData* pack)
{
    pack->width = 1;
    pack->height = 1;
    pack->layerCount = TEXTURE_PACK_LAYER_COUNT;
    pack->mipCount = 1;
    pack->pixels = g_fallbackPixels;
    pack->pixelBytes = sizeof(g_fallbackPixels);
    pack->normalPixels = NULL;
}

static uint32_t LiteralLength(const wchar_t* text)
{
    uint32_t length = 0;
    while (text[length] != L'\0') ++length;
    return length;
}

static bool BuildPath(wchar_t* path, uint32_t capacity,
    uint32_t directoryLength, const wchar_t* suffix)
{
    uint32_t suffixLength = LiteralLength(suffix);
    if (directoryLength + suffixLength + 1u > capacity)
    {
        return false;
    }

    for (uint32_t i = 0; i < suffixLength; ++i)
    {
        path[directoryLength + i] = suffix[i];
    }
    path[directoryLength + suffixLength] = L'\0';
    return true;
}

static bool GetExecutableDirectory(
    wchar_t* path, uint32_t capacity, uint32_t* outDirectoryLength)
{
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }

    for (uint32_t i = (uint32_t)length; i > 0; --i)
    {
        wchar_t character = path[i - 1u];
        if (character == L'\\' || character == L'/')
        {
            *outDirectoryLength = i - 1u;
            return true;
        }
    }
    return false;
}

static bool ReadFileExact(HANDLE file, void* destination, uint32_t byteCount)
{
    uint8_t* output = (uint8_t*)destination;
    uint32_t completed = 0;
    while (completed < byteCount)
    {
        DWORD read = 0;
        if (!ReadFile(file, output + completed, byteCount - completed, &read, NULL)
            || read == 0)
        {
            return false;
        }
        completed += read;
    }
    return true;
}

static bool IsAsciiLetterOrDigit(uint8_t character)
{
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9');
}

static uint8_t AsciiLower(uint8_t character)
{
    return character >= 'A' && character <= 'Z'
        ? (uint8_t)(character + ('a' - 'A'))
        : character;
}

static bool HasPackExtension(const uint8_t* name, uint32_t length)
{
    if (length <= 4u) return false;
    uint8_t ext4 = AsciiLower(name[length - 4u]);
    uint8_t ext3 = AsciiLower(name[length - 3u]);
    uint8_t ext2 = AsciiLower(name[length - 2u]);
    uint8_t ext1 = AsciiLower(name[length - 1u]);
    return ext4 == '.' && ext3 == 'l' && ext2 == 't' && ext1 == 'p';
}

#define HasLtpExtension HasPackExtension

static bool ReadActiveName(
    const wchar_t* activePath, uint8_t outName[ACTIVE_NAME_MAX_BYTES],
    uint32_t* outLength)
{
    HANDLE file = CreateFileW(activePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    uint8_t bytes[ACTIVE_NAME_MAX_BYTES + 8u];
    LARGE_INTEGER size;
    bool succeeded = GetFileSizeEx(file, &size)
        && size.QuadPart > 0
        && size.QuadPart <= (LONGLONG)sizeof(bytes)
        && ReadFileExact(file, bytes, (uint32_t)size.QuadPart);
    CloseHandle(file);
    if (!succeeded)
    {
        return false;
    }

    uint32_t begin = 0;
    uint32_t end = (uint32_t)size.QuadPart;
    if (end >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu)
    {
        begin = 3u;
    }
    while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t'
        || bytes[begin] == '\r' || bytes[begin] == '\n'))
    {
        ++begin;
    }
    while (end > begin && (bytes[end - 1u] == ' ' || bytes[end - 1u] == '\t'
        || bytes[end - 1u] == '\r' || bytes[end - 1u] == '\n'))
    {
        --end;
    }

    uint32_t length = end - begin;
    if (length == 0 || length > ACTIVE_NAME_MAX_BYTES
        || !IsAsciiLetterOrDigit(bytes[begin])
        || !HasLtpExtension(bytes + begin, length))
    {
        return false;
    }

    for (uint32_t i = 0; i < length; ++i)
    {
        uint8_t character = bytes[begin + i];
        if (!IsAsciiLetterOrDigit(character)
            && character != '_' && character != '-' && character != '.')
        {
            return false;
        }
        outName[i] = character;
    }
    *outLength = length;
    return true;
}

static uint16_t FullMipCount(uint32_t width, uint32_t height)
{
    uint16_t count = 1;
    while (width > 1u || height > 1u)
    {
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
        ++count;
    }
    return count;
}

static bool CalculatePayloadBytes(uint32_t width, uint32_t height,
    uint32_t layerCount, uint32_t mipCount, uint32_t* outBytes)
{
    uint64_t bytesPerLayer = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        bytesPerLayer += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
    }

    uint64_t total = bytesPerLayer * layerCount;
    if (total == 0 || total > UINT32_MAX)
    {
        return false;
    }
    *outBytes = (uint32_t)total;
    return true;
}

static bool LoadLtp(const wchar_t* path, TexturePackData* outPack)
{
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER fileSize;
    uint8_t header[LTP_HEADER_SIZE];
    bool headerRead = GetFileSizeEx(file, &fileSize)
        && fileSize.QuadPart >= LTP_HEADER_SIZE
        && ReadFileExact(file, header, sizeof(header));
    if (!headerRead)
    {
        CloseHandle(file);
        return false;
    }

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
    bool withNormals = version == LTP_VERSION_NORMALS
        && format == LTP_FORMAT_RGBA8_NORMALS;
    bool versionValid = withNormals
        || (version == LTP_VERSION && format == LTP_FORMAT_RGBA8);

    uint32_t albedoBytes = 0;
    bool valid = magic == LTP_MAGIC
        && versionValid
        && headerSize == LTP_HEADER_SIZE
        && width > 0 && width <= TEXTURE_MAX_DIMENSION
        && height > 0 && height <= TEXTURE_MAX_DIMENSION
        && layerCount == TEXTURE_PACK_LAYER_COUNT
        && mipCount == FullMipCount(width, height)
        && CalculatePayloadBytes(width, height, layerCount, mipCount, &albedoBytes)
        && albedoBytes <= UINT32_MAX / 2u
        && dataBytes == (withNormals ? albedoBytes * 2u : albedoBytes)
        && fileSize.QuadPart == (LONGLONG)LTP_HEADER_SIZE + dataBytes;
    if (!valid)
    {
        CloseHandle(file);
        return false;
    }

    uint8_t* pixels = HeapAlloc(GetProcessHeap(), 0, dataBytes);
    if (pixels == NULL || !ReadFileExact(file, pixels, dataBytes))
    {
        if (pixels != NULL) HeapFree(GetProcessHeap(), 0, pixels);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);

    outPack->width = width;
    outPack->height = height;
    outPack->layerCount = layerCount;
    outPack->mipCount = mipCount;
    outPack->pixels = pixels;
    outPack->pixelBytes = albedoBytes;
    outPack->normalPixels = withNormals ? pixels + albedoBytes : NULL;
    return true;
}

TexturePackLoadStatus TexturePackLoadActive(TexturePackData* outPack)
{
    if (outPack == NULL)
    {
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }
    SetFallback(outPack);

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)PATH_CAPACITY_CHARS * sizeof(wchar_t));
    if (path == NULL)
    {
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    uint32_t directoryLength = 0;
    uint8_t activeName[ACTIVE_NAME_MAX_BYTES];
    uint32_t activeNameLength = 0;
    if (!GetExecutableDirectory(path, PATH_CAPACITY_CHARS, &directoryLength)
        || !BuildPath(path, PATH_CAPACITY_CHARS, directoryLength,
            L"\\textures\\active.txt"))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }
    DWORD activeAttributes = GetFileAttributesW(path);
    if (!ReadActiveName(path, activeName, &activeNameLength))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return activeAttributes == INVALID_FILE_ATTRIBUTES
            ? TEXTURE_PACK_LOAD_NO_ACTIVE_PACK : TEXTURE_PACK_LOAD_INVALID;
    }
    bool ready = BuildPath(path, PATH_CAPACITY_CHARS, directoryLength,
        L"\\textures\\");

    if (ready)
    {
        uint32_t prefixLength = directoryLength
            + LiteralLength(L"\\textures\\");
        if (prefixLength + activeNameLength + 1u <= PATH_CAPACITY_CHARS)
        {
            for (uint32_t i = 0; i < activeNameLength; ++i)
            {
                path[prefixLength + i] = (wchar_t)activeName[i];
            }
            path[prefixLength + activeNameLength] = L'\0';

            TexturePackData loaded;
            if (LoadLtp(path, &loaded))
            {
                *outPack = loaded;
                HeapFree(GetProcessHeap(), 0, path);
                return TEXTURE_PACK_LOAD_OK;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, path);
    return ready ? TEXTURE_PACK_LOAD_INVALID : TEXTURE_PACK_LOAD_IO_ERROR;
}

static bool GetSubresourceFrom(const TexturePackData* pack,
    const uint8_t* base, uint32_t layer, uint32_t mip,
    TexturePackSubresource* outSubresource)
{
    if (pack == NULL || outSubresource == NULL || base == NULL
        || layer >= pack->layerCount || mip >= pack->mipCount)
    {
        return false;
    }

    uint64_t bytesPerLayer = 0;
    uint32_t width = pack->width;
    uint32_t height = pack->height;
    for (uint32_t level = 0; level < pack->mipCount; ++level)
    {
        bytesPerLayer += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
    }

    uint64_t offset = bytesPerLayer * layer;
    width = pack->width;
    height = pack->height;
    for (uint32_t level = 0; level < mip; ++level)
    {
        offset += (uint64_t)width * height * 4u;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
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

bool TexturePackGetSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource)
{
    return GetSubresourceFrom(pack,
        pack != NULL ? pack->pixels : NULL, layer, mip, outSubresource);
}

bool TexturePackGetNormalSubresource(const TexturePackData* pack,
    uint32_t layer, uint32_t mip, TexturePackSubresource* outSubresource)
{
    return GetSubresourceFrom(pack,
        pack != NULL ? pack->normalPixels : NULL, layer, mip, outSubresource);
}

void TexturePackRelease(TexturePackData* pack)
{
    if (pack == NULL)
    {
        return;
    }
    if (pack->pixels != NULL && pack->pixels != g_fallbackPixels)
    {
        HeapFree(GetProcessHeap(), 0, (void*)pack->pixels);
    }
    pack->width = 0;
    pack->height = 0;
    pack->layerCount = 0;
    pack->mipCount = 0;
    pack->pixels = NULL;
    pack->pixelBytes = 0;
    pack->normalPixels = NULL;
}

bool TexturePackEnumerate(TexturePackList* outList)
{
    if (outList == NULL)
    {
        return false;
    }
    outList->entries = NULL;
    outList->count = 0;

    LaiueContentList contentList;
    if (!LaiueContentEnumerate(
            LAIUE_CONTENT_TEXTURE_PACK, &contentList))
    {
        return false;
    }
    if (contentList.count == 0)
    {
        LaiueContentListRelease(&contentList);
        return true;
    }
    outList->entries = HeapAlloc(GetProcessHeap(), 0,
        (size_t)contentList.count * sizeof(TexturePackEntry));
    if (outList->entries == NULL)
    {
        LaiueContentListRelease(&contentList);
        return false;
    }
    for (uint32_t index = 0; index < contentList.count; ++index)
    {
        uint32_t length = 0;
        while (contentList.entries[index].name[length] != L'\0'
            && length + 1u < TEXTURE_PACK_NAME_MAX)
        {
            outList->entries[index].name[length] =
                contentList.entries[index].name[length];
            ++length;
        }
        outList->entries[index].name[length] = L'\0';
        outList->entries[index].active = contentList.entries[index].active;
    }
    outList->count = contentList.count;
    LaiueContentListRelease(&contentList);
    return true;
}

void TexturePackListRelease(TexturePackList* list)
{
    if (list == NULL)
    {
        return;
    }
    if (list->entries != NULL)
    {
        HeapFree(GetProcessHeap(), 0, list->entries);
    }
    list->entries = NULL;
    list->count = 0;
}

bool TexturePackActivate(const wchar_t* name)
{
    // NULL или пустая строка сбрасывают выбор — игра вернётся к встроенной
    // текстуре при следующей перезагрузке пака.
    if (name == NULL || name[0] == L'\0')
    {
        return LaiueContentSetActivePack(LAIUE_CONTENT_TEXTURE_PACK, NULL);
    }

    // Валидация имени и формата, запись active.txt и проверка существования
    // пака целиком живут в едином каталоге содержимого.
    return LaiueContentSetActivePack(LAIUE_CONTENT_TEXTURE_PACK, name);
}
