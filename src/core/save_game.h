#pragma once

#include "core/mods.h"
#include "game/camera.h"
#include "gameplay/game_mode.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Сохранение игры (Laiue World Format v1, docs/world_format.md):
// каталог saves/default рядом с исполняемым файлом.
//
//   world.meta  — текст: версия формата, seed, время суток, версия игры
//   chunks.dat  — правки мира (пишет и читает модуль world)
//   player.dat  — позиция, взгляд и режим игрока
//   mods.lock   — включённые моды и их версии на момент сохранения
//   moddata/    — по блобу на мод (API writeModData/readModData)
//
// Сохранение выполняется при выходе и кнопкой «Сохранить мир» в меню;
// загрузка — на старте, если сохранение читаемо (иначе новый мир).

#define SAVE_GAME_PATH_CAPACITY 1024u

// false — сохранения нет или метаданные нечитаемы (стартуем новый мир).
bool SaveGameReadMeta(int64_t* outSeed, int32_t* outTimeMinutes);

bool SaveGameWriteAll(World* world, const Camera* camera,
    GameMode gameMode, float timeOfDayHours, int64_t seed,
    const ModsState* mods);

// Вызывать на свежесозданном мире до запуска стриминга чанков.
bool SaveGameLoadWorld(World* world);
bool SaveGameLoadPlayer(Camera* camera, GameMode* outGameMode);

// Предупреждение в отладочный вывод при расхождении состава модов
// с mods.lock сохранения.
void SaveGameCheckModsLock(const ModsState* mods);

// Каталог блобов модов (для хоста DLL-модов).
bool SaveGameModDataDirectory(wchar_t* destination, uint32_t capacity);
bool SaveGameEnsureDirectories(void);
