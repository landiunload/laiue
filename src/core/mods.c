#include "core/mods.h"
#include "content/content_catalog.h"

#include "laiue_mod_api.h"

#include <windows.h>
#include <string.h>

#define MOD_PACK_MANIFEST_NAME L"mod.lm"

#define MOD_FILE_MAX_BYTES (64u * 1024u)
#define MOD_FORMAT_VERSION 1
#define ENABLED_FILE_NAME L"enabled.txt"

// === Байтовые утилиты парсера (файлы модов — построчный UTF-8) ===

typedef struct ByteSlice
{
    const uint8_t* data;
    uint32_t length;
} ByteSlice;

static bool SliceEqualsAscii(ByteSlice slice, const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0') ++length;
    if (slice.length != length)
    {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i)
    {
        uint8_t a = slice.data[i];
        if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + ('a' - 'A'));
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
    while (slice.length > 0 && (slice.data[0] == ' ' || slice.data[0] == '\t'
        || slice.data[0] == '\r'))
    {
        ++slice.data;
        --slice.length;
    }
    while (slice.length > 0 && (slice.data[slice.length - 1] == ' '
        || slice.data[slice.length - 1] == '\t'
        || slice.data[slice.length - 1] == '\r'))
    {
        --slice.length;
    }
    return slice;
}

static bool SliceParseFloat(ByteSlice slice, float* outValue)
{
    if (slice.length == 0)
    {
        return false;
    }

    uint32_t index = 0;
    float sign = 1.0f;
    if (slice.data[0] == '-' || slice.data[0] == '+')
    {
        sign = slice.data[0] == '-' ? -1.0f : 1.0f;
        index = 1;
    }

    bool anyDigit = false;
    float value = 0.0f;
    while (index < slice.length
        && slice.data[index] >= '0' && slice.data[index] <= '9')
    {
        value = value * 10.0f + (float)(slice.data[index] - '0');
        anyDigit = true;
        ++index;
    }
    if (index < slice.length && slice.data[index] == '.')
    {
        ++index;
        float scale = 0.1f;
        while (index < slice.length
            && slice.data[index] >= '0' && slice.data[index] <= '9')
        {
            value += (float)(slice.data[index] - '0') * scale;
            scale *= 0.1f;
            anyDigit = true;
            ++index;
        }
    }

    if (!anyDigit || index != slice.length)
    {
        return false;
    }
    *outValue = sign * value;
    return true;
}

static bool SliceParseInt(ByteSlice slice, int32_t* outValue)
{
    float value;
    if (!SliceParseFloat(slice, &value))
    {
        return false;
    }
    *outValue = (int32_t)(value + (value >= 0.0f ? 0.5f : -0.5f));
    return true;
}

static void SliceToWide(ByteSlice slice, wchar_t* destination,
    uint32_t capacity)
{
    destination[0] = L'\0';
    if (slice.length == 0 || capacity < 2)
    {
        return;
    }
    int written = MultiByteToWideChar(CP_UTF8, 0,
        (const char*)slice.data, (int)slice.length,
        destination, (int)capacity - 1);
    if (written < 0)
    {
        written = 0;
    }
    destination[written] = L'\0';
}

// === Файловый ввод-вывод ===

static uint8_t* ReadWholeFile(const wchar_t* path, uint32_t* outLength)
{
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > MOD_FILE_MAX_BYTES)
    {
        CloseHandle(file);
        return NULL;
    }

    uint32_t length = (uint32_t)size.QuadPart;
    uint8_t* bytes = HeapAlloc(GetProcessHeap(), 0, length);
    if (bytes == NULL)
    {
        CloseHandle(file);
        return NULL;
    }

    uint32_t completed = 0;
    while (completed < length)
    {
        DWORD read = 0;
        if (!ReadFile(file, bytes + completed, length - completed, &read, NULL)
            || read == 0)
        {
            HeapFree(GetProcessHeap(), 0, bytes);
            CloseHandle(file);
            return NULL;
        }
        completed += read;
    }
    CloseHandle(file);

    *outLength = length;
    return bytes;
}

