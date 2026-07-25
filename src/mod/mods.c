#include "mod/mods.h"
#include "content/content_catalog.h"
#include "platform/system.h"

#include "laiue_mod_api.h"

#include <string.h>

#define MOD_PACK_MANIFEST_NAME L"mod.lm"

#define MOD_FILE_MAX_BYTES (64u * 1024u)
#define MOD_NATIVE_MAX_BYTES (256ULL * 1024ULL * 1024ULL)
#define MOD_FORMAT_VERSION 2
#define MODS_NAME_UTF8_CAPACITY (MODS_NAME_CAPACITY * 4U)

// === Байтовые утилиты парсера (файлы модов — построчный UTF-8) ===

typedef struct ByteSlice
{
    const uint8_t *data;
    uint32_t length;
} ByteSlice;

static bool SliceEqualsAscii(ByteSlice slice, const char *text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
        ++length;
    if (slice.length != length)
    {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i)
    {
        uint8_t a = slice.data[i];
        if (a >= 'A' && a <= 'Z')
            a = (uint8_t)(a + ('a' - 'A'));
        uint8_t b = (uint8_t)text[i];
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

static ByteSlice SliceTrim(ByteSlice slice)
{
    while (slice.length > 0 &&
           (slice.data[0] == ' ' || slice.data[0] == '\t' || slice.data[0] == '\r'))
    {
        ++slice.data;
        --slice.length;
    }
    while (slice.length > 0 &&
           (slice.data[slice.length - 1] == ' ' || slice.data[slice.length - 1] == '\t' ||
            slice.data[slice.length - 1] == '\r'))
    {
        --slice.length;
    }
    return slice;
}

static bool SliceParseUint32(ByteSlice slice, uint32_t *outValue)
{
    if (slice.length == 0 || outValue == NULL)
    {
        return false;
    }

    uint32_t value = 0;
    for (uint32_t index = 0; index < slice.length; ++index)
    {
        uint8_t digit = slice.data[index];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        digit = (uint8_t)(digit - '0');
        if (value > (UINT32_MAX - digit) / 10u)
        {
            return false;
        }
        value = value * 10u + digit;
    }
    *outValue = value;
    return true;
}

static void SliceToWide(ByteSlice slice, wchar_t *destination, uint32_t capacity)
{
    destination[0] = L'\0';
    if (slice.length == 0 || capacity < 2)
    {
        return;
    }
    uint32_t written = 0;
    if (!PlatformUtf8ToWide((const char *)slice.data, slice.length, destination, capacity,
                            &written))
        destination[0] = L'\0';
}

static bool SliceToIdentifier(ByteSlice slice, char *destination, uint32_t capacity)
{
    if (slice.length == 0 || slice.length >= capacity)
    {
        return false;
    }
    for (uint32_t i = 0; i < slice.length; ++i)
    {
        uint8_t c = slice.data[i];
        if (c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + ('a' - 'A'));
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
        {
            destination[0] = '\0';
            return false;
        }
        destination[i] = (char)c;
    }
    destination[slice.length] = '\0';
    return true;
}

static bool HashBuffer(const uint8_t *bytes, uint32_t length,
                       uint8_t output[MODS_CONTENT_HASH_SIZE])
{
    return PlatformSha256(bytes, length, output);
}

// === Файловый ввод-вывод ===

static uint8_t *ReadWholeFile(const wchar_t *path, uint32_t *outLength)
{
    uint8_t *bytes = NULL;
    uint64_t length = 0;
    if (outLength == NULL || !PlatformReadEntireFile(path, MOD_FILE_MAX_BYTES, &bytes, &length) ||
        length == 0 || length > UINT32_MAX)
    {
        PlatformFree(bytes);
        return NULL;
    }
    *outLength = (uint32_t)length;
    return bytes;
}

static bool HashFile(const wchar_t *path, uint8_t output[MODS_CONTENT_HASH_SIZE])
{
    uint8_t *bytes = NULL;
    uint64_t size = 0;
    bool succeeded = PlatformReadEntireFile(path, MOD_NATIVE_MAX_BYTES, &bytes, &size) &&
                     size != 0 && PlatformSha256(bytes, size, output);
    PlatformFree(bytes);
    return succeeded;
}

static bool CombineHashes(const uint8_t left[MODS_CONTENT_HASH_SIZE],
                          const uint8_t right[MODS_CONTENT_HASH_SIZE],
                          uint8_t output[MODS_CONTENT_HASH_SIZE])
{
    uint8_t pair[MODS_CONTENT_HASH_SIZE * 2U];
    memcpy(pair, left, MODS_CONTENT_HASH_SIZE);
    memcpy(pair + MODS_CONTENT_HASH_SIZE, right, MODS_CONTENT_HASH_SIZE);
    return HashBuffer(pair, sizeof(pair), output);
}

static bool HashesEqual(const uint8_t *left, const uint8_t *right)
{
    return PlatformConstantTimeEqual(left, right, MODS_CONTENT_HASH_SIZE);
}

static bool BuildEnabledPath(const ModsState *mods, wchar_t *destination, uint32_t capacity)
{
    if (!LaiueContentBuildPath(LAIUE_CONTENT_MOD, NULL, NULL, destination, capacity))
    {
        return false;
    }
    uint32_t length = 0;
    while (destination[length] != L'\0')
        ++length;
    uint32_t nameLength = 0;
    while (mods->enabledFileName[nameLength] != L'\0')
        ++nameLength;
    if (length + nameLength + 2 > capacity)
    {
        return false;
    }
    destination[length++] = L'/';
    for (uint32_t i = 0; i <= nameLength; ++i)
    {
        destination[length + i] = mods->enabledFileName[i];
    }
    return true;
}

static bool WriteFileAtomically(const wchar_t *path, const uint8_t *bytes, uint32_t length)
{
    return PlatformWriteFileAtomic(path, bytes, length);
}

// === Разбор файла мода ===

// Колбек получает секцию (пустая до первого заголовка), ключ и значение.
typedef void (*ModPairCallback)(void *context, ByteSlice section, ByteSlice key, ByteSlice value);

// Разбирает файл: первая значимая строка обязана быть заголовком
// `LAIUE MOD <версия>` с версией не новее поддерживаемой.
static bool ParseModFile(const uint8_t *bytes, uint32_t length, ModPairCallback callback,
                         void *context, uint32_t *outFormatVersion)
{
    ByteSlice section = {NULL, 0};
    bool headerSeen = false;
    uint32_t formatVersion = 0;

    uint32_t offset = 0;
    if (length >= 3 && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu)
    {
        offset = 3; // BOM
    }

    while (offset < length)
    {
        uint32_t lineEnd = offset;
        while (lineEnd < length && bytes[lineEnd] != '\n')
            ++lineEnd;

        ByteSlice line = SliceTrim((ByteSlice){bytes + offset, lineEnd - offset});
        offset = lineEnd + 1;

        if (line.length == 0 || line.data[0] == '#')
        {
            continue;
        }

        if (!headerSeen)
        {
            // "LAIUE MOD <n>"
            if (line.length < 11 || !SliceEqualsAscii((ByteSlice){line.data, 9}, "laiue mod"))
            {
                return false;
            }
            if (!SliceParseUint32(SliceTrim((ByteSlice){line.data + 9, line.length - 9}),
                                  &formatVersion) ||
                formatVersion < 1 || formatVersion > MOD_FORMAT_VERSION)
            {
                return false;
            }
            headerSeen = true;
            continue;
        }

        if (line.data[0] == '[')
        {
            if (line.data[line.length - 1] == ']' && line.length >= 3)
            {
                section = SliceTrim((ByteSlice){line.data + 1, line.length - 2});
            }
            continue;
        }

        uint32_t equals = 0;
        while (equals < line.length && line.data[equals] != '=')
            ++equals;
        if (equals == 0 || equals == line.length)
        {
            continue;
        }

        ByteSlice key = SliceTrim((ByteSlice){line.data, equals});
        ByteSlice value = SliceTrim((ByteSlice){line.data + equals + 1, line.length - equals - 1});
        if (key.length > 0)
        {
            callback(context, section, key, value);
        }
    }

    if (headerSeen && outFormatVersion != NULL)
    {
        *outFormatVersion = formatVersion;
    }
    return headerSeen;
}

// === Потребитель 1: манифест (имя, версия, требуемая игра) ===

typedef struct ManifestParseState
{
    ModEntry *entry;
    bool valid;
    bool legacyEntrySeen;
    bool windowsEntrySeen;
    bool linuxGnuEntrySeen;
    bool linuxMuslEntrySeen;
} ManifestParseState;

static void ParseNativeEntry(ManifestParseState *state, ByteSlice value, bool *seen,
                             wchar_t *destination)
{
    if (*seen)
    {
        state->valid = false;
        return;
    }
    *seen = true;
    SliceToWide(value, destination, MODS_NAME_CAPACITY);
    if (destination[0] == L'\0')
    {
        state->valid = false;
    }
}

static void ManifestCallback(void *context, ByteSlice section, ByteSlice key, ByteSlice value)
{
    ManifestParseState *state = context;
    ModEntry *entry = state->entry;
    if (section.length == 0)
    {
        if (SliceEqualsAscii(key, "name"))
        {
            SliceToWide(value, entry->displayName, MODS_NAME_CAPACITY);
        }
        else if (SliceEqualsAscii(key, "version"))
        {
            SliceToWide(value, entry->version, 16);
        }
        else if (SliceEqualsAscii(key, "game"))
        {
            SliceToWide(value, entry->requiredGame, 16);
        }
        else if (SliceEqualsAscii(key, "id"))
        {
            SliceToIdentifier(value, entry->id, MODS_ID_CAPACITY);
        }
        else if (SliceEqualsAscii(key, "side"))
        {
            if (SliceEqualsAscii(value, "client"))
                entry->side = MOD_SIDE_CLIENT;
            else if (SliceEqualsAscii(value, "server"))
                entry->side = MOD_SIDE_SERVER;
            else if (SliceEqualsAscii(value, "both"))
                entry->side = MOD_SIDE_BOTH;
            else
                entry->sideValid = false;
        }
        return;
    }

    // Нативный мод: платформенные точки входа и версия API SDK.
    if (SliceEqualsAscii(section, "native"))
    {
        if (SliceEqualsAscii(key, "entry"))
        {
            ParseNativeEntry(state, value, &state->legacyEntrySeen, entry->entryWindowsX86_64);
        }
        else if (SliceEqualsAscii(key, "entry_windows_x86_64"))
        {
            ParseNativeEntry(state, value, &state->windowsEntrySeen, entry->entryWindowsX86_64);
        }
        else if (SliceEqualsAscii(key, "entry_linux_x86_64_gnu"))
        {
            ParseNativeEntry(state, value, &state->linuxGnuEntrySeen, entry->entryLinuxX86_64Gnu);
        }
        else if (SliceEqualsAscii(key, "entry_linux_x86_64_musl"))
        {
            ParseNativeEntry(state, value, &state->linuxMuslEntrySeen, entry->entryLinuxX86_64Musl);
        }
        else if (SliceEqualsAscii(key, "api"))
        {
            uint32_t api;
            if (SliceParseUint32(value, &api) && api > 0)
            {
                entry->requiredApi = api;
            }
        }
    }
}

// game = MAJOR.MINOR: совместим, если мажор совпадает, а минор не новее
// текущего (0.x трактуется строго, как и положено до 1.0).
static bool GameVersionCompatible(const wchar_t *required)
{
    if (required[0] == L'\0')
    {
        return true;
    }

    int32_t major = 0;
    int32_t minor = 0;
    uint32_t index = 0;
    bool anyDigit = false;
    while (required[index] >= L'0' && required[index] <= L'9')
    {
        major = major * 10 + (int32_t)(required[index] - L'0');
        anyDigit = true;
        ++index;
    }
    if (!anyDigit)
    {
        return false;
    }
    if (required[index] == L'.')
    {
        ++index;
        while (required[index] >= L'0' && required[index] <= L'9')
        {
            minor = minor * 10 + (int32_t)(required[index] - L'0');
            ++index;
        }
    }

    return major == LAIUE_VERSION_MAJOR && minor <= LAIUE_VERSION_MINOR;
}

// === Состояние ===

void ModsInit(ModsState *mods, const wchar_t *enabledFileName)
{
    memset(mods, 0, sizeof(*mods));
    const wchar_t *source =
        enabledFileName != NULL && enabledFileName[0] != L'\0' ? enabledFileName : L"enabled.txt";
    uint32_t i = 0;
    while (source[i] != L'\0' && i + 1 < MODS_NAME_CAPACITY)
    {
        mods->enabledFileName[i] = source[i];
        ++i;
    }
    mods->enabledFileName[i] = L'\0';
}

static ModEntry *FindEntry(ModsState *mods, const wchar_t *fileName)
{
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        const wchar_t *a = mods->entries[i].fileName;
        const wchar_t *b = fileName;
        uint32_t index = 0;
        while (a[index] != L'\0' && a[index] == b[index])
            ++index;
        if (a[index] == b[index])
        {
            return &mods->entries[i];
        }
    }
    return NULL;
}

// Путь к манифесту mod.lm внутри каталога пака.
static bool BuildManifestPath(const ModEntry *entry, wchar_t *path, uint32_t capacity)
{
    return LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, entry->fileName, MOD_PACK_MANIFEST_NAME,
                                 path, capacity);
}

