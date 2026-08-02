#include "gameplay/player_controller.h"

#include "gameplay/player_jump.h"
#include "gameplay/player_stance.h"

#include <string.h>

#define PLAYER_PENETRATION_STEPS 256u
#define PLAYER_STATE_POSITION_LIMIT 1099511627776.0
#define PLAYER_STATE_VELOCITY_LIMIT 1048576.0
#define PLAYER_STATE_TIMER_LIMIT 3600.0
#define PLAYER_STATE_AIR_JUMP_LIMIT 4096
#define PLAYER_STATE_TIMER_EPSILON 1e-9
#define PLAYER_CANONICAL_VELOCITY_EPSILON 1e-12
#define PLAYER_PLATFORM_MAX_DISPLACEMENT_PER_SUBSTEP 0.5


static VoxelBodyShape ActiveShape(const PlayerController* controller)
{
    return PlayerStanceGetShape(&controller->stance);
}

static double CanonicalVelocity(double value)
{
    return value > -PLAYER_CANONICAL_VELOCITY_EPSILON
            && value < PLAYER_CANONICAL_VELOCITY_EPSILON
        ? 0.0 : value;
}

static bool IsFiniteBounded(double value, double limit)
{
    return value == value && value >= -limit && value <= limit;
}

static bool SnapDownToGround(const PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const VoxelBodyShape* shape)
{
    double probeDepth = controller->config.groundProbeDepth;
    if (probeDepth <= 0.0)
    {
        return false;
    }

    double originalZ = camera->position[2];
    if (VoxelBodyMoveAxis(collision, camera->position,
            shape, 2, -probeDepth))
    {
        return true;
    }

    // Query и sweep используют один collision view, поэтому штатно этот
    // путь недостижим. Сохраняем atomic semantics даже для ошибочного
    // callback, изменившего мир между запросами.
    camera->position[2] = originalZ;
    return false;
}

static double BoundedPlatformDisplacement(double velocity,
    double stepSeconds);

static void RefreshGroundState(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double stepSeconds, VoxelGroundContact* outContact)
{
    VoxelBodyShape shape = ActiveShape(controller);
    VoxelBodyQueryGroundContact(collision, camera->position,
        &shape, controller->config.groundProbeDepth, outContact);

    // Probe только обнаруживает поверхность. Падающее тело становится
    // grounded лишь после детерминированного sweep к реальному контакту:
    // иначе оно могло навсегда зависнуть в пределах groundProbeDepth.
    bool supported = outContact->supported
        && controller->jump.verticalVelocity <= 0.0
        && SnapDownToGround(controller, collision, camera, &shape);
    outContact->supported = supported;
    PlayerJumpObserveGround(
        &controller->jump, supported, stepSeconds);
    controller->grounded = supported;
}

static bool TryLaunchQueuedJump(PlayerController* controller,
    const PlayerControllerCommand* command,
    const VoxelGroundContact* groundContact, double stepSeconds)
{
    if (!PlayerJumpTryLaunch(&controller->jump))
    {
        return false;
    }

    controller->grounded = false;
    if (groundContact->supported
        && groundContact->surfaceStableId != 0u)
    {
        // Once the feet leave a moving surface its horizontal world velocity
        // becomes ordinary external momentum instead of disappearing.
        double inheritedX = BoundedPlatformDisplacement(
            groundContact->surfaceVelocity[0], stepSeconds);
        double inheritedY = BoundedPlatformDisplacement(
            groundContact->surfaceVelocity[1], stepSeconds);
        if (stepSeconds > 0.0)
        {
            controller->externalVelocityX += inheritedX / stepSeconds;
            controller->externalVelocityY += inheritedY / stepSeconds;
        }
    }
    if (command->sprintHeld
        && !PlayerStanceIsCrouching(&controller->stance))
    {
        PlayerLocomotionApplySprintJumpImpulse(&controller->locomotion,
            command->movementX, command->movementY,
            controller->config.sprintingSpeed);
    }
    return true;
}

static double BoundedPlatformDisplacement(double velocity,
    double stepSeconds)
{
    if (!IsFiniteBounded(velocity, PLAYER_STATE_VELOCITY_LIMIT)
        || !IsFiniteBounded(stepSeconds, PLAYER_STATE_TIMER_LIMIT)
        || stepSeconds <= 0.0)
    {
        return 0.0;
    }
    double distance = velocity * stepSeconds;
    if (distance > PLAYER_PLATFORM_MAX_DISPLACEMENT_PER_SUBSTEP)
    {
        return PLAYER_PLATFORM_MAX_DISPLACEMENT_PER_SUBSTEP;
    }
    if (distance < -PLAYER_PLATFORM_MAX_DISPLACEMENT_PER_SUBSTEP)
    {
        return -PLAYER_PLATFORM_MAX_DISPLACEMENT_PER_SUBSTEP;
    }
    return distance;
}

