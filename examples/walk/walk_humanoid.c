#include "walk_humanoid.h"
#include "walk_math.h"

#include "humanoid_ragdoll.h"
#include "platform/system.h"

#include <math.h>
#include <stddef.h>

enum
{
    WALK_HUMANOID_LEFT_FOOT = 0,
    WALK_HUMANOID_RIGHT_FOOT = 1,
};

static const uint32_t walkHumanoidFootBodies[2] = {
    WALK_RAGDOLL_LEFT_FOOT,
    WALK_RAGDOLL_RIGHT_FOOT,
};

static bool FindFootTargetSurface(const VoxelCollisionSource *collision,
                                  double x, double y, double rootHeight,
                                  double *outSurfaceHeight)
{
    if (collision == NULL || collision->queryBlockPhysics == NULL ||
        outSurfaceHeight == NULL ||
        !WalkRagdollCoordinateSafeForVoxelQuery(x) ||
        !WalkRagdollCoordinateSafeForVoxelQuery(y) ||
        !WalkRagdollCoordinateSafeForVoxelQuery(rootHeight))
        return false;

    const int64_t blockX = (int64_t)WalkMathFloor(x);
    const int64_t blockY = (int64_t)WalkMathFloor(y);
    const int64_t highestSurface = (int64_t)WalkMathFloor(rootHeight - 0.30);
    const int64_t lowestSurface = (int64_t)WalkMathFloor(rootHeight - 1.50);
    for (int64_t surface = highestSurface; surface >= lowestSurface; --surface)
        {
            VoxelBlockPhysics below = {0};
            VoxelBlockPhysics above = {0};
            collision->queryBlockPhysics(
                collision->context, blockX, blockY, surface - 1, &below);
            collision->queryBlockPhysics(
                collision->context, blockX, blockY, surface, &above);
            if ((below.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u &&
                (above.flags & VOXEL_BLOCK_PHYSICS_SOLID) == 0u)
            {
                *outSurfaceHeight = (double)surface;
                return true;
            }
        }
    return false;
}

static double ClampMagnitude3(double value[3], double maximum)
{
    const double length = ScalarSqrtDouble(value[0] * value[0] +
                                           value[1] * value[1] +
                                           value[2] * value[2]);
    if (length > maximum)
    {
        const double scale = maximum / length;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            value[axis] *= scale;
    }
    return length;
}

static bool DriveFootToward(VoxelRagdoll *ragdoll, uint32_t footIndex,
                            const double target[3], const double targetVelocity[3],
                            bool planted, double deltaSeconds)
{
    VoxelRigidBody *foot = &ragdoll->bodies[walkHumanoidFootBodies[footIndex]];
    double position[3];
    double velocity[3];
    if (!VoxelRigidBodyLocalPosition(foot, position) ||
        !VoxelRigidBodyLinearVelocity(foot, velocity))
        return false;
    double acceleration[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double stiffness = planted ? 350.0 : 52.0;
        const double damping = planted ? 37.0 : 14.0;
        acceleration[axis] = stiffness * (target[axis] - position[axis]) -
                             damping * (velocity[axis] - targetVelocity[axis]);
    }
    (void)ClampMagnitude3(acceleration, planted ? 68.0 : 48.0);
    double deltaVelocity[3] = {
        acceleration[0] * deltaSeconds,
        acceleration[1] * deltaSeconds,
        acceleration[2] * deltaSeconds,
    };
    (void)ClampMagnitude3(deltaVelocity, planted ? 0.42 : 0.32);
    if (!VoxelRigidBodyAddLinearVelocity(foot, deltaVelocity))
        return false;
    VoxelRigidBodyWake(foot);
    return true;
}

static bool DriveFootsteps(VoxelRagdoll *ragdoll,
                           WalkHumanoidControllerState *controller,
                           const VoxelCollisionSource *collision,
                           const double root[3], double facingYaw,
                           double magnitude,
                           bool grounded, bool jumped, bool sprint,
                           double gaitPhase, double deltaSeconds)
{
    double footPosition[2][3];
    for (uint32_t foot = 0u; foot < 2u; ++foot)
        if (!VoxelRigidBodyLocalPosition(
                &ragdoll->bodies[walkHumanoidFootBodies[foot]], footPosition[foot]))
            return false;

    if (!controller->initialized)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            controller->previousRootPosition[axis] = root[axis];
        for (uint32_t foot = 0u; foot < 2u; ++foot)
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                controller->footAnchorRelativeToRoot[foot][axis] =
                    footPosition[foot][axis] - root[axis];
            controller->footAnchorValid[foot] = grounded &&
                WalkRagdollFootGrounded(ragdoll, collision, walkHumanoidFootBodies[foot]);
        }
        controller->initialized = true;
    }
    else
    {
        const double rootDelta[3] = {
            root[0] - controller->previousRootPosition[0],
            root[1] - controller->previousRootPosition[1],
            root[2] - controller->previousRootPosition[2],
        };
        for (uint32_t foot = 0u; foot < 2u; ++foot)
            if (controller->footAnchorValid[foot])
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                    controller->footAnchorRelativeToRoot[foot][axis] -=
                        rootDelta[axis];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            controller->previousRootPosition[axis] = root[axis];
    }

    const double dutyCycle = WALK_RAGDOLL_STANCE_FRACTION;
    const double cycleFrequency = sprint ? 1.85 : 1.30;
    const double targetSpeed = sprint ? 3.2 : 1.2;
    double strideLength = targetSpeed / cycleFrequency;
    if (strideLength < 0.30)
        strideLength = 0.30;
    const double maximumStride = sprint ? 0.85 : 0.55;
    if (strideLength > maximumStride)
        strideLength = maximumStride;
    const double actualCycleFrequency = cycleFrequency * magnitude;
    const double forward[2] = {-ScalarSin((float)facingYaw),
                                ScalarCos((float)facingYaw)};
    const double right[2] = {ScalarCos((float)facingYaw),
                              ScalarSin((float)facingYaw)};
    const bool activeJump = jumped || !grounded;

    if (activeJump)
    {
        controller->footAnchorValid[0] = false;
        controller->footAnchorValid[1] = false;
    }
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        const double side = foot == WALK_HUMANOID_LEFT_FOOT ? -1.0 : 1.0;
        const double phase = WalkRagdollFootPhase(gaitPhase, foot);
        const bool stance = magnitude < 0.05 || phase < dutyCycle;
        /* Contact can arrive after the phase boundary, especially on landing.
         * Capture it then, instead of dragging the foot until the next cycle. */
        if (!activeJump && stance && !controller->footAnchorValid[foot] &&
            WalkRagdollFootGrounded(ragdoll, collision, walkHumanoidFootBodies[foot]))
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                controller->footAnchorRelativeToRoot[foot][axis] =
                    footPosition[foot][axis] - root[axis];
            controller->footAnchorValid[foot] = true;
        }
        if (!stance || !WalkRagdollFootContact(
                           ragdoll, collision, walkHumanoidFootBodies[foot], 0.75))
            controller->footAnchorValid[foot] = false;
        controller->previousFootStance[foot] = stance;

        if (activeJump)
            continue;

        double target[3];
        double targetVelocity[3] = {0.0, 0.0, 0.0};
        if (stance && controller->footAnchorValid[foot])
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                target[axis] = root[axis] +
                    controller->footAnchorRelativeToRoot[foot][axis];
            /* Plant the horizontal contact, not the body's center height.
             * A sole can tilt around that contact as the articulated leg
             * moves; holding its old center height drives its corners below
             * the floor. */
            const VoxelRigidBody *footBody =
                &ragdoll->bodies[walkHumanoidFootBodies[foot]];
            float rotation[9];
            VoxelRigidBodyOrientationMatrix(footBody, rotation);
            double surface;
            if (FindFootTargetSurface(collision, target[0], target[1], root[2], &surface))
                target[2] = surface + 0.005 +
                    footBody->halfExtent[0] * WalkMathAbs((double)rotation[2]) +
                    footBody->halfExtent[1] * WalkMathAbs((double)rotation[5]) +
                    footBody->halfExtent[2] * WalkMathAbs((double)rotation[8]);
        }
        else
        {
            double step = 0.0;
            double lift = 0.0;
            if (!stance)
            {
                const double swingPhase = (phase - dutyCycle) / (1.0 - dutyCycle);
                const double smoothStep = swingPhase * swingPhase *
                                          (3.0 - 2.0 * swingPhase);
                const double smoothDerivative = 6.0 * swingPhase *
                                                (1.0 - swingPhase);
                step = (smoothStep - 0.5) * strideLength;
                const double liftSin = ScalarSin((float)(3.1415926535897932385 *
                                                           swingPhase));
                lift = 0.16 * liftSin * liftSin;
                const double swingDuration =
                    (1.0 - dutyCycle) / actualCycleFrequency;
                if (swingDuration > 1.0e-5)
                {
                    const double stepSpeed = strideLength * smoothDerivative /
                                             swingDuration;
                    targetVelocity[0] = forward[0] * stepSpeed;
                    targetVelocity[1] = forward[1] * stepSpeed;
                    targetVelocity[2] = 0.32 * 3.1415926535897932385 * liftSin *
                        ScalarCos((float)(3.1415926535897932385 * swingPhase)) /
                        swingDuration;
                }
            }
            else
            {
                const double desiredStep =
                    (0.5 - phase / dutyCycle) * 0.08;
                step = desiredStep;
            }
            const double halfStance = 0.10 + 0.04 * magnitude;
            target[0] = root[0] + side * halfStance * right[0] +
                        (0.06 + step) * forward[0];
            target[1] = root[1] + side * halfStance * right[1] +
                        (0.06 + step) * forward[1];
            if (!controller->footAnchorValid[foot])
            {
                /* Turn around the supporting foot, not through it. Project
                 * both oriented soles onto the outward stepping direction. */
                double separation = 0.025;
                for (uint32_t sole = 0u; sole < 2u; ++sole)
                {
                    const VoxelRigidBody *body = &ragdoll->bodies[walkHumanoidFootBodies[sole]];
                    float rotation[9];
                    VoxelRigidBodyOrientationMatrix(body, rotation);
                    for (uint32_t axis = 0u; axis < 3u; ++axis)
                        separation += body->halfExtent[axis] *
                            WalkMathAbs(rotation[3u * axis] * right[0] +
                                 rotation[3u * axis + 1u] * right[1]);
                }
                const uint32_t other = 1u - foot;
                const double projected = side *
                    ((target[0] - footPosition[other][0]) * right[0] +
                     (target[1] - footPosition[other][1]) * right[1]);
                if (projected < separation)
                {
                    target[0] += side * (separation - projected) * right[0];
                    target[1] += side * (separation - projected) * right[1];
                }
            }
            float footRotation[9];
            VoxelRigidBodyOrientationMatrix(
                &ragdoll->bodies[walkHumanoidFootBodies[foot]], footRotation);
            const VoxelRigidBody *footBody =
                &ragdoll->bodies[walkHumanoidFootBodies[foot]];
            const double footVerticalExtent =
                footBody->halfExtent[0] * WalkMathAbs((double)footRotation[2]) +
                footBody->halfExtent[1] * WalkMathAbs((double)footRotation[5]) +
                footBody->halfExtent[2] * WalkMathAbs((double)footRotation[8]);
            double surfaceHeight;
            const bool surfaceFound = FindFootTargetSurface(
                collision, target[0], target[1], root[2], &surfaceHeight);
            /* A neighboring voxel is not support under this target. Let the
             * foot fall into empty space instead of holding it over a ledge. */
            if (!surfaceFound)
                continue;
            target[2] = surfaceHeight + footVerticalExtent + 0.02 + lift;
        }
        if (!DriveFootToward(ragdoll, foot, target, targetVelocity,
                             stance && controller->footAnchorValid[foot],
                             deltaSeconds))
            return false;
    }
    return true;
}

