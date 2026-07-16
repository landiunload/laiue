#include "gameplay/player_controller.h"

#include "gameplay/player_jump.h"
#include "gameplay/player_stance.h"


#define PLAYER_PENETRATION_STEPS 256u


static VoxelBodyShape ActiveShape(const PlayerController* controller)
{
    return PlayerStanceGetShape(&controller->stance);
}

static void RefreshGroundState(PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera,
    double stepSeconds, VoxelGroundContact* outContact)
{
    VoxelBodyShape shape = ActiveShape(controller);
    VoxelBodyQueryGroundContact(collision, camera->position,
        &shape, controller->config.groundProbeDepth, outContact);
    PlayerJumpObserveGround(
        &controller->jump, outContact->supported, stepSeconds);
    controller->grounded = outContact->supported
        && controller->jump.verticalVelocity <= 0.0;
}

static bool TryLaunchQueuedJump(PlayerController* controller,
    const PlayerControllerCommand* command)
{
    if (!PlayerJumpTryLaunch(&controller->jump))
    {
        return false;
    }

    controller->grounded = false;
    if (command->sprintHeld
        && !PlayerStanceIsCrouching(&controller->stance))
    {
        PlayerLocomotionApplySprintJumpImpulse(&controller->locomotion,
            command->movementX, command->movementY,
            controller->config.sprintingSpeed);
    }
    return true;
}

static void MoveVoluntary(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, bool supported,
    double surfaceFriction, double stepSeconds)
{
    double standingSpeed = command->sprintHeld
        ? controller->config.sprintingSpeed
        : controller->config.walkingSpeed;
    double crouchAmount = PlayerStanceGetCrouchAmount(
        &controller->stance);
    double groundTargetSpeed = standingSpeed
        + ((double)controller->config.crouchingSpeed - standingSpeed)
            * crouchAmount;
    double targetSpeed = controller->grounded
        ? groundTargetSpeed
        : controller->config.walkingSpeed;

    double movementX;
    double movementY;
    PlayerLocomotionStep(&controller->locomotion,
        command->movementX, command->movementY, targetSpeed,
        controller->grounded, surfaceFriction, stepSeconds,
        &movementX, &movementY);
    VoxelBodyShape shape = ActiveShape(controller);

    // Minecraft-подобный sneak: добровольное движение обрезается до
    // последнего положения, под которым ещё существует опора.
    if (PlayerStanceIsCrouching(&controller->stance)
        && supported)
    {
        double unclippedMovementX = movementX;
        double unclippedMovementY = movementY;
        VoxelBodyClipSneakingMovement(collision, camera->position,
            &shape, controller->config.sneakProbeDepth,
            &movementX, &movementY);
        if (movementX != unclippedMovementX)
        {
            PlayerLocomotionStopAxis(&controller->locomotion, 0);
        }
        if (movementY != unclippedMovementY)
        {
            PlayerLocomotionStopAxis(&controller->locomotion, 1);
        }
    }

    if (VoxelBodyMoveAxis(
            collision, camera->position, &shape, 0, movementX))
    {
        PlayerLocomotionStopAxis(&controller->locomotion, 0);
    }
    if (VoxelBodyMoveAxis(
            collision, camera->position, &shape, 1, movementY))
    {
        PlayerLocomotionStopAxis(&controller->locomotion, 1);
    }
}

// Внешние толчки движутся отдельно от управляемой скорости: sneak не должен
// приклеивать вытолкнутого игрока к краю блока.
static void MoveExternalVelocity(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double surfaceFriction, double stepSeconds)
{
    VoxelBodyShape shape = ActiveShape(controller);

    if (controller->externalVelocityX != 0.0
        && VoxelBodyMoveAxis(collision, camera->position, &shape, 0,
            controller->externalVelocityX * stepSeconds))
    {
        controller->externalVelocityX = 0.0;
    }
    if (controller->externalVelocityY != 0.0
        && VoxelBodyMoveAxis(collision, camera->position, &shape, 1,
            controller->externalVelocityY * stepSeconds))
    {
        controller->externalVelocityY = 0.0;
    }

    double dampingScale = controller->grounded ? surfaceFriction : 1.0;
    if (dampingScale < 0.0) dampingScale = 0.0;
    if (dampingScale > 1.0) dampingScale = 1.0;
    double damping = 1.0
        - (double)controller->config.externalVelocityDamping
            * dampingScale * stepSeconds;
    if (damping < 0.0)
    {
        damping = 0.0;
    }
    controller->externalVelocityX *= damping;
    controller->externalVelocityY *= damping;
}