static void MoveWithGroundSurface(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const VoxelGroundContact* groundContact, double stepSeconds)
{
    if (!groundContact->supported
        || groundContact->surfaceStableId == 0u)
    {
        return;
    }

    VoxelBodyShape shape = ActiveShape(controller);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double distance = BoundedPlatformDisplacement(
            groundContact->surfaceVelocity[axis], stepSeconds);
        if (distance != 0.0)
        {
            // The regular exact sweep also clips against walls, ceilings and
            // other moving bodies. A very fast platform can therefore never
            // teleport a passenger through geometry.
            VoxelBodyMoveAxis(collision, camera->position,
                &shape, axis, distance);
        }
    }
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
        // Horizontal movement или внешний импульс могли увести тело с края
        // уже после начального ground probe. Повторная проверка одновременно
        // даёт bounded ground snap и запускает падение в том же substep.
        VoxelBodyShape shape = ActiveShape(controller);
        VoxelGroundContact contact;
        VoxelBodyQueryGroundContact(collision, camera->position,
            &shape, controller->config.groundProbeDepth, &contact);
        if (contact.supported
            && SnapDownToGround(controller, collision, camera, &shape))
        {
            return;
        }
        controller->grounded = false;
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
    bool launched = TryLaunchQueuedJump(
        controller, command, &groundContact, stepSeconds);
    if (!launched)
    {
        MoveWithGroundSurface(controller, collision, camera,
            &groundContact, stepSeconds);
    }
    MoveExternalVelocity(controller, collision, camera,
        groundContact.friction, stepSeconds);
    IntegrateVertical(controller, collision, camera, stepSeconds);

    if (!launched)
    {
        PlayerJumpAgeBuffer(&controller->jump, stepSeconds);
    }

    controller->locomotion.velocityX =
        CanonicalVelocity(controller->locomotion.velocityX);
    controller->locomotion.velocityY =
        CanonicalVelocity(controller->locomotion.velocityY);
    controller->externalVelocityX =
        CanonicalVelocity(controller->externalVelocityX);
    controller->externalVelocityY =
        CanonicalVelocity(controller->externalVelocityY);
    controller->jump.verticalVelocity =
        CanonicalVelocity(controller->jump.verticalVelocity);
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
    const PlayerControllerCommand* command, double deltaSeconds)
{
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

    uint32_t steps = 0;
    while (controller->simulationAccumulator + 1e-12 >= fixedStep
        && steps < maximumSubsteps)
    {
        controller->simulationAccumulator -= fixedStep;
        ++steps;
    }

    return PlayerControllerSimulateFixedSteps(controller,
        collision, camera, command, steps);
}

bool PlayerControllerSimulateFixedSteps(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, uint32_t steps)
{
    bool presentationChanged = PlayerStanceSetCrouching(
        &controller->stance, command->crouchHeld);

    // Edge принимается ровно один раз на command, даже если command
    // исполняется четырьмя physics substeps серверного тика.
    if (command->jumpPressed)
    {
        PlayerJumpQueue(&controller->jump);
    }

    double fixedStep = controller->config.fixedStepSeconds;
    if (fixedStep <= 0.0)
    {
        fixedStep = 1.0 / 240.0;
    }
    for (uint32_t step = 0; step < steps; ++step)
    {
        SimulateStep(controller,
            collision, camera, command, fixedStep,
            &presentationChanged);
    }
    return presentationChanged;
}

void PlayerControllerCaptureState(const PlayerController* controller,
    const Camera* camera, PlayerControllerState* outState)
{
    outState->position[0] = camera->position[0];
    outState->position[1] = camera->position[1];
    outState->position[2] = camera->position[2];
    outState->locomotionVelocityX = controller->locomotion.velocityX;
    outState->locomotionVelocityY = controller->locomotion.velocityY;
    outState->verticalVelocity = controller->jump.verticalVelocity;
    outState->externalVelocityX = controller->externalVelocityX;
    outState->externalVelocityY = controller->externalVelocityY;
    outState->jumpBufferRemaining = controller->jump.jumpBufferRemaining;
    outState->coyoteTimeRemaining = controller->jump.coyoteTimeRemaining;
    outState->colliderCrouchProgress =
        controller->stance.colliderCrouchProgress;
    outState->eyeCrouchProgress = controller->stance.eyeCrouchProgress;
    outState->airJumpsRemaining = controller->jump.airJumpsRemaining;
    outState->crouchingRequested = controller->stance.crouchingRequested;
    outState->grounded = controller->grounded;
}

