#pragma once

#include "core/ui.h"
#include "gameplay/game_mode.h"
#include "gameplay/inventory.h"

#include <stdbool.h>
#include <stdint.h>

void InventoryUiDraw(UiContext* ui, Inventory* inventory, GameMode gameMode,
    bool inventoryOpen, bool allowRearrange, float breakingProgress,
    int32_t viewportWidth, int32_t viewportHeight);