static bool ManifestNativeEntriesValid(const ManifestParseState *state, uint32_t formatVersion)
{
    if (!state->valid || (state->legacyEntrySeen && state->windowsEntrySeen))
    {
        return false;
    }

    if (formatVersion == 1u)
    {
        return state->legacyEntrySeen && !state->windowsEntrySeen && !state->linuxGnuEntrySeen &&
               !state->linuxMuslEntrySeen;
    }

    return formatVersion == 2u && (state->legacyEntrySeen || state->windowsEntrySeen ||
                                   state->linuxGnuEntrySeen || state->linuxMuslEntrySeen);
}

static void CopyNativeEntry(wchar_t destination[MODS_NAME_CAPACITY],
                            const wchar_t source[MODS_NAME_CAPACITY])
{
    uint32_t index = 0;
    while (source[index] != L'\0' && index + 1 < MODS_NAME_CAPACITY)
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = L'\0';
}

static bool SelectCurrentPlatformEntry(ModEntry *entry)
{
    const wchar_t *selected = NULL;
#if defined(_WIN32)
    selected = entry->entryWindowsX86_64;
#elif defined(__linux__) && defined(LAIUE_LINUX_LIBC_MUSL)
    selected = entry->entryLinuxX86_64Musl;
#elif defined(__linux__)
    selected = entry->entryLinuxX86_64Gnu;
#endif
    if (selected == NULL || selected[0] == L'\0')
    {
        entry->entryDll[0] = L'\0';
        return false;
    }
    CopyNativeEntry(entry->entryDll, selected);
    return true;
}