bool PlayerControllerRestoreState(PlayerController* controller,
    Camera* camera, const PlayerControllerState* state)
{
    for (uint32_t axis = 0; axis < 3u; ++axis)
    {
        if (!IsFiniteBounded(
                state->position[axis], PLAYER_STATE_POSITION_LIMIT))
        {
            return false;
        }
    }

    const double velocities[] = {
        state->locomotionVelocityX,
        state->locomotionVelocityY,
        state->verticalVelocity,
        state->externalVelocityX,
        state->externalVelocityY,
    };
    for (uint32_t index = 0;
        index < sizeof(velocities) / sizeof(velocities[0]); ++index)
    {
        if (!IsFiniteBounded(
                velocities[index], PLAYER_STATE_VELOCITY_LIMIT))
        {
            return false;
        }
    }

    double maximumJumpBuffer = controller->jump.config.jumpBufferSeconds;
    double maximumCoyoteTime = controller->jump.config.coyoteTimeSeconds;
    if (!IsFiniteBounded(maximumJumpBuffer, PLAYER_STATE_TIMER_LIMIT)
        || !IsFiniteBounded(maximumCoyoteTime, PLAYER_STATE_TIMER_LIMIT))
    {
        return false;
    }
    if (maximumJumpBuffer < 0.0) maximumJumpBuffer = 0.0;
    if (maximumCoyoteTime < 0.0) maximumCoyoteTime = 0.0;
    if (!IsFiniteBounded(
            state->jumpBufferRemaining, PLAYER_STATE_TIMER_LIMIT)
        || state->jumpBufferRemaining
            > maximumJumpBuffer + PLAYER_STATE_TIMER_EPSILON
        || state->jumpBufferRemaining < 0.0
        || !IsFiniteBounded(
            state->coyoteTimeRemaining, PLAYER_STATE_TIMER_LIMIT)
        || state->coyoteTimeRemaining
            > maximumCoyoteTime + PLAYER_STATE_TIMER_EPSILON
        || state->coyoteTimeRemaining < 0.0
        || !IsFiniteBounded(state->colliderCrouchProgress, 1.0)
        || state->colliderCrouchProgress < 0.0
        || !IsFiniteBounded(state->eyeCrouchProgress, 1.0)
        || state->eyeCrouchProgress < 0.0)
    {
        return false;
    }

    int32_t maximumAirJumps = controller->jump.config.extraAirJumps;
    if (maximumAirJumps < 0) maximumAirJumps = 0;
    if (maximumAirJumps > PLAYER_STATE_AIR_JUMP_LIMIT)
    {
        maximumAirJumps = PLAYER_STATE_AIR_JUMP_LIMIT;
    }
    if (state->airJumpsRemaining < 0
        || state->airJumpsRemaining > maximumAirJumps
        || (state->grounded
            && state->verticalVelocity
                > PLAYER_CANONICAL_VELOCITY_EPSILON))
    {
        return false;
    }

    camera->position[0] = state->position[0];
    camera->position[1] = state->position[1];
    camera->position[2] = state->position[2];
    controller->locomotion.velocityX =
        CanonicalVelocity(state->locomotionVelocityX);
    controller->locomotion.velocityY =
        CanonicalVelocity(state->locomotionVelocityY);
    controller->jump.verticalVelocity =
        CanonicalVelocity(state->verticalVelocity);
    controller->externalVelocityX =
        CanonicalVelocity(state->externalVelocityX);
    controller->externalVelocityY =
        CanonicalVelocity(state->externalVelocityY);
    controller->jump.jumpBufferRemaining =
        state->jumpBufferRemaining == 0.0
        ? 0.0 : state->jumpBufferRemaining;
    controller->jump.coyoteTimeRemaining =
        state->coyoteTimeRemaining == 0.0
        ? 0.0 : state->coyoteTimeRemaining;
    controller->stance.colliderCrouchProgress =
        state->colliderCrouchProgress == 0.0
        ? 0.0 : state->colliderCrouchProgress;
    controller->stance.eyeCrouchProgress =
        state->eyeCrouchProgress == 0.0
        ? 0.0 : state->eyeCrouchProgress;
    controller->jump.airJumpsRemaining = state->airJumpsRemaining;
    controller->stance.crouchingRequested = state->crouchingRequested;
    controller->grounded = state->grounded;
    controller->simulationAccumulator = 0.0;
    return true;
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
void PlayerControllerGetDefaultConfig(PlayerControllerConfig* outConfig)
{
    if (outConfig == NULL)
    {
        return;
    }
    const PlayerControllerConfig defaults = {
        .walkingSpeed = 4.0f,
        .sprintingSpeed = 6.0f,
        .crouchingSpeed = 3.5f,
        .groundAcceleration = 20.0f,
        .groundDeceleration = 30.0f,
        .airAcceleration = 4.0f,
        .sprintJumpSpeed = 8.0f,
        .gravity = 26.0f,
        .maximumFallSpeed = 55.0f,
        .jumpBufferSeconds = 0.14f,
        .coyoteTimeSeconds = 0.10f,
        .externalVelocityDamping = 8.0f,
        .fixedStepSeconds = 1.0 / 240.0,
        .maximumSubsteps = 32u,
        .jumpHeight = 1.275,
        .radius = 0.30,
        .standingHeight = 1.80,
        .standingEyeHeight = 1.75,
        .crouchingHeight = 1.30,
        .crouchingEyeHeight = 1.25,
        .collisionEpsilon = 0.001,
        .groundProbeDepth = 0.03,
        .sneakProbeDepth = 0.60,
        .crouchEyeDuration = 0.175,
        .crouchColliderDuration = 0.200,
        .standColliderDuration = 0.200,
        .standEyeDuration = 0.250,
    };
    memcpy(outConfig, &defaults, sizeof(*outConfig));
}
