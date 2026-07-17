#include "core/mod_host.h"
#include "core/chunk_streaming.h"
#include "core/mods.h"
#include "core/save_game.h"
#include "content/content_catalog.h"
#include "world/block_properties.h"

#include "../../sdk/laiue_mod_api.h"

#include <windows.h>
#include <string.h>

#define MOD_LOG_FILE_NAME L"mod_log.txt"
#define MOD_LOG_MAX_CHARS 512

struct ModHostSlot
{
    bool used;
    wchar_t packName[MOD_HOST_NAME_CAPACITY];  // имя каталога .lmp
    HMODULE module;
    LaiueModShutdownFunction shutdown;
    LaiueModApi api;                            // api.host -> этот слот
    ModHost* owner;

    LaiueFrameCallback frameCallback;
    void* frameUser;
    LaiueFixedTickCallback fixedTickCallback;
    void* fixedTickUser;
    LaiueBlockEditCallback blockEditCallback;
    void* blockEditUser;
};

static ModHostSlot* SlotFromHostPointer(void* hostPointer)
{
    return (ModHostSlot*)hostPointer;
}

// === Журнал: OutputDebugString + mods/mod_log.txt (UTF-8, append) ===

static void AppendLogFile(const wchar_t* line)
{
    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL)
    {
        return;
    }

    if (LaiueContentBuildPath(LAIUE_CONTENT_MOD, NULL, NULL,
            path, LAIUE_CONTENT_PATH_CAPACITY))
    {
        uint32_t length = 0;
        while (path[length] != L'\0') ++length;
        const wchar_t* suffix = L"\\" MOD_LOG_FILE_NAME;
        uint32_t suffixLength = 0;
        while (suffix[suffixLength] != L'\0') ++suffixLength;
        if (length + suffixLength + 1 <= LAIUE_CONTENT_PATH_CAPACITY)
        {
            for (uint32_t i = 0; i <= suffixLength; ++i)
            {
                path[length + i] = suffix[i];
            }

            HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE)
            {
                char utf8[MOD_LOG_MAX_CHARS * 3];
                int written = WideCharToMultiByte(CP_UTF8, 0, line, -1,
                    utf8, (int)sizeof(utf8) - 2, NULL, NULL);
                if (written > 1)
                {
                    utf8[written - 1] = '\r';
                    utf8[written] = '\n';
                    DWORD ignored = 0;
                    WriteFile(file, utf8, (DWORD)written + 1, &ignored, NULL);
                }
                CloseHandle(file);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, path);
}

static void ApiLog(void* hostPointer, const wchar_t* message)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (slot == NULL || message == NULL)
    {
        return;
    }

    wchar_t line[MOD_LOG_MAX_CHARS];
    uint32_t length = 0;
    line[length++] = L'[';
    for (uint32_t i = 0; slot->packName[i] != L'\0'
        && length + 2 < MOD_LOG_MAX_CHARS; ++i)
    {
        line[length++] = slot->packName[i];
    }
    line[length++] = L']';
    line[length++] = L' ';
    for (uint32_t i = 0; message[i] != L'\0'
        && length + 1 < MOD_LOG_MAX_CHARS; ++i)
    {
        line[length++] = message[i];
    }
    line[length] = L'\0';

    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
    AppendLogFile(line);
}

// === Таблица API ===

static uint8_t ApiGetBlock(void* hostPointer,
    int64_t x, int64_t y, int64_t z)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    return (uint8_t)WorldGetBlock(slot->owner->bindings.world, x, y, z);
}

static bool ApiSetBlock(void* hostPointer,
    int64_t x, int64_t y, int64_t z, uint8_t block)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (block > LAIUE_BLOCK_GRASS)
    {
        return false;  // неизвестный этой версии игры тип
    }

    ModHostBindings* bindings = &slot->owner->bindings;
    WorldSetBlock(bindings->world, x, y, z, (BlockType)block);
    ChunkStreamingInvalidateBlock(bindings->chunkStreaming, x, y, z);
    return true;
}

static void ApiGetPlayerPosition(void* hostPointer, double outPosition[3])
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    const Camera* camera = slot->owner->bindings.camera;
    outPosition[0] = camera->position[0];
    outPosition[1] = camera->position[1];
    outPosition[2] = camera->position[2];
}

