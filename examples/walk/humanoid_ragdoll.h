#pragma once

#include "physics/ragdoll.h"
#include "math/scalar.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum
{
    WALK_RAGDOLL_PELVIS,
    WALK_RAGDOLL_TORSO,
    WALK_RAGDOLL_HEAD,
    WALK_RAGDOLL_LEFT_UPPER_ARM,
    WALK_RAGDOLL_RIGHT_UPPER_ARM,
    WALK_RAGDOLL_LEFT_FOREARM,
    WALK_RAGDOLL_RIGHT_FOREARM,
    WALK_RAGDOLL_LEFT_THIGH,
    WALK_RAGDOLL_RIGHT_THIGH,
    WALK_RAGDOLL_LEFT_SHIN,
    WALK_RAGDOLL_RIGHT_SHIN,
    WALK_RAGDOLL_LEFT_FOOT,
    WALK_RAGDOLL_RIGHT_FOOT,
    WALK_RAGDOLL_BODY_COUNT,
    WALK_RAGDOLL_JOINT_COUNT = WALK_RAGDOLL_BODY_COUNT - 1,
};

static const VoxelRagdollBodyDefinition walkRagdollBodies[WALK_RAGDOLL_BODY_COUNT] = {
    [WALK_RAGDOLL_PELVIS] = {{0.0, 0.0, 2.00}, {0.44, 0.23, 0.22}, 2.0, 0.0, 0.72},
    [WALK_RAGDOLL_TORSO] = {{0.0, 0.0, 2.69}, {0.34, 0.24, 0.47}, 3.0, 0.0, 0.72},
    [WALK_RAGDOLL_HEAD] = {{0.0, 0.0, 3.39}, {0.23, 0.22, 0.23}, 1.0, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_UPPER_ARM] = {{-0.57, 0.0, 2.66}, {0.23, 0.19, 0.33}, 0.9, 0.0, 0.68},
    [WALK_RAGDOLL_RIGHT_UPPER_ARM] = {{0.57, 0.0, 2.66}, {0.23, 0.19, 0.33}, 0.9, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_FOREARM] = {{-0.98, 0.0, 2.08}, {0.18, 0.17, 0.30}, 0.7, 0.0, 0.68},
    [WALK_RAGDOLL_RIGHT_FOREARM] = {{0.98, 0.0, 2.08}, {0.18, 0.17, 0.30}, 0.7, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_THIGH] = {{-0.42, 0.0, 1.38}, {0.21, 0.20, 0.40}, 1.4, 0.0, 0.75},
    [WALK_RAGDOLL_RIGHT_THIGH] = {{0.42, 0.0, 1.38}, {0.21, 0.20, 0.40}, 1.4, 0.0, 0.75},
    [WALK_RAGDOLL_LEFT_SHIN] = {{-0.42, 0.0, 0.79}, {0.15, 0.15, 0.19}, 0.8, 0.0, 0.75},
    [WALK_RAGDOLL_RIGHT_SHIN] = {{0.42, 0.0, 0.79}, {0.15, 0.15, 0.19}, 0.8, 0.0, 0.75},
    [WALK_RAGDOLL_LEFT_FOOT] = {{-0.50, 0.15, 0.45}, {0.16, 0.30, 0.15}, 0.55, 0.0, 0.25},
    [WALK_RAGDOLL_RIGHT_FOOT] = {{0.50, 0.15, 0.45}, {0.16, 0.30, 0.15}, 0.55, 0.0, 0.25},
};

