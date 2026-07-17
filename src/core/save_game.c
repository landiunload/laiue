#include "core/save_game.h"
#include "core/ui_format.h"

#include <windows.h>
#include <string.h>

#define SAVE_DIRECTORY_SUFFIX L"\\saves"

#define PLAYER_SAVE_MAGIC   0x3150574Cu  // байты 'L' 'W' 'P' '1'
#define PLAYER_SAVE_VERSION 1u
#define INVENTORY_SAVE_MAGIC   0x3156494Cu  // байты 'L' 'I' 'V' '1'
#define INVENTORY_SAVE_VERSION 1u

#define SAVE_TEXT_CAPACITY 4096u

// Стек без CRT не имеет проб роста (__chkstk): крупные текстовые буферы
// живут на куче одной арендой на операцию.
typedef struct SaveScratch
{
    wchar_t path[SAVE_GAME_PATH_CAPACITY];
    char text[SAVE_TEXT_CAPACITY];
    char extra[SAVE_TEXT_CAPACITY];
} SaveScratch;

static wchar_t g_saveSlotName[SAVE_GAME_SLOT_NAME_CAPACITY] = L"default";

static SaveScratch* SaveScratchAcquire(void)
{
    return HeapAlloc(GetProcessHeap(), 0, sizeof(SaveScratch));
}

static void SaveScratchRelease(SaveScratch* scratch)
{
    if (scratch != NULL)
    {
        HeapFree(GetProcessHeap(), 0, scratch);
    }
}

