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
    // Time is accumulated in double precision. Keeping the canonical
    // 1/240 value as float makes common render partitions (notably 30 and
    // 144 FPS) occasionally produce one substep too few.
    double fixedStepSeconds;
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

// Полный изменяемый результат fixed-step симуляции. Конфигурация в snapshot
// не входит: обе стороны соединения обязаны заранее согласовать один physics
// contract. Положение хранится вместе с внутренними скоростями и таймерами,
// поэтому состояние можно безопасно восстановить перед replay ввода.
typedef struct PlayerControllerState
{
    double position[3];
    double locomotionVelocityX;
    double locomotionVelocityY;
    double verticalVelocity;
    double externalVelocityX;
    double externalVelocityY;
    double jumpBufferRemaining;
    double coyoteTimeRemaining;
    double colliderCrouchProgress;
    double eyeCrouchProgress;
    int32_t airJumpsRemaining;
    bool crouchingRequested;
    bool grounded;
} PlayerControllerState;

// Единственный источник базовых правил движения для клиента и dedicated
// server. Вызывающая сторона может изменить копию до PlayerControllerInit.
LAIUE_GAMEPLAY_API void PlayerControllerGetDefaultConfig(
    PlayerControllerConfig* outConfig);

LAIUE_GAMEPLAY_API void PlayerControllerInit(
    PlayerController* controller, const PlayerControllerConfig* config);
LAIUE_GAMEPLAY_API void PlayerControllerReset(
    PlayerController* controller, Camera* camera);

// Возвращает true, если изменилось состояние, показываемое в интерфейсе.
LAIUE_GAMEPLAY_API bool PlayerControllerUpdate(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, double deltaSeconds);

// Исполняет ровно steps одинаковых fixed substeps. Эта функция не читает и
// не изменяет frame accumulator: сервер и prediction replay используют её с
// целым числом шагов. Edge-событие jumpPressed и запрос стойки принимаются
// один раз на команду, а held-флаги действуют на каждом substep.
LAIUE_GAMEPLAY_API bool PlayerControllerSimulateFixedSteps(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, uint32_t steps);

LAIUE_GAMEPLAY_API void PlayerControllerCaptureState(
    const PlayerController* controller, const Camera* camera,
    PlayerControllerState* outState);

// Проверяет snapshot целиком до изменения controller/camera. Неконечные и
// выходящие за безопасные границы значения отклоняются; accumulator после
// успешного authoritative restore всегда равен нулю.
LAIUE_GAMEPLAY_API bool PlayerControllerRestoreState(
    PlayerController* controller, Camera* camera,
    const PlayerControllerState* state);

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

// Правила воздушных прыжков (реестр параметров модов): применяется
// вживую; в полёте лимит только ужимается, дар прыжка не происходит.
LAIUE_GAMEPLAY_API void PlayerControllerSetAirJumps(
    PlayerController* controller, int32_t extraAirJumps,
    double airJumpImpulse, bool refillOnGround);
