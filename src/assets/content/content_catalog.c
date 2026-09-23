#include "content/content_catalog.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

#define ACTIVE_FILE_NAME L"active.txt"
#define ACTIVE_UTF8_CAPACITY 512U
#define FORMATS_FILE_NAME L"formats.txt"
#define FORMATS_UTF8_CAPACITY 512U
#define CONTENT_ENUMERATION_LIMIT 4096U

struct LaiueContentCatalog
{
    PlatformRwLock lock;
    uint32_t rootLength;
    wchar_t root[1];
};

enum DefaultCatalogState
{
    DEFAULT_CATALOG_UNINITIALIZED = 0,
    DEFAULT_CATALOG_INITIALIZING = 1,
    DEFAULT_CATALOG_READY = 2,
    DEFAULT_CATALOG_FAILED = 3,
};

static volatile uint32_t g_defaultCatalogState = DEFAULT_CATALOG_UNINITIALIZED;
static LaiueContentCatalog *g_defaultCatalog;

static wchar_t* AllocatePathBuffer(void)
{
    return PlatformAllocate(
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
}

static uint32_t TextLengthBounded(const wchar_t *text, uint32_t capacity)
{
    if (text == NULL)
        return 0;
    uint32_t length = 0;
    while (length < capacity && text[length] != L'\0')
        ++length;
    return length;
}

static bool TextEquals(const wchar_t* left, const wchar_t* right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && right[index] != L'\0')
    {
        if (left[index] != right[index])
            return false;
        ++index;
    }
    return left[index] == right[index];
}

static wchar_t FoldAsciiCase(wchar_t character)
{
    return character >= L'A' && character <= L'Z' ? character + (L'a' - L'A') : character;
}

static bool TextEqualsAsciiCaseInsensitive(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && right[index] != L'\0')
    {
        if (FoldAsciiCase(left[index]) != FoldAsciiCase(right[index]))
            return false;
        ++index;
    }
    return left[index] == right[index];
}

static int32_t TextCompare(const wchar_t* left, const wchar_t* right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && right[index] != L'\0' && left[index] == right[index])
        ++index;
    return left[index] < right[index] ? -1 : left[index] > right[index] ? 1 : 0;
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
    if (destination == NULL || length == NULL || *length + 1U >= capacity)
        return false;
    destination[(*length)++] = character;
    destination[*length] = L'\0';
    return true;
}

static bool AppendSeparator(wchar_t *destination, uint32_t capacity, uint32_t *length)
{
    if (*length > 0U)
    {
        wchar_t last = destination[*length - 1U];
        if (last == L'/' || last == L'\\')
            return true;
    }
    return AppendCharacter(destination, capacity, length, L'/');
}

LaiueContentCatalog *LaiueContentCatalogCreate(const wchar_t *rootDirectory)
{
    wchar_t executableRoot[LAIUE_PLATFORM_PATH_CAPACITY];
    const wchar_t *root = rootDirectory;
    if (root == NULL || root[0] == L'\0')
    {
        if (!PlatformExecutableDirectory(executableRoot, LAIUE_PLATFORM_PATH_CAPACITY))
            return NULL;
        root = executableRoot;
    }

    uint32_t length = TextLengthBounded(root, LAIUE_CONTENT_PATH_CAPACITY);
    if (length == 0U || length >= LAIUE_CONTENT_PATH_CAPACITY)
        return NULL;

    size_t allocationSize =
        offsetof(LaiueContentCatalog, root) + (size_t)(length + 1U) * sizeof(wchar_t);
    LaiueContentCatalog *catalog = PlatformAllocate(allocationSize, true);
    if (catalog == NULL)
        return NULL;
    if (!PlatformRwLockInitialize(&catalog->lock))
    {
        PlatformFree(catalog);
        return NULL;
    }
    catalog->rootLength = length;
    memcpy(catalog->root, root, (size_t)(length + 1U) * sizeof(wchar_t));
    return catalog;
}