// memcmp не входит в laiue_runtime — своё побайтовое сравнение.
static bool BytesEqual(const void* a, const void* b, uint32_t count)
{
    const uint8_t* left = a;
    const uint8_t* right = b;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

// === Пути ===

static bool BuildSavePath(const wchar_t* suffix,
    wchar_t* destination, uint32_t capacity)
{
    DWORD length = GetModuleFileNameW(NULL, destination, capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }

    uint32_t directoryLength = 0;
    for (uint32_t i = (uint32_t)length; i > 0; --i)
    {
        wchar_t character = destination[i - 1u];
        if (character == L'\\' || character == L'/')
        {
            directoryLength = i - 1u;
            break;
        }
    }

    uint32_t suffixLength = 0;
    while (suffix[suffixLength] != L'\0') ++suffixLength;
    if (directoryLength + suffixLength + 1u > capacity)
    {
        return false;
    }
    for (uint32_t i = 0; i <= suffixLength; ++i)
    {
        destination[directoryLength + i] = suffix[i];
    }
    return true;
}

static bool BuildSaveFilePath(const wchar_t* fileName,
    wchar_t* destination, uint32_t capacity)
{
    if (!BuildSavePath(SAVE_DIRECTORY_SUFFIX, destination, capacity))
    {
        return false;
    }
    uint32_t length = 0;
    while (destination[length] != L'\0') ++length;
    uint32_t slotLength = 0;
    while (g_saveSlotName[slotLength] != L'\0') ++slotLength;
    uint32_t nameLength = 0;
    while (fileName[nameLength] != L'\0') ++nameLength;
    if (length + slotLength + nameLength + 3u > capacity)
    {
        return false;
    }
    destination[length] = L'\\';
    memcpy(destination + length + 1u, g_saveSlotName,
        (size_t)slotLength * sizeof(wchar_t));
    length += slotLength + 1u;
    destination[length] = L'\\';
    for (uint32_t i = 0; i <= nameLength; ++i)
    {
        destination[length + 1u + i] = fileName[i];
    }
    return true;
}

static bool BuildSlotDirectoryPath(bool modData,
    wchar_t* destination, uint32_t capacity)
{
    if (!BuildSavePath(SAVE_DIRECTORY_SUFFIX, destination, capacity)) return false;
    uint32_t length = 0;
    while (destination[length] != L'\0') ++length;
    uint32_t slotLength = 0;
    while (g_saveSlotName[slotLength] != L'\0') ++slotLength;
    uint32_t extra = modData ? 9u : 1u; // '\\' + "moddata" + NUL
    if (length + 1u + slotLength + extra > capacity) return false;
    destination[length++] = L'\\';
    memcpy(destination + length, g_saveSlotName,
        (size_t)slotLength * sizeof(wchar_t));
    length += slotLength;
    if (modData)
    {
        memcpy(destination + length, L"\\moddata", 9u * sizeof(wchar_t));
    }
    else
    {
        destination[length] = L'\0';
    }
    return true;
}

bool SaveGameSetSlot(const wchar_t* name)
{
    if (name == NULL || name[0] == L'\0') return false;
    uint32_t length = 0;
    for (; name[length] != L'\0'; ++length)
    {
        wchar_t c = name[length];
        if (length + 1 >= SAVE_GAME_SLOT_NAME_CAPACITY || c < 0x20
            || c == L'\\' || c == L'/' || c == L':' || c == L'*'
            || c == L'?' || c == L'"' || c == L'<' || c == L'>'
            || c == L'|') return false;
    }
    if ((length == 1 && name[0] == L'.')
        || (length == 2 && name[0] == L'.' && name[1] == L'.')) return false;
    memcpy(g_saveSlotName, name, (size_t)(length + 1u) * sizeof(wchar_t));
    return true;
}

const wchar_t* SaveGameGetSlot(void)
{
    return g_saveSlotName;
}

bool SaveGameEnumerateSlots(SaveGameSlotList* outList)
{
    if (outList == NULL) return false;
    memset(outList, 0, sizeof(*outList));
    wchar_t path[SAVE_GAME_PATH_CAPACITY];
    if (!BuildSavePath(SAVE_DIRECTORY_SUFFIX, path, SAVE_GAME_PATH_CAPACITY)) return false;
    uint32_t length = 0;
    while (path[length] != L'\0') ++length;
    if (length + 3u > SAVE_GAME_PATH_CAPACITY) return false;
    uint32_t savesLength = length;
    path[length++] = L'\\';
    path[length++] = L'*';
    path[length] = L'\0';
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(path, &data);
    if (search == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            || GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || data.cFileName[0] == L'.') continue;
        uint32_t directoryLength = 0;
        while (data.cFileName[directoryLength] != L'\0') ++directoryLength;
        static const wchar_t metaName[] = L"\\world.meta";
        if (savesLength + 1U + directoryLength
                + (uint32_t)(sizeof(metaName) / sizeof(metaName[0]))
                > SAVE_GAME_PATH_CAPACITY) continue;
        memcpy(path + savesLength + 1U, data.cFileName,
            (size_t)directoryLength * sizeof(wchar_t));
        memcpy(path + savesLength + 1U + directoryLength,
            metaName, sizeof(metaName));
        DWORD metaAttributes = GetFileAttributesW(path);
        path[savesLength + 1U] = L'*';
        path[savesLength + 2U] = L'\0';
        if (metaAttributes == INVALID_FILE_ATTRIBUTES
            || (metaAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (outList->count >= SAVE_GAME_MAX_SLOTS) break;
        SaveGameSlot* slot = &outList->entries[outList->count++];
        uint32_t i = 0;
        while (data.cFileName[i] != L'\0'
            && i + 1 < SAVE_GAME_SLOT_NAME_CAPACITY)
        {
            slot->name[i] = data.cFileName[i];
            ++i;
        }
        slot->name[i] = L'\0';
    }
    while (FindNextFileW(search, &data));
    FindClose(search);
    for (uint32_t i = 1; i < outList->count; ++i)
    {
        SaveGameSlot value = outList->entries[i];
        uint32_t insert = i;
        while (insert != 0)
        {
            const wchar_t* left = outList->entries[insert - 1U].name;
            const wchar_t* right = value.name;
            uint32_t c = 0;
            while (left[c] != L'\0' && right[c] != L'\0'
                && left[c] == right[c]) ++c;
            if (left[c] <= right[c]) break;
            outList->entries[insert] = outList->entries[insert - 1U];
            --insert;
        }
        outList->entries[insert] = value;
    }
    return true;
}

static bool SlotListContains(const SaveGameSlotList* list,
    const wchar_t* name)
{
    for (uint32_t i = 0; i < list->count; ++i)
    {
        uint32_t c = 0;
        while (list->entries[i].name[c] != L'\0'
            && list->entries[i].name[c] == name[c]) ++c;
        if (list->entries[i].name[c] == name[c]) return true;
    }
    return false;
}

bool SaveGameChooseNewSlot(wchar_t* destination, uint32_t capacity)
{
    if (destination == NULL || capacity < 8U) return false;
    SaveGameSlotList* slots = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*slots));
    if (slots == NULL) return false;
    bool succeeded = SaveGameEnumerateSlots(slots);
    if (succeeded)
    {
        for (uint32_t number = 1; number <= 9999U; ++number)
        {
            wchar_t candidate[SAVE_GAME_SLOT_NAME_CAPACITY];
            UiTextBuilder builder;
            UiTextBuilderInit(&builder, candidate,
                SAVE_GAME_SLOT_NAME_CAPACITY);
            UiTextBuilderAppend(&builder, L"world_");
            UiTextBuilderAppendUnsigned(&builder, number);
            if (builder.length > 0 && builder.length + 1U <= capacity
                && !SlotListContains(slots, candidate))
            {
                memcpy(destination, candidate,
                    ((size_t)builder.length + 1U) * sizeof(wchar_t));
                break;
            }
        }
        succeeded = destination[0] != L'\0';
    }
    HeapFree(GetProcessHeap(), 0, slots);
    return succeeded;
}

