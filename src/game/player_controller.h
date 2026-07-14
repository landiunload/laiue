#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game/camera.h"

typedef bool (*PlayerSolidBlockQuery)(
    void* context, int64_t x, int64_t y, int64_t z);

typedef struct PlayerCollisionSource
{
    void* context;
    PlayerSolidBlockQuery isSolidBlock;
} PlayerCollisionSource;

typedef struct PlayerControllerConfig
{
    float walkingSpeed;
    float crouchingSpeed;
    float gravity;
    float jumpSpeed;
    float maximumFallSpeed;
    float jumpBufferSeconds;
    float coyoteTimeSeconds;
    float externalVelocityDamping;
    double radius;
    double standingHeight;
    double standingEyeHeight;
    double crouchingHeight;
    double crouchingEyeHeight;
    double collisionEpsilon;
    double groundProbeDepth;
} PlayerControllerConfig;

typedef struct PlayerControllerCommand
{
    float forward;
    float right;
    bool jumpPressed;
    bool crouchHeld;
} PlayerControllerCommand;

typedef struct PlayerController
{
    PlayerControllerConfig config;
    float verticalVelocity;
    float externalVelocityX;
    float externalVelocityY;
    float jumpBufferRemaining;
    float coyoteTimeRemaining;
    bool grounded;
    bool crouching;
} PlayerController;

void PlayerControllerInit(
    PlayerController* controller, const PlayerControllerConfig* config);
void PlayerControllerReset(PlayerController* controller, Camera* camera);

// Возвращает true, когда изменилось состояние, отображаемое в UI.
bool PlayerControllerUpdate(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command,
    float yaw, float deltaSeconds);

bool PlayerControllerResolvePenetration(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera);

bool PlayerControllerOverlapsBlock(const PlayerController* controller,
    const Camera* camera, const int64_t block[3]);

bool PlayerControllerIsGrounded(const PlayerController* controller);
bool PlayerControllerIsCrouching(const PlayerController* controller);

// Внешний толчок намеренно обходит защиту края при приседании.
// Это граница для будущих мобов, взрывов и движущихся платформ.
void PlayerControllerApplyImpulse(
    PlayerController* controller, float x, float y, float z);
