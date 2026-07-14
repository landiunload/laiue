#pragma once

#include <stdint.h>
#include <wchar.h>

#include "game/game_mode.h"
#include "game/player_controller.h"
#include "world/world.h"

void DebugOverlayBuildText(World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond,
    const int64_t cameraBlockPosition[3],
    wchar_t* destination, uint32_t capacity);
