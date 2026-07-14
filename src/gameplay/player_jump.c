#include "gameplay/player_jump.h"

#include <stdint.h>

static double PositiveSquareRoot(double value)
{
    if (value <= 0.0)
    {
        return 0.0;
    }

    double estimate = value >= 1.0 ? value : 1.0;
    for (uint32_t iteration = 0; iteration < 20u; ++iteration)
    {
        estimate = 0.5 * (estimate + value / estimate);
    }
    return estimate;
}

void PlayerJumpInit(PlayerJump* jump, const PlayerJumpConfig* config)
{
    jump->config = *config;
    jump->launchSpeed = PositiveSquareRoot(
        2.0 * config->gravity * config->jumpHeight);
    PlayerJumpReset(jump);
}

void PlayerJumpReset(PlayerJump* jump)
{
    jump->verticalVelocity = 0.0;
    jump->jumpBufferRemaining = 0.0;
    jump->coyoteTimeRemaining = 0.0;
}

void PlayerJumpQueue(PlayerJump* jump)
{
    jump->jumpBufferRemaining = jump->config.jumpBufferSeconds;
}

void PlayerJumpObserveGround(PlayerJump* jump,
    bool supported, double stepSeconds)
{
    if (supported && jump->verticalVelocity <= 0.0)
    {
        jump->verticalVelocity = 0.0;
        jump->coyoteTimeRemaining = jump->config.coyoteTimeSeconds;
        return;
    }

    jump->coyoteTimeRemaining -= stepSeconds;
    if (jump->coyoteTimeRemaining < 0.0)
    {
        jump->coyoteTimeRemaining = 0.0;
    }
}

bool PlayerJumpTryLaunch(PlayerJump* jump)
{
    if (jump->jumpBufferRemaining <= 0.0
        || jump->coyoteTimeRemaining <= 0.0)
    {
        return false;
    }

    jump->verticalVelocity = jump->launchSpeed;
    jump->jumpBufferRemaining = 0.0;
    jump->coyoteTimeRemaining = 0.0;
    return true;
}

void PlayerJumpAgeBuffer(PlayerJump* jump, double stepSeconds)
{
    jump->jumpBufferRemaining -= stepSeconds;
    if (jump->jumpBufferRemaining < 0.0)
    {
        jump->jumpBufferRemaining = 0.0;
    }
}

void PlayerJumpLand(PlayerJump* jump)
{
    jump->verticalVelocity = 0.0;
    jump->coyoteTimeRemaining = jump->config.coyoteTimeSeconds;
}

void PlayerJumpHitCeiling(PlayerJump* jump)
{
    if (jump->verticalVelocity > 0.0)
    {
        jump->verticalVelocity = 0.0;
    }
}