bool SaveGameEnsureDirectories(void)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL)
    {
        return false;
    }

    bool succeeded = true;
    succeeded = BuildSavePath(SAVE_DIRECTORY_SUFFIX,
            scratch->path, SAVE_GAME_PATH_CAPACITY)
        && (CreateDirectoryW(scratch->path, NULL)
            || GetLastError() == ERROR_ALREADY_EXISTS);
    if (succeeded)
    {
        succeeded = BuildSlotDirectoryPath(false,
                scratch->path, SAVE_GAME_PATH_CAPACITY)
            && (CreateDirectoryW(scratch->path, NULL)
                || GetLastError() == ERROR_ALREADY_EXISTS);
    }
    if (succeeded)
    {
        succeeded = BuildSlotDirectoryPath(true,
                scratch->path, SAVE_GAME_PATH_CAPACITY)
            && (CreateDirectoryW(scratch->path, NULL)
                || GetLastError() == ERROR_ALREADY_EXISTS);
    }

    SaveScratchRelease(scratch);
    return succeeded;
}

bool SaveGameModDataDirectory(wchar_t* destination, uint32_t capacity)
{
    return BuildSlotDirectoryPath(true, destination, capacity);
}

// === Файловые помощники ===

static bool WriteWholeFile(const wchar_t* path,
    const void* bytes, uint32_t length)
{
    uint32_t pathLength = 0;
    while (path[pathLength] != L'\0') ++pathLength;
    wchar_t* temporaryPath = HeapAlloc(GetProcessHeap(), 0,
        (size_t)(pathLength + 5u) * sizeof(wchar_t));
    if (temporaryPath == NULL) return false;
    memcpy(temporaryPath, path, (size_t)pathLength * sizeof(wchar_t));
    memcpy(temporaryPath + pathLength, L".tmp", 5u * sizeof(wchar_t));

    HANDLE file = CreateFileW(temporaryPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        HeapFree(GetProcessHeap(), 0, temporaryPath);
        return false;
    }
    DWORD written = 0;
    bool succeeded = WriteFile(file, bytes, length, &written, NULL)
        && written == length && FlushFileBuffers(file);
    CloseHandle(file);
    if (succeeded)
    {
        succeeded = MoveFileExW(temporaryPath, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!succeeded) DeleteFileW(temporaryPath);
    HeapFree(GetProcessHeap(), 0, temporaryPath);
    return succeeded;
}

static uint32_t ReadWholeFileInto(const wchar_t* path,
    void* destination, uint32_t capacity)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > (LONGLONG)capacity)
    {
        CloseHandle(file);
        return 0;
    }

    uint32_t length = (uint32_t)size.QuadPart;
    uint32_t completed = 0;
    while (completed < length)
    {
        DWORD read = 0;
        if (!ReadFile(file, (uint8_t*)destination + completed,
                length - completed, &read, NULL) || read == 0)
        {
            CloseHandle(file);
            return 0;
        }
        completed += read;
    }
    CloseHandle(file);
    return length;
}