static bool HashDeclaredArtifact(const ModEntry *entry, const wchar_t *artifactName, wchar_t *path,
                                 uint8_t aggregate[MODS_CONTENT_HASH_SIZE])
{
    if (artifactName[0] == L'\0')
    {
        return true;
    }

    uint8_t artifactHash[MODS_CONTENT_HASH_SIZE];
    uint8_t combined[MODS_CONTENT_HASH_SIZE];
    if (!LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, entry->fileName, artifactName, path,
                               LAIUE_CONTENT_PATH_CAPACITY) ||
        !HashFile(path, artifactHash) || !CombineHashes(aggregate, artifactHash, combined))
    {
        return false;
    }
    memcpy(aggregate, combined, MODS_CONTENT_HASH_SIZE);
    return true;
}

static bool HashDeclaredArtifacts(const ModEntry *entry, wchar_t *path,
                                  const uint8_t manifestHash[MODS_CONTENT_HASH_SIZE],
                                  uint8_t output[MODS_CONTENT_HASH_SIZE])
{
    memcpy(output, manifestHash, MODS_CONTENT_HASH_SIZE);
    // Порядок является частью wire-совместимости и не зависит от
    // платформы, на которой каталог сканируется.
    return HashDeclaredArtifact(entry, entry->entryWindowsX86_64, path, output) &&
           HashDeclaredArtifact(entry, entry->entryLinuxX86_64Gnu, path, output) &&
           HashDeclaredArtifact(entry, entry->entryLinuxX86_64Musl, path, output);
}