static void ApiSetPlayerPosition(void* hostPointer,
    const double position[3])
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    ModHostBindings* bindings = &slot->owner->bindings;
    bindings->camera->position[0] = position[0];
    bindings->camera->position[1] = position[1];
    bindings->camera->position[2] = position[2];
    // Телепорт обнуляет накопленные скорости — без рывка после переноса.
    PlayerControllerReset(bindings->player, bindings->camera);
}

static void ApiGetViewDirection(void* hostPointer, float outDirection[3])
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    CameraGetForwardVector(slot->owner->bindings.camera, outDirection);
}

static void ApiApplyImpulse(void* hostPointer, float x, float y, float z)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    PlayerControllerApplyImpulse(slot->owner->bindings.player, x, y, z);
}

static bool ApiIsPlayerGrounded(void* hostPointer)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    return PlayerControllerIsGrounded(slot->owner->bindings.player);
}

static uint32_t ApiGetGameMode(void* hostPointer)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    return *slot->owner->bindings.gameMode == GAME_MODE_WALK
        ? LAIUE_GAME_MODE_WALK : LAIUE_GAME_MODE_FLY;
}

static float ApiGetTimeHours(void* hostPointer)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    return *slot->owner->bindings.timeOfDayHours;
}

static void ApiSetTimeHours(void* hostPointer, float hours)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    while (hours >= 24.0f) hours -= 24.0f;
    while (hours < 0.0f) hours += 24.0f;
    *slot->owner->bindings.timeOfDayHours = hours;
}

static void ApiSetAirJumps(void* hostPointer,
    int32_t extraJumps, float impulse, bool refillOnGround)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (extraJumps < 0) extraJumps = 0;
    if (extraJumps > 3) extraJumps = 3;
    if (impulse < 1.0f) impulse = 1.0f;
    if (impulse > 20.0f) impulse = 20.0f;
    PlayerControllerSetAirJumps(slot->owner->bindings.player,
        extraJumps, (double)impulse, refillOnGround);
}

// === Межмодовые интерфейсы ===

static bool InterfaceNamesEqual(const char* a, const char* b)
{
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

static bool ApiPublishInterface(void* hostPointer,
    const char* name, uint32_t version, void* interfacePointer)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (name == NULL || name[0] == '\0' || interfacePointer == NULL)
    {
        return false;
    }

    ModHost* host = slot->owner;
    ModHostInterface* free = NULL;
    for (uint32_t i = 0; i < MOD_HOST_MAX_INTERFACES; ++i)
    {
        ModHostInterface* entry = &host->interfaces[i];
        if (entry->used && InterfaceNamesEqual(entry->name, name))
        {
            return false;  // имя занято другой библиотекой
        }
        if (!entry->used && free == NULL)
        {
            free = entry;
        }
    }
    if (free == NULL)
    {
        return false;
    }

    uint32_t i = 0;
    while (name[i] != '\0'
        && i + 1 < MOD_HOST_INTERFACE_NAME_CAPACITY)
    {
        free->name[i] = name[i];
        ++i;
    }
    free->name[i] = '\0';
    free->version = version;
    free->pointer = interfacePointer;
    free->owner = slot;
    free->used = true;
    return true;
}

static void* ApiQueryInterface(void* hostPointer,
    const char* name, uint32_t minimumVersion)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (name == NULL)
    {
        return NULL;
    }
    ModHost* host = slot->owner;
    for (uint32_t i = 0; i < MOD_HOST_MAX_INTERFACES; ++i)
    {
        ModHostInterface* entry = &host->interfaces[i];
        if (entry->used && entry->version >= minimumVersion
            && InterfaceNamesEqual(entry->name, name))
        {
            return entry->pointer;
        }
    }
    return NULL;
}

static void RemoveSlotInterfaces(ModHostSlot* slot)
{
    ModHost* host = slot->owner;
    for (uint32_t i = 0; i < MOD_HOST_MAX_INTERFACES; ++i)
    {
        if (host->interfaces[i].used && host->interfaces[i].owner == slot)
        {
            memset(&host->interfaces[i], 0, sizeof(host->interfaces[i]));
        }
    }
}

static void ApiSetFrameCallback(void* hostPointer,
    LaiueFrameCallback callback, void* user)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    slot->frameCallback = callback;
    slot->frameUser = user;
}