static bool DriveSupportedPelvisHeight(VoxelRagdoll *ragdoll,
                                       const VoxelCollisionSource *collision,
                                       const bool stance[2],
                                       double gaitAmount, bool sprint,
                                       double deltaSeconds)
{
    double supportHeight = 0.0;
    uint32_t supportCount = 0u;
    bool supported[2] = {false, false};
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        const uint32_t bodyIndex = walkHumanoidFootBodies[foot];
        if (!stance[foot] || !WalkRagdollFootContact(ragdoll, collision, bodyIndex, 0.75))
            continue;
        double position[3];
        if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[bodyIndex], position))
            return false;
        double surface;
        if (!FindFootTargetSurface(collision, position[0], position[1],
                                    position[2] + 0.94, &surface))
            continue;
        /* Voxels are stationary. Following the sole's bounce velocity fed it
         * back into the pelvis spring and amplified each small contact bounce. */
        supportHeight += surface + walkRagdollBodies[bodyIndex].halfExtent[2];
        supported[foot] = true;
        ++supportCount;
    }
    double rootPosition[3];
    double rootVelocity[3];
    VoxelRigidBody *pelvis = &ragdoll->bodies[WALK_RAGDOLL_PELVIS];
    if (!VoxelRigidBodyLocalPosition(pelvis, rootPosition) ||
        !VoxelRigidBodyLinearVelocity(pelvis, rootVelocity))
        return false;
    double targetHeight = 0.0;
    double targetVelocity = 0.0;
    if (supportCount != 0u)
    {
        /* Bias the moving support height slightly lower. Leg IK and the
         * compliant physical joints supply the rest of the knee flexion. */
        targetHeight = supportHeight / supportCount + 0.94 -
                       (sprint ? 0.06 : 0.02) * gaitAmount;
    }
    else
    {
        /* A fallen pelvis can touch the floor before either foot does. Recover
         * only when the body is actually at a nearby surface and descending;
         * this avoids turning the support spring into mid-air hover or canceling
         * a jump. */
        if (rootVelocity[2] > 0.5 || collision->queryBlockPhysics == NULL)
            return true;
        float rotation[9];
        VoxelRigidBodyOrientationMatrix(pelvis, rotation);
        const double bottomExtent =
            pelvis->halfExtent[0] * WalkMathAbs((double)rotation[2]) +
            pelvis->halfExtent[1] * WalkMathAbs((double)rotation[5]) +
            pelvis->halfExtent[2] * WalkMathAbs((double)rotation[8]);
        const double bottom = rootPosition[2] - bottomExtent;
        if (!WalkRagdollCoordinateSafeForVoxelQuery(bottom) ||
            !WalkRagdollCoordinateSafeForVoxelQuery(rootPosition[0]) ||
            !WalkRagdollCoordinateSafeForVoxelQuery(rootPosition[1]))
            return true;
        const int64_t centerX = (int64_t)WalkMathFloor(rootPosition[0]);
        const int64_t centerY = (int64_t)WalkMathFloor(rootPosition[1]);
        const int64_t nearestSurface = (int64_t)WalkMathFloor(bottom + 0.5);
        bool bodyContact = false;
        int64_t contactSurface = 0;
        double nearestGap = INFINITY;
        for (int64_t offset = -1; offset <= 1; ++offset)
            {
                const int64_t surface = nearestSurface + offset;
                const double gap = bottom - (double)surface;
                if (gap < -0.08 || gap > 0.10 || WalkMathAbs(gap) >= nearestGap)
                    continue;
                VoxelBlockPhysics below = {0};
                VoxelBlockPhysics above = {0};
                collision->queryBlockPhysics(
                    collision->context, centerX, centerY, surface - 1, &below);
                collision->queryBlockPhysics(
                    collision->context, centerX, centerY, surface, &above);
                if ((below.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u &&
                    (above.flags & VOXEL_BLOCK_PHYSICS_SOLID) == 0u)
                {
                    bodyContact = true;
                    contactSurface = surface;
                    nearestGap = WalkMathAbs(gap);
                }
            }
        if (!bodyContact)
            return true;
        targetHeight = (double)contactSurface + bottomExtent + 0.03;
    }
    /* The spring controls height error, while the feed-forward term carries
     * weight. Without it the shorter human rig settles into a permanent squat
     * (gravity/stiffness metres below its intended stance). */
    double deltaZ = (supportCount != 0u ? 9.81 : 0.0) +
                    80.0 * (targetHeight - rootPosition[2]) -
                    16.0 * (rootVelocity[2] - targetVelocity);
    if (deltaZ > 26.0)
        deltaZ = 26.0;
    else if (deltaZ < -26.0)
        deltaZ = -26.0;
    if (supportCount != 0u && deltaZ < 0.0)
        deltaZ = 0.0;
    const double delta[3] = {0.0, 0.0, deltaZ * deltaSeconds};
    /* Apply a shared support impulse to the articulated island. The contacts
     * on planted feet absorb the downward part, so this restores leg extension
     * without injecting upward velocity while airborne. */
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[body], delta))
            return false;
        VoxelRigidBodyWake(&ragdoll->bodies[body]);
    }
    if (supportCount != 0u)
    {
        /* Keep the weight-bearing part on the supporting soles so grounding
         * retains friction. The extra height correction remains an assisted
         * balance motor, bounded separately from the character's weight. */
        double totalMass = 0.0;
        for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
            totalMass += 1.0 / ragdoll->bodies[body].inverseMass;
        for (uint32_t foot = 0u; foot < 2u; ++foot)
            if (supported[foot])
            {
                VoxelRigidBody *sole = &ragdoll->bodies[walkHumanoidFootBodies[foot]];
                double reaction[3] = {0.0, 0.0,
                    -(deltaZ < 9.81 ? deltaZ : 9.81) * deltaSeconds *
                    totalMass * sole->inverseMass * 0.5};
                if (reaction[2] < -32.0 * deltaSeconds)
                    reaction[2] = -32.0 * deltaSeconds;
                if (!VoxelRigidBodyAddLinearVelocity(sole, reaction))
                    return false;
            }
    }
    return true;
}