static const VoxelRagdollBallJointDefinition walkRagdollJoints[WALK_RAGDOLL_JOINT_COUNT] = {
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_TORSO, {0.0, 0.0, 0.22}, {0.0, 0.0, -0.47}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_HEAD, {0.0, 0.0, 0.47}, {0.0, 0.0, -0.23}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_LEFT_UPPER_ARM, {-0.34, 0.0, 0.25}, {0.23, 0.0, 0.28}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_RIGHT_UPPER_ARM, {0.34, 0.0, 0.25}, {-0.23, 0.0, 0.28}},
    {WALK_RAGDOLL_LEFT_UPPER_ARM, WALK_RAGDOLL_LEFT_FOREARM, {-0.23, 0.0, -0.28}, {0.18, 0.0, 0.30}},
    {WALK_RAGDOLL_RIGHT_UPPER_ARM, WALK_RAGDOLL_RIGHT_FOREARM, {0.23, 0.0, -0.28}, {-0.18, 0.0, 0.30}},
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_LEFT_THIGH, {-0.42, 0.0, -0.22}, {0.0, 0.0, 0.40}},
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_RIGHT_THIGH, {0.42, 0.0, -0.22}, {0.0, 0.0, 0.40}},
    {WALK_RAGDOLL_LEFT_THIGH, WALK_RAGDOLL_LEFT_SHIN, {0.0, 0.0, -0.40}, {0.0, 0.0, 0.19}},
    {WALK_RAGDOLL_RIGHT_THIGH, WALK_RAGDOLL_RIGHT_SHIN, {0.0, 0.0, -0.40}, {0.0, 0.0, 0.19}},
    {WALK_RAGDOLL_LEFT_SHIN, WALK_RAGDOLL_LEFT_FOOT, {0.0, 0.0, -0.19}, {0.08, -0.15, 0.15}},
    {WALK_RAGDOLL_RIGHT_SHIN, WALK_RAGDOLL_RIGHT_FOOT, {0.0, 0.0, -0.19}, {-0.08, -0.15, 0.15}},
};

static inline void WalkRagdollQuaternionMultiply(const double left[4],
                                                const double right[4],
                                                double out[4])
{
    out[0] = left[3] * right[0] + left[0] * right[3] + left[1] * right[2] -
             left[2] * right[1];
    out[1] = left[3] * right[1] - left[0] * right[2] + left[1] * right[3] +
             left[2] * right[0];
    out[2] = left[3] * right[2] + left[0] * right[1] - left[1] * right[0] +
             left[2] * right[3];
    out[3] = left[3] * right[3] - left[0] * right[0] - left[1] * right[1] -
             left[2] * right[2];
}

static inline void WalkRagdollRotateVector(const double quaternion[4],
                                           const double vector[3],
                                           double out[3])
{
    const double x = quaternion[0];
    const double y = quaternion[1];
    const double z = quaternion[2];
    const double w = quaternion[3];
    out[0] = (1.0 - 2.0 * (y * y + z * z)) * vector[0] +
             2.0 * (x * y - z * w) * vector[1] +
             2.0 * (x * z + y * w) * vector[2];
    out[1] = 2.0 * (x * y + z * w) * vector[0] +
             (1.0 - 2.0 * (x * x + z * z)) * vector[1] +
             2.0 * (y * z - x * w) * vector[2];
    out[2] = 2.0 * (x * z - y * w) * vector[0] +
             2.0 * (y * z + x * w) * vector[1] +
             (1.0 - 2.0 * (x * x + y * y)) * vector[2];
}

static inline bool WalkRagdollMotorBody(VoxelRigidBody *body,
                                        const double target[4],
                                        double stiffness, double damping,
                                        double deltaSeconds)
{
    const double inverse[4] = {-body->orientation[0], -body->orientation[1],
                                -body->orientation[2], body->orientation[3]};
    double errorQuaternion[4];
    WalkRagdollQuaternionMultiply(target, inverse, errorQuaternion);
    if (errorQuaternion[3] < 0.0)
        for (uint32_t axis = 0u; axis < 4u; ++axis)
            errorQuaternion[axis] = -errorQuaternion[axis];

    const double vectorLength = ScalarSqrtDouble(
        errorQuaternion[0] * errorQuaternion[0] +
        errorQuaternion[1] * errorQuaternion[1] +
        errorQuaternion[2] * errorQuaternion[2]);
    double rotationError[3] = {0.0, 0.0, 0.0};
    if (vectorLength > 1.0e-9)
    {
        const double angle = 2.0 * ScalarAtan2((float)vectorLength,
                                                (float)errorQuaternion[3]);
        const double scale = angle / vectorLength;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            rotationError[axis] = errorQuaternion[axis] * scale;
    }

    double angularVelocity[3];
    if (!VoxelRigidBodyAngularVelocity(body, angularVelocity))
        return false;
    double delta[3];
    double accelerationSquared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        delta[axis] = (stiffness * rotationError[axis] -
                       damping * angularVelocity[axis]) * deltaSeconds;
        accelerationSquared += delta[axis] * delta[axis];
    }
    const double maximumDelta = 160.0 * deltaSeconds;
    const double deltaLength = ScalarSqrtDouble(accelerationSquared);
    if (deltaLength > maximumDelta)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            delta[axis] *= maximumDelta / deltaLength;
    return VoxelRigidBodyAddAngularVelocity(body, delta);
}