static bool BuildEnabledPath(wchar_t* destination, uint32_t capacity)
{
    if (!LaiueContentBuildPath(LAIUE_CONTENT_MOD, NULL, NULL,
            destination, capacity))
    {
        return false;
    }
    uint32_t length = 0;
    while (destination[length] != L'\0') ++length;
    const wchar_t* suffix = L"\\" ENABLED_FILE_NAME;
    uint32_t suffixLength = 0;
    while (suffix[suffixLength] != L'\0') ++suffixLength;
    if (length + suffixLength + 1 > capacity)
    {
        return false;
    }
    for (uint32_t i = 0; i <= suffixLength; ++i)
    {
        destination[length + i] = suffix[i];
    }
    return true;
}

// === Разбор файла мода ===

// Колбек получает секцию (пустая до первого заголовка), ключ и значение.
typedef void (*ModPairCallback)(void* context,
    ByteSlice section, ByteSlice key, ByteSlice value);

// Разбирает файл: первая значимая строка обязана быть заголовком
// `LAIUE MOD <версия>` с версией не новее поддерживаемой.
static bool ParseModFile(const uint8_t* bytes, uint32_t length,
    ModPairCallback callback, void* context)
{
    ByteSlice section = { NULL, 0 };
    bool headerSeen = false;

    uint32_t offset = 0;
    if (length >= 3 && bytes[0] == 0xEFu && bytes[1] == 0xBBu
        && bytes[2] == 0xBFu)
    {
        offset = 3; // BOM
    }

    while (offset < length)
    {
        uint32_t lineEnd = offset;
        while (lineEnd < length && bytes[lineEnd] != '\n') ++lineEnd;

        ByteSlice line = SliceTrim(
            (ByteSlice){ bytes + offset, lineEnd - offset });
        offset = lineEnd + 1;

        if (line.length == 0 || line.data[0] == '#')
        {
            continue;
        }

        if (!headerSeen)
        {
            // "LAIUE MOD <n>"
            if (line.length < 11
                || !SliceEqualsAscii((ByteSlice){ line.data, 9 }, "laiue mod"))
            {
                return false;
            }
            int32_t version;
            if (!SliceParseInt(SliceTrim(
                    (ByteSlice){ line.data + 9, line.length - 9 }), &version)
                || version < 1 || version > MOD_FORMAT_VERSION)
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
                section = SliceTrim(
                    (ByteSlice){ line.data + 1, line.length - 2 });
            }
            continue;
        }

        uint32_t equals = 0;
        while (equals < line.length && line.data[equals] != '=') ++equals;
        if (equals == 0 || equals == line.length)
        {
            continue;
        }

        ByteSlice key = SliceTrim((ByteSlice){ line.data, equals });
        ByteSlice value = SliceTrim((ByteSlice){
            line.data + equals + 1, line.length - equals - 1 });
        if (key.length > 0)
        {
            callback(context, section, key, value);
        }
    }

    return headerSeen;
}

// === Потребитель 1: манифест (имя, версия, требуемая игра) ===

static void ManifestCallback(void* context,
    ByteSlice section, ByteSlice key, ByteSlice value)
{
    ModEntry* entry = context;
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
        return;
    }

    // DLL-мод: имя точки входа и требуемая версия API SDK.
    if (SliceEqualsAscii(section, "native"))
    {
        if (SliceEqualsAscii(key, "entry"))
        {
            SliceToWide(value, entry->entryDll, MODS_NAME_CAPACITY);
        }
        else if (SliceEqualsAscii(key, "api"))
        {
            int32_t api;
            if (SliceParseInt(value, &api) && api > 0)
            {
                entry->requiredApi = (uint32_t)api;
            }
        }
    }
}

// game = MAJOR.MINOR: совместим, если мажор совпадает, а минор не новее
// текущего (0.x трактуется строго, как и положено до 1.0).
static bool GameVersionCompatible(const wchar_t* required)
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

