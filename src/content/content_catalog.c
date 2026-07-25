#include "content/content_catalog.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

#define ACTIVE_FILE_NAME L"active.txt"
#define ACTIVE_UTF8_CAPACITY 512U

static wchar_t* AllocatePathBuffer(void)
{
    return PlatformAllocate(
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
}

static uint32_t TextLength(const wchar_t* text)
{
    uint32_t length = 0;
    if (text != NULL)
        while (text[length] != L'\0') ++length;
    return length;
}

static bool TextEquals(const wchar_t* left, const wchar_t* right)
{
    uint32_t i = 0;
    while (left[i] != L'\0' && right[i] != L'\0')
    {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == right[i];
}

static int32_t TextCompare(const wchar_t* left, const wchar_t* right)
{
    uint32_t i = 0;
    while (left[i] != L'\0' && right[i] != L'\0'
        && left[i] == right[i])
        ++i;
    return left[i] < right[i] ? -1 : left[i] > right[i] ? 1 : 0;
}

static bool AppendText(wchar_t* destination, uint32_t capacity,
    uint32_t* length, const wchar_t* source)
{
    if (destination == NULL || length == NULL || source == NULL) return false;
    while (*source != L'\0')
    {
        if (*length + 1U >= capacity) return false;
        destination[(*length)++] = *source++;
    }
    destination[*length] = L'\0';
    return true;
}

static bool AppendCharacter(wchar_t* destination, uint32_t capacity,
    uint32_t* length, wchar_t character)
{
    if (*length + 1U >= capacity) return false;
    destination[(*length)++] = character;
    destination[*length] = L'\0';
    return true;
}

static bool GetExecutableDirectory(wchar_t* destination, uint32_t capacity,
    uint32_t* outLength)
{
    if (!PlatformExecutableDirectory(destination, capacity)) return false;
    *outLength = TextLength(destination);
    return true;
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
        return false;

    uint32_t length = 0;
    if (!GetExecutableDirectory(destination, capacity, &length)
        || !AppendCharacter(destination, capacity, &length, L'/')
        || !AppendText(destination, capacity, &length, format->directoryName))
        return false;
    if (name != NULL
        && (!AppendCharacter(destination, capacity, &length, L'/')
            || !AppendText(destination, capacity, &length, name)))
        return false;
    if (childName != NULL
        && (!AppendCharacter(destination, capacity, &length, L'/')
            || !AppendText(destination, capacity, &length, childName)))
        return false;
    return true;
}

static bool BuildDirectoryPath(LaiueContentType type, const wchar_t* suffix,
    wchar_t* destination, uint32_t capacity)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || destination == NULL || capacity == 0) return false;
    uint32_t length = 0;
    if (!GetExecutableDirectory(destination, capacity, &length)
        || !AppendCharacter(destination, capacity, &length, L'/')
        || !AppendText(destination, capacity, &length, format->directoryName))
        return false;
    return suffix == NULL
        || (AppendCharacter(destination, capacity, &length, L'/')
            && AppendText(destination, capacity, &length, suffix));
}

static bool StorageMatches(
    const LaiueContentFormat* format, bool directory)
{
    uint32_t storage = directory
        ? LAIUE_CONTENT_STORAGE_DIRECTORY : LAIUE_CONTENT_STORAGE_FILE;
    return (format->storageMask & storage) != 0;
}

