#pragma once

#include "core/pause_menu.h"
#include "game/camera.h"
#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Хост нативных DLL-модов (уровень B): загружает mods/<имя>.lmp/<dll>,
// отдаёт моду версионируемую таблицу функций (sdk/laiue_mod_api.h)
// и диспетчеризует хуки строго на главном потоке. Выгрузка снимает
// колбеки мода до FreeLibrary, поэтому забытый Shutdown не роняет игру.

#define MOD_HOST_MAX_MODS 8
#define MOD_HOST_NAME_CAPACITY 64
#define MOD_HOST_MAX_INTERFACES 16
#define MOD_HOST_INTERFACE_NAME_CAPACITY 64

typedef struct ChunkStreaming ChunkStreaming;
typedef struct ModsState ModsState;

// Указатели на подсистемы, которыми пользуется таблица API.
typedef struct ModHostBindings
{
    World* world;
    ChunkStreaming* chunkStreaming;
    PlayerController* player;
    Camera* camera;
    GameSettings* settings;
    GameMode* gameMode;
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
} ModHost;

bool ModHostInit(ModHost* host, const ModHostBindings* bindings);
void ModHostShutdown(ModHost* host);

// Приводит загруженные DLL в соответствие включённым нативным модам
// (вызывается при смене ревизии ModsState и на старте).
void ModHostSync(ModHost* host, const ModsState* mods);

// Хуки: кадр (вне меню паузы) и правка блока игроком.
void ModHostDispatchFrame(ModHost* host, float deltaSeconds);
void ModHostDispatchBlockEdit(ModHost* host,
    int64_t x, int64_t y, int64_t z,
    uint8_t previousBlock, uint8_t newBlock);