// Пересчёт включённых от файла enabled.txt (источник истины порядка).
static void RecomputeEnabled(ModsState *mods)
{
    mods->enabledCount = 0;
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        ModEntry *entry = &mods->entries[i];
        entry->enabled = false;
        entry->runtimeStatus = entry->compatible ? MOD_RUNTIME_DISABLED : MOD_RUNTIME_INCOMPATIBLE;
        entry->initResult = 0;
    }

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
    {
        mods->revision++;
        return;
    }

    uint8_t *bytes = NULL;
    uint32_t length = 0;
    if (BuildEnabledPath(mods, path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        bytes = ReadWholeFile(path, &length);
    }

    if (bytes != NULL)
    {
        uint32_t offset = 0;
        while (offset < length)
        {
            uint32_t lineEnd = offset;
            while (lineEnd < length && bytes[lineEnd] != '\n')
                ++lineEnd;
            ByteSlice line = SliceTrim((ByteSlice){bytes + offset, lineEnd - offset});
            offset = lineEnd + 1;
            if (line.length == 0 || line.data[0] == '#')
            {
                continue;
            }

            wchar_t fileName[MODS_NAME_CAPACITY];
            SliceToWide(line, fileName, MODS_NAME_CAPACITY);

            ModEntry *entry = FindEntry(mods, fileName);
            if (entry == NULL || !entry->compatible || entry->enabled)
            {
                continue; // удалённый, несовместимый или повтор строки
            }
            entry->enabled = true;
            // Оптимистично LOADED: факт уточнит ближайший ModHostSync.
            entry->runtimeStatus = MOD_RUNTIME_LOADED;
            mods->enabledOrder[mods->enabledCount++] = (uint32_t)(entry - mods->entries);
        }
        PlatformFree(bytes);
    }

    PlatformFree(path);
    mods->revision++;
}