static void ApiSetFixedTickCallback(void* hostPointer,
    LaiueFixedTickCallback callback, void* user)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    slot->fixedTickCallback = callback;
    slot->fixedTickUser = user;
}

// === Данные мода в сохранении: saves/default/moddata/<имя>.bin ===

#define MOD_DATA_MAX_BYTES (16u * 1024u * 1024u)

static bool BuildModDataPath(ModHostSlot* slot,
    wchar_t* path, uint32_t capacity)
{
    const wchar_t* directory = slot->owner->bindings.modDataDirectory;
    if (directory == NULL || directory[0] == L'\0')
    {
        return false;
    }

    uint32_t length = 0;
    while (directory[length] != L'\0' && length + 1 < capacity)
    {
        path[length] = directory[length];
        ++length;
    }
    if (length + 2 >= capacity)
    {
        return false;
    }
    path[length++] = L'\\';
    for (uint32_t i = 0; slot->packName[i] != L'\0'
        && length + 5 < capacity; ++i)
    {
        path[length++] = slot->packName[i];
    }
    const wchar_t* extension = L".bin";
    for (uint32_t i = 0; extension[i] != L'\0' && length + 1 < capacity; ++i)
    {
        path[length++] = extension[i];
    }
    path[length] = L'\0';
    return true;
}

static bool ApiWriteModData(void* hostPointer,
    const void* bytes, uint32_t size)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (bytes == NULL || size == 0 || size > MOD_DATA_MAX_BYTES
        || !SaveGameEnsureDirectories())
    {
        return false;
    }

    wchar_t path[SAVE_GAME_PATH_CAPACITY];
    if (!BuildModDataPath(slot, path, SAVE_GAME_PATH_CAPACITY))
    {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    bool succeeded = WriteFile(file, bytes, size, &written, NULL)
        && written == size;
    CloseHandle(file);
    return succeeded;
}

static uint32_t ApiReadModData(void* hostPointer,
    void* buffer, uint32_t capacity)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    if (buffer == NULL || capacity == 0)
    {
        return 0;
    }

    wchar_t path[SAVE_GAME_PATH_CAPACITY];
    if (!BuildModDataPath(slot, path, SAVE_GAME_PATH_CAPACITY))
    {
        return 0;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    LARGE_INTEGER size;
    uint32_t result = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0
        && size.QuadPart <= (LONGLONG)capacity)
    {
        uint32_t length = (uint32_t)size.QuadPart;
        uint32_t completed = 0;
        while (completed < length)
        {
            DWORD read = 0;
            if (!ReadFile(file, (uint8_t*)buffer + completed,
                    length - completed, &read, NULL) || read == 0)
            {
                break;
            }
            completed += read;
        }
        result = completed == length ? length : 0;
    }
    CloseHandle(file);
    return result;
}

static void ApiSetBlockEditCallback(void* hostPointer,
    LaiueBlockEditCallback callback, void* user)
{
    ModHostSlot* slot = SlotFromHostPointer(hostPointer);
    slot->blockEditCallback = callback;
    slot->blockEditUser = user;
}

static void FillApi(ModHostSlot* slot)
{
    LaiueModApi* api = &slot->api;
    memset(api, 0, sizeof(*api));
    api->structSize = sizeof(*api);
    api->apiVersion = LAIUE_MOD_API_VERSION;
    api->gameVersionMajor = LAIUE_VERSION_MAJOR;
    api->gameVersionMinor = LAIUE_VERSION_MINOR;
    api->host = slot;
    api->log = ApiLog;
    api->getBlock = ApiGetBlock;
    api->setBlock = ApiSetBlock;
    api->getPlayerPosition = ApiGetPlayerPosition;
    api->setPlayerPosition = ApiSetPlayerPosition;
    api->getViewDirection = ApiGetViewDirection;
    api->applyImpulse = ApiApplyImpulse;
    api->isPlayerGrounded = ApiIsPlayerGrounded;
    api->getGameMode = ApiGetGameMode;
    api->getTimeHours = ApiGetTimeHours;
    api->setTimeHours = ApiSetTimeHours;
    api->setFrameCallback = ApiSetFrameCallback;
    api->setBlockEditCallback = ApiSetBlockEditCallback;
    api->setAirJumps = ApiSetAirJumps;
    api->publishInterface = ApiPublishInterface;
    api->queryInterface = ApiQueryInterface;
    api->setFixedTickCallback = ApiSetFixedTickCallback;
    api->writeModData = ApiWriteModData;
    api->readModData = ApiReadModData;
}

