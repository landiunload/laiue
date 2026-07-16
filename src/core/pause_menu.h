#pragma once

#include "core/game_time.h"
#include "core/mods.h"
#include "core/panorama.h"
#include "core/ui.h"
#include "platform/window.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Меню паузы (внутренний компонент ядра): открывается по Esc,
// экраны «Пауза» и «Настройки» с разделами «Графика», «Текстуры»,
// «Шейдеры», «Моды», «Админ» и «Управление». Всё применяется сразу.

typedef enum PauseMenuScreen
{
    PAUSE_MENU_CLOSED = 0,
    PAUSE_MENU_MAIN,
    PAUSE_MENU_SETTINGS,
} PauseMenuScreen;

typedef enum PauseMenuAction
{
    PAUSE_MENU_ACTION_NONE = 0,
    PAUSE_MENU_ACTION_RESUME,
    PAUSE_MENU_ACTION_QUIT,
} PauseMenuAction;

typedef struct GameSettings
{
    // Графика
    int32_t fovDegrees;           // ползунок 1..360
    RenderProjection projection;  // выбор рендера

    // Администрирование
    float timeOfDayHours;         // 0..24
    TimeSpeedPreset timeSpeed;

    // Управление
    int32_t mouseSensitivityPercent;  // 25..300, базовая чувствительность = 100
    int32_t flySpeedBlocks;           // скорость полёта, блоков/с

    // Шейдер
    bool wireframe;               // каркасный режим
    int32_t gamma;                // гамма-коррекция 50..150 (100 = нейтрально)

    // Выбранные (но ещё не применённые) пакеты
    int32_t selectedTexturePack;  // индекс в списке текстурпаков, -1 если не выбран
    int32_t selectedShaderPack;   // индекс в списке шейдерпаков, -1 если не выбран
    bool applyTexturePack;        // true → применить выбранный текстурпак
    bool applyShaderPack;         // true → применить выбранный шейдерпак
} GameSettings;

typedef struct PauseMenu
{
    PauseMenuScreen screen;
    // 0 — графика, 1 — текстуры, 2 — шейдеры, 3 — моды,
    // 4 — администрирование, 5 — управление.
    int32_t settingsTab;
    float settingsScroll;  // прокрутка контента настроек, px (0 — верх)
} PauseMenu;

void PauseMenuOpen(PauseMenu* menu);

// Обновляет состояние и собирает квады меню в ui.
// escapePressed — Esc в этом кадре (в настройках возвращает на главный
// экран, на главном — продолжает игру). dayLengthMinutes — длительность
// суток для подписи пресета «Обычная». Переключения модов сразу пишутся
// в mods/enabled.txt и меняют ревизию mods.
PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, Window* window,
    ModsState* mods, float dayLengthMinutes, int32_t width, int32_t height,
    bool escapePressed);
