#include "render/shader_pack.h"
#include "content/content_catalog.h"

#include <windows.h>
#include <string.h>

#define SHADER_PACK_DIR L"\\shaders\\"
#define PATH_CAPACITY_CHARS 32768u
#define SHADER_FILE_MAX 0x40000u  // 256 КБ на шейдер
#define SHADER_MANIFEST_MAX 4096u

static uint32_t LiteralLength(const wchar_t* text)
{
    uint32_t length = 0;
    while (text[length] != L'\0') ++length;
    return length;
}

static bool BytesEqual(const uint8_t* left, const char* right, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (left[i] != (uint8_t)right[i]) return false;
    }
    return true;
}

static bool HasExactLine(const uint8_t* data, uint32_t length,
    const char* expected, uint32_t expectedLength, bool firstLineOnly)
{
    uint32_t start = 0;
    while (start < length)
    {
        uint32_t end = start;
        while (end < length && data[end] != '\n' && data[end] != '\r') ++end;
        if (end - start == expectedLength
            && BytesEqual(data + start, expected, expectedLength))
        {
            return true;
        }
        if (firstLineOnly) return false;
        while (end < length && (data[end] == '\n' || data[end] == '\r')) ++end;
        start = end;
    }
    return false;
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

bool ShaderPackEnumerate(ShaderPackList* outList)
{
    if (outList == NULL)
    {
        return false;
    }
    outList->entries = NULL;
    outList->count = 0;

    LaiueContentList contentList;
    if (!LaiueContentEnumerate(LAIUE_CONTENT_SHADER_PACK, &contentList))
    {
        return false;
    }
    outList->entries = HeapAlloc(GetProcessHeap(), 0,
        (size_t)(contentList.count + 1u) * sizeof(ShaderPackEntry));
    if (outList->entries == NULL)
    {
        LaiueContentListRelease(&contentList);
        return false;
    }
    memcpy(outList->entries[0].name, L"Default", 8u * sizeof(wchar_t));
    bool hasActive = false;
    for (uint32_t sourceIndex = 0;
        sourceIndex < contentList.count; ++sourceIndex)
    {
        uint32_t destinationIndex = sourceIndex + 1u;
        uint32_t length = 0;
        while (contentList.entries[sourceIndex].name[length] != L'\0'
            && length + 1u < SHADER_PACK_NAME_MAX)
        {
            outList->entries[destinationIndex].name[length] =
                contentList.entries[sourceIndex].name[length];
            ++length;
        }
        outList->entries[destinationIndex].name[length] = L'\0';
        outList->entries[destinationIndex].active =
            contentList.entries[sourceIndex].active;
        hasActive = hasActive || contentList.entries[sourceIndex].active;
    }
    outList->entries[0].active = !hasActive;
    outList->count = contentList.count + 1u;
    LaiueContentListRelease(&contentList);
    return true;
}

void ShaderPackListRelease(ShaderPackList* list)
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

bool ShaderPackActivate(const wchar_t* name)
{
    // Пустой выбор сбрасывает активный пак; иначе имя и существование
    // каталога .lsp проверяет единый каталог содержимого.
    if (name == NULL || name[0] == L'\0')
    {
        return LaiueContentSetActivePack(LAIUE_CONTENT_SHADER_PACK, NULL);
    }
    return LaiueContentSetActivePack(LAIUE_CONTENT_SHADER_PACK, name);
}
static bool ReadActivePackName(wchar_t* dirPath, uint32_t dirPrefixLen,
    uint8_t* outName, uint32_t* outNameLen, uint32_t nameCapacity)
{
    // Используем dirPath как буфер (он выделен на куче, PATH_CAPACITY_CHARS).
    memcpy(dirPath + dirPrefixLen, L"active.txt", 12 * sizeof(wchar_t));

    *outNameLen = 0;
    bool hasActive = false;
    HANDLE activeFile = CreateFileW(dirPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (activeFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(activeFile, &size)
            && size.QuadPart > 0
            && (LONGLONG)size.QuadPart <= (LONGLONG)nameCapacity)
        {
            DWORD read = 0;
            if (ReadFile(activeFile, outName, (DWORD)size.QuadPart, &read, NULL))
            {
                uint32_t len = (uint32_t)size.QuadPart;
                while (len > 0 && (outName[len - 1] == ' '
                    || outName[len - 1] == '\n' || outName[len - 1] == '\r'))
                {
                    --len;
                }
                outName[len] = '\0';
                *outNameLen = len;
                hasActive = len > 0;
            }
        }
        CloseHandle(activeFile);
    }
    return hasActive;
}

static void* LoadCsoFile(const wchar_t* fullPath, uint32_t* outLength)
{
    HANDLE file = CreateFileW(fullPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart <= 0
        || (uint64_t)size.QuadPart > SHADER_FILE_MAX)
    {
        CloseHandle(file);
        return NULL;
    }

    uint32_t byteCount = (uint32_t)size.QuadPart;
    void* data = HeapAlloc(GetProcessHeap(), 0, byteCount);
    if (data == NULL)
    {
        CloseHandle(file);
        return NULL;
    }

    DWORD read = 0;
    if (!ReadFile(file, data, byteCount, &read, NULL) || read != byteCount)
    {
        HeapFree(GetProcessHeap(), 0, data);
        CloseHandle(file);
        return NULL;
    }

    CloseHandle(file);
    *outLength = byteCount;
    return data;
}

static bool IsCompatibleManifest(const wchar_t* fullPath)
{
    uint32_t length = 0;
    uint8_t* data = LoadCsoFile(fullPath, &length);
    if (data == NULL || length > SHADER_MANIFEST_MAX)
    {
        if (data != NULL) HeapFree(GetProcessHeap(), 0, data);
        return false;
    }

    static const char header[] = "LAIUE SHADER 1";
    static const char contract[] = "contract = 1";
    bool hasHeader = HasExactLine(data, length,
        header, sizeof(header) - 1u, true);
    bool hasContract = HasExactLine(data, length,
        contract, sizeof(contract) - 1u, false);
    HeapFree(GetProcessHeap(), 0, data);
    return hasHeader && hasContract;
}

bool ShaderPackLoadActiveBytecode(
    void** outChunkVS, uint32_t* outChunkVSLength,
    void** outChunkPS, uint32_t* outChunkPSLength,
    void** outPanoramaVS, uint32_t* outPanoramaVSLength,
    void** outPanoramaPS, uint32_t* outPanoramaPSLength,
    void** outUIVS, uint32_t* outUIVSLength,
    void** outUIPS, uint32_t* outUIPSLength,
    ShaderPackLoadStatus* outStatus)
{
    if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_IO_ERROR;
    // Инициализируем все выходные параметры в NULL/0
    *outChunkVS = NULL; *outChunkVSLength = 0;
    *outChunkPS = NULL; *outChunkPSLength = 0;
    *outPanoramaVS = NULL; *outPanoramaVSLength = 0;
    *outPanoramaPS = NULL; *outPanoramaPSLength = 0;
    *outUIVS = NULL; *outUIVSLength = 0;
    *outUIPS = NULL; *outUIPSLength = 0;

    wchar_t* pathBuf = HeapAlloc(GetProcessHeap(), 0,
        (size_t)PATH_CAPACITY_CHARS * sizeof(wchar_t));
    if (pathBuf == NULL)
    {
        return false;
    }

    uint32_t directoryLength = 0;
    if (!GetExecutableDirectory(pathBuf, PATH_CAPACITY_CHARS, &directoryLength))
    {
        HeapFree(GetProcessHeap(), 0, pathBuf);
        return false;
    }
    uint32_t dirPrefixLen = directoryLength + LiteralLength(SHADER_PACK_DIR);
    if (dirPrefixLen + 1u > PATH_CAPACITY_CHARS)
    {
        HeapFree(GetProcessHeap(), 0, pathBuf);
        return false;
    }
    for (uint32_t i = 0; i < dirPrefixLen; ++i)
    {
        pathBuf[i] = i < directoryLength
            ? pathBuf[i]
            : SHADER_PACK_DIR[i - directoryLength];
    }
    pathBuf[dirPrefixLen] = L'\0';

    // Читаем active.txt
    uint8_t activeName[64];
    uint32_t activeNameLen = 0;
    if (!ReadActivePackName(pathBuf, dirPrefixLen, activeName, &activeNameLen, sizeof(activeName)))
    {
        if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_NO_ACTIVE_PACK;
        HeapFree(GetProcessHeap(), 0, pathBuf);
        return false; // нет активного пака — используем встроенные
    }

    // Строим путь к директории пака в том же буфере
    for (uint32_t i = 0; i < activeNameLen && dirPrefixLen + i + 1u < PATH_CAPACITY_CHARS; ++i)
    {
        pathBuf[dirPrefixLen + i] = (wchar_t)activeName[i];
    }
    uint32_t packDirLen = dirPrefixLen + activeNameLen;
    pathBuf[packDirLen] = L'\\';
    packDirLen++;

    if (!BuildPath(pathBuf, PATH_CAPACITY_CHARS, packDirLen, L"pack.lm")
        || !IsCompatibleManifest(pathBuf))
    {
        if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_INVALID_MANIFEST;
        HeapFree(GetProcessHeap(), 0, pathBuf);
        return false;
    }

    static const wchar_t* const shaderFiles[6] = {
        L"chunk_vs.ls", L"chunk_ps.ls",
        L"panorama_vs.ls", L"panorama_ps.ls",
        L"ui_vs.ls", L"ui_ps.ls",
    };
    void** outPtrs[6] = {
        outChunkVS, outChunkPS,
        outPanoramaVS, outPanoramaPS,
        outUIVS, outUIPS,
    };
    uint32_t* outLengths[6] = {
        outChunkVSLength, outChunkPSLength,
        outPanoramaVSLength, outPanoramaPSLength,
        outUIVSLength, outUIPSLength,
    };

    bool anyLoaded = false;
    for (uint32_t i = 0; i < 6; ++i)
    {
        // Строим полный путь, переиспользуя тот же буфер pathBuf
        for (uint32_t j = 0; j < packDirLen; ++j)
        {
            pathBuf[j] = pathBuf[j]; // уже есть
        }
        uint32_t sfxLen = LiteralLength(shaderFiles[i]);
        if (packDirLen + sfxLen + 1u > PATH_CAPACITY_CHARS)
        {
            anyLoaded = false;
            break;
        }
        for (uint32_t j = 0; j < sfxLen; ++j)
        {
            pathBuf[packDirLen + j] = shaderFiles[i][j];
        }
        pathBuf[packDirLen + sfxLen] = L'\0';

        void* data = LoadCsoFile(pathBuf, outLengths[i]);
        *outPtrs[i] = data;
        anyLoaded = anyLoaded || data != NULL;
    }

    if (!anyLoaded)
    {
        if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_EMPTY;
        for (uint32_t i = 0; i < 6; ++i)
        {
            if (*outPtrs[i] != NULL)
            {
                HeapFree(GetProcessHeap(), 0, *outPtrs[i]);
                *outPtrs[i] = NULL;
                *outLengths[i] = 0;
            }
        }
        HeapFree(GetProcessHeap(), 0, pathBuf);
        return false;
    }

    HeapFree(GetProcessHeap(), 0, pathBuf);
    if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_OK;
    return true;
}
