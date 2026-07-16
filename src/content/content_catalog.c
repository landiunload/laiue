#include "content/content_catalog.h"

#include <windows.h>
#include <stddef.h>

#define ACTIVE_FILE_NAME L"active.txt"
#define ACTIVE_UTF8_CAPACITY 512u

static uint32_t TextLength(const wchar_t* text)
{
    uint32_t length = 0;
    if (text == NULL)
    {
        return 0;
    }
    while (text[length] != L'\0')
    {
        ++length;
    }
    return length;
}

static bool AppendText(wchar_t* destination, uint32_t capacity,
    uint32_t* length, const wchar_t* source)
{
    if (destination == NULL || length == NULL || source == NULL)
    {
        return false;
    }
    while (*source != L'\0')
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        destination[(*length)++] = *source++;
    }
    destination[*length] = L'\0';
    return true;
}

static bool AppendCharacter(wchar_t* destination, uint32_t capacity,
    uint32_t* length, wchar_t character)
{
    if (*length + 1u >= capacity)
    {
        return false;
    }
    destination[(*length)++] = character;
    destination[*length] = L'\0';
    return true;
}

static bool GetExecutableDirectory(wchar_t* destination, uint32_t capacity,
    uint32_t* outLength)
{
    DWORD length = GetModuleFileNameW(NULL, destination, capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }
    for (uint32_t i = (uint32_t)length; i > 0; --i)
    {
        if (destination[i - 1u] == L'\\' || destination[i - 1u] == L'/')
        {
            destination[i - 1u] = L'\0';
            *outLength = i - 1u;
            return true;
        }
    }
    return false;
}

static bool ChildNameIsSafe(const wchar_t* name)
{
    return name == NULL || LaiueContentNameIsSafe(name);
}

bool LaiueContentBuildPath(LaiueContentType type,
    const wchar_t* name, const wchar_t* childName,
    wchar_t* destination, uint32_t capacity)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || destination == NULL || capacity == 0
        || (name != NULL && (!LaiueContentNameIsSafe(name)
            || !LaiueContentNameMatches(type, name)))
        || !ChildNameIsSafe(childName))
    {
        return false;
    }

    uint32_t length = 0;
    if (!GetExecutableDirectory(destination, capacity, &length)
        || !AppendCharacter(destination, capacity, &length, L'\\')
        || !AppendText(destination, capacity, &length, format->directoryName))
    {
        return false;
    }

    if (name != NULL)
    {
        if (!AppendCharacter(destination, capacity, &length, L'\\')
            || !AppendText(destination, capacity, &length, name))
        {
            return false;
        }
    }
    if (childName != NULL)
    {
        if (!AppendCharacter(destination, capacity, &length, L'\\')
            || !AppendText(destination, capacity, &length, childName))
        {
            return false;
        }
    }
    return true;
}

static bool BuildDirectoryPath(LaiueContentType type, const wchar_t* suffix,
    wchar_t* destination, uint32_t capacity)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || destination == NULL || capacity == 0)
    {
        return false;
    }

    uint32_t length = 0;
    if (!GetExecutableDirectory(destination, capacity, &length)
        || !AppendCharacter(destination, capacity, &length, L'\\')
        || !AppendText(destination, capacity, &length, format->directoryName))
    {
        return false;
    }
    if (suffix != NULL)
    {
        if (!AppendCharacter(destination, capacity, &length, L'\\')
            || !AppendText(destination, capacity, &length, suffix))
        {
            return false;
        }
    }
    return true;
}

static bool StorageMatches(const LaiueContentFormat* format, DWORD attributes)
{
    bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint32_t storage = directory
        ? LAIUE_CONTENT_STORAGE_DIRECTORY : LAIUE_CONTENT_STORAGE_FILE;
    return (format->storageMask & storage) != 0;
}

static bool TextEquals(const wchar_t* left, const wchar_t* right)
{
    uint32_t i = 0;
    while (left[i] != L'\0' && right[i] != L'\0')
    {
        if (left[i] != right[i])
        {
            return false;
        }
        ++i;
    }
    return left[i] == right[i];
}

bool LaiueContentGetActivePack(LaiueContentType type,
    wchar_t* destination, uint32_t capacity)
{
    if (!LaiueContentTypeIsPack(type) || destination == NULL || capacity == 0)
    {
        return false;
    }
    destination[0] = L'\0';

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL)
    {
        return false;
    }
    if (!BuildDirectoryPath(type, ACTIVE_FILE_NAME,
            path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HeapFree(GetProcessHeap(), 0, path);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    uint8_t bytes[ACTIVE_UTF8_CAPACITY];
    LARGE_INTEGER size;
    DWORD read = 0;
    bool valid = GetFileSizeEx(file, &size)
        && size.QuadPart > 0
        && size.QuadPart < (LONGLONG)sizeof(bytes)
        && ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL)
        && read == (DWORD)size.QuadPart;
    CloseHandle(file);
    if (!valid)
    {
        return false;
    }

    uint32_t begin = 0;
    uint32_t end = read;
    if (end >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu
        && bytes[2] == 0xBFu)
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
    if (begin == end)
    {
        return false;
    }

    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        (const char*)bytes + begin, (int)(end - begin),
        destination, (int)(capacity - 1u));
    if (converted <= 0 || (uint32_t)converted >= capacity)
    {
        destination[0] = L'\0';
        return false;
    }
    destination[converted] = L'\0';
    if (!LaiueContentNameIsSafe(destination)
        || !LaiueContentNameMatches(type, destination))
    {
        destination[0] = L'\0';
        return false;
    }
    return true;
}