static inline bool WalkRagdollPoseDrive(VoxelRagdoll *ragdoll, double facingYaw,
                                        double gaitPhase, double gaitAmount,
                                        double deltaSeconds)
{
    if (ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT ||
        !isfinite(facingYaw) || !isfinite(gaitPhase) || !isfinite(gaitAmount) ||
        gaitAmount < 0.0 || gaitAmount > 1.0 ||
        !(deltaSeconds > 0.0) || deltaSeconds > 1.0 / 30.0)
        return false;

    const double gaitSin = ScalarSin((float)gaitPhase);
    const double swing = 0.34 * gaitAmount * gaitSin;
    const double oppositeSwing = -swing;
    const double armSwing = 0.12 * gaitAmount * gaitSin;
    const double leftKnee = gaitAmount * (gaitSin > 0.0 ? gaitSin : 0.0) * 0.52;
    const double rightKnee = gaitAmount * (gaitSin < 0.0 ? -gaitSin : 0.0) * 0.52;
    const double pitches[WALK_RAGDOLL_BODY_COUNT] = {
        [WALK_RAGDOLL_PELVIS] = 0.0,
        [WALK_RAGDOLL_TORSO] = 0.0,
        [WALK_RAGDOLL_HEAD] = 0.0,
        [WALK_RAGDOLL_LEFT_UPPER_ARM] = -armSwing,
        [WALK_RAGDOLL_RIGHT_UPPER_ARM] = armSwing,
        [WALK_RAGDOLL_LEFT_FOREARM] = -armSwing * 0.6,
        [WALK_RAGDOLL_RIGHT_FOREARM] = armSwing * 0.6,
        [WALK_RAGDOLL_LEFT_THIGH] = swing,
        [WALK_RAGDOLL_RIGHT_THIGH] = oppositeSwing,
        [WALK_RAGDOLL_LEFT_SHIN] = swing - leftKnee,
        [WALK_RAGDOLL_RIGHT_SHIN] = oppositeSwing - rightKnee,
        [WALK_RAGDOLL_LEFT_FOOT] = 0.0,
        [WALK_RAGDOLL_RIGHT_FOOT] = 0.0,
    };
    const double armTilts[WALK_RAGDOLL_BODY_COUNT] = {
        [WALK_RAGDOLL_LEFT_UPPER_ARM] = 0.18,
        [WALK_RAGDOLL_RIGHT_UPPER_ARM] = -0.18,
        [WALK_RAGDOLL_LEFT_FOREARM] = 0.18,
        [WALK_RAGDOLL_RIGHT_FOREARM] = -0.18,
    };
    const double yawSin = ScalarSin((float)(facingYaw * 0.5));
    const double yawCos = ScalarCos((float)(facingYaw * 0.5));
    double rootPosition[3];
    double rootVelocity[3];
    if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_PELVIS],
                                     rootPosition) ||
        !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[WALK_RAGDOLL_PELVIS],
                                      rootVelocity))
        return false;
    double torsoAngularVelocity[3];
    if (!VoxelRigidBodyAngularVelocity(&ragdoll->bodies[WALK_RAGDOLL_TORSO],
                                       torsoAngularVelocity))
        return false;
    const double torsoAngularSpeed = ScalarSqrtDouble(
        torsoAngularVelocity[0] * torsoAngularVelocity[0] +
        torsoAngularVelocity[1] * torsoAngularVelocity[1] +
        torsoAngularVelocity[2] * torsoAngularVelocity[2]);
    double fallBrace = (-rootVelocity[2] - 1.0) / 5.0;
    double tumbleBrace = (torsoAngularSpeed - 3.0) / 7.0;
    if (fallBrace < 0.0)
        fallBrace = 0.0;
    if (tumbleBrace < 0.0)
        tumbleBrace = 0.0;
    double braceAmount = fallBrace > tumbleBrace ? fallBrace : tumbleBrace;
    if (braceAmount > 1.0)
        braceAmount = 1.0;
    double leftFootPosition[3];
    double rightFootPosition[3];
    double leftFootVelocity[3];
    double rightFootVelocity[3];
    if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_LEFT_FOOT],
                                     leftFootPosition) ||
        !VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_RIGHT_FOOT],
                                     rightFootPosition) ||
        !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[WALK_RAGDOLL_LEFT_FOOT],
                                      leftFootVelocity) ||
        !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[WALK_RAGDOLL_RIGHT_FOOT],
                                      rightFootVelocity))
        return false;
    const double targetRootHeight =
        0.5 * (leftFootPosition[2] + rightFootPosition[2]) +
        walkRagdollBodies[WALK_RAGDOLL_PELVIS].offset[2] -
        0.5 * (walkRagdollBodies[WALK_RAGDOLL_LEFT_FOOT].offset[2] +
               walkRagdollBodies[WALK_RAGDOLL_RIGHT_FOOT].offset[2]);
    const double targetRootVelocity =
        0.5 * (leftFootVelocity[2] + rightFootVelocity[2]);
    double rootAccelerationZ =
        60.0 * (targetRootHeight - rootPosition[2]) -
        14.0 * (rootVelocity[2] - targetRootVelocity);
    if (rootAccelerationZ > 32.0)
        rootAccelerationZ = 32.0;
    else if (rootAccelerationZ < -32.0)
        rootAccelerationZ = -32.0;
    const double rootLift[3] = {0.0, 0.0, rootAccelerationZ * deltaSeconds};
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
        if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[body], rootLift))
            return false;
    double targetOrientations[WALK_RAGDOLL_BODY_COUNT][4];
    for (uint32_t body = 0u; body < WALK_RAGDOLL_BODY_COUNT; ++body)
    {
        double targetPitch = pitches[body];
        if (body == WALK_RAGDOLL_LEFT_UPPER_ARM ||
            body == WALK_RAGDOLL_RIGHT_UPPER_ARM)
            targetPitch -= 0.7 * braceAmount;
        else if (body == WALK_RAGDOLL_LEFT_FOREARM ||
                 body == WALK_RAGDOLL_RIGHT_FOREARM)
            targetPitch -= 0.45 * braceAmount;
        const double halfPitch = targetPitch * 0.5;
        const double halfTilt = armTilts[body] * 0.5;
        const double pitchSin = ScalarSin((float)halfPitch);
        const double pitchCos = ScalarCos((float)halfPitch);
        const double tiltQuaternion[4] = {0.0, ScalarSin((float)halfTilt), 0.0,
                                           ScalarCos((float)halfTilt)};
        const double pitchQuaternion[4] = {pitchSin, 0.0, 0.0, pitchCos};
        const double yawQuaternion[4] = {0.0, 0.0, yawSin, yawCos};
        double yawTilt[4];
        double target[4];
        WalkRagdollQuaternionMultiply(yawQuaternion, tiltQuaternion, yawTilt);
        WalkRagdollQuaternionMultiply(yawTilt, pitchQuaternion, target);
        memcpy(targetOrientations[body], target, sizeof(target));
        const double stiffness = body == WALK_RAGDOLL_PELVIS ? 85.0 :
                                 (body <= WALK_RAGDOLL_HEAD ? 50.0 :
                                  (body >= WALK_RAGDOLL_LEFT_THIGH ? 60.0 : 20.0));
        const double damping = body == WALK_RAGDOLL_PELVIS ? 24.0 :
                               (body <= WALK_RAGDOLL_HEAD ? 16.0 :
                                (body >= WALK_RAGDOLL_LEFT_THIGH ? 18.0 : 8.0));
        if (!WalkRagdollMotorBody(&ragdoll->bodies[body], target,
                                  stiffness, damping, deltaSeconds))
            return false;
    }

    double targetPositions[WALK_RAGDOLL_BODY_COUNT][3] = {{0.0}};
    bool positioned[WALK_RAGDOLL_BODY_COUNT] = {false};
    memcpy(targetPositions[ragdoll->rootBody], rootPosition, sizeof(rootPosition));
    positioned[ragdoll->rootBody] = true;
    uint32_t positionedCount = 1u;
    for (uint32_t pass = 0u; pass < ragdoll->bodyCount &&
                             positionedCount < ragdoll->bodyCount; ++pass)
    {
        bool progressed = false;
        for (uint32_t jointIndex = 0u; jointIndex < ragdoll->jointCount; ++jointIndex)
        {
            const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[jointIndex];
            uint32_t parent;
            uint32_t child;
            const double *parentAnchor;
            const double *childAnchor;
            if (positioned[joint->bodyA] && !positioned[joint->bodyB])
            {
                parent = joint->bodyA;
                child = joint->bodyB;
                parentAnchor = joint->anchorA;
                childAnchor = joint->anchorB;
            }
            else if (positioned[joint->bodyB] && !positioned[joint->bodyA])
            {
                parent = joint->bodyB;
                child = joint->bodyA;
                parentAnchor = joint->anchorB;
                childAnchor = joint->anchorA;
            }
            else
                continue;
            double parentOffset[3];
            double childOffset[3];
            WalkRagdollRotateVector(targetOrientations[parent], parentAnchor,
                                    parentOffset);
            WalkRagdollRotateVector(targetOrientations[child], childAnchor,
                                    childOffset);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                targetPositions[child][axis] = targetPositions[parent][axis] +
                                               parentOffset[axis] - childOffset[axis];
            positioned[child] = true;
            ++positionedCount;
            progressed = true;
        }
        if (!progressed)
            break;
    }
    if (positionedCount != ragdoll->bodyCount)
        return false;

    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        if (body == ragdoll->rootBody)
            continue;
        double position[3];
        double velocity[3];
        if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[body], position) ||
            !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[body], velocity))
            return false;
        double acceleration[3];
        double accelerationSquared = 0.0;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            acceleration[axis] = 24.0 * (targetPositions[body][axis] - position[axis]) -
                                 8.0 * (velocity[axis] - rootVelocity[axis]);
            accelerationSquared += acceleration[axis] * acceleration[axis];
        }
        const double accelerationLength = ScalarSqrtDouble(accelerationSquared);
            if (accelerationLength > 32.0)
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                acceleration[axis] *= 32.0 / accelerationLength;
        const double deltaVelocity[3] = {
            acceleration[0] * deltaSeconds,
            acceleration[1] * deltaSeconds,
            acceleration[2] * deltaSeconds,
        };
        if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[body], deltaVelocity))
            return false;
    }
    return true;
}

