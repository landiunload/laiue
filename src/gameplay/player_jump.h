#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct PlayerJumpConfig
{
    double gravity;
    double maximumFallSpeed;
    double jumpHeight;
    double jumpBufferSeconds;
    double coyoteTimeSeconds;
    // Воздушные прыжки (управляются модами): дополнительные прыжки без
    // опоры, вертикальная скорость воздушного прыжка и правило
    // восстановления при касании земли.
    int32_t extraAirJumps;
    double airJumpImpulse;
    bool airJumpRefillOnGround;
} PlayerJumpConfig;

typedef struct PlayerJump
{
    PlayerJumpConfig config;
    double verticalVelocity;
    double jumpBufferRemaining;
    double coyoteTimeRemaining;
    double launchSpeed;
    int32_t airJumpsRemaining;
} PlayerJump;

void PlayerJumpInit(PlayerJump* jump, const PlayerJumpConfig* config);
void PlayerJumpReset(PlayerJump* jump);
// Рантайм-обновление правил воздушных прыжков (моды применяются вживую).
void PlayerJumpSetAirJumps(PlayerJump* jump,
    int32_t extraAirJumps, double airJumpImpulse, bool refillOnGround,
    bool grounded);
void PlayerJumpQueue(PlayerJump* jump);
void PlayerJumpObserveGround(PlayerJump* jump,
    bool supported, double stepSeconds);
bool PlayerJumpTryLaunch(PlayerJump* jump);
void PlayerJumpAgeBuffer(PlayerJump* jump, double stepSeconds);
void PlayerJumpLand(PlayerJump* jump);
void PlayerJumpHitCeiling(PlayerJump* jump);