void LaiueContentCatalogDestroy(LaiueContentCatalog *catalog)
{
    if (catalog == NULL || catalog == g_defaultCatalog)
        return;
    PlatformRwLockDestroy(&catalog->lock);
    PlatformFree(catalog);
}

LaiueContentCatalog *LaiueContentCatalogDefault(void)
{
    uint32_t state = PlatformAtomicLoadU32Acquire(&g_defaultCatalogState);
    if (state == DEFAULT_CATALOG_READY)
        return g_defaultCatalog;
    if (state == DEFAULT_CATALOG_FAILED)
        return NULL;

    uint32_t expected = DEFAULT_CATALOG_UNINITIALIZED;
    if (PlatformAtomicCompareExchangeU32(&g_defaultCatalogState, &expected,
                                         DEFAULT_CATALOG_INITIALIZING))
    {
        g_defaultCatalog = LaiueContentCatalogCreate(NULL);
        PlatformAtomicStoreU32Release(&g_defaultCatalogState, g_defaultCatalog != NULL
                                                                  ? DEFAULT_CATALOG_READY
                                                                  : DEFAULT_CATALOG_FAILED);
        return g_defaultCatalog;
    }

    do
    {
        PlatformSleepMilliseconds(0U);
        state = PlatformAtomicLoadU32Acquire(&g_defaultCatalogState);
    } while (state == DEFAULT_CATALOG_INITIALIZING);
    return state == DEFAULT_CATALOG_READY ? g_defaultCatalog : NULL;
}

bool LaiueContentCatalogGetRoot(LaiueContentCatalog *catalog, wchar_t *destination,
                                uint32_t capacity)
{
    if (catalog == NULL || destination == NULL || capacity <= catalog->rootLength)
        return false;
    PlatformRwLockAcquireShared(&catalog->lock);
    memcpy(destination, catalog->root, (size_t)(catalog->rootLength + 1U) * sizeof(wchar_t));
    PlatformRwLockReleaseShared(&catalog->lock);
    return true;
}

static bool ChildNameIsSafe(const wchar_t* name)
{
    return name == NULL || LaiueContentNameIsSafe(name);
}

static bool BuildPathUnlocked(LaiueContentCatalog *catalog, LaiueContentType type,
                              const wchar_t *name, const wchar_t *childName, wchar_t *destination,
                              uint32_t capacity)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (catalog == NULL || format == NULL || destination == NULL || capacity == 0U ||
        (name != NULL && (!LaiueContentNameIsSafe(name) || !LaiueContentNameMatches(type, name))) ||
        !ChildNameIsSafe(childName))
        return false;

    if (catalog->rootLength + 1U > capacity)
        return false;
    memcpy(destination, catalog->root, (size_t)(catalog->rootLength + 1U) * sizeof(wchar_t));
    uint32_t length = catalog->rootLength;
    if (!AppendSeparator(destination, capacity, &length) ||
        !AppendText(destination, capacity, &length, format->directoryName))
        return false;
    if (name != NULL && (!AppendSeparator(destination, capacity, &length) ||
                         !AppendText(destination, capacity, &length, name)))
        return false;
    if (childName != NULL && (!AppendSeparator(destination, capacity, &length) ||
                              !AppendText(destination, capacity, &length, childName)))
        return false;
    return true;
}

// Путь к ресурсу внутри пака. Имя ресурса вправе содержать '/', и
// каждый его сегмент проверяется отдельно: разбирать путь целиком
// значило бы решать, что делать с `..`, а так этот вопрос не возникает.
static bool BuildResourcePathUnlocked(LaiueContentCatalog *catalog, LaiueContentType packType,
                                      const wchar_t *packName, const wchar_t *resourcePath,
                                      const wchar_t *extension, wchar_t *destination,
                                      uint32_t capacity)
{
    if (resourcePath == NULL || !LaiueContentPathIsSafe(resourcePath)) return false;
    if (!BuildPathUnlocked(catalog, packType, packName, NULL, destination, capacity)) return false;

    uint32_t length = 0;
    while (destination[length] != 0) ++length;
    if (!AppendSeparator(destination, capacity, &length) ||
        !AppendText(destination, capacity, &length, resourcePath))
    {
        return false;
    }
    return extension == NULL || AppendText(destination, capacity, &length, extension);
}

