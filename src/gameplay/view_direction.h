#pragma once

#include "api.h"

// Shared deterministic view basis used by client prediction and the
// authoritative server. It intentionally avoids platform libm functions.
LAIUE_GAMEPLAY_API void GameplayViewForward(
    float yaw, float pitch, float outForward[3]);
