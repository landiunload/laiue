#pragma once

#include "core/panorama.h"
#include "core/ui.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Меню паузы (внутренний компонент ядра): открывается по Esc,
// экраны «Пауза» и «Настройки». Настройки применяются сразу.

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
    int32_t fovDegrees;           // ползунок 0..360 (рендер поднимает до >= 1)
    RenderProjection projection;  // выбор рендера
} GameSettings;

typedef struct PauseMenu
{
    PauseMenuScreen screen;
} PauseMenu;

void PauseMenuOpen(PauseMenu* menu);

// Обновляет состояние и собирает квады меню в ui.
// escapePressed — Esc в этом кадре (в настройках возвращает на главный
// экран, на главном — продолжает игру).
PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer,
    int32_t width, int32_t height, bool escapePressed);