// === Текстовые помощники (ASCII) ===

static void TextAppend(char* buffer, uint32_t capacity,
    uint32_t* length, const char* text)
{
    while (*text != '\0' && *length + 1u < capacity)
    {
        buffer[(*length)++] = *text++;
    }
    buffer[*length] = '\0';
}

static void TextAppendInt64(char* buffer, uint32_t capacity,
    uint32_t* length, int64_t value)
{
    if (value < 0)
    {
        TextAppend(buffer, capacity, length, "-");
    }
    uint64_t magnitude = value < 0 ? (uint64_t)(-value) : (uint64_t)value;
    char digits[24];
    uint32_t digitCount = 0;
    do
    {
        digits[digitCount++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    }
    while (magnitude != 0u && digitCount < 23u);
    while (digitCount > 0u && *length + 1u < capacity)
    {
        buffer[(*length)++] = digits[--digitCount];
    }
    buffer[*length] = '\0';
}

// Значение ключа "key = value" в построчном ASCII-тексте.
static bool TextFindInt64(const char* text, uint32_t length,
    const char* key, int64_t* outValue)
{
    uint32_t keyLength = 0;
    while (key[keyLength] != '\0') ++keyLength;

    uint32_t offset = 0;
    while (offset < length)
    {
        uint32_t lineEnd = offset;
        while (lineEnd < length && text[lineEnd] != '\n') ++lineEnd;

        uint32_t position = offset;
        while (position < lineEnd
            && (text[position] == ' ' || text[position] == '\t'))
        {
            ++position;
        }
        if (position + keyLength <= lineEnd
            && BytesEqual(text + position, key, keyLength))
        {
            position += keyLength;
            while (position < lineEnd && (text[position] == ' '
                || text[position] == '\t' || text[position] == '='))
            {
                ++position;
            }

            bool negative = false;
            if (position < lineEnd && text[position] == '-')
            {
                negative = true;
                ++position;
            }
            bool anyDigit = false;
            int64_t value = 0;
            while (position < lineEnd
                && text[position] >= '0' && text[position] <= '9')
            {
                value = value * 10 + (text[position] - '0');
                anyDigit = true;
                ++position;
            }
            if (anyDigit)
            {
                *outValue = negative ? -value : value;
                return true;
            }
        }
        offset = lineEnd + 1u;
    }
    return false;
}

// === world.meta ===

bool SaveGameReadMeta(int64_t* outSeed, int32_t* outTimeMinutes)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL)
    {
        return false;
    }

    bool succeeded = false;
    if (BuildSaveFilePath(L"world.meta",
            scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        uint32_t length = ReadWholeFileInto(scratch->path,
            scratch->text, SAVE_TEXT_CAPACITY - 1u);
        scratch->text[length] = '\0';

        // Первая строка обязана быть "LAIUE WORLD 1".
        int64_t seed = 0;
        int64_t timeMinutes = 0;
        if (length >= 13u
            && BytesEqual(scratch->text, "LAIUE WORLD 1", 13u)
            && TextFindInt64(scratch->text, length, "seed", &seed)
            && TextFindInt64(scratch->text, length,
                   "time_minutes", &timeMinutes))
        {
            if (timeMinutes < 0) timeMinutes = 0;
            if (timeMinutes > 1439) timeMinutes = 1439;
            *outSeed = seed;
            *outTimeMinutes = (int32_t)timeMinutes;
            succeeded = true;
        }
    }

    SaveScratchRelease(scratch);
    return succeeded;
}

