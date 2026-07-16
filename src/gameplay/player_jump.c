#include "gameplay/player_jump.h"

#include <emmintrin.h>

static double PositiveSquareRoot(double value)
{
    if (value <= 0.0)
    {
        return 0.0;
    }
    __m128d input = _mm_set_sd(value);
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), input));
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
    jump->airJumpsRemaining = jump->config.extraAirJumps;
}

void PlayerJumpSetAirJumps(PlayerJump* jump,
    int32_t extraAirJumps, double airJumpImpulse, bool refillOnGround,
    bool grounded)
{
    jump->config.extraAirJumps = extraAirJumps;
    jump->config.airJumpImpulse = airJumpImpulse;
    jump->config.airJumpRefillOnGround = refillOnGround;

    // На земле новый лимит доступен сразу; в воздухе остаток лишь
    // ужимается — включение мода не должно дарить прыжок в полёте.
    if (grounded)
    {
        jump->airJumpsRemaining = extraAirJumps;
    }
    else if (jump->airJumpsRemaining > extraAirJumps)
    {
        jump->airJumpsRemaining = extraAirJumps;
    }
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
        if (jump->config.airJumpRefillOnGround)
        {
            jump->airJumpsRemaining = jump->config.extraAirJumps;
        }
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
    if (jump->jumpBufferRemaining <= 0.0)
    {
        return false;
    }

    if (jump->coyoteTimeRemaining > 0.0)
    {
        jump->verticalVelocity = jump->launchSpeed;
        jump->jumpBufferRemaining = 0.0;
        jump->coyoteTimeRemaining = 0.0;
        return true;
    }

    // Воздушный прыжок (моды): отдельное нажатие в полёте.
    if (jump->airJumpsRemaining > 0)
    {
        jump->verticalVelocity = jump->config.airJumpImpulse;
        jump->jumpBufferRemaining = 0.0;
        jump->airJumpsRemaining--;
        return true;
    }

    return false;
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
    if (jump->config.airJumpRefillOnGround)
    {
        jump->airJumpsRemaining = jump->config.extraAirJumps;
    }
}

void PlayerJumpHitCeiling(PlayerJump* jump)
{
    if (jump->verticalVelocity > 0.0)
    {
        jump->verticalVelocity = 0.0;
    }
}