static inline bool WalkRagdollDrivePlanar(VoxelRagdoll *ragdoll,
                                          double directionX, double directionY,
                                          double targetSpeed,
                                          double maximumAcceleration,
                                          double deltaSeconds)
{
    if (ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT ||
        !isfinite(directionX) || !isfinite(directionY) ||
        !isfinite(targetSpeed) || !isfinite(maximumAcceleration) ||
        directionX * directionX + directionY * directionY > 1.000001 ||
        targetSpeed < 0.0 || !(maximumAcceleration > 0.0) ||
        !(deltaSeconds > 0.0) || deltaSeconds > 1.0 / 30.0)
        return false;
    double rootVelocity[3];
    if (!VoxelRigidBodyLinearVelocity(&ragdoll->bodies[ragdoll->rootBody],
                                      rootVelocity))
        return false;
    const double maximumDelta = maximumAcceleration * deltaSeconds;
    const double targetDelta[2] = {
        directionX * targetSpeed - rootVelocity[0],
        directionY * targetSpeed - rootVelocity[1],
    };
    const double delta[2] = {
        targetDelta[0] < -maximumDelta ? -maximumDelta :
            (targetDelta[0] > maximumDelta ? maximumDelta : targetDelta[0]),
        targetDelta[1] < -maximumDelta ? -maximumDelta :
            (targetDelta[1] > maximumDelta ? maximumDelta : targetDelta[1]),
    };
    if (delta[0] == 0.0 && delta[1] == 0.0)
        return true;
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        const double bodyDelta[3] = {delta[0], delta[1], 0.0};
        if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[body], bodyDelta))
            return false;
        VoxelRigidBodyWake(&ragdoll->bodies[body]);
    }
    return true;
}