static bool WriteMeta(int64_t seed, float timeOfDayHours)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"world.meta",
               scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }

    int32_t timeMinutes = (int32_t)(timeOfDayHours * 60.0f + 0.5f);
    if (timeMinutes < 0) timeMinutes = 0;
    if (timeMinutes > 1439) timeMinutes = 1439;

    char* text = scratch->text;
    uint32_t length = 0;
    text[0] = '\0';
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, "LAIUE WORLD 1\n");
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, "seed = ");
    TextAppendInt64(text, SAVE_TEXT_CAPACITY, &length, seed);
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, "\ntime_minutes = ");
    TextAppendInt64(text, SAVE_TEXT_CAPACITY, &length, timeMinutes);
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, "\ngame = ");
    TextAppendInt64(text, SAVE_TEXT_CAPACITY, &length, LAIUE_VERSION_MAJOR);
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, ".");
    TextAppendInt64(text, SAVE_TEXT_CAPACITY, &length, LAIUE_VERSION_MINOR);
    TextAppend(text, SAVE_TEXT_CAPACITY, &length, "\n");

    bool succeeded = WriteWholeFile(scratch->path, text, length);
    SaveScratchRelease(scratch);
    return succeeded;
}

// === player.dat ===

typedef struct PlayerSaveRecord
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    double position[3];   // локальные координаты к началу мира
    float yaw;
    float pitch;
    uint8_t gameMode;
    uint8_t padding[7];
} PlayerSaveRecord;

static bool WritePlayer(const Camera* camera, GameMode gameMode)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"player.dat",
               scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }

    PlayerSaveRecord record;
    memset(&record, 0, sizeof(record));
    record.magic = PLAYER_SAVE_MAGIC;
    record.version = PLAYER_SAVE_VERSION;
    record.position[0] = camera->position[0];
    record.position[1] = camera->position[1];
    record.position[2] = camera->position[2];
    record.yaw = camera->yaw;
    record.pitch = camera->pitch;
    record.gameMode = gameMode == GAME_MODE_WALK ? 1u : 0u;

    bool succeeded = WriteWholeFile(scratch->path, &record, sizeof(record));
    SaveScratchRelease(scratch);
    return succeeded;
}

bool SaveGameLoadPlayer(Camera* camera, GameMode* outGameMode)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"player.dat",
               scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }

    PlayerSaveRecord record;
    bool succeeded = ReadWholeFileInto(scratch->path,
            &record, sizeof(record)) == sizeof(record)
        && record.magic == PLAYER_SAVE_MAGIC
        && record.version == PLAYER_SAVE_VERSION;
    SaveScratchRelease(scratch);
    if (!succeeded)
    {
        return false;
    }

    camera->position[0] = record.position[0];
    camera->position[1] = record.position[1];
    camera->position[2] = record.position[2];
    camera->yaw = record.yaw;
    camera->pitch = record.pitch;
    *outGameMode = record.gameMode == 1u ? GAME_MODE_WALK : GAME_MODE_FLY;
    return true;
}

// === inventory.dat ===

typedef struct InventorySaveRecord
{
    uint32_t magic;
    uint16_t version;
    uint8_t selectedHotbarSlot;
    uint8_t reserved;
    InventorySlot slots[INVENTORY_SLOT_COUNT];
} InventorySaveRecord;

static bool WriteInventory(const Inventory* inventory)
{
    if (inventory == NULL) return false;
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"inventory.dat", scratch->path,
            SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }
    InventorySaveRecord record;
    memset(&record, 0, sizeof(record));
    record.magic = INVENTORY_SAVE_MAGIC;
    record.version = INVENTORY_SAVE_VERSION;
    record.selectedHotbarSlot = inventory->selectedHotbarSlot;
    memcpy(record.slots, inventory->slots, sizeof(record.slots));
    bool succeeded = WriteWholeFile(scratch->path, &record, sizeof(record));
    SaveScratchRelease(scratch);
    return succeeded;
}

bool SaveGameLoadInventory(Inventory* inventory)
{
    if (inventory == NULL) return false;
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"inventory.dat", scratch->path,
            SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }
    InventorySaveRecord record;
    bool succeeded = ReadWholeFileInto(scratch->path,
            &record, sizeof(record)) == sizeof(record)
        && record.magic == INVENTORY_SAVE_MAGIC
        && record.version == INVENTORY_SAVE_VERSION
        && record.selectedHotbarSlot < INVENTORY_HOTBAR_SLOT_COUNT;
    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT && succeeded; ++i)
    {
        succeeded = record.slots[i].count <= INVENTORY_STACK_LIMIT
            && ((record.slots[i].count == 0
                    && record.slots[i].item == INVENTORY_ITEM_NONE)
                || (record.slots[i].count != 0
                    && record.slots[i].item != INVENTORY_ITEM_NONE));
    }
    SaveScratchRelease(scratch);
    if (!succeeded) return false;
    inventory->selectedHotbarSlot = record.selectedHotbarSlot;
    memcpy(inventory->slots, record.slots, sizeof(record.slots));
    return true;
}

