#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Моды laiue — только нативные DLL (см. docs/modding.md): каталог
// mods/<имя>.lmp с манифестом mod.lm и DLL точки входа. Манифест —
// построчный UTF-8: заголовок `LAIUE MOD 1`, метаданные (name, version,
// game) и секция [native] с entry (имя DLL) и api (версия API SDK).
//
// Включённые моды и их порядок хранит mods/enabled.txt (по строке —
// имя каталога). Порядок = порядок загрузки DLL: библиотеки должны
// стоять раньше своих потребителей. Файл — источник истины: пересчёт
// всегда идёт от него, правка вручную равносильна щелчкам в меню.

// Единый лимит каталога и хоста: включённый мод не может «молча»
// не попасть в цепочку загрузки.
#define MODS_MAX_ENTRIES 32
#define MODS_NAME_CAPACITY 64

// Фактическое состояние мода после синхронизации хоста — показывается
// на вкладке «Моды» вместе с причиной отказа.
typedef enum ModRuntimeStatus
{
    MOD_RUNTIME_DISABLED = 0,
    MOD_RUNTIME_INCOMPATIBLE,   // версия игры или API не подходит
    MOD_RUNTIME_LOADED,
    MOD_RUNTIME_LOAD_FAILED,    // DLL или экспорт LaiueModInit не найдены
    MOD_RUNTIME_INIT_FAILED,    // LaiueModInit вернул ненулевой код
} ModRuntimeStatus;

typedef struct ModEntry
{
    wchar_t fileName[MODS_NAME_CAPACITY];     // имя каталога .lmp
    wchar_t displayName[MODS_NAME_CAPACITY];  // name = из манифеста
    wchar_t version[16];                      // version = из манифеста
    wchar_t requiredGame[16];                 // game = (пусто — любая)
    wchar_t entryDll[MODS_NAME_CAPACITY];     // [native] entry =
    uint32_t requiredApi;                     // [native] api =
    bool enabled;
    bool compatible;   // версия игры и версия API

    ModRuntimeStatus runtimeStatus;  // заполняет хост при синхронизации
    int32_t initResult;              // код отказа LaiueModInit
} ModEntry;

typedef struct ModsState
{
    ModEntry entries[MODS_MAX_ENTRIES];
    uint32_t count;

    // Включённые моды в порядке enabled.txt (индексы entries).
    uint32_t enabledOrder[MODS_MAX_ENTRIES];
    uint32_t enabledCount;

    // Растёт при каждом пересчёте: приложение сравнивает со своей
    // применённой ревизией и синхронизирует хост DLL-модов.
    uint32_t revision;
} ModsState;

void ModsInit(ModsState* mods);

// Перечитывает каталог mods и enabled.txt.
void ModsRefresh(ModsState* mods);

// Включает или выключает мод по индексу списка: переписывает
// enabled.txt (включение добавляется в конец порядка) и пересчитывает.
// Несовместимые моды включить нельзя.
bool ModsSetEnabled(ModsState* mods, uint32_t index, bool enabled);
