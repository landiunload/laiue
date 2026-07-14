#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game/camera.h"
#include "physics/voxel_body.h"

typedef VoxelSolidBlockQuery PlayerSolidBlockQuery;
typedef VoxelCollisionSource PlayerCollisionSource;

typedef struct PlayerControllerConfig
{
    float walkingSpeed;
    float crouchingSpeed;
    float gravity;
    float maximumFallSpeed;
    float jumpBufferSeconds;
    float coyoteTimeSeconds;
    float externalVelocityDamping;
    float fixedStepSeconds;
    uint32_t maximumSubsteps;
    double jumpHeight;
    double radius;
    double standingHeight;
    double standingEyeHeight;
    double crouchingHeight;
    double crouchingEyeHeight;
    double collisionEpsilon;
    double groundProbeDepth;
    double ledgeSupportRadius;
} PlayerControllerConfig;

typedef struct PlayerControllerCommand
{
    float forward;
    float right;
    bool jumpPressed;
    bool jumpHeld;
    bool crouchHeld;
} PlayerControllerCommand;

typedef struct PlayerController
{
    PlayerControllerConfig config;
    double verticalVelocity;
    double externalVelocityX;
    double externalVelocityY;
    double jumpBufferRemaining;
    double coyoteTimeRemaining;
    double simulationAccumulator;
    double jumpLaunchSpeed;
    bool grounded;
    bool crouching;
    bool jumpHeldPrevious;
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