// Собирает список DLL-модов: каталоги .lmp с манифестом mod.lm.
static void CollectEntries(ModsState *mods)
{
    LaiueContentList list;
    if (!LaiueContentEnumerate(LAIUE_CONTENT_MOD_PACK, &list))
    {
        return;
    }

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);

    for (uint32_t i = 0; i < list.count && mods->count < MODS_MAX_ENTRIES; ++i)
    {
        ModEntry *entry = &mods->entries[mods->count];
        memset(entry, 0, sizeof(*entry));
        entry->side = MOD_SIDE_BOTH;
        entry->sideValid = true;

        // Имя каталога (уже проверено каталогом на безопасность).
        uint32_t index = 0;
        while (list.entries[i].name[index] != L'\0' && index + 1 < MODS_NAME_CAPACITY)
        {
            entry->fileName[index] = list.entries[i].name[index];
            ++index;
        }
        entry->fileName[index] = L'\0';

        // Манифест: метаданные и платформенные [native] entries.
        bool parsed = false;
        bool manifestHashed = false;
        ManifestParseState manifestState;
        memset(&manifestState, 0, sizeof(manifestState));
        manifestState.entry = entry;
        manifestState.valid = true;
        uint8_t manifestHash[MODS_CONTENT_HASH_SIZE];
        if (path != NULL && BuildManifestPath(entry, path, LAIUE_CONTENT_PATH_CAPACITY))
        {
            uint32_t length = 0;
            uint8_t *bytes = ReadWholeFile(path, &length);
            if (bytes != NULL)
            {
                parsed = ParseModFile(bytes, length, ManifestCallback, &manifestState,
                                      &entry->manifestVersion);
                manifestHashed = HashBuffer(bytes, length, manifestHash);
                PlatformFree(bytes);
            }
        }
        if (!parsed || !ManifestNativeEntriesValid(&manifestState, entry->manifestVersion))
        {
            continue; // не мод формата LAIUE MOD — пропускаем
        }

        if (entry->displayName[0] == L'\0')
        {
            // Без name = показываем имя файла.
            for (index = 0; entry->fileName[index] != L'\0' && index + 1 < MODS_NAME_CAPACITY;
                 ++index)
            {
                entry->displayName[index] = entry->fileName[index];
            }
            entry->displayName[index] = L'\0';
        }

        bool platformEntrySelected = SelectCurrentPlatformEntry(entry);
        // Строгий v2 pack обязан содержать каждый объявленный artifact.
        // Это даёт один fingerprint полного pack на Windows/glibc/musl.
        bool artifactsHashed = path != NULL && manifestHashed &&
                               HashDeclaredArtifacts(entry, path, manifestHash, entry->contentHash);

        // Совместимость: версия игры, стабильный id, native artifact
        // текущей платформы, полный hash и версия API SDK.
        entry->compatible = GameVersionCompatible(entry->requiredGame) && entry->sideValid &&
                            entry->id[0] != '\0' && entry->version[0] != L'\0' &&
                            platformEntrySelected && artifactsHashed && entry->requiredApi >= 1u &&
                            entry->requiredApi <= LAIUE_MOD_API_VERSION;
        mods->count++;
    }

    if (path != NULL)
    {
        PlatformFree(path);
    }
    LaiueContentListRelease(&list);
}

