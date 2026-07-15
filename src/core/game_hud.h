#pragma once

#include "core/ui.h"
#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Кешируемая модель HUD. Форматирование координат и значений выполняется
// только при изменении данных; квады всё равно собираются каждый кадр.
typedef struct GameHud
{
    bool initialized;
    int64_t blockPosition[3];
    uint32_t framesPerSecond;
    uint32_t timeMinutes;
    GameMode gameMode;
    bool crouching;

    int32_t measuredPixelSize;
    float panelWidth;

    wchar_t coordinateText[3][32];
    wchar_t framesPerSecondText[16];
    wchar_t timeText[8];
    wchar_t modeText[16];
} GameHud;

void GameHudInit(GameHud* hud);

void GameHudDraw(GameHud* hud, UiContext* ui,
    World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond, uint32_t timeMinutes,
    const int64_t cameraBlockPosition[3],
    int32_t viewportWidth, int32_t viewportHeight);