bool LaiueContentEnumerate(LaiueContentType type, LaiueContentList* outList)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || outList == NULL)
    {
        return false;
    }
    outList->entries = NULL;
    outList->count = 0;

    wchar_t* searchPath = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (searchPath == NULL
        || !BuildDirectoryPath(type, L"*", searchPath,
            LAIUE_CONTENT_PATH_CAPACITY))
    {
        if (searchPath != NULL) HeapFree(GetProcessHeap(), 0, searchPath);
        return false;
    }

    uint32_t count = 0;
    WIN32_FIND_DATAW findData;
    HANDLE find = FindFirstFileW(searchPath, &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (findData.cFileName[0] != L'.'
                && StorageMatches(format, findData.dwFileAttributes)
                && LaiueContentNameIsSafe(findData.cFileName)
                && LaiueContentNameMatches(type, findData.cFileName))
            {
                ++count;
            }
        }
        while (FindNextFileW(find, &findData));
        FindClose(find);
    }

    if (count == 0)
    {
        HeapFree(GetProcessHeap(), 0, searchPath);
        return true;
    }

    LaiueContentEntry* entries = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, (size_t)count * sizeof(*entries));
    if (entries == NULL)
    {
        HeapFree(GetProcessHeap(), 0, searchPath);
        return false;
    }

    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    bool hasActive = format->pack
        && LaiueContentGetActivePack(type, activeName,
            LAIUE_CONTENT_NAME_CAPACITY);

    uint32_t index = 0;
    find = FindFirstFileW(searchPath, &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (findData.cFileName[0] == L'.'
                || !StorageMatches(format, findData.dwFileAttributes)
                || !LaiueContentNameIsSafe(findData.cFileName)
                || !LaiueContentNameMatches(type, findData.cFileName))
            {
                continue;
            }

            uint32_t length = TextLength(findData.cFileName);
            if (length >= LAIUE_CONTENT_NAME_CAPACITY)
            {
                continue;
            }
            for (uint32_t i = 0; i <= length; ++i)
            {
                entries[index].name[i] = findData.cFileName[i];
            }
            entries[index].directory =
                (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entries[index].active = hasActive
                && TextEquals(entries[index].name, activeName);
            ++index;
        }
        while (index < count && FindNextFileW(find, &findData));
        FindClose(find);
    }

    HeapFree(GetProcessHeap(), 0, searchPath);
    outList->entries = entries;
    outList->count = index;
    return true;
}

void LaiueContentListRelease(LaiueContentList* list)
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

bool LaiueContentSetActivePack(LaiueContentType type, const wchar_t* name)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || !format->pack)
    {
        return false;
    }

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL)
    {
        return false;
    }

    if (!BuildDirectoryPath(type, ACTIVE_FILE_NAME,
            path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }

    if (name == NULL || name[0] == L'\0')
    {
        bool removed = DeleteFileW(path) != FALSE;
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, path);
        return removed || error == ERROR_FILE_NOT_FOUND;
    }

    if (!LaiueContentNameIsSafe(name)
        || !LaiueContentNameMatches(type, name))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }

    wchar_t* contentPath = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (contentPath == NULL)
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }
    bool exists = LaiueContentBuildPath(type, name, NULL,
        contentPath, LAIUE_CONTENT_PATH_CAPACITY);
    DWORD attributes = exists ? GetFileAttributesW(contentPath)
        : INVALID_FILE_ATTRIBUTES;
    HeapFree(GetProcessHeap(), 0, contentPath);
    if (attributes == INVALID_FILE_ATTRIBUTES
        || !StorageMatches(format, attributes))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }

    char utf8[ACTIVE_UTF8_CAPACITY];
    int byteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        name, -1, utf8, (int)sizeof(utf8) - 1, NULL, NULL);
    if (byteCount <= 1 || byteCount >= (int)sizeof(utf8))
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }
    utf8[byteCount - 1] = '\n';

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HeapFree(GetProcessHeap(), 0, path);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    bool succeeded = WriteFile(file, utf8, (DWORD)byteCount, &written, NULL)
        && written == (DWORD)byteCount;
    CloseHandle(file);
    return succeeded;
}