void ModsRefresh(ModsState *mods)
{
    mods->count = 0;
    CollectEntries(mods);
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        for (uint32_t j = i + 1; j < mods->count; ++j)
        {
            uint32_t c = 0;
            while (mods->entries[i].id[c] != '\0' &&
                   mods->entries[i].id[c] == mods->entries[j].id[c])
                ++c;
            if (mods->entries[i].id[c] == mods->entries[j].id[c])
            {
                mods->entries[i].compatible = false;
                mods->entries[j].compatible = false;
            }
        }
    }
    RecomputeEnabled(mods);
}

static bool WriteEnabledFile(const ModsState *mods, const wchar_t *toggleName, bool enable)
{
    // Новый порядок: старые включённые (кроме выключаемого) в прежнем
    // порядке — их даёт текущее состояние entries + порядок enabled.txt,
    // который уже отражён флагами. Для простоты и стабильности порядок
    // берём как: все ныне включённые в порядке каталога... нет — порядок
    // важен. Поэтому: читаем enabled.txt, фильтруем, дописываем.
    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL || !BuildEnabledPath(mods, path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        if (path != NULL)
            PlatformFree(path);
        return false;
    }

    // Собираем новый файл в UTF-8 буфере.
    uint32_t capacity = MODS_MAX_ENTRIES * (MODS_NAME_UTF8_CAPACITY + 1U) + 64U;
    uint8_t *output = PlatformAllocate(capacity, false);
    if (output == NULL)
    {
        PlatformFree(path);
        return false;
    }
    uint32_t outputLength = 0;
    bool outputValid = true;

    uint32_t existingLength = 0;
    uint8_t *existing = ReadWholeFile(path, &existingLength);
    if (existing != NULL)
    {
        uint32_t offset = 0;
        while (offset < existingLength)
        {
            uint32_t lineEnd = offset;
            while (lineEnd < existingLength && existing[lineEnd] != '\n')
            {
                ++lineEnd;
            }
            ByteSlice line = SliceTrim((ByteSlice){existing + offset, lineEnd - offset});
            offset = lineEnd + 1;
            if (line.length == 0)
            {
                continue;
            }

            wchar_t lineName[MODS_NAME_CAPACITY];
            SliceToWide(line, lineName, MODS_NAME_CAPACITY);

            // Выключаемый мод выбрасывается; включаемый не дублируется.
            uint32_t ci = 0;
            while (toggleName[ci] != L'\0' && toggleName[ci] == lineName[ci])
            {
                ++ci;
            }
            if (toggleName[ci] == lineName[ci])
            {
                continue;
            }

            if (outputLength + line.length + 1 <= capacity)
            {
                memcpy(output + outputLength, line.data, line.length);
                outputLength += line.length;
                output[outputLength++] = '\n';
            }
            else
            {
                outputValid = false;
                break;
            }
        }
        PlatformFree(existing);
    }

    if (enable)
    {
        // Включение — в конец порядка (последний побеждает в конфликтах).
        char utf8[MODS_NAME_UTF8_CAPACITY];
        uint32_t written = 0;
        if (PlatformWideToUtf8(toggleName, utf8, sizeof(utf8), &written) && written > 0 &&
            outputLength + written + 1U <= capacity)
        {
            memcpy(output + outputLength, utf8, written);
            outputLength += written;
            output[outputLength++] = '\n';
        }
        else
        {
            outputValid = false;
        }
    }

    bool succeeded = outputValid && WriteFileAtomically(path, output, outputLength);

    PlatformFree(output);
    PlatformFree(path);
    return succeeded;
}