bool WalkHumanoidInitialize(const LaiuePhysicsServiceV1 *physics,
                           VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const double origin[3], uint64_t stableIdBase,
                           VoxelRigidStepSettings *outRigidSettings,
                           VoxelRagdollSettings *outRagdollSettings,
                           void **outScratch, uint32_t *outScratchBytes)
{
    if (outScratch != NULL)
        *outScratch = NULL;
    if (outScratchBytes != NULL)
        *outScratchBytes = 0u;
    if (physics == NULL || ragdoll == NULL || collision == NULL || origin == NULL ||
        outRigidSettings == NULL || outRagdollSettings == NULL || outScratch == NULL ||
        outScratchBytes == NULL || physics->configureThread == NULL ||
        physics->stepScratchBytes == NULL || physics->ragdollInitialize == NULL ||
        physics->ragdollSettingsDefault == NULL || physics->ragdollStep == NULL)
        return false;

    const VoxelRagdollDefinition definition = {
        .stableIdBase = stableIdBase,
        .origin = {origin[0], origin[1], origin[2]},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    *outRigidSettings = (VoxelRigidStepSettings){
        .gravity = {0.0, 0.0, -9.81},
        .solverIterations = 16u,
        .penetrationCorrection = 0.55,
        .penetrationSlop = 0.002,
        .sleepLinearSpeed = 0.02,
        .sleepAngularSpeed = 0.05,
        .sleepFrames = 30u,
    };
    physics->configureThread();
    physics->ragdollSettingsDefault(outRagdollSettings);
    outRagdollSettings->solverIterations = 16u;
    outRagdollSettings->errorCorrection = 0.5;
    *outScratchBytes = physics->stepScratchBytes(WALK_RAGDOLL_BODY_COUNT);
    if (*outScratchBytes == 0u)
        return false;
    *outScratch = PlatformAllocate(*outScratchBytes, false);
    if (*outScratch == NULL)
    {
        *outScratchBytes = 0u;
        return false;
    }
    if (!physics->ragdollInitialize(ragdoll, &definition))
    {
        PlatformFree(*outScratch);
        *outScratch = NULL;
        *outScratchBytes = 0u;
        return false;
    }
    return true;
}

bool WalkHumanoidStep(const LaiuePhysicsServiceV1 *physics,
                      VoxelRagdoll *ragdoll,
                      WalkHumanoidControllerState *controllerState,
                      const VoxelCollisionSource *collision,
                      const VoxelRigidStepSettings *rigidSettings,
                      const VoxelRagdollSettings *ragdollSettings,
                      void *scratch, uint32_t scratchBytes,
                      double moveX, double moveY, bool sprint, bool jump,
                      double deltaSeconds, bool *inOutGrounded,
                      double *inOutFacingYaw, double *inOutGaitPhase,
                      WalkHumanoidStepFailure *outFailure)
{
    if (outFailure != NULL)
        *outFailure = WALK_HUMANOID_STEP_OK;
    if (physics == NULL || physics->ragdollStep == NULL ||
        !WalkRagdollHasHumanoidTopology(ragdoll) || controllerState == NULL ||
        collision == NULL || rigidSettings == NULL || ragdollSettings == NULL ||
        scratch == NULL || scratchBytes == 0u || inOutGrounded == NULL ||
        inOutFacingYaw == NULL || inOutGaitPhase == NULL || !WalkMathFinite(moveX) ||
        !WalkMathFinite(moveY) || !WalkMathFinite(deltaSeconds) || !(deltaSeconds > 0.0) ||
        deltaSeconds > 1.0 / 30.0 || !WalkMathFinite(*inOutFacingYaw) ||
        !WalkMathFinite(*inOutGaitPhase) || !WalkMathFinite(controllerState->gaitAmount) ||
        controllerState->gaitAmount < 0.0 || controllerState->gaitAmount > 1.0)
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_INVALID_STATE;
        return false;
    }
    const double magnitudeSquared = moveX * moveX + moveY * moveY;
    if (!WalkMathFinite(magnitudeSquared))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_INVALID_STATE;
        return false;
    }
    double magnitude = ScalarSqrtDouble(magnitudeSquared);
    if (magnitude > 1.0)
    {
        moveX /= magnitude;
        moveY /= magnitude;
        magnitude = 1.0;
    }
    const double targetSpeed = sprint ? 3.2 : 1.2;
    if (magnitude > 1.0e-3)
    {
        const double tau = 6.2831853071795864769;
        const double desiredYaw = ScalarAtan2((float)-moveX, (float)moveY);
        double yawDelta = desiredYaw - *inOutFacingYaw;
        yawDelta -= WalkMathFloor((yawDelta + 3.1415926535897932385) / tau) * tau;
        const double maximumTurn = 2.0 * deltaSeconds;
        if (yawDelta > maximumTurn)
            yawDelta = maximumTurn;
        else if (yawDelta < -maximumTurn)
            yawDelta = -maximumTurn;
        *inOutFacingYaw += yawDelta;
    }
    *inOutGrounded = WalkRagdollGrounded(ragdoll, collision);
    const double gaitTarget = *inOutGrounded ? magnitude : 0.0;
    const double gaitDelta = gaitTarget - controllerState->gaitAmount;
    const double maximumGaitDelta = 5.0 * deltaSeconds;
    controllerState->gaitAmount += gaitDelta < -maximumGaitDelta ? -maximumGaitDelta :
        (gaitDelta > maximumGaitDelta ? maximumGaitDelta : gaitDelta);
    const double gaitAmount = controllerState->gaitAmount;
    if (gaitAmount > 0.05)
        *inOutGaitPhase += 6.2831853071795864769 *
                           (sprint ? 1.85 : 1.30) * gaitAmount * deltaSeconds;
    if (*inOutGaitPhase >= 6.2831853071795864769)
        *inOutGaitPhase -= WalkMathFloor(*inOutGaitPhase /
                                 6.2831853071795864769) *
                           6.2831853071795864769;
    const bool jumpPressed = jump && !controllerState->previousJumpInput;
    controllerState->previousJumpInput = jump;
    const bool jumped = jumpPressed && WalkRagdollJumpIfGrounded(ragdoll, collision, 4.2);
    if (jumped)
        *inOutGrounded = false;
    double rootPosition[3];
    if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[ragdoll->rootBody],
                                     rootPosition))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_INVALID_STATE;
        return false;
    }
    /* Acceleration follows the same heading as the steps. Applying the new
     * input direction instantly used to pull the hips sideways across planted
     * legs during reversals, even though the torso was still turning. */
    double driveX = -ScalarSin((float)*inOutFacingYaw);
    double driveY = ScalarCos((float)*inOutFacingYaw);
    const double headingLength = ScalarSqrtDouble(driveX * driveX + driveY * driveY);
    driveX *= magnitude / headingLength;
    driveY *= magnitude / headingLength;
    if (!WalkRagdollDrivePlanar(ragdoll, driveX, driveY, targetSpeed,
                                *inOutGrounded ? 3.5 : 0.25, deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    if (!DriveFootsteps(ragdoll, controllerState, collision, rootPosition,
                        *inOutFacingYaw, gaitAmount,
                        *inOutGrounded, jumped, sprint, *inOutGaitPhase,
                        deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    if (!jumped && !DriveSupportedPelvisHeight(ragdoll, collision,
                                               controllerState->previousFootStance,
                                               gaitAmount, sprint, deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    if (!WalkRagdollPoseDriveWithSupport(ragdoll, *inOutFacingYaw, *inOutGaitPhase,
                              gaitAmount, deltaSeconds, controllerState->footAnchorValid))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_POSE_DRIVE;
        return false;
    }
    if (!physics->ragdollStep(ragdoll, collision, rigidSettings, ragdollSettings,
                              scratch, scratchBytes, NULL))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PHYSICS;
        return false;
    }
    *inOutGrounded = WalkRagdollGrounded(ragdoll, collision);
    return true;
}

void WalkHumanoidControllerRebase(WalkHumanoidControllerState *controllerState,
                                  const int64_t blockShift[3])
{
    if (controllerState == NULL || blockShift == NULL || !controllerState->initialized)
        return;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        controllerState->previousRootPosition[axis] -= (double)blockShift[axis];
}

bool WalkHumanoidIsGrounded(const VoxelRagdoll *ragdoll,
                            const VoxelCollisionSource *collision)
{
    return WalkRagdollGrounded(ragdoll, collision);
}

void WalkHumanoidRelease(const LaiuePhysicsServiceV1 *physics,
                         VoxelRagdoll *ragdoll, void **scratch)
{
    if (ragdoll != NULL && ragdoll->initialized && physics != NULL &&
        physics->ragdollRelease != NULL)
        physics->ragdollRelease(ragdoll);
    if (scratch != NULL)
    {
        PlatformFree(*scratch);
        *scratch = NULL;
    }
}

static bool CameraSpaceClear(const VoxelCollisionSource *collision,
                              const double point[3])
{
    int64_t lower[3], upper[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!WalkRagdollCoordinateSafeForVoxelQuery(point[axis]))
            return false;
        lower[axis] = (int64_t)WalkMathFloor(point[axis] - 0.15);
        upper[axis] = (int64_t)WalkMathFloor(point[axis] + 0.15);
    }
    for (int64_t z = lower[2]; z <= upper[2]; ++z)
        for (int64_t y = lower[1]; y <= upper[1]; ++y)
            for (int64_t x = lower[0]; x <= upper[0]; ++x)
            {
                VoxelBlockPhysics block = {0};
                collision->queryBlockPhysics(collision->context, x, y, z, &block);
                if ((block.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u)
                    return false;
            }
    return true;
}

bool WalkHumanoidCameraEye(const VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const float forward[3], bool firstPerson,
                           double outEye[3])
{
    if (ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT || collision == NULL ||
        collision->queryBlockPhysics == NULL || forward == NULL || outEye == NULL)
        return false;
    const double length = ScalarSqrtDouble((double)forward[0] * forward[0] +
                                            (double)forward[1] * forward[1] +
                                            (double)forward[2] * forward[2]);
    if (!WalkMathFinite(length) || length < 1.0e-6)
        return false;
    double target[3];
    if (!VoxelRigidBodyLocalPosition(
            &ragdoll->bodies[firstPerson ? WALK_RAGDOLL_HEAD : WALK_RAGDOLL_PELVIS],
            target))
        return false;
    target[2] += firstPerson ? 0.02 : 0.40;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!WalkRagdollCoordinateSafeForVoxelQuery(target[axis]))
            return false;
    if (firstPerson)
    {
        memcpy(outEye, target, sizeof(target));
        return true;
    }
    const double boom[3] = {-3.0 * forward[0] / length,
                             -3.0 * forward[1] / length,
                             -3.0 * forward[2] / length};
    double clearFraction = 0.0;
    bool foundClearPosition = false;
    for (uint32_t sample = 1u; sample <= 64u; ++sample)
    {
        double blockedFraction = (double)sample / 64.0;
        double eye[3];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            eye[axis] = target[axis] + boom[axis] * blockedFraction;
        if (CameraSpaceClear(collision, eye))
        {
            clearFraction = blockedFraction;
            foundClearPosition = true;
            continue;
        }
        /* A shoulder target can briefly be inside terrain while the ragdoll
         * is being pushed free. Do not assume the initial point is clear when
         * refining the sweep: start at the first clear sample and only bisect
         * after the boom has entered a blocked region from clear space. */
        if (!foundClearPosition)
            continue;
        /* Refine the first obstruction so slow camera motion is not quantized
         * to the coarse sweep spacing. No allocation or unbounded world scan. */
        for (uint32_t iteration = 0u; iteration < 5u; ++iteration)
        {
            const double middle = (clearFraction + blockedFraction) * 0.5;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                eye[axis] = target[axis] + boom[axis] * middle;
            if (CameraSpaceClear(collision, eye))
                clearFraction = middle;
            else
                blockedFraction = middle;
        }
        break;
    }
    if (!foundClearPosition)
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        outEye[axis] = target[axis] + boom[axis] * clearFraction;
    return true;
}

bool WalkHumanoidResolveCameraEye(
    const VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
    const float forward[3], bool firstPerson, const double fallbackEye[3],
    double lastSafeThirdPersonEye[3], bool *hasLastSafeThirdPersonEye,
    double outEye[3])
{
    if (fallbackEye == NULL || lastSafeThirdPersonEye == NULL ||
        hasLastSafeThirdPersonEye == NULL || outEye == NULL)
        return false;
    double eye[3];
    if (WalkHumanoidCameraEye(ragdoll, collision, forward, firstPerson, eye))
    {
        if (!firstPerson)
        {
            memcpy(lastSafeThirdPersonEye, eye, sizeof(eye));
            *hasLastSafeThirdPersonEye = true;
        }
        memcpy(outEye, eye, sizeof(eye));
        return true;
    }
    if (!firstPerson && *hasLastSafeThirdPersonEye)
    {
        bool cacheValid = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            cacheValid = cacheValid && WalkMathFinite(lastSafeThirdPersonEye[axis]);
        if (cacheValid)
        {
            memcpy(outEye, lastSafeThirdPersonEye, sizeof(eye));
            return true;
        }
        *hasLastSafeThirdPersonEye = false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!WalkMathFinite(fallbackEye[axis]))
            return false;
        eye[axis] = fallbackEye[axis];
    }
    memcpy(outEye, eye, sizeof(eye));
    return true;
}
