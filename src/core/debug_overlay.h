#pragma once

#include <stdint.h>
#include <wchar.h>

#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "world/world.h"

// timeMinutes — игровое время суток в минутах 0..1439 (строка «HH:MM»).
void DebugOverlayBuildText(World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond, uint32_t timeMinutes,
    const int64_t cameraBlockPosition[3],
    wchar_t* destination, uint32_t capacity);