bool LaiueContentCatalogBuildResourcePath(LaiueContentCatalog *catalog, LaiueContentType packType,
                                          const wchar_t *packName, const wchar_t *resourcePath,
                                          const wchar_t *extension, wchar_t *destination,
                                          uint32_t capacity)
{
    if (catalog == NULL) return false;
    PlatformRwLockAcquireShared(&catalog->lock);
    bool built = BuildResourcePathUnlocked(catalog, packType, packName, resourcePath, extension,
                                           destination, capacity);
    PlatformRwLockReleaseShared(&catalog->lock);
    return built;
}

bool LaiueContentCatalogBuildPath(LaiueContentCatalog *catalog, LaiueContentType type,
                                  const wchar_t *name, const wchar_t *childName,
                                  wchar_t *destination, uint32_t capacity)
{
    if (catalog == NULL)
        return false;
    PlatformRwLockAcquireShared(&catalog->lock);
    bool built = BuildPathUnlocked(catalog, type, name, childName, destination, capacity);
    PlatformRwLockReleaseShared(&catalog->lock);
    return built;
}

static bool BuildDirectoryPathUnlocked(LaiueContentCatalog *catalog, LaiueContentType type,
                                       const wchar_t *suffix, wchar_t *destination,
                                       uint32_t capacity)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (catalog == NULL || format == NULL || destination == NULL || capacity == 0U ||
        catalog->rootLength + 1U > capacity)
        return false;

    memcpy(destination, catalog->root, (size_t)(catalog->rootLength + 1U) * sizeof(wchar_t));
    uint32_t length = catalog->rootLength;
    if (!AppendSeparator(destination, capacity, &length) ||
        !AppendText(destination, capacity, &length, format->directoryName))
        return false;
    return suffix == NULL || (AppendSeparator(destination, capacity, &length) &&
                              AppendText(destination, capacity, &length, suffix));
}

// Строка formats.txt против расширения из `defaults`. Точка в начале
// необязательна с обеих сторон: человек пишет и `wav`, и `.wav`, и
// спорить с ним об этом файл настройки не должен.
static bool ExtensionMatchesLine(const wchar_t *extension, const uint8_t *line, uint32_t length)
{
    if (extension[0] == L'.') ++extension;
    if (length != 0U && line[0] == '.')
    {
        ++line;
        --length;
    }
    uint32_t index = 0U;
    while (index < length)
    {
        if (extension[index] == L'\0') return false;
        if (FoldAsciiCase(extension[index]) != FoldAsciiCase((wchar_t)line[index])) return false;
        ++index;
    }
    return extension[index] == L'\0';
}

static uint8_t *ReadFormatsFile(LaiueContentCatalog *catalog, LaiueContentType packType,
                                uint32_t *outSize)
{
    *outSize = 0U;
    wchar_t *path = AllocatePathBuffer();
    if (path == NULL) return NULL;

    PlatformRwLockAcquireShared(&catalog->lock);
    bool built = BuildDirectoryPathUnlocked(catalog, packType, FORMATS_FILE_NAME, path,
                                            LAIUE_CONTENT_PATH_CAPACITY);
    PlatformRwLockReleaseShared(&catalog->lock);

    uint8_t *bytes = NULL;
    uint64_t size = 0U;
    if (!built || !PlatformReadEntireFile(path, FORMATS_UTF8_CAPACITY, &bytes, &size))
    {
        PlatformFree(path);
        PlatformFree(bytes);
        return NULL;
    }
    PlatformFree(path);
    *outSize = (uint32_t)size;
    return bytes;
}