// При переходе через вершину оба участка движения
// интегрируются внутри того же fixed-step.
static void IntegrateVertical(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double stepSeconds)
{
    if (controller->grounded
        && controller->jump.verticalVelocity == 0.0)
    {
        return;
    }

    double gravity = controller->jump.config.gravity;
    double maximumFallSpeed = controller->jump.config.maximumFallSpeed;
    double oldVelocity = controller->jump.verticalVelocity;
    if (oldVelocity < -maximumFallSpeed)
    {
        oldVelocity = -maximumFallSpeed;
    }

    double newVelocity = oldVelocity - gravity * stepSeconds;
    VoxelBodyShape shape = ActiveShape(controller);

    if (oldVelocity > 0.0 && newVelocity <= 0.0)
    {
        double timeToApex = oldVelocity / gravity;
        double distanceToApex = oldVelocity * timeToApex
            - 0.5 * gravity * timeToApex * timeToApex;
        if (VoxelBodyMoveAxis(collision, camera->position,
                &shape, 2, distanceToApex))
        {
            PlayerJumpHitCeiling(&controller->jump);
            return;
        }

        double remainingSeconds = stepSeconds - timeToApex;
        if (remainingSeconds < 0.0)
        {
            remainingSeconds = 0.0;
        }
        double distanceFromApex =
            -0.5 * gravity * remainingSeconds * remainingSeconds;
        if (VoxelBodyMoveAxis(collision, camera->position,
                &shape, 2, distanceFromApex))
        {
            PlayerJumpLand(&controller->jump);
            controller->grounded = true;
            return;
        }

        controller->jump.verticalVelocity =
            -gravity * remainingSeconds;
        controller->grounded = false;
        return;
    }

    double distance;
    if (newVelocity < -maximumFallSpeed)
    {
        double timeToTerminal =
            (oldVelocity + maximumFallSpeed) / gravity;
        if (timeToTerminal < 0.0) timeToTerminal = 0.0;
        if (timeToTerminal > stepSeconds) timeToTerminal = stepSeconds;

        distance = oldVelocity * timeToTerminal
            - 0.5 * gravity * timeToTerminal * timeToTerminal
            - maximumFallSpeed * (stepSeconds - timeToTerminal);
        newVelocity = -maximumFallSpeed;
    }
    else
    {
        distance = oldVelocity * stepSeconds
            - 0.5 * gravity * stepSeconds * stepSeconds;
    }

    if (VoxelBodyMoveAxis(collision, camera->position,
            &shape, 2, distance))
    {
        if (distance < 0.0)
        {
            PlayerJumpLand(&controller->jump);
            controller->grounded = true;
        }
        else
        {
            PlayerJumpHitCeiling(&controller->jump);
            controller->grounded = false;
        }
        return;
    }

    controller->jump.verticalVelocity = newVelocity;
    controller->grounded = false;
}

static void SimulateStep(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, double stepSeconds,
    bool* presentationChanged)
{
    if (PlayerStanceStep(&controller->stance,
            collision, camera->position, stepSeconds))
    {
        *presentationChanged = true;
    }

    VoxelGroundContact groundContact;
    RefreshGroundState(controller, collision, camera,
        stepSeconds, &groundContact);

    // Удержание прыжка ставит новый запрос только при наличии опоры.
    // Поэтому следующий прыжок начинается после приземления, а не в воздухе.
    if (command->jumpHeld && controller->grounded)
    {
        PlayerJumpQueue(&controller->jump);
    }
    MoveVoluntary(controller, collision, camera, command,
        groundContact.supported, groundContact.friction, stepSeconds);
    bool launched = TryLaunchQueuedJump(controller, command);
    MoveExternalVelocity(controller, collision, camera,
        groundContact.friction, stepSeconds);
    IntegrateVertical(controller, collision, camera, stepSeconds);

    if (!launched)
    {
        PlayerJumpAgeBuffer(&controller->jump, stepSeconds);
    }
}

