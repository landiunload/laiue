#include "render/shader_pack.h"
#include "content/content_format.h"
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

    wchar_t* basePath = HeapAlloc(GetProcessHeap(), 0,
        (size_t)PATH_CAPACITY_CHARS * sizeof(wchar_t));
    if (basePath == NULL)
    {
        return false;
    }

    uint32_t directoryLength = 0;
    if (!GetExecutableDirectory(basePath, PATH_CAPACITY_CHARS, &directoryLength)
        || !BuildPath(basePath, PATH_CAPACITY_CHARS, directoryLength,
            SHADER_PACK_DIR))
    {
        HeapFree(GetProcessHeap(), 0, basePath);
        return false;
    }

    uint32_t dirPrefixLen = directoryLength + LiteralLength(SHADER_PACK_DIR);

    // Строим шаблон поиска "<dir>*" для FindFirstFile
    basePath[dirPrefixLen] = L'*';
    basePath[dirPrefixLen + 1] = L'\0';

    // Считаем поддиректории
    uint32_t subdirCount = 1; // всегда есть "Default"
    WIN32_FIND_DATAW findData;
    HANDLE findHandle = FindFirstFileW(basePath, &findData);
    if (findHandle != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                && findData.cFileName[0] != L'.'
                && LaiueContentNameMatches(
                    LAIUE_CONTENT_SHADER_PACK, findData.cFileName))
            {
                ++subdirCount;
            }
        }
        while (FindNextFileW(findHandle, &findData));
        FindClose(findHandle);
    }

    outList->entries = HeapAlloc(GetProcessHeap(), 0,
        (size_t)subdirCount * sizeof(ShaderPackEntry));
    if (outList->entries == NULL)
    {
        HeapFree(GetProcessHeap(), 0, basePath);
        return false;
    }

    // Заполняем: сначала "Default" (built-in)
    uint32_t index = 0;
    memcpy(outList->entries[index].name, L"Default", 8 * sizeof(wchar_t));
    outList->entries[index].active = true;
    ++index;

    // Восстанавливаем путь (убираем * в конце)
    basePath[dirPrefixLen] = L'\0';

    // Читаем active.txt
    uint8_t activeName[64];
    uint32_t activeNameLen = 0;
    memcpy(basePath + dirPrefixLen, L"active.txt", 12 * sizeof(wchar_t));

    HANDLE activeFile = CreateFileW(basePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (activeFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(activeFile, &size)
            && size.QuadPart > 0
            && (LONGLONG)size.QuadPart <= (LONGLONG)sizeof(activeName))
        {
            DWORD read = 0;
            if (ReadFile(activeFile, activeName, (DWORD)size.QuadPart, &read, NULL))
            {
                uint32_t len = (uint32_t)size.QuadPart;
                while (len > 0 && (activeName[len - 1] == ' '
                    || activeName[len - 1] == '\n' || activeName[len - 1] == '\r'))
                {
                    --len;
                }
                activeName[len] = '\0';
                activeNameLen = len;
            }
        }
        CloseHandle(activeFile);
    }

    if (activeNameLen > 0)
    {
        outList->entries[0].active = false;
    }

    // Заполняем поддиректории (восстанавливаем шаблон поиска)
    basePath[dirPrefixLen] = L'*';
    basePath[dirPrefixLen + 1] = L'\0';

    findHandle = FindFirstFileW(basePath, &findData);
    if (findHandle != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                || findData.cFileName[0] == L'.'
                || !LaiueContentNameMatches(
                    LAIUE_CONTENT_SHADER_PACK, findData.cFileName))
            {
                continue;
            }
            if (index >= subdirCount)
            {
                break;
            }

            uint32_t nameLen = 0;
            while (findData.cFileName[nameLen] != L'\0'
                && nameLen < SHADER_PACK_NAME_MAX - 1)
            {
                outList->entries[index].name[nameLen] = findData.cFileName[nameLen];
                ++nameLen;
            }
            outList->entries[index].name[nameLen] = L'\0';
            outList->entries[index].active = false;

            if (activeNameLen > 0 && nameLen == activeNameLen)
            {
                bool match = true;
                for (uint32_t i = 0; i < nameLen; ++i)
                {
                    if ((wchar_t)activeName[i] != outList->entries[index].name[i])
                    {
                        match = false;
                        break;
                    }
                }
                outList->entries[index].active = match;
            }
            ++index;
        }
        while (FindNextFileW(findHandle, &findData));
        FindClose(findHandle);
    }

    HeapFree(GetProcessHeap(), 0, basePath);
    outList->count = index;
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
    bool hasHeader = length >= sizeof(header) - 1u
        && BytesEqual(data, header, sizeof(header) - 1u);
    bool hasContract = false;
    for (uint32_t i = 0; i + sizeof(contract) - 1u <= length; ++i)
    {
        if (BytesEqual(data + i, contract, sizeof(contract) - 1u))
        {
            hasContract = true;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
    return hasHeader && hasContract;
}

bool ShaderPackLoadActiveBytecode(
    void** outChunkVS, uint32_t* outChunkVSLength,
    void** outChunkPS, uint32_t* outChunkPSLength,
    void** outPanoramaVS, uint32_t* outPanoramaVSLength,
    void** outPanoramaPS, uint32_t* outPanoramaPSLength,
    void** outUIVS, uint32_t* outUIVSLength,
    void** outUIPS, uint32_t* outUIPSLength)
{
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
    return true;
}