static inline bool WalkRagdollGrounded(const VoxelRagdoll *ragdoll,
                                       const VoxelCollisionSource *collision)
{
    if (ragdoll == NULL || !ragdoll->initialized || collision == NULL ||
        collision->queryBlockPhysics == NULL ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT)
        return false;

    for (uint32_t footIndex = WALK_RAGDOLL_LEFT_FOOT;
         footIndex <= WALK_RAGDOLL_RIGHT_FOOT; ++footIndex)
    {
        const VoxelRigidBody *foot = &ragdoll->bodies[footIndex];
        double center[3];
        double linearVelocity[3];
        float rotation[9];
        if (!VoxelRigidBodyLocalPosition(foot, center) ||
            !VoxelRigidBodyLinearVelocity(foot, linearVelocity))
            continue;
        if (linearVelocity[2] > 0.5)
            continue;
        VoxelRigidBodyOrientationMatrix(foot, rotation);
        const double localZ = rotation[8] >= 0.0 ? -foot->halfExtent[2]
                                                 : foot->halfExtent[2];
        for (int32_t xSign = -1; xSign <= 1; xSign += 2)
            for (int32_t ySign = -1; ySign <= 1; ySign += 2)
            {
                const double local[3] = {
                    xSign * foot->halfExtent[0],
                    ySign * foot->halfExtent[1],
                    localZ,
                };
                double point[3];
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                    point[axis] = center[axis] +
                        (double)rotation[axis] * local[0] +
                        (double)rotation[3u + axis] * local[1] +
                        (double)rotation[6u + axis] * local[2];
                if (!isfinite(point[0]) || !isfinite(point[1]) ||
                    !isfinite(point[2]) || point[0] < (double)INT64_MIN + 2.0 ||
                    point[0] > (double)INT64_MAX - 2.0 ||
                    point[1] < (double)INT64_MIN + 2.0 ||
                    point[1] > (double)INT64_MAX - 2.0 ||
                    point[2] < (double)INT64_MIN + 2.0 ||
                    point[2] > (double)INT64_MAX - 2.0)
                    continue;
                const int64_t blockX = (int64_t)floor(point[0]);
                const int64_t blockY = (int64_t)floor(point[1]);
                const int64_t nearestSurface = (int64_t)floor(point[2] + 0.5);
                for (int64_t offset = -1; offset <= 1; ++offset)
                {
                    const int64_t surfaceZ = nearestSurface + offset;
                    if (fabs(point[2] - (double)surfaceZ) > 0.16)
                        continue;
                    VoxelBlockPhysics below = {0};
                    VoxelBlockPhysics above = {0};
                    collision->queryBlockPhysics(collision->context, blockX, blockY,
                                                 surfaceZ - 1, &below);
                    collision->queryBlockPhysics(collision->context, blockX, blockY,
                                                 surfaceZ, &above);
                    if ((below.flags & VOXEL_BLOCK_PHYSICS_SOLID) != 0u &&
                        (above.flags & VOXEL_BLOCK_PHYSICS_SOLID) == 0u)
                        return true;
                }
            }
    }
    return false;
}

static inline bool WalkRagdollJumpIfGrounded(VoxelRagdoll *ragdoll,
                                             const VoxelCollisionSource *collision,
                                             double upwardSpeed)
{
    return WalkRagdollGrounded(ragdoll, collision) &&
           VoxelRagdollJump(ragdoll, upwardSpeed);
}
