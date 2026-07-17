#pragma once

#include "api.h"
#include "mod/mods.h"
#include "game/camera.h"
#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Хост нативных DLL-модов: загружает mods/<имя>.lmp/<dll>, отдаёт моду
// версионируемую таблицу функций (sdk/laiue_mod_api.h) и диспетчеризует
// хуки строго на главном потоке. Кадровому хуку сопутствует фиксированный
// тик (60 Гц, аккумулятор) — геймплейные моды не зависят от FPS.
// Хост не знает об интерфейсе: он получает голые указатели подсистем,
// а фактический статус каждого мода отписывает обратно в ModsState.

// Единый лимит с каталогом: включённый мод всегда попадает в цепочку.
#define MOD_HOST_MAX_MODS MODS_MAX_ENTRIES
#define MOD_HOST_NAME_CAPACITY 64
#define MOD_HOST_MAX_INTERFACES 16
#define MOD_HOST_INTERFACE_NAME_CAPACITY 64

// Фиксированный тик модов: 60 Гц, не больше 5 шагов за кадр.
#define MOD_HOST_FIXED_STEP_SECONDS (1.0f / 60.0f)
#define MOD_HOST_MAX_FIXED_STEPS_PER_FRAME 5

// Указатели на подсистемы, которыми пользуется таблица API.
typedef struct ModHostBindings
{
    World* world;
    PlayerController* player;
    Camera* camera;
    GameMode* gameMode;
    float* timeOfDayHours;   // игровое время суток, 0..24
    ModSide runtimeSide;
    void* invalidateContext;
    void (*invalidateBlock)(void* context,
        int64_t x, int64_t y, int64_t z);
    void* viewContext;
    void (*getViewDirection)(void* context, float outDirection[3]);
    // Каталог блобов модов в сохранении (moddata/), может быть пустым.
    const wchar_t* modDataDirectory;
} ModHostBindings;

typedef struct ModHostSlot ModHostSlot;

// Межмодовый интерфейс: именованная таблица функций библиотеки.
typedef struct ModHostInterface
{
    bool used;
    char name[MOD_HOST_INTERFACE_NAME_CAPACITY];
    uint32_t version;
    void* pointer;
    ModHostSlot* owner;
} ModHostInterface;

typedef struct ModHost
{
    ModHostBindings bindings;
    ModHostSlot* slots;      // MOD_HOST_MAX_MODS, на куче
    ModHostInterface interfaces[MOD_HOST_MAX_INTERFACES];
    float fixedTickAccumulator;
} ModHost;

LAIUE_MOD_API bool ModHostInit(
    ModHost* host, const ModHostBindings* bindings);
LAIUE_MOD_API void ModHostShutdown(ModHost* host);

// Приводит загруженные DLL в соответствие включённым модам (вызывается
// при смене ревизии ModsState и на старте) и отписывает в entries
// фактический runtimeStatus и код отказа инициализации.
LAIUE_MOD_API void ModHostSync(ModHost* host, ModsState* mods);

// Хуки: фиксированный тик + кадр (вне меню паузы) и правка блока игроком.
LAIUE_MOD_API void ModHostDispatchFrame(ModHost* host, float deltaSeconds);
LAIUE_MOD_API void ModHostDispatchBlockEdit(ModHost* host,
    int64_t x, int64_t y, int64_t z,
    uint8_t previousBlock, uint8_t newBlock);