uint32_t LaiueContentCatalogOrderFormats(LaiueContentCatalog *catalog, LaiueContentType packType,
                                         const wchar_t *const *defaults, uint32_t defaultCount,
                                         const wchar_t **outOrder, uint32_t capacity)
{
    if (defaults == NULL || outOrder == NULL) return 0U;
    if (defaultCount > LAIUE_CONTENT_FORMAT_ORDER_MAX) defaultCount = LAIUE_CONTENT_FORMAT_ORDER_MAX;
    if (defaultCount > capacity) defaultCount = capacity;

    bool taken[LAIUE_CONTENT_FORMAT_ORDER_MAX];
    for (uint32_t index = 0; index < LAIUE_CONTENT_FORMAT_ORDER_MAX; ++index) taken[index] = false;
    uint32_t count = 0U;

    uint32_t size = 0U;
    uint8_t *bytes = catalog != NULL && LaiueContentTypeIsPack(packType)
                         ? ReadFormatsFile(catalog, packType, &size)
                         : NULL;

    uint32_t position = size >= 3U && bytes != NULL && bytes[0] == 0xefU && bytes[1] == 0xbbU &&
                                bytes[2] == 0xbfU
                            ? 3U
                            : 0U;
    while (bytes != NULL && position < size && count < defaultCount)
    {
        uint32_t begin = position;
        while (position < size && bytes[position] != '\n') ++position;
        uint32_t end = position;
        if (position < size) ++position;

        while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t')) ++begin;
        while (end > begin &&
               (bytes[end - 1U] == ' ' || bytes[end - 1U] == '\t' || bytes[end - 1U] == '\r'))
            --end;
        // Пустая строка и комментарий не значат ничего: файл читает
        // человек, и он вправе оставить в нём пометку.
        if (begin == end || bytes[begin] == '#') continue;

        for (uint32_t index = 0; index < defaultCount; ++index)
        {
            if (taken[index] || !ExtensionMatchesLine(defaults[index], bytes + begin, end - begin))
                continue;
            outOrder[count++] = defaults[index];
            taken[index] = true;
            break;
        }
    }
    PlatformFree(bytes);

    for (uint32_t index = 0; index < defaultCount && count < defaultCount; ++index)
    {
        if (taken[index]) continue;
        outOrder[count++] = defaults[index];
    }
    return count;
}

static bool StorageMatches(
    const LaiueContentFormat* format, bool directory)
{
    uint32_t storage = directory
        ? LAIUE_CONTENT_STORAGE_DIRECTORY : LAIUE_CONTENT_STORAGE_FILE;
    return (format->storageMask & storage) != 0U;
}

static bool ContentPathMatchesStorageUnlocked(LaiueContentCatalog *catalog, LaiueContentType type,
                                              const wchar_t *name);