bool ModsSetEnabled(ModsState *mods, uint32_t index, bool enabled)
{
    if (index >= mods->count)
    {
        return false;
    }
    ModEntry *entry = &mods->entries[index];
    if (!entry->compatible && enabled)
    {
        return false;
    }
    if (entry->enabled == enabled)
    {
        return true;
    }

    if (!WriteEnabledFile(mods, entry->fileName, enabled))
    {
        return false;
    }

    RecomputeEnabled(mods);
    return true;
}

static bool CopyVersionToUtf8(const wchar_t *source, char *destination)
{
    uint32_t written = 0;
    return PlatformWideToUtf8(source, destination, MODS_VERSION_CAPACITY, &written) && written > 0;
}

bool ModsBuildCompatibilitySet(const ModsState *mods, ModCompatibilityEntry *output,
                               uint32_t capacity, uint32_t *outCount)
{
    if (mods == NULL || outCount == NULL)
    {
        return false;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < mods->enabledCount; ++i)
    {
        const ModEntry *entry = &mods->entries[mods->enabledOrder[i]];
        if (entry->enabled && entry->compatible && entry->side != MOD_SIDE_CLIENT)
        {
            ++count;
        }
    }
    *outCount = count;
    if (count > capacity || (count != 0 && output == NULL))
    {
        return false;
    }

    uint32_t write = 0;
    for (uint32_t i = 0; i < mods->enabledCount; ++i)
    {
        const ModEntry *entry = &mods->entries[mods->enabledOrder[i]];
        if (!entry->enabled || !entry->compatible || entry->side == MOD_SIDE_CLIENT)
        {
            continue;
        }
        ModCompatibilityEntry *item = &output[write++];
        memset(item, 0, sizeof(*item));
        uint32_t c = 0;
        while (entry->id[c] != '\0' && c + 1 < MODS_ID_CAPACITY)
        {
            item->id[c] = entry->id[c];
            ++c;
        }
        if (!CopyVersionToUtf8(entry->version, item->version))
        {
            return false;
        }
        memcpy(item->contentHash, entry->contentHash, MODS_CONTENT_HASH_SIZE);
    }

    return true;
}