bool LaiueContentGetActivePack(LaiueContentType type,
    wchar_t* destination, uint32_t capacity)
{
    if (!LaiueContentTypeIsPack(type) || destination == NULL || capacity == 0)
        return false;
    destination[0] = L'\0';

    wchar_t* path = AllocatePathBuffer();
    if (path == NULL) return false;
    if (!BuildDirectoryPath(
            type, ACTIVE_FILE_NAME, path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(path);
        return false;
    }
    uint8_t* bytes = NULL;
    uint64_t size = 0;
    if (!PlatformReadEntireFile(
            path, ACTIVE_UTF8_CAPACITY - 1U, &bytes, &size)
        || size == 0)
    {
        PlatformFree(path);
        PlatformFree(bytes);
        return false;
    }
    PlatformFree(path);

    uint32_t begin = size >= 3U && bytes[0] == 0xefU
        && bytes[1] == 0xbbU && bytes[2] == 0xbfU ? 3U : 0U;
    uint32_t end = (uint32_t)size;
    while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t'
        || bytes[begin] == '\r' || bytes[begin] == '\n'))
        ++begin;
    while (end > begin && (bytes[end - 1U] == ' '
        || bytes[end - 1U] == '\t' || bytes[end - 1U] == '\r'
        || bytes[end - 1U] == '\n'))
        --end;
    bool converted = begin < end && PlatformUtf8ToWide(
        (const char*)bytes + begin, end - begin,
        destination, capacity, NULL);
    PlatformFree(bytes);
    if (!converted || !LaiueContentNameIsSafe(destination)
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
    if (format == NULL || outList == NULL) return false;
    outList->entries = NULL;
    outList->count = 0;

    wchar_t* directoryPath = AllocatePathBuffer();
    if (directoryPath == NULL) return false;
    if (!BuildDirectoryPath(
            type, NULL, directoryPath, LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(directoryPath);
        return false;
    }
    PlatformDirectoryIterator* iterator =
        PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry* file =
        PlatformAllocate(sizeof(*file), false);
    if (iterator == NULL || file == NULL)
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        return false;
    }
    if (!PlatformDirectoryOpen(iterator, directoryPath))
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        return true;
    }

    uint32_t count = 0;
    while (PlatformDirectoryNext(iterator, file))
    {
        if (!file->isSymbolicLink
            && StorageMatches(format, file->isDirectory)
            && LaiueContentNameIsSafe(file->name)
            && LaiueContentNameMatches(type, file->name))
            ++count;
    }
    PlatformDirectoryClose(iterator);
    if (count == 0)
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        return true;
    }

    LaiueContentEntry* entries = PlatformAllocate(
        (size_t)count * sizeof(*entries), true);
    if (entries == NULL)
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        return false;
    }
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    bool hasActive = format->pack && LaiueContentGetActivePack(
        type, activeName, LAIUE_CONTENT_NAME_CAPACITY);

    uint32_t index = 0;
    if (!PlatformDirectoryOpen(iterator, directoryPath))
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        PlatformFree(entries);
        return false;
    }
    while (index < count && PlatformDirectoryNext(iterator, file))
    {
        if (file->isSymbolicLink
            || !StorageMatches(format, file->isDirectory)
            || !LaiueContentNameIsSafe(file->name)
            || !LaiueContentNameMatches(type, file->name))
            continue;
        uint32_t length = TextLength(file->name);
        if (length >= LAIUE_CONTENT_NAME_CAPACITY) continue;
        memcpy(entries[index].name, file->name,
            (size_t)(length + 1U) * sizeof(wchar_t));
        entries[index].directory = file->isDirectory;
        entries[index].active = hasActive
            && TextEquals(entries[index].name, activeName);
        ++index;
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(file);
    PlatformFree(iterator);
    PlatformFree(directoryPath);

    for (uint32_t i = 1; i < index; ++i)
    {
        LaiueContentEntry value = entries[i];
        uint32_t position = i;
        while (position > 0
            && TextCompare(value.name, entries[position - 1U].name) < 0)
        {
            entries[position] = entries[position - 1U];
            --position;
        }
        entries[position] = value;
    }
    outList->entries = entries;
    outList->count = index;
    return true;
}

void LaiueContentListRelease(LaiueContentList* list)
{
    if (list == NULL) return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0;
}

static bool ContentPathMatchesStorage(
    LaiueContentType type, const wchar_t* name)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL) return false;
    wchar_t* directoryPath = AllocatePathBuffer();
    if (directoryPath == NULL || !BuildDirectoryPath(
            type, NULL, directoryPath, LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(directoryPath);
        return false;
    }
    PlatformDirectoryIterator* iterator =
        PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry* entry =
        PlatformAllocate(sizeof(*entry), false);
    if (iterator == NULL || entry == NULL
        || !PlatformDirectoryOpen(iterator, directoryPath))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        return false;
    }
    PlatformFree(directoryPath);
    bool found = false;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (!entry->isSymbolicLink && TextEquals(entry->name, name)
            && StorageMatches(format, entry->isDirectory))
        {
            found = true;
            break;
        }
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);
    return found;
}

bool LaiueContentSetActivePack(LaiueContentType type, const wchar_t* name)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL || !format->pack) return false;
    wchar_t* path = AllocatePathBuffer();
    if (path == NULL) return false;
    if (!BuildDirectoryPath(
            type, ACTIVE_FILE_NAME, path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(path);
        return false;
    }
    if (name == NULL || name[0] == L'\0')
    {
        bool removed = !PlatformPathExists(path) || PlatformDeleteFile(path);
        PlatformFree(path);
        return removed;
    }
    if (!LaiueContentNameIsSafe(name)
        || !LaiueContentNameMatches(type, name)
        || !ContentPathMatchesStorage(type, name))
    {
        PlatformFree(path);
        return false;
    }

    char utf8[ACTIVE_UTF8_CAPACITY];
    uint32_t byteCount = 0;
    if (!PlatformWideToUtf8(name, utf8,
            sizeof(utf8) - 1U, &byteCount)
        || byteCount + 1U > sizeof(utf8))
    {
        PlatformFree(path);
        return false;
    }
    utf8[byteCount++] = '\n';
    bool written = PlatformWriteFileAtomic(path, utf8, byteCount);
    PlatformFree(path);
    return written;
}