static bool GetActivePackUnlocked(LaiueContentCatalog *catalog, LaiueContentType type,
                                  wchar_t *destination, uint32_t capacity)
{
    if (!LaiueContentTypeIsPack(type) || destination == NULL || capacity == 0U)
        return false;
    destination[0] = L'\0';

    wchar_t* path = AllocatePathBuffer();
    if (path == NULL) return false;
    if (!BuildDirectoryPathUnlocked(catalog, type, ACTIVE_FILE_NAME, path,
                                    LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(path);
        return false;
    }
    uint8_t* bytes = NULL;
    uint64_t size = 0;
    if (!PlatformReadEntireFile(path, ACTIVE_UTF8_CAPACITY - 1U, &bytes, &size) || size == 0U)
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
    if (!converted || !LaiueContentNameIsSafe(destination) ||
        !LaiueContentNameMatches(type, destination) ||
        !ContentPathMatchesStorageUnlocked(catalog, type, destination))
    {
        destination[0] = L'\0';
        return false;
    }
    return true;
}

bool LaiueContentCatalogGetActivePack(LaiueContentCatalog *catalog, LaiueContentType type,
                                      wchar_t *destination, uint32_t capacity)
{
    if (catalog == NULL)
        return false;
    PlatformRwLockAcquireShared(&catalog->lock);
    bool read = GetActivePackUnlocked(catalog, type, destination, capacity);
    PlatformRwLockReleaseShared(&catalog->lock);
    return read;
}

bool LaiueContentCatalogEnumerate(LaiueContentCatalog *catalog, LaiueContentType type,
                                  LaiueContentList *outList)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (catalog == NULL || format == NULL || outList == NULL)
        return false;
    outList->entries = NULL;
    outList->count = 0;

    PlatformRwLockAcquireShared(&catalog->lock);
    wchar_t* directoryPath = AllocatePathBuffer();
    if (directoryPath == NULL || !BuildDirectoryPathUnlocked(catalog, type, NULL, directoryPath,
                                                             LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(directoryPath);
        PlatformRwLockReleaseShared(&catalog->lock);
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
        PlatformRwLockReleaseShared(&catalog->lock);
        return false;
    }
    if (!PlatformDirectoryOpen(iterator, directoryPath))
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        PlatformRwLockReleaseShared(&catalog->lock);
        return true;
    }

    uint32_t count = 0;
    while (PlatformDirectoryNext(iterator, file))
    {
        if (!file->isSymbolicLink
            && StorageMatches(format, file->isDirectory)
            && LaiueContentNameIsSafe(file->name)
            && LaiueContentNameMatches(type, file->name))
        {
            if (count == CONTENT_ENUMERATION_LIMIT)
            {
                PlatformDirectoryClose(iterator);
                PlatformFree(file);
                PlatformFree(iterator);
                PlatformFree(directoryPath);
                PlatformRwLockReleaseShared(&catalog->lock);
                return false;
            }
            ++count;
        }
    }
    PlatformDirectoryClose(iterator);
    if (count == 0U)
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        PlatformRwLockReleaseShared(&catalog->lock);
        return true;
    }

    LaiueContentEntry* entries = PlatformAllocate(
        (size_t)count * sizeof(*entries), true);
    if (entries == NULL)
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        PlatformRwLockReleaseShared(&catalog->lock);
        return false;
    }
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    bool hasActive = format->pack &&
                     GetActivePackUnlocked(catalog, type, activeName, LAIUE_CONTENT_NAME_CAPACITY);

    uint32_t index = 0;
    if (!PlatformDirectoryOpen(iterator, directoryPath))
    {
        PlatformFree(file);
        PlatformFree(iterator);
        PlatformFree(directoryPath);
        PlatformFree(entries);
        PlatformRwLockReleaseShared(&catalog->lock);
        return false;
    }
    while (index < count && PlatformDirectoryNext(iterator, file))
    {
        if (file->isSymbolicLink
            || !StorageMatches(format, file->isDirectory)
            || !LaiueContentNameIsSafe(file->name)
            || !LaiueContentNameMatches(type, file->name))
            continue;
        uint32_t length = TextLengthBounded(file->name, LAIUE_CONTENT_NAME_CAPACITY);
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
        while (position > 0U && TextCompare(value.name, entries[position - 1U].name) < 0)
        {
            entries[position] = entries[position - 1U];
            --position;
        }
        entries[position] = value;
    }

    // A content tree must resolve identically on case-sensitive and
    // case-insensitive filesystems.  Reject the entire ambiguous view instead
    // of selecting a platform-dependent winner.
    for (uint32_t left = 0; left < index; ++left)
    {
        for (uint32_t right = left + 1U; right < index; ++right)
        {
            if (TextEqualsAsciiCaseInsensitive(entries[left].name, entries[right].name))
            {
                PlatformFree(entries);
                PlatformRwLockReleaseShared(&catalog->lock);
                return false;
            }
        }
    }
    outList->entries = entries;
    outList->count = index;
    PlatformRwLockReleaseShared(&catalog->lock);
    return true;
}

