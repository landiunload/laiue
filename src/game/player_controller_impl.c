#include "game/player_controller.h"

#include "core/math.h"

#define PLAYER_PENETRATION_STEPS 256u
#define LEDGE_SEARCH_ITERATIONS 24u

static double PositiveSquareRoot(double value)
{
    if (value <= 0.0)
    {
        return 0.0;
    }

    double estimate = value >= 1.0 ? value : 1.0;
    for (uint32_t iteration = 0; iteration < 16u; ++iteration)
    {
        estimate = 0.5 * (estimate + value / estimate);
    }
    return estimate;
}

static double ActiveHeight(const PlayerController* controller)
{
    return controller->crouching
        ? controller->config.crouchingHeight
        : controller->config.standingHeight;
}

static double ActiveEyeHeight(const PlayerController* controller)
{
    return controller->crouching
        ? controller->config.crouchingEyeHeight
        : controller->config.standingEyeHeight;
}

static VoxelBodyShape ActiveShape(const PlayerController* controller)
{
    VoxelBodyShape shape = {
        .radius = controller->config.radius,
        .height = ActiveHeight(controller),
        .eyeHeight = ActiveEyeHeight(controller),
        .collisionEpsilon = controller->config.collisionEpsilon,
    };
    return shape;
}

static bool HasGroundContact(const PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera)
{
    VoxelBodyShape shape = ActiveShape(controller);
    return VoxelBodyHasGroundContact(collision, camera->position,
        &shape, controller->config.groundProbeDepth);
}

static bool HasStableGround(const PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera)
{
    VoxelBodyShape shape = ActiveShape(controller);
    return VoxelBodyHasStableGround(collision, camera->position,
        &shape, controller->config.groundProbeDepth,
        controller->config.ledgeSupportRadius);
}

static bool TrySetCrouching(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera, bool crouching)
{
    if (controller->crouching == crouching)
    {
        return false;
    }

    bool previousCrouching = controller->crouching;
    double previousEyeHeight = ActiveEyeHeight(controller);
    double feet = camera->position[2] - previousEyeHeight;

    controller->crouching = crouching;
    camera->position[2] = feet + ActiveEyeHeight(controller);

    VoxelBodyShape shape = ActiveShape(controller);
    if (!crouching
        && VoxelBodyCollides(collision, camera->position, &shape))
    {
        controller->crouching = previousCrouching;
        camera->position[2] = feet + previousEyeHeight;
        return false;
    }
    return true;
}

static void MoveVoluntaryAxisWithLedgeGuard(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    int32_t axis, double distance)
{
    if (distance == 0.0)
    {
        return;
    }

    bool guardEdge = controller->crouching
        && controller->grounded
        && HasStableGround(controller, collision, camera);
    double start = camera->position[axis];
    VoxelBodyShape shape = ActiveShape(controller);

    VoxelBodyMoveAxis(
        collision, camera->position, &shape, axis, distance);

    if (!guardEdge || HasStableGround(controller, collision, camera))
    {
        return;
    }

    double allowed = start;
    double denied = camera->position[axis];
    for (uint32_t iteration = 0;
         iteration < LEDGE_SEARCH_ITERATIONS; ++iteration)
    {
        double middle = (allowed + denied) * 0.5;
        camera->position[axis] = middle;
        if (HasStableGround(controller, collision, camera))
        {
            allowed = middle;
        }
        else
        {
            denied = middle;
        }
    }
    camera->position[axis] = allowed;
}

static void LatchJumpInput(PlayerController* controller,
    const PlayerControllerCommand* command)
{
    bool risingHeld = command->jumpHeld
        && !controller->jumpHeldPrevious;
    controller->jumpHeldPrevious = command->jumpHeld;

    if (command->jumpPressed || risingHeld)
    {
        controller->jumpBufferRemaining =
            controller->config.jumpBufferSeconds;
    }
}

static void UpdateGroundStateAtStepStart(PlayerController* controller,
    const PlayerCollisionSource* collision, const Camera* camera,
    double stepSeconds)
{
    bool supported = HasGroundContact(controller, collision, camera);
    if (supported && controller->verticalVelocity <= 0.0)
    {
        controller->grounded = true;
        controller->verticalVelocity = 0.0;
        controller->coyoteTimeRemaining =
            controller->config.coyoteTimeSeconds;
        return;
    }

    controller->grounded = false;
    controller->coyoteTimeRemaining -= stepSeconds;
    if (controller->coyoteTimeRemaining < 0.0)
    {
        controller->coyoteTimeRemaining = 0.0;
    }
}

static bool TryStartJump(PlayerController* controller)
{
    if (controller->crouching
        || controller->jumpBufferRemaining <= 0.0
        || controller->coyoteTimeRemaining <= 0.0)
    {
        return false;
    }

    controller->verticalVelocity = controller->jumpLaunchSpeed;
    controller->jumpBufferRemaining = 0.0;
    controller->coyoteTimeRemaining = 0.0;
    controller->grounded = false;
    return true;
}

