#include "gameplay/player_controller.h"

#include "gameplay/player_jump.h"
#include "gameplay/player_stance.h"


#define PLAYER_PENETRATION_STEPS 256u


static VoxelBodyShape ActiveShape(const PlayerController* controller)
{
    return PlayerStanceGetShape(&controller->stance);
}

static bool HasGroundContact(const PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera)
{
    VoxelBodyShape shape = ActiveShape(controller);
    return VoxelBodyHasGroundContact(collision, camera->position,
        &shape, controller->config.groundProbeDepth);
}

static void RefreshGroundState(PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera,
    double stepSeconds)
{
    bool supported = HasGroundContact(controller, collision, camera);
    PlayerJumpObserveGround(&controller->jump, supported, stepSeconds);
    controller->grounded = supported
        && controller->jump.verticalVelocity <= 0.0;
}

static void TryLaunchQueuedJump(PlayerController* controller)
{
    if (PlayerJumpTryLaunch(&controller->jump))
    {
        controller->grounded = false;
    }
}

static void MoveVoluntary(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, double stepSeconds)
{
    double speed = PlayerStanceIsCrouching(&controller->stance)
        ? controller->config.crouchingSpeed
        : controller->config.walkingSpeed;
    double movementX = command->movementX * speed * stepSeconds;
    double movementY = command->movementY * speed * stepSeconds;
    VoxelBodyShape shape = ActiveShape(controller);

    // Minecraft-подобный sneak: добровольное движение обрезается до
    // последнего положения, под которым ещё существует опора.
    if (PlayerStanceIsCrouching(&controller->stance)
        && HasGroundContact(controller, collision, camera))
    {
        VoxelBodyClipSneakingMovement(collision, camera->position,
            &shape, controller->config.sneakProbeDepth,
            &movementX, &movementY);
    }

    VoxelBodyMoveAxis(
        collision, camera->position, &shape, 0, movementX);
    VoxelBodyMoveAxis(
        collision, camera->position, &shape, 1, movementY);
}

static void MoveExternalVelocity(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double stepSeconds)
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

    double damping = 1.0
        - (double)controller->config.externalVelocityDamping * stepSeconds;
    if (damping < 0.0)
    {
        damping = 0.0;
    }
    controller->externalVelocityX *= damping;
    controller->externalVelocityY *= damping;
}

// Возвращает true в точной вершине прыжка.
static bool IntegrateVertical(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double stepSeconds)
{
    if (controller->grounded
        && controller->jump.verticalVelocity == 0.0)
    {
        return false;
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
            return false;
        }

        controller->jump.verticalVelocity = 0.0;
        controller->grounded = false;
        return true;
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
        return false;
    }

    controller->jump.verticalVelocity = newVelocity;
    controller->grounded = false;
    return false;
}

static bool SimulateStep(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, double stepSeconds)
{
    RefreshGroundState(controller, collision, camera, stepSeconds);
    bool launched = PlayerJumpTryLaunch(&controller->jump);
    if (launched)
    {
        controller->grounded = false;
    }

    MoveVoluntary(controller, collision, camera, command, stepSeconds);
    MoveExternalVelocity(controller, collision, camera, stepSeconds);
    bool reachedApex = IntegrateVertical(
        controller, collision, camera, stepSeconds);

    if (!launched)
    {
        PlayerJumpAgeBuffer(&controller->jump, stepSeconds);
    }

    if (controller->jump.verticalVelocity <= 0.0
        && HasGroundContact(controller, collision, camera))
    {
        PlayerJumpLand(&controller->jump);
        controller->grounded = true;
    }
    return reachedApex;
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
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->grounded = false;
}

void PlayerControllerReset(PlayerController* controller, Camera* camera)
{
    PlayerStanceReset(&controller->stance, camera->position);
    PlayerJumpReset(&controller->jump);
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->grounded = false;
}

bool PlayerControllerUpdate(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, float deltaSeconds)
{
    bool presentationChanged = PlayerStanceTrySet(
        &controller->stance, collision,
        camera->position, command->crouchHeld);

    if (command->jumpPressed)
    {
        PlayerJumpQueue(&controller->jump);
    }

    // Нажатие обрабатывается немедленно, а не ждёт accumulation fixed-step.
    // Поэтому при сотнях тысяч render-кадров событие не может потеряться.
    RefreshGroundState(controller, collision, camera, 0.0);
    TryLaunchQueuedJump(controller);

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
        bool reachedApex = SimulateStep(controller,
            collision, camera, command, fixedStep);
        ++substep;
        if (reachedApex)
        {
            break;
        }
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