void LaiueContentListRelease(LaiueContentList* list)
{
    if (list == NULL) return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0;
}

static bool ContentPathMatchesStorageUnlocked(LaiueContentCatalog *catalog, LaiueContentType type,
                                              const wchar_t *name)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (format == NULL) return false;
    wchar_t* directoryPath = AllocatePathBuffer();
    if (directoryPath == NULL || !BuildDirectoryPathUnlocked(catalog, type, NULL, directoryPath,
                                                             LAIUE_CONTENT_PATH_CAPACITY))
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
    uint32_t foldedMatchCount = 0U;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (entry->isSymbolicLink || !StorageMatches(format, entry->isDirectory) ||
            !LaiueContentNameIsSafe(entry->name) || !LaiueContentNameMatches(type, entry->name) ||
            !TextEqualsAsciiCaseInsensitive(entry->name, name))
            continue;
        ++foldedMatchCount;
        if (TextEquals(entry->name, name))
        {
            found = true;
        }
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);
    return found && foldedMatchCount == 1U;
}

bool LaiueContentCatalogSetActivePack(LaiueContentCatalog *catalog, LaiueContentType type,
                                      const wchar_t *name)
{
    const LaiueContentFormat* format = LaiueContentFormatGet(type);
    if (catalog == NULL || format == NULL || !format->pack)
        return false;

    PlatformRwLockAcquireExclusive(&catalog->lock);
    wchar_t* path = AllocatePathBuffer();
    if (path == NULL || !BuildDirectoryPathUnlocked(catalog, type, ACTIVE_FILE_NAME, path,
                                                    LAIUE_CONTENT_PATH_CAPACITY))
    {
        PlatformFree(path);
        PlatformRwLockReleaseExclusive(&catalog->lock);
        return false;
    }
    if (name == NULL || name[0] == L'\0')
    {
        bool removed = !PlatformPathExists(path) || PlatformDeleteFile(path);
        PlatformFree(path);
        PlatformRwLockReleaseExclusive(&catalog->lock);
        return removed;
    }
    if (!LaiueContentNameIsSafe(name) || !LaiueContentNameMatches(type, name) ||
        !ContentPathMatchesStorageUnlocked(catalog, type, name))
    {
        PlatformFree(path);
        PlatformRwLockReleaseExclusive(&catalog->lock);
        return false;
    }

    char utf8[ACTIVE_UTF8_CAPACITY];
    uint32_t byteCount = 0;
    bool converted = PlatformWideToUtf8(name, utf8, sizeof(utf8) - 1U, &byteCount);
    bool written = false;
    if (converted && byteCount + 1U <= sizeof(utf8))
    {
        utf8[byteCount++] = '\n';
        written = PlatformWriteFileAtomic(path, utf8, byteCount);
    }
    PlatformFree(path);
    PlatformRwLockReleaseExclusive(&catalog->lock);
    return written;
}

bool LaiueContentEnumerate(LaiueContentType type, LaiueContentList *outList)
{
    return LaiueContentCatalogEnumerate(LaiueContentCatalogDefault(), type, outList);
}

bool LaiueContentSetActivePack(LaiueContentType type, const wchar_t *name)
{
    return LaiueContentCatalogSetActivePack(LaiueContentCatalogDefault(), type, name);
}

bool LaiueContentGetActivePack(LaiueContentType type, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogGetActivePack(LaiueContentCatalogDefault(), type, destination,
                                            capacity);
}

bool LaiueContentBuildPath(LaiueContentType type, const wchar_t *name, const wchar_t *childName,
                           wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogBuildPath(LaiueContentCatalogDefault(), type, name, childName,
                                        destination, capacity);
}
