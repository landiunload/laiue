#pragma once

#include "core/game_time.h"
#include "core/panorama.h"
#include "core/ui.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Меню паузы (внутренний компонент ядра): открывается по Esc,
// экраны «Пауза» и «Настройки» с разделами «Графика»,
// «Администрирование» и «Управление». Настройки применяются сразу.

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
} GameSettings;

typedef struct PauseMenu
{
    PauseMenuScreen screen;
    int32_t settingsTab;   // 0 — графика, 1 — администрирование, 2 — управление
} PauseMenu;

void PauseMenuOpen(PauseMenu* menu);

// Обновляет состояние и собирает квады меню в ui.
// escapePressed — Esc в этом кадре (в настройках возвращает на главный
// экран, на главном — продолжает игру). dayLengthMinutes — длительность
// суток для подписи пресета «Обычная».
PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, float dayLengthMinutes,
    int32_t width, int32_t height, bool escapePressed);
