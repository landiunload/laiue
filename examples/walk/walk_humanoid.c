#include "walk_humanoid.h"

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

    const int64_t blockX = (int64_t)floor(x);
    const int64_t blockY = (int64_t)floor(y);
    const int64_t highestSurface = (int64_t)floor(rootHeight - 1.0);
    const int64_t lowestSurface = (int64_t)floor(rootHeight - 3.0);
    static const int8_t sampleOffsets[5][2] = {
        {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    };
    for (int64_t surface = highestSurface; surface >= lowestSurface; --surface)
        for (uint32_t sample = 0u; sample < 5u; ++sample)
        {
            VoxelBlockPhysics below = {0};
            VoxelBlockPhysics above = {0};
            collision->queryBlockPhysics(
                collision->context, blockX + sampleOffsets[sample][0],
                blockY + sampleOffsets[sample][1], surface - 1, &below);
            collision->queryBlockPhysics(
                collision->context, blockX + sampleOffsets[sample][0],
                blockY + sampleOffsets[sample][1], surface, &above);
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
    const double stiffness = planted ? 82.0 : 52.0;
    const double damping = planted ? 18.0 : 14.0;
    double acceleration[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        acceleration[axis] = stiffness * (target[axis] - position[axis]) -
                             damping * (velocity[axis] - targetVelocity[axis]);
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
            controller->footAnchorValid[foot] = grounded;
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

    const double tau = 6.2831853071795864769;
    const double dutyCycle = 0.60;
    const double cycleFrequency = sprint ? 1.85 : 1.30;
    const double targetSpeed = sprint ? 3.2 : 1.2;
    double strideLength = targetSpeed / cycleFrequency;
    if (strideLength < 0.65)
        strideLength = 0.65;
    if (strideLength > 1.5)
        strideLength = 1.5;
    const double actualCycleFrequency = cycleFrequency * magnitude;
    const double phaseCycle = gaitPhase / tau;
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
        double phase = phaseCycle + (foot == WALK_HUMANOID_RIGHT_FOOT ? 0.5 : 0.0);
        phase -= floor(phase);
        const bool stance = magnitude < 0.05 || phase < dutyCycle;
        if (stance && !controller->previousFootStance[foot] &&
            WalkRagdollFootGrounded(ragdoll, collision, walkHumanoidFootBodies[foot]))
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                controller->footAnchorRelativeToRoot[foot][axis] =
                    footPosition[foot][axis] - root[axis];
            controller->footAnchorValid[foot] = true;
        }
        if (!stance)
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
                lift = 0.22 * ScalarSin((float)(3.1415926535897932385 *
                                                swingPhase));
                const double swingDuration =
                    (1.0 - dutyCycle) / actualCycleFrequency;
                if (swingDuration > 1.0e-5)
                {
                    const double stepSpeed = strideLength * smoothDerivative /
                                             swingDuration;
                    targetVelocity[0] = forward[0] * stepSpeed;
                    targetVelocity[1] = forward[1] * stepSpeed;
                    targetVelocity[2] = 0.22 * 3.1415926535897932385 *
                        ScalarCos((float)(3.1415926535897932385 * swingPhase)) /
                        swingDuration;
                }
            }
            else
            {
                const double desiredStep =
                    (0.5 - phase / dutyCycle) * 0.18;
                step = desiredStep;
            }
            target[0] = root[0] + side * 0.48 * right[0] +
                        (0.15 + step) * forward[0];
            target[1] = root[1] + side * 0.48 * right[1] +
                        (0.15 + step) * forward[1];
            float footRotation[9];
            VoxelRigidBodyOrientationMatrix(
                &ragdoll->bodies[walkHumanoidFootBodies[foot]], footRotation);
            const VoxelRigidBody *footBody =
                &ragdoll->bodies[walkHumanoidFootBodies[foot]];
            const double footVerticalExtent =
                footBody->halfExtent[0] * fabs((double)footRotation[2]) +
                footBody->halfExtent[1] * fabs((double)footRotation[5]) +
                footBody->halfExtent[2] * fabs((double)footRotation[8]);
            double surfaceHeight;
            const bool surfaceFound = FindFootTargetSurface(
                collision, target[0], target[1], root[2], &surfaceHeight);
            target[2] = (surfaceFound ? surfaceHeight + footVerticalExtent + 0.02
                                      : footPosition[foot][2]) + lift;
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
                                       double deltaSeconds)
{
    double supportHeight = 0.0;
    double supportVelocity = 0.0;
    uint32_t supportCount = 0u;
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        const uint32_t bodyIndex = walkHumanoidFootBodies[foot];
        if (!WalkRagdollFootGrounded(ragdoll, collision, bodyIndex))
            continue;
        double position[3];
        double velocity[3];
        if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[bodyIndex], position) ||
            !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[bodyIndex], velocity))
            return false;
        supportHeight += position[2];
        supportVelocity += velocity[2];
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
        targetHeight = supportHeight / supportCount + 1.55;
        targetVelocity = supportVelocity / supportCount;
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
            pelvis->halfExtent[0] * fabs((double)rotation[2]) +
            pelvis->halfExtent[1] * fabs((double)rotation[5]) +
            pelvis->halfExtent[2] * fabs((double)rotation[8]);
        const double bottom = rootPosition[2] - bottomExtent;
        if (!WalkRagdollCoordinateSafeForVoxelQuery(bottom) ||
            !WalkRagdollCoordinateSafeForVoxelQuery(rootPosition[0]) ||
            !WalkRagdollCoordinateSafeForVoxelQuery(rootPosition[1]))
            return true;
        const int64_t centerX = (int64_t)floor(rootPosition[0]);
        const int64_t centerY = (int64_t)floor(rootPosition[1]);
        const int64_t nearestSurface = (int64_t)floor(bottom + 0.5);
        static const int8_t sampleOffsets[5][2] = {
            {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        };
        bool bodyContact = false;
        int64_t contactSurface = 0;
        double nearestGap = INFINITY;
        for (uint32_t sample = 0u; sample < 5u; ++sample)
            for (int64_t offset = -1; offset <= 1; ++offset)
            {
                const int64_t surface = nearestSurface + offset;
                const double gap = bottom - (double)surface;
                if (gap < -0.08 || gap > 0.10 || fabs(gap) >= nearestGap)
                    continue;
                VoxelBlockPhysics below = {0};
                VoxelBlockPhysics above = {0};
                collision->queryBlockPhysics(
                    collision->context, centerX + sampleOffsets[sample][0],
                    centerY + sampleOffsets[sample][1], surface - 1, &below);
                collision->queryBlockPhysics(
                    collision->context, centerX + sampleOffsets[sample][0],
                    centerY + sampleOffsets[sample][1], surface, &above);
                if ((below.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u &&
                    (above.flags & VOXEL_BLOCK_PHYSICS_SOLID) == 0u)
                {
                    bodyContact = true;
                    contactSurface = surface;
                    nearestGap = fabs(gap);
                }
            }
        if (!bodyContact)
            return true;
        targetHeight = (double)contactSurface + bottomExtent + 0.03;
    }
    double deltaZ = 34.0 * (targetHeight - rootPosition[2]) -
                    11.0 * (rootVelocity[2] - targetVelocity);
    if (deltaZ > 16.0)
        deltaZ = 16.0;
    else if (deltaZ < -16.0)
        deltaZ = -16.0;
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
        .penetrationSlop = 0.005,
        .sleepLinearSpeed = 0.02,
        .sleepAngularSpeed = 0.05,
        .sleepFrames = 30u,
    };
    physics->configureThread();
    physics->ragdollSettingsDefault(outRagdollSettings);
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
    if (physics == NULL || physics->ragdollStep == NULL || ragdoll == NULL ||
        controllerState == NULL ||
        collision == NULL || rigidSettings == NULL || ragdollSettings == NULL ||
        scratch == NULL || scratchBytes == 0u || inOutGrounded == NULL ||
        inOutFacingYaw == NULL || inOutGaitPhase == NULL || !isfinite(moveX) ||
        !isfinite(moveY) || !isfinite(deltaSeconds) || !(deltaSeconds > 0.0) ||
        deltaSeconds > 1.0 / 30.0 || !isfinite(*inOutFacingYaw) ||
        !isfinite(*inOutGaitPhase))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_INVALID_STATE;
        return false;
    }
    const double magnitudeSquared = moveX * moveX + moveY * moveY;
    if (!isfinite(magnitudeSquared))
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
        yawDelta -= floor((yawDelta + 3.1415926535897932385) / tau) * tau;
        const double maximumTurn = 3.5 * deltaSeconds;
        if (yawDelta > maximumTurn)
            yawDelta = maximumTurn;
        else if (yawDelta < -maximumTurn)
            yawDelta = -maximumTurn;
        *inOutFacingYaw += yawDelta;
    }
    if (magnitude > 0.05)
        *inOutGaitPhase += 6.2831853071795864769 *
                           (sprint ? 1.85 : 1.30) * magnitude * deltaSeconds;
    if (*inOutGaitPhase >= 6.2831853071795864769)
        *inOutGaitPhase -= floor(*inOutGaitPhase /
                                 6.2831853071795864769) *
                           6.2831853071795864769;
    const bool jumped = jump && WalkRagdollJumpIfGrounded(ragdoll, collision, 4.2);
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
    if (!WalkRagdollDrivePlanar(ragdoll, moveX, moveY, targetSpeed,
                                *inOutGrounded ? 3.5 : 0.25, deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    const double footYaw = magnitude > 0.05
        ? ScalarAtan2((float)-moveX, (float)moveY)
        : *inOutFacingYaw;
    if (!DriveFootsteps(ragdoll, controllerState, collision, rootPosition,
                        footYaw, magnitude,
                        *inOutGrounded, jumped, sprint, *inOutGaitPhase,
                        deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    if (!jumped && !DriveSupportedPelvisHeight(ragdoll, collision, deltaSeconds))
    {
        if (outFailure != NULL)
            *outFailure = WALK_HUMANOID_STEP_PLANAR_DRIVE;
        return false;
    }
    if (!WalkRagdollPoseDrive(ragdoll, *inOutFacingYaw, *inOutGaitPhase,
                              magnitude, deltaSeconds))
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