void ModsInit(ModsState* mods)
{
    memset(mods, 0, sizeof(*mods));
}

static ModEntry* FindEntry(ModsState* mods, const wchar_t* fileName)
{
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        const wchar_t* a = mods->entries[i].fileName;
        const wchar_t* b = fileName;
        uint32_t index = 0;
        while (a[index] != L'\0' && a[index] == b[index]) ++index;
        if (a[index] == b[index])
        {
            return &mods->entries[i];
        }
    }
    return NULL;
}

// Путь к манифесту mod.lm внутри каталога пака.
static bool BuildManifestPath(const ModEntry* entry,
    wchar_t* path, uint32_t capacity)
{
    return LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, entry->fileName,
        MOD_PACK_MANIFEST_NAME, path, capacity);
}

// Пересчёт включённых от файла enabled.txt (источник истины порядка).
static void RecomputeEnabled(ModsState* mods)
{
    mods->enabledCount = 0;
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        ModEntry* entry = &mods->entries[i];
        entry->enabled = false;
        entry->runtimeStatus = entry->compatible
            ? MOD_RUNTIME_DISABLED : MOD_RUNTIME_INCOMPATIBLE;
        entry->initResult = 0;
    }

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL)
    {
        mods->revision++;
        return;
    }

    uint8_t* bytes = NULL;
    uint32_t length = 0;
    if (BuildEnabledPath(path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        bytes = ReadWholeFile(path, &length);
    }

    if (bytes != NULL)
    {
        uint32_t offset = 0;
        while (offset < length)
        {
            uint32_t lineEnd = offset;
            while (lineEnd < length && bytes[lineEnd] != '\n') ++lineEnd;
            ByteSlice line = SliceTrim(
                (ByteSlice){ bytes + offset, lineEnd - offset });
            offset = lineEnd + 1;
            if (line.length == 0 || line.data[0] == '#')
            {
                continue;
            }

            wchar_t fileName[MODS_NAME_CAPACITY];
            SliceToWide(line, fileName, MODS_NAME_CAPACITY);

            ModEntry* entry = FindEntry(mods, fileName);
            if (entry == NULL || !entry->compatible || entry->enabled)
            {
                continue; // удалённый, несовместимый или повтор строки
            }
            entry->enabled = true;
            // Оптимистично LOADED: факт уточнит ближайший ModHostSync.
            entry->runtimeStatus = MOD_RUNTIME_LOADED;
            mods->enabledOrder[mods->enabledCount++] =
                (uint32_t)(entry - mods->entries);
        }
        HeapFree(GetProcessHeap(), 0, bytes);
    }

    HeapFree(GetProcessHeap(), 0, path);
    mods->revision++;
}

// Собирает список DLL-модов: каталоги .lmp с манифестом mod.lm.
static void CollectEntries(ModsState* mods)
{
    LaiueContentList list;
    if (!LaiueContentEnumerate(LAIUE_CONTENT_MOD_PACK, &list))
    {
        return;
    }

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));

    for (uint32_t i = 0; i < list.count
        && mods->count < MODS_MAX_ENTRIES; ++i)
    {
        ModEntry* entry = &mods->entries[mods->count];
        memset(entry, 0, sizeof(*entry));

        // Имя каталога (уже проверено каталогом на безопасность).
        uint32_t index = 0;
        while (list.entries[i].name[index] != L'\0'
            && index + 1 < MODS_NAME_CAPACITY)
        {
            entry->fileName[index] = list.entries[i].name[index];
            ++index;
        }
        entry->fileName[index] = L'\0';

        // Манифест: имя, версия, требуемая игра, [native] entry/api.
        bool parsed = false;
        if (path != NULL
            && BuildManifestPath(entry, path, LAIUE_CONTENT_PATH_CAPACITY))
        {
            uint32_t length = 0;
            uint8_t* bytes = ReadWholeFile(path, &length);
            if (bytes != NULL)
            {
                parsed = ParseModFile(bytes, length,
                    ManifestCallback, entry);
                HeapFree(GetProcessHeap(), 0, bytes);
            }
        }
        if (!parsed)
        {
            continue; // не мод формата LAIUE MOD — пропускаем
        }

        if (entry->displayName[0] == L'\0')
        {
            // Без name = показываем имя файла.
            for (index = 0; entry->fileName[index] != L'\0'
                && index + 1 < MODS_NAME_CAPACITY; ++index)
            {
                entry->displayName[index] = entry->fileName[index];
            }
            entry->displayName[index] = L'\0';
        }

        // Совместимость: версия игры, точка входа и версия API SDK.
        entry->compatible = GameVersionCompatible(entry->requiredGame)
            && entry->entryDll[0] != L'\0'
            && entry->requiredApi >= 1u
            && entry->requiredApi <= LAIUE_MOD_API_VERSION;
        mods->count++;
    }

    if (path != NULL)
    {
        HeapFree(GetProcessHeap(), 0, path);
    }
    LaiueContentListRelease(&list);
}