// === Жизненный цикл ===

bool ModHostInit(ModHost* host, const ModHostBindings* bindings)
{
    host->bindings = *bindings;
    memset(host->interfaces, 0, sizeof(host->interfaces));
    host->fixedTickAccumulator = 0.0f;
    host->slots = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)MOD_HOST_MAX_MODS * sizeof(ModHostSlot));
    return host->slots != NULL;
}

static void UnloadSlot(ModHostSlot* slot)
{
    if (!slot->used)
    {
        return;
    }

    // Колбеки и интерфейсы снимаются до FreeLibrary: указатели
    // в выгруженную DLL не должны пережить её.
    slot->frameCallback = NULL;
    slot->blockEditCallback = NULL;
    RemoveSlotInterfaces(slot);
    if (slot->shutdown != NULL)
    {
        slot->shutdown();
    }
    if (slot->module != NULL)
    {
        FreeLibrary(slot->module);
    }
    memset(slot, 0, sizeof(*slot));
}

void ModHostShutdown(ModHost* host)
{
    if (host->slots == NULL)
    {
        return;
    }
    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
    {
        UnloadSlot(&host->slots[i]);
    }
    HeapFree(GetProcessHeap(), 0, host->slots);
    host->slots = NULL;
}

static bool WideNamesEqual(const wchar_t* a, const wchar_t* b)
{
    uint32_t i = 0;
    while (a[i] != L'\0' && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

// Загружает мод и отписывает фактический статус прямо в entry.
static void LoadSlot(ModHost* host, ModEntry* entry)
{
    ModHostSlot* slot = NULL;
    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
    {
        if (!host->slots[i].used)
        {
            slot = &host->slots[i];
            break;
        }
    }
    if (slot == NULL)
    {
        entry->runtimeStatus = MOD_RUNTIME_LOAD_FAILED;
        return;
    }

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    if (path == NULL)
    {
        entry->runtimeStatus = MOD_RUNTIME_LOAD_FAILED;
        return;
    }
    bool pathOk = LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK,
        entry->fileName, entry->entryDll, path,
        LAIUE_CONTENT_PATH_CAPACITY);

    HMODULE module = pathOk ? LoadLibraryW(path) : NULL;
    HeapFree(GetProcessHeap(), 0, path);
    if (module == NULL)
    {
        entry->runtimeStatus = MOD_RUNTIME_LOAD_FAILED;
        return;
    }

    LaiueModInitFunction initialize = (LaiueModInitFunction)
        (void*)GetProcAddress(module, "LaiueModInit");
    if (initialize == NULL)
    {
        FreeLibrary(module);
        entry->runtimeStatus = MOD_RUNTIME_LOAD_FAILED;
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->owner = host;
    slot->module = module;
    slot->shutdown = (LaiueModShutdownFunction)
        (void*)GetProcAddress(module, "LaiueModShutdown");
    uint32_t c = 0;
    while (entry->fileName[c] != L'\0' && c + 1 < MOD_HOST_NAME_CAPACITY)
    {
        slot->packName[c] = entry->fileName[c];
        ++c;
    }
    slot->packName[c] = L'\0';
    FillApi(slot);

    int32_t initResult = initialize(&slot->api);
    if (initResult != 0)
    {
        // Причина в журнал: типично «нет библиотеки-зависимости».
        wchar_t line[64] = L"отклонил инициализацию, код ";
        uint32_t length = 0;
        while (line[length] != L'\0') ++length;
        uint32_t value = initResult < 0
            ? (uint32_t)(-initResult) : (uint32_t)initResult;
        if (initResult < 0 && length + 1 < 63)
        {
            line[length++] = L'-';
        }
        wchar_t digits[12];
        uint32_t digitCount = 0;
        do
        {
            digits[digitCount++] = (wchar_t)(L'0' + value % 10u);
            value /= 10u;
        }
        while (value != 0u && digitCount < 11u);
        while (digitCount > 0u && length + 1 < 63)
        {
            line[length++] = digits[--digitCount];
        }
        line[length] = L'\0';
        ApiLog(slot, line);

        UnloadSlot(slot);
        entry->runtimeStatus = MOD_RUNTIME_INIT_FAILED;
        entry->initResult = initResult;
        return;
    }

    ApiLog(slot, L"мод загружен");
    entry->runtimeStatus = MOD_RUNTIME_LOADED;
}

void ModHostSync(ModHost* host, ModsState* mods)
{
    if (host->slots == NULL)
    {
        return;
    }

    // Желаемая цепочка: включённые совместимые моды в порядке
    // enabled.txt (он же порядок публикации библиотек и хуков).
    ModEntry* desired[MOD_HOST_MAX_MODS];
    uint32_t desiredCount = 0;
    for (uint32_t i = 0; i < mods->enabledCount
        && desiredCount < MOD_HOST_MAX_MODS; ++i)
    {
        ModEntry* entry = &mods->entries[mods->enabledOrder[i]];
        if (entry->enabled && entry->compatible)
        {
            desired[desiredCount++] = entry;
        }
    }

    // Загруженная цепочка совпадает? Слоты заполняются по порядку
    // загрузки, поэтому достаточно сравнить последовательности имён.
    uint32_t loaded = 0;
    bool matches = true;
    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS && matches; ++i)
    {
        if (!host->slots[i].used)
        {
            continue;
        }
        matches = loaded < desiredCount && WideNamesEqual(
            host->slots[i].packName, desired[loaded]->fileName);
        ++loaded;
    }
    if (matches && loaded == desiredCount)
    {
        return;
    }

    // Полная перезагрузка цепочки: межмодовые интерфейсы не переживают
    // смену состава, зато не бывает висячих указателей между модами.
    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
    {
        if (host->slots[i].used)
        {
            ApiLog(&host->slots[i], L"мод выгружается");
            UnloadSlot(&host->slots[i]);
        }
    }

    // Дефолты движка, которые моды могли перекрутить.
    PlayerControllerSetAirJumps(host->bindings.player, 0, 7.0, true);

    for (uint32_t i = 0; i < desiredCount; ++i)
    {
        LoadSlot(host, desired[i]);
    }
}

void ModHostDispatchFrame(ModHost* host, float deltaSeconds)
{
    if (host->slots == NULL)
    {
        return;
    }

    // Фиксированный тик: постоянный шаг независимо от FPS. Кап шагов
    // защищает от лавины после долгого кадра; хвост не копится.
    host->fixedTickAccumulator += deltaSeconds;
    uint32_t steps = 0;
    while (host->fixedTickAccumulator >= MOD_HOST_FIXED_STEP_SECONDS
        && steps < MOD_HOST_MAX_FIXED_STEPS_PER_FRAME)
    {
        host->fixedTickAccumulator -= MOD_HOST_FIXED_STEP_SECONDS;
        ++steps;
        for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
        {
            ModHostSlot* slot = &host->slots[i];
            if (slot->used && slot->fixedTickCallback != NULL)
            {
                slot->fixedTickCallback(slot->fixedTickUser,
                    MOD_HOST_FIXED_STEP_SECONDS);
            }
        }
    }
    if (host->fixedTickAccumulator > MOD_HOST_FIXED_STEP_SECONDS)
    {
        host->fixedTickAccumulator = MOD_HOST_FIXED_STEP_SECONDS;
    }

    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
    {
        ModHostSlot* slot = &host->slots[i];
        if (slot->used && slot->frameCallback != NULL)
        {
            slot->frameCallback(slot->frameUser, deltaSeconds);
        }
    }
}

void ModHostDispatchBlockEdit(ModHost* host,
    int64_t x, int64_t y, int64_t z,
    uint8_t previousBlock, uint8_t newBlock)
{
    if (host->slots == NULL)
    {
        return;
    }
    for (uint32_t i = 0; i < MOD_HOST_MAX_MODS; ++i)
    {
        ModHostSlot* slot = &host->slots[i];
        if (slot->used && slot->blockEditCallback != NULL)
        {
            slot->blockEditCallback(slot->blockEditUser,
                x, y, z, previousBlock, newBlock);
        }
    }
}