// === mods.lock ===

static uint32_t BuildModsLockText(const ModsState* mods,
    char* buffer, uint32_t capacity)
{
    uint32_t length = 0;
    buffer[0] = '\0';
    for (uint32_t i = 0; i < mods->enabledCount; ++i)
    {
        const ModEntry* entry = &mods->entries[mods->enabledOrder[i]];

        char utf8[MODS_NAME_CAPACITY * 3];
        int written = WideCharToMultiByte(CP_UTF8, 0, entry->fileName, -1,
            utf8, (int)sizeof(utf8), NULL, NULL);
        if (written > 1)
        {
            TextAppend(buffer, capacity, &length, utf8);
        }
        if (entry->version[0] != L'\0')
        {
            written = WideCharToMultiByte(CP_UTF8, 0, entry->version, -1,
                utf8, (int)sizeof(utf8), NULL, NULL);
            if (written > 1)
            {
                TextAppend(buffer, capacity, &length, " ");
                TextAppend(buffer, capacity, &length, utf8);
            }
        }
        TextAppend(buffer, capacity, &length, "\n");
    }
    return length;
}

static bool WriteModsLock(const ModsState* mods)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"mods.lock",
               scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return false;
    }

    uint32_t length =
        BuildModsLockText(mods, scratch->text, SAVE_TEXT_CAPACITY);
    bool succeeded = WriteWholeFile(scratch->path, scratch->text, length);
    SaveScratchRelease(scratch);
    return succeeded;
}

void SaveGameCheckModsLock(const ModsState* mods)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL
        || !BuildSaveFilePath(L"mods.lock",
               scratch->path, SAVE_GAME_PATH_CAPACITY))
    {
        SaveScratchRelease(scratch);
        return;
    }

    uint32_t savedLength = ReadWholeFileInto(scratch->path,
        scratch->text, SAVE_TEXT_CAPACITY - 1u);
    if (savedLength > 0)  // сохранение без mods.lock — сравнивать не с чем
    {
        uint32_t currentLength =
            BuildModsLockText(mods, scratch->extra, SAVE_TEXT_CAPACITY);
        if (savedLength != currentLength
            || !BytesEqual(scratch->text, scratch->extra, savedLength))
        {
            OutputDebugStringW(L"[laiue] состав модов отличается от "
                L"mods.lock сохранения\n");
        }
    }

    SaveScratchRelease(scratch);
}

// === Сборка ===

bool SaveGameWriteAll(World* world, const Camera* camera,
    GameMode gameMode, float timeOfDayHours, int64_t seed,
    const ModsState* mods, const Inventory* inventory)
{
    if (!SaveGameEnsureDirectories())
    {
        return false;
    }

    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL)
    {
        return false;
    }
    bool succeeded = BuildSaveFilePath(L"chunks.dat",
            scratch->path, SAVE_GAME_PATH_CAPACITY)
        && WorldSaveDeltas(world, scratch->path);
    SaveScratchRelease(scratch);

    succeeded = WriteMeta(seed, timeOfDayHours) && succeeded;
    succeeded = WritePlayer(camera, gameMode) && succeeded;
    succeeded = WriteInventory(inventory) && succeeded;
    succeeded = WriteModsLock(mods) && succeeded;
    return succeeded;
}

bool SaveGameLoadWorld(World* world)
{
    SaveScratch* scratch = SaveScratchAcquire();
    if (scratch == NULL)
    {
        return false;
    }
    bool succeeded = BuildSaveFilePath(L"chunks.dat",
            scratch->path, SAVE_GAME_PATH_CAPACITY)
        && WorldLoadDeltas(world, scratch->path);
    SaveScratchRelease(scratch);
    return succeeded;
}