void PlayerControllerInit(PlayerController* controller,
    const PlayerControllerConfig* config)
{
    controller->config = *config;
    PlayerStanceConfig stanceConfig = {
        .radius = config->radius,
        .standingHeight = config->standingHeight,
        .standingEyeHeight = config->standingEyeHeight,
        .crouchingHeight = config->crouchingHeight,
        .crouchingEyeHeight = config->crouchingEyeHeight,
        .collisionEpsilon = config->collisionEpsilon,
        .crouchEyeDuration = config->crouchEyeDuration,
        .crouchColliderDuration = config->crouchColliderDuration,
        .standColliderDuration = config->standColliderDuration,
        .standEyeDuration = config->standEyeDuration,
    };
    PlayerStanceInit(&controller->stance, &stanceConfig);

    PlayerJumpConfig jumpConfig = {
        .gravity = config->gravity,
        .maximumFallSpeed = config->maximumFallSpeed,
        .jumpHeight = config->jumpHeight,
        .jumpBufferSeconds = config->jumpBufferSeconds,
        .coyoteTimeSeconds = config->coyoteTimeSeconds,
    };
    PlayerJumpInit(&controller->jump, &jumpConfig);

    PlayerLocomotionConfig locomotionConfig = {
        .groundAcceleration = config->groundAcceleration,
        .groundDeceleration = config->groundDeceleration,
        .airAcceleration = config->airAcceleration,
        .sprintJumpSpeed = config->sprintJumpSpeed,
    };
    PlayerLocomotionInit(&controller->locomotion, &locomotionConfig);
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->grounded = false;
}

void PlayerControllerReset(PlayerController* controller, Camera* camera)
{
    PlayerStanceReset(&controller->stance, camera->position);
    PlayerJumpReset(&controller->jump);
    PlayerLocomotionReset(&controller->locomotion);
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->grounded = false;
}

bool PlayerControllerUpdate(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, float deltaSeconds)
{
    bool presentationChanged = PlayerStanceSetCrouching(
        &controller->stance, command->crouchHeld);

    // Нажатие не теряется между render-кадрами, но сам запуск
    // выполняется в fixed-step после текущего шага разгона.
    if (command->jumpPressed)
    {
        PlayerJumpQueue(&controller->jump);
    }

    double fixedStep = controller->config.fixedStepSeconds;
    if (fixedStep <= 0.0)
    {
        fixedStep = 1.0 / 240.0;
    }
    uint32_t maximumSubsteps = controller->config.maximumSubsteps;
    if (maximumSubsteps == 0u)
    {
        maximumSubsteps = 32u;
    }

    if (deltaSeconds > 0.0f)
    {
        controller->simulationAccumulator += deltaSeconds;
    }
    double maximumAccumulator = fixedStep * (double)maximumSubsteps;
    if (controller->simulationAccumulator > maximumAccumulator)
    {
        controller->simulationAccumulator = maximumAccumulator;
    }

    uint32_t substep = 0;
    while (controller->simulationAccumulator + 1e-12 >= fixedStep
        && substep < maximumSubsteps)
    {
        controller->simulationAccumulator -= fixedStep;
        SimulateStep(controller,
            collision, camera, command, fixedStep,
            &presentationChanged);
        ++substep;
    }

    return presentationChanged;
}

bool PlayerControllerResolvePenetration(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera)
{
    VoxelBodyShape shape = ActiveShape(controller);
    for (uint32_t step = 0; step < PLAYER_PENETRATION_STEPS; ++step)
    {
        if (!VoxelBodyCollides(collision, camera->position, &shape))
        {
            return true;
        }
        camera->position[2] += 1.0;
    }
    return !VoxelBodyCollides(collision, camera->position, &shape);
}

bool PlayerControllerOverlapsBlock(const PlayerController* controller,
    const Camera* camera, const int64_t block[3])
{
    VoxelBodyShape shape = ActiveShape(controller);
    return VoxelBodyOverlapsBlock(camera->position, &shape, block);
}

void PlayerControllerGetBodyShape(const PlayerController* controller,
    VoxelBodyShape* outShape)
{
    *outShape = ActiveShape(controller);
}

bool PlayerControllerIsGrounded(const PlayerController* controller)
{
    return controller->grounded;
}

bool PlayerControllerIsCrouching(const PlayerController* controller)
{
    return PlayerStanceIsCrouching(&controller->stance);
}

void PlayerControllerApplyImpulse(PlayerController* controller,
    float x, float y, float z)
{
    controller->externalVelocityX += x;
    controller->externalVelocityY += y;
    controller->jump.verticalVelocity += z;
    if (z > 0.0f)
    {
        controller->grounded = false;
        controller->jump.coyoteTimeRemaining = 0.0;
    }
}

void PlayerControllerSetAirJumps(PlayerController* controller,
    int32_t extraAirJumps, double airJumpImpulse, bool refillOnGround)
{
    PlayerJumpSetAirJumps(&controller->jump,
        extraAirJumps, airJumpImpulse, refillOnGround,
        controller->grounded);
}