static void MoveHorizontal(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, float yaw,
    double stepSeconds)
{
    float forward = command->forward;
    float right = command->right;
    if (forward != 0.0f && right != 0.0f)
    {
        forward *= 0.70710678f;
        right *= 0.70710678f;
    }

    float sinYaw = ScalarSin(yaw);
    float cosYaw = ScalarCos(yaw);
    double speed = controller->crouching
        ? controller->config.crouchingSpeed
        : controller->config.walkingSpeed;
    double movement = speed * stepSeconds;
    double movementX =
        (double)(sinYaw * forward + cosYaw * right) * movement;
    double movementY =
        (double)(cosYaw * forward - sinYaw * right) * movement;

    MoveVoluntaryAxisWithLedgeGuard(
        controller, collision, camera, 0, movementX);
    MoveVoluntaryAxisWithLedgeGuard(
        controller, collision, camera, 1, movementY);
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

// Возвращает true, когда за этот шаг достигнута точная вершина прыжка.
static bool IntegrateVertical(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    double stepSeconds)
{
    if (controller->grounded && controller->verticalVelocity == 0.0)
    {
        return false;
    }

    double gravity = controller->config.gravity;
    double maximumFallSpeed = controller->config.maximumFallSpeed;
    double oldVelocity = controller->verticalVelocity;
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

        if (VoxelBodyMoveAxis(collision, camera->position, &shape, 2,
                distanceToApex))
        {
            controller->verticalVelocity = 0.0;
            return false;
        }

        // Кадр обязательно увидит вершину. Остаток одного fixed-step
        // сознательно отбрасывается: это не больше 1/240 секунды.
        controller->verticalVelocity = 0.0;
        controller->grounded = false;
        return true;
    }

    double distance;
    if (newVelocity < -maximumFallSpeed)
    {
        double timeToTerminal =
            (oldVelocity + maximumFallSpeed) / gravity;
        if (timeToTerminal < 0.0)
        {
            timeToTerminal = 0.0;
        }
        if (timeToTerminal > stepSeconds)
        {
            timeToTerminal = stepSeconds;
        }
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

    if (VoxelBodyMoveAxis(collision, camera->position, &shape, 2, distance))
    {
        controller->grounded = distance < 0.0;
        controller->verticalVelocity = 0.0;
        return false;
    }

    controller->verticalVelocity = newVelocity;
    return false;
}

static bool SimulateStep(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command, float yaw,
    double stepSeconds)
{
    UpdateGroundStateAtStepStart(
        controller, collision, camera, stepSeconds);
    bool jumped = TryStartJump(controller);

    MoveHorizontal(controller, collision, camera,
        command, yaw, stepSeconds);
    MoveExternalVelocity(controller, collision, camera, stepSeconds);

    bool reachedApex = IntegrateVertical(
        controller, collision, camera, stepSeconds);

    if (!jumped && controller->jumpBufferRemaining > 0.0)
    {
        controller->jumpBufferRemaining -= stepSeconds;
        if (controller->jumpBufferRemaining < 0.0)
        {
            controller->jumpBufferRemaining = 0.0;
        }
    }

    if (controller->verticalVelocity <= 0.0
        && HasGroundContact(controller, collision, camera))
    {
        controller->grounded = true;
        controller->verticalVelocity = 0.0;
    }
    else if (controller->verticalVelocity != 0.0)
    {
        controller->grounded = false;
    }

    return reachedApex;
}

void PlayerControllerInit(PlayerController* controller,
    const PlayerControllerConfig* config)
{
    controller->config = *config;
    controller->verticalVelocity = 0.0;
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->jumpBufferRemaining = 0.0;
    controller->coyoteTimeRemaining = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->jumpLaunchSpeed = PositiveSquareRoot(
        2.0 * (double)config->gravity * config->jumpHeight);
    controller->grounded = false;
    controller->crouching = false;
    controller->jumpHeldPrevious = false;
}

void PlayerControllerReset(PlayerController* controller, Camera* camera)
{
    if (controller->crouching)
    {
        double feet = camera->position[2]
            - controller->config.crouchingEyeHeight;
        camera->position[2] =
            feet + controller->config.standingEyeHeight;
    }

    controller->verticalVelocity = 0.0;
    controller->externalVelocityX = 0.0;
    controller->externalVelocityY = 0.0;
    controller->jumpBufferRemaining = 0.0;
    controller->coyoteTimeRemaining = 0.0;
    controller->simulationAccumulator = 0.0;
    controller->grounded = false;
    controller->crouching = false;
    controller->jumpHeldPrevious = false;
}

bool PlayerControllerUpdate(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command,
    float yaw, float deltaSeconds)
{
    bool presentationChanged = TrySetCrouching(
        controller, collision, camera, command->crouchHeld);
    LatchJumpInput(controller, command);

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
            collision, camera, command, yaw, fixedStep);
        ++substep;

        // Не проглатываем точную вершину несколькими catch-up шагами
        // в одном отрисованном кадре.
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

bool PlayerControllerIsGrounded(const PlayerController* controller)
{
    return controller->grounded;
}

bool PlayerControllerIsCrouching(const PlayerController* controller)
{
    return controller->crouching;
}

void PlayerControllerApplyImpulse(PlayerController* controller,
    float x, float y, float z)
{
    controller->externalVelocityX += x;
    controller->externalVelocityY += y;
    controller->verticalVelocity += z;
    if (z > 0.0f)
    {
        controller->grounded = false;
        controller->coyoteTimeRemaining = 0.0;
    }
}
