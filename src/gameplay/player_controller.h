#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "game/camera.h"
#include "gameplay/player_jump.h"
#include "gameplay/player_locomotion.h"
#include "gameplay/player_stance.h"
#include "physics/voxel_body.h"

typedef VoxelCollisionSource PlayerCollisionSource;

typedef struct PlayerControllerConfig
{
    float walkingSpeed;
    float sprintingSpeed;
    float crouchingSpeed;
    float groundAcceleration;
    float groundDeceleration;
    float airAcceleration;
    float sprintJumpSpeed;
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
    double sneakProbeDepth;
    double crouchEyeDuration;
    double crouchColliderDuration;
    double standColliderDuration;
    double standEyeDuration;
} PlayerControllerConfig;

typedef struct PlayerControllerCommand
{
    // Нормализованное желаемое движение в мировых горизонтальных осях.
    double movementX;
    double movementY;
    bool jumpPressed;
    bool jumpHeld;
    bool sprintHeld;
    bool crouchHeld;
} PlayerControllerCommand;

typedef struct PlayerController
{
    PlayerControllerConfig config;
    PlayerStance stance;
    PlayerJump jump;
    PlayerLocomotion locomotion;
    double externalVelocityX;
    double externalVelocityY;
    double simulationAccumulator;
    bool grounded;
} PlayerController;

LAIUE_GAMEPLAY_API void PlayerControllerInit(
    PlayerController* controller, const PlayerControllerConfig* config);
LAIUE_GAMEPLAY_API void PlayerControllerReset(
    PlayerController* controller, Camera* camera);

// Возвращает true, если изменилось состояние, показываемое в интерфейсе.
LAIUE_GAMEPLAY_API bool PlayerControllerUpdate(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, float deltaSeconds);

LAIUE_GAMEPLAY_API bool PlayerControllerResolvePenetration(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera);
LAIUE_GAMEPLAY_API bool PlayerControllerOverlapsBlock(
    const PlayerController* controller,
    const Camera* camera, const int64_t block[3]);
LAIUE_GAMEPLAY_API void PlayerControllerGetBodyShape(
    const PlayerController* controller, VoxelBodyShape* outShape);

LAIUE_GAMEPLAY_API bool PlayerControllerIsGrounded(
    const PlayerController* controller);
LAIUE_GAMEPLAY_API bool PlayerControllerIsCrouching(
    const PlayerController* controller);

// Внешний толчок не проходит через sneak-защиту края.
LAIUE_GAMEPLAY_API void PlayerControllerApplyImpulse(
    PlayerController* controller, float x, float y, float z);
