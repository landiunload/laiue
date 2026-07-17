#pragma once

#include "mod/mods.h"
#include "game/camera.h"
#include "gameplay/game_mode.h"
#include "gameplay/inventory.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Сохранение игры (Laiue World Format v1, docs/world_format.md):
// Текущий слот находится в saves/<имя> рядом с исполняемым файлом.
//
//   world.meta  — текст: версия формата, seed, время суток, версия игры
//   chunks.dat  — правки мира (пишет и читает модуль world)
//   player.dat  — позиция, взгляд и режим игрока
//   inventory.dat — 36 фиксированных слотов и выбранная ячейка хотбара
//   mods.lock   — включённые моды и их версии на момент сохранения
//   moddata/    — по блобу на мод (API writeModData/readModData)
//
// Сохранение выполняется при выходе и кнопкой «Сохранить мир» в меню;
// загрузка — после выбора слота, если сохранение читаемо (иначе новый мир).

#define SAVE_GAME_PATH_CAPACITY 1024u
#define SAVE_GAME_MAX_SLOTS 32u
#define SAVE_GAME_SLOT_NAME_CAPACITY 64u

typedef struct SaveGameSlot
{
    wchar_t name[SAVE_GAME_SLOT_NAME_CAPACITY];
} SaveGameSlot;

typedef struct SaveGameSlotList
{
    SaveGameSlot entries[SAVE_GAME_MAX_SLOTS];
    uint32_t count;
} SaveGameSlotList;

bool SaveGameSetSlot(const wchar_t* name);
const wchar_t* SaveGameGetSlot(void);
bool SaveGameEnumerateSlots(SaveGameSlotList* outList);
bool SaveGameChooseNewSlot(wchar_t* destination, uint32_t capacity);

// false — сохранения нет или метаданные нечитаемы (стартуем новый мир).
bool SaveGameReadMeta(int64_t* outSeed, int32_t* outTimeMinutes);

bool SaveGameWriteAll(World* world, const Camera* camera,
    GameMode gameMode, float timeOfDayHours, int64_t seed,
    const ModsState* mods, const Inventory* inventory);

// Вызывать на свежесозданном мире до запуска стриминга чанков.
bool SaveGameLoadWorld(World* world);
bool SaveGameLoadPlayer(Camera* camera, GameMode* outGameMode);
bool SaveGameLoadInventory(Inventory* inventory);

// Предупреждение в отладочный вывод при расхождении состава модов
// с mods.lock сохранения.
void SaveGameCheckModsLock(const ModsState* mods);

// Каталог блобов модов (для хоста DLL-модов).
bool SaveGameModDataDirectory(wchar_t* destination, uint32_t capacity);
bool SaveGameEnsureDirectories(void);