void ModsRefresh(ModsState* mods)
{
    mods->count = 0;
    CollectEntries(mods);
    RecomputeEnabled(mods);
}

static bool WriteEnabledFile(const wchar_t* toggleName, bool enable)
{
    // Новый порядок: старые включённые (кроме выключаемого) в прежнем
    // порядке — их даёт текущее состояние entries + порядок enabled.txt,
    // который уже отражён флагами. Для простоты и стабильности порядок
    // берём как: все ныне включённые в порядке каталога... нет — порядок
    // важен. Поэтому: читаем enabled.txt, фильтруем, дописываем.
    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL || !BuildEnabledPath(path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        if (path != NULL) HeapFree(GetProcessHeap(), 0, path);
        return false;
    }

    // Собираем новый файл в UTF-8 буфере.
    uint32_t capacity = MODS_MAX_ENTRIES * (MODS_NAME_CAPACITY * 3 + 2) + 64;
    uint8_t* output = HeapAlloc(GetProcessHeap(), 0, capacity);
    if (output == NULL)
    {
        HeapFree(GetProcessHeap(), 0, path);
        return false;
    }
    uint32_t outputLength = 0;

    uint32_t existingLength = 0;
    uint8_t* existing = ReadWholeFile(path, &existingLength);
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
            ByteSlice line = SliceTrim(
                (ByteSlice){ existing + offset, lineEnd - offset });
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
        }
        HeapFree(GetProcessHeap(), 0, existing);
    }

    if (enable)
    {
        // Включение — в конец порядка (последний побеждает в конфликтах).
        char utf8[MODS_NAME_CAPACITY * 3];
        int written = WideCharToMultiByte(CP_UTF8, 0, toggleName, -1,
            utf8, (int)sizeof(utf8), NULL, NULL);
        if (written > 1 && outputLength + (uint32_t)written + 1 <= capacity)
        {
            memcpy(output + outputLength, utf8, (size_t)written - 1);
            outputLength += (uint32_t)written - 1;
            output[outputLength++] = '\n';
        }
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool succeeded = file != INVALID_HANDLE_VALUE;
    if (succeeded)
    {
        DWORD writtenBytes = 0;
        succeeded = WriteFile(file, output, outputLength, &writtenBytes, NULL)
            && writtenBytes == outputLength;
        CloseHandle(file);
    }

    HeapFree(GetProcessHeap(), 0, output);
    HeapFree(GetProcessHeap(), 0, path);
    return succeeded;
}

bool ModsSetEnabled(ModsState* mods, uint32_t index, bool enabled)
{
    if (index >= mods->count)
    {
        return false;
    }
    ModEntry* entry = &mods->entries[index];
    if (!entry->compatible && enabled)
    {
        return false;
    }
    if (entry->enabled == enabled)
    {
        return true;
    }

    if (!WriteEnabledFile(entry->fileName, enabled))
    {
        return false;
    }

    RecomputeEnabled(mods);
    return true;
}
