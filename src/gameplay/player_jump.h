#pragma once

#include <stdbool.h>

typedef struct PlayerJumpConfig
{
    double gravity;
    double maximumFallSpeed;
    double jumpHeight;
    double jumpBufferSeconds;
    double coyoteTimeSeconds;
} PlayerJumpConfig;

typedef struct PlayerJump
{
    PlayerJumpConfig config;
    double verticalVelocity;
    double jumpBufferRemaining;
    double coyoteTimeRemaining;
    double launchSpeed;
} PlayerJump;

void PlayerJumpInit(PlayerJump* jump, const PlayerJumpConfig* config);
void PlayerJumpReset(PlayerJump* jump);
void PlayerJumpQueue(PlayerJump* jump);
void PlayerJumpObserveGround(PlayerJump* jump,
    bool supported, double stepSeconds);
bool PlayerJumpTryLaunch(PlayerJump* jump);
void PlayerJumpAgeBuffer(PlayerJump* jump, double stepSeconds);
void PlayerJumpLand(PlayerJump* jump);
void PlayerJumpHitCeiling(PlayerJump* jump);
