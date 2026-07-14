#include "game/player_controller.h"

#include "core/math.h"
#include "core/numeric.h"

#define PLAYER_PENETRATION_STEPS 256u
#define LEDGE_SEARCH_ITERATIONS 14u

typedef struct PlayerBounds
{
    double minimum[3];
    double maximum[3];
} PlayerBounds;

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

static void CalculateBounds(const PlayerController* controller,
    const Camera* camera, PlayerBounds* bounds)
{
    double feet = camera->position[2] - ActiveEyeHeight(controller);
    double radius = controller->config.radius;

    bounds->minimum[0] = camera->position[0] - radius;
    bounds->maximum[0] = camera->position[0] + radius;
    bounds->minimum[1] = camera->position[1] - radius;
    bounds->maximum[1] = camera->position[1] + radius;
    bounds->minimum[2] = feet;
    bounds->maximum[2] = feet + ActiveHeight(controller);
}

static bool BoundsContainSolidBlock(
    const PlayerCollisionSource* collision,
    const PlayerController* controller, const PlayerBounds* bounds)
{
    double epsilon = controller->config.collisionEpsilon;
    int64_t minimumBlock[3];
    int64_t maximumBlock[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        minimumBlock[axis] =
            FloorDoubleToInt64(bounds->minimum[axis] + epsilon);
        maximumBlock[axis] =
            FloorDoubleToInt64(bounds->maximum[axis] - epsilon);
    }

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (collision->isSolidBlock(collision->context, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool PlayerCollidesAt(
    const PlayerCollisionSource* collision,
    const PlayerController* controller, const Camera* camera)
{
    PlayerBounds bounds;
    CalculateBounds(controller, camera, &bounds);
    return BoundsContainSolidBlock(collision, controller, &bounds);
}

static bool BlockPlaneCollides(
    const PlayerCollisionSource* collision,
    const PlayerController* controller, int32_t axis, int64_t plane,
    const PlayerBounds* bounds)
{
    double epsilon = controller->config.collisionEpsilon;
    int64_t minimumBlock[3];
    int64_t maximumBlock[3];

    for (int32_t currentAxis = 0; currentAxis < 3; ++currentAxis)
    {
        minimumBlock[currentAxis] =
            FloorDoubleToInt64(bounds->minimum[currentAxis] + epsilon);
        maximumBlock[currentAxis] =
            FloorDoubleToInt64(bounds->maximum[currentAxis] - epsilon);
    }
    minimumBlock[axis] = plane;
    maximumBlock[axis] = plane;

    for (int64_t z = minimumBlock[2]; z <= maximumBlock[2]; ++z)
    {
        for (int64_t y = minimumBlock[1]; y <= maximumBlock[1]; ++y)
        {
            for (int64_t x = minimumBlock[0]; x <= maximumBlock[0]; ++x)
            {
                if (collision->isSolidBlock(collision->context, x, y, z))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool HasGroundSupport(
    const PlayerCollisionSource* collision,
    const PlayerController* controller, const Camera* camera)
{
    PlayerBounds bounds;
    CalculateBounds(controller, camera, &bounds);

    double epsilon = controller->config.collisionEpsilon;
    int64_t minimumX = FloorDoubleToInt64(bounds.minimum[0] + epsilon);
    int64_t maximumX = FloorDoubleToInt64(bounds.maximum[0] - epsilon);
    int64_t minimumY = FloorDoubleToInt64(bounds.minimum[1] + epsilon);
    int64_t maximumY = FloorDoubleToInt64(bounds.maximum[1] - epsilon);
    int64_t supportZ = FloorDoubleToInt64(
        bounds.minimum[2] - controller->config.groundProbeDepth);

    for (int64_t y = minimumY; y <= maximumY; ++y)
    {
        for (int64_t x = minimumX; x <= maximumX; ++x)
        {
            if (collision->isSolidBlock(collision->context, x, y, supportZ))
            {
                return true;
            }
        }
    }
    return false;
}

static bool MoveAxis(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    int32_t axis, double distance)
{
    if (distance == 0.0)
    {
        return false;
    }

    PlayerBounds oldBounds;
    CalculateBounds(controller, camera, &oldBounds);

    Camera target = *camera;
    target.position[axis] += distance;

    PlayerBounds newBounds;
    CalculateBounds(controller, &target, &newBounds);

    double negativeExtent = axis == 2
        ? ActiveEyeHeight(controller)
        : controller->config.radius;
    double positiveExtent = axis == 2
        ? ActiveHeight(controller) - ActiveEyeHeight(controller)
        : controller->config.radius;
    double epsilon = controller->config.collisionEpsilon;

    if (distance > 0.0)
    {
        int64_t firstPlane =
            FloorDoubleToInt64(oldBounds.maximum[axis] - epsilon) + 1;
        int64_t lastPlane =
            FloorDoubleToInt64(newBounds.maximum[axis] - epsilon);

        for (int64_t plane = firstPlane; plane <= lastPlane; ++plane)
        {
            if (BlockPlaneCollides(
                    collision, controller, axis, plane, &newBounds))
            {
                camera->position[axis] =
                    (double)plane - positiveExtent - epsilon;
                return true;
            }
        }
    }
    else
    {
        int64_t firstPlane =
            FloorDoubleToInt64(oldBounds.minimum[axis] + epsilon) - 1;
        int64_t lastPlane =
            FloorDoubleToInt64(newBounds.minimum[axis] + epsilon);

        for (int64_t plane = firstPlane; plane >= lastPlane; --plane)
        {
            if (BlockPlaneCollides(
                    collision, controller, axis, plane, &newBounds))
            {
                camera->position[axis] =
                    (double)plane + 1.0 + negativeExtent + epsilon;
                return true;
            }
        }
    }

    camera->position[axis] = target.position[axis];
    return false;
}

static void MoveVoluntaryAxisWithLedgeGuard(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    int32_t axis, double distance)
{
    double start = camera->position[axis];
    MoveAxis(controller, collision, camera, axis, distance);

    if (!controller->crouching || !controller->grounded
        || HasGroundSupport(collision, controller, camera))
    {
        return;
    }

    double allowed = start;
    double blocked = camera->position[axis];
    for (uint32_t iteration = 0;
         iteration < LEDGE_SEARCH_ITERATIONS; ++iteration)
    {
        double middle = (allowed + blocked) * 0.5;
        camera->position[axis] = middle;
        if (HasGroundSupport(collision, controller, camera))
        {
            allowed = middle;
        }
        else
        {
            blocked = middle;
        }
    }
    camera->position[axis] = allowed;
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

    if (!crouching && PlayerCollidesAt(collision, controller, camera))
    {
        controller->crouching = previousCrouching;
        camera->position[2] = feet + previousEyeHeight;
        return false;
    }
    return true;
}

void PlayerControllerInit(
    PlayerController* controller, const PlayerControllerConfig* config)
{
    controller->config = *config;
    controller->verticalVelocity = 0.0f;
    controller->externalVelocityX = 0.0f;
    controller->externalVelocityY = 0.0f;
    controller->jumpBufferRemaining = 0.0f;
    controller->coyoteTimeRemaining = 0.0f;
    controller->grounded = false;
    controller->crouching = false;
}

void PlayerControllerReset(PlayerController* controller, Camera* camera)
{
    if (controller->crouching)
    {
        double feet = camera->position[2] - controller->config.crouchingEyeHeight;
        camera->position[2] = feet + controller->config.standingEyeHeight;
    }

    controller->verticalVelocity = 0.0f;
    controller->externalVelocityX = 0.0f;
    controller->externalVelocityY = 0.0f;
    controller->jumpBufferRemaining = 0.0f;
    controller->coyoteTimeRemaining = 0.0f;
    controller->grounded = false;
    controller->crouching = false;
}

bool PlayerControllerUpdate(PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera,
    const PlayerControllerCommand* command,
    float yaw, float deltaSeconds)
{
    bool presentationChanged = TrySetCrouching(
        controller, collision, camera, command->crouchHeld);

    bool supportedAtStart = HasGroundSupport(collision, controller, camera);
    if (supportedAtStart && controller->verticalVelocity <= 0.0f)
    {
        controller->grounded = true;
        controller->verticalVelocity = 0.0f;
        controller->coyoteTimeRemaining =
            controller->config.coyoteTimeSeconds;
    }
    else
    {
        controller->grounded = false;
        controller->coyoteTimeRemaining -= deltaSeconds;
        if (controller->coyoteTimeRemaining < 0.0f)
        {
            controller->coyoteTimeRemaining = 0.0f;
        }
    }

    if (command->jumpPressed)
    {
        controller->jumpBufferRemaining =
            controller->config.jumpBufferSeconds;
    }
    else
    {
        controller->jumpBufferRemaining -= deltaSeconds;
        if (controller->jumpBufferRemaining < 0.0f)
        {
            controller->jumpBufferRemaining = 0.0f;
        }
    }

    if (!controller->crouching
        && controller->jumpBufferRemaining > 0.0f
        && controller->coyoteTimeRemaining > 0.0f)
    {
        controller->verticalVelocity = controller->config.jumpSpeed;
        controller->jumpBufferRemaining = 0.0f;
        controller->coyoteTimeRemaining = 0.0f;
        controller->grounded = false;
    }

    float forward = command->forward;
    float right = command->right;
    if (forward != 0.0f && right != 0.0f)
    {
        forward *= 0.70710678f;
        right *= 0.70710678f;
    }

    float sinYaw = ScalarSin(yaw);
    float cosYaw = ScalarCos(yaw);
    float speed = controller->crouching
        ? controller->config.crouchingSpeed
        : controller->config.walkingSpeed;
    double movement = (double)(speed * deltaSeconds);
    double movementX =
        (double)(sinYaw * forward + cosYaw * right) * movement;
    double movementY =
        (double)(cosYaw * forward - sinYaw * right) * movement;

    MoveVoluntaryAxisWithLedgeGuard(
        controller, collision, camera, 0, movementX);
    MoveVoluntaryAxisWithLedgeGuard(
        controller, collision, camera, 1, movementY);

    if (controller->externalVelocityX != 0.0f)
    {
        if (MoveAxis(controller, collision, camera, 0,
                (double)(controller->externalVelocityX * deltaSeconds)))
        {
            controller->externalVelocityX = 0.0f;
        }
    }
    if (controller->externalVelocityY != 0.0f)
    {
        if (MoveAxis(controller, collision, camera, 1,
                (double)(controller->externalVelocityY * deltaSeconds)))
        {
            controller->externalVelocityY = 0.0f;
        }
    }

    float damping = 1.0f
        - controller->config.externalVelocityDamping * deltaSeconds;
    if (damping < 0.0f) damping = 0.0f;
    controller->externalVelocityX *= damping;
    controller->externalVelocityY *= damping;

    if (!controller->grounded || controller->verticalVelocity > 0.0f)
    {
        controller->verticalVelocity -=
            controller->config.gravity * deltaSeconds;
        if (controller->verticalVelocity
            < -controller->config.maximumFallSpeed)
        {
            controller->verticalVelocity =
                -controller->config.maximumFallSpeed;
        }

        float previousVelocity = controller->verticalVelocity;
        if (MoveAxis(controller, collision, camera, 2,
                (double)(controller->verticalVelocity * deltaSeconds)))
        {
            controller->grounded = previousVelocity < 0.0f;
            controller->verticalVelocity = 0.0f;
        }
    }

    if (controller->verticalVelocity <= 0.0f
        && HasGroundSupport(collision, controller, camera))
    {
        controller->grounded = true;
        controller->verticalVelocity = 0.0f;
    }
    else if (controller->verticalVelocity != 0.0f)
    {
        controller->grounded = false;
    }

    return presentationChanged;
}

bool PlayerControllerResolvePenetration(
    PlayerController* controller,
    const PlayerCollisionSource* collision, Camera* camera)
{
    for (uint32_t step = 0; step < PLAYER_PENETRATION_STEPS; ++step)
    {
        if (!PlayerCollidesAt(collision, controller, camera))
        {
            return true;
        }
        camera->position[2] += 1.0;
    }
    return !PlayerCollidesAt(collision, controller, camera);
}

bool PlayerControllerOverlapsBlock(const PlayerController* controller,
    const Camera* camera, const int64_t block[3])
{
    PlayerBounds bounds;
    CalculateBounds(controller, camera, &bounds);
    double epsilon = controller->config.collisionEpsilon;

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double blockMinimum = (double)block[axis];
        double blockMaximum = blockMinimum + 1.0;
        if (bounds.maximum[axis] <= blockMinimum + epsilon
            || bounds.minimum[axis] >= blockMaximum - epsilon)
        {
            return false;
        }
    }
    return true;
}

bool PlayerControllerIsGrounded(const PlayerController* controller)
{
    return controller->grounded;
}

bool PlayerControllerIsCrouching(const PlayerController* controller)
{
    return controller->crouching;
}

void PlayerControllerApplyImpulse(
    PlayerController* controller, float x, float y, float z)
{
    controller->externalVelocityX += x;
    controller->externalVelocityY += y;
    controller->verticalVelocity += z;
    if (z > 0.0f)
    {
        controller->grounded = false;
        controller->coyoteTimeRemaining = 0.0f;
    }
}