static bool CompatibilityMatchesEntry(const ModCompatibilityEntry *required, const ModEntry *entry)
{
    uint32_t i = 0;
    while (required->id[i] != '\0' && required->id[i] == entry->id[i])
        ++i;
    if (required->id[i] != entry->id[i] || !HashesEqual(required->contentHash, entry->contentHash))
    {
        return false;
    }
    char version[MODS_VERSION_CAPACITY];
    if (!CopyVersionToUtf8(entry->version, version))
        return false;
    i = 0;
    while (required->version[i] != '\0' && required->version[i] == version[i])
        ++i;
    return required->version[i] == version[i];
}

static const ModEntry *FindCompatibilityEntry(const ModsState *mods,
                                              const ModCompatibilityEntry *required)
{
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        const ModEntry *entry = &mods->entries[i];
        if (entry->compatible && entry->side != MOD_SIDE_CLIENT &&
            CompatibilityMatchesEntry(required, entry))
        {
            return entry;
        }
    }
    return NULL;
}

bool ModsCanApplyServerCompatibilitySet(const ModsState *mods,
                                        const ModCompatibilityEntry *required, uint32_t count)
{
    if (mods == NULL || count > MODS_MAX_ENTRIES || (count != 0 && required == NULL))
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (FindCompatibilityEntry(mods, &required[i]) == NULL)
        {
            return false;
        }
    }
    return true;
}

static bool AppendEnabledName(uint8_t *output, uint32_t capacity, uint32_t *length,
                              const wchar_t *name)
{
    char utf8[MODS_NAME_UTF8_CAPACITY];
    uint32_t written = 0;
    if (!PlatformWideToUtf8(name, utf8, sizeof(utf8), &written) || written == 0 ||
        *length + written + 1U > capacity)
    {
        return false;
    }
    memcpy(output + *length, utf8, written);
    *length += written;
    output[(*length)++] = '\n';
    return true;
}

bool ModsApplyServerCompatibilitySet(ModsState *mods, const ModCompatibilityEntry *required,
                                     uint32_t count)
{
    if (!ModsCanApplyServerCompatibilitySet(mods, required, count))
    {
        return false;
    }
    const ModEntry *resolved[MODS_MAX_ENTRIES];
    for (uint32_t i = 0; i < count; ++i)
    {
        resolved[i] = FindCompatibilityEntry(mods, &required[i]);
        if (resolved[i] == NULL)
            return false;
    }

    uint32_t capacity = MODS_MAX_ENTRIES * (MODS_NAME_UTF8_CAPACITY + 1U);
    uint8_t *output = PlatformAllocate(capacity, false);
    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (output == NULL || path == NULL ||
        !BuildEnabledPath(mods, path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        if (output != NULL)
            PlatformFree(output);
        if (path != NULL)
            PlatformFree(path);
        return false;
    }

    uint32_t length = 0;
    bool succeeded = true;
    // Client-only состав остаётся пользовательским и не зависит от сервера.
    for (uint32_t i = 0; i < mods->enabledCount && succeeded; ++i)
    {
        ModEntry *entry = &mods->entries[mods->enabledOrder[i]];
        if (entry->enabled && entry->side == MOD_SIDE_CLIENT)
        {
            succeeded = AppendEnabledName(output, capacity, &length, entry->fileName);
        }
    }
    for (uint32_t i = 0; i < count && succeeded; ++i)
    {
        succeeded = AppendEnabledName(output, capacity, &length, resolved[i]->fileName);
    }

    if (succeeded)
        succeeded = WriteFileAtomically(path, output, length);
    PlatformFree(path);
    PlatformFree(output);
    if (succeeded)
        RecomputeEnabled(mods);
    return succeeded;
}
