#pragma once

#include "physics/ragdoll.h"
#include "math/scalar.h"
#include "walk_math.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static inline bool WalkRagdollCoordinateSafeForVoxelQuery(double coordinate)
{
    /* Leave ample room for floor(), neighbor samples, and surface +/- 1.
     * Comparing against INT64_MAX as a double is unsafe because it rounds to
     * 2^63, which is outside the representable int64_t range. */
    return WalkMathFinite(coordinate) && coordinate > -0x1p62 &&
           coordinate < 0x1p62;
}

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
    /* Metres, with the sole at zero and crown at 1.80 in the neutral stance.
     * Masses total 75 kg. Leg segments share length rather than the old
     * oversized thighs and very short shins. */
    [WALK_RAGDOLL_PELVIS] = {{0.0, 0.0, 1.00}, {0.17, 0.105, 0.10}, 12.0, 0.0, 0.72},
    [WALK_RAGDOLL_TORSO] = {{0.0, 0.0, 1.33}, {0.21, 0.11, 0.23}, 28.0, 0.0, 0.72},
    [WALK_RAGDOLL_HEAD] = {{0.0, 0.0, 1.68}, {0.105, 0.10, 0.12}, 5.0, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_UPPER_ARM] = {{-0.285, 0.0, 1.34}, {0.075, 0.07, 0.17}, 2.2, 0.0, 0.68},
    [WALK_RAGDOLL_RIGHT_UPPER_ARM] = {{0.285, 0.0, 1.34}, {0.075, 0.07, 0.17}, 2.2, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_FOREARM] = {{-0.335, 0.0, 1.055}, {0.055, 0.055, 0.135}, 1.6, 0.0, 0.68},
    [WALK_RAGDOLL_RIGHT_FOREARM] = {{0.335, 0.0, 1.055}, {0.055, 0.055, 0.135}, 1.6, 0.0, 0.68},
    [WALK_RAGDOLL_LEFT_THIGH] = {{-0.10, 0.0, 0.70}, {0.085, 0.085, 0.20}, 7.0, 0.0, 0.75},
    [WALK_RAGDOLL_RIGHT_THIGH] = {{0.10, 0.0, 0.70}, {0.085, 0.085, 0.20}, 7.0, 0.0, 0.75},
    [WALK_RAGDOLL_LEFT_SHIN] = {{-0.10, 0.0, 0.31}, {0.065, 0.065, 0.19}, 3.0, 0.0, 0.75},
    [WALK_RAGDOLL_RIGHT_SHIN] = {{0.10, 0.0, 0.31}, {0.065, 0.065, 0.19}, 3.0, 0.0, 0.75},
    [WALK_RAGDOLL_LEFT_FOOT] = {{-0.10, 0.06, 0.06}, {0.075, 0.14, 0.06}, 1.2, 0.0, 0.85},
    [WALK_RAGDOLL_RIGHT_FOOT] = {{0.10, 0.06, 0.06}, {0.075, 0.14, 0.06}, 1.2, 0.0, 0.85},
};

static const VoxelRagdollBallJointDefinition walkRagdollJoints[WALK_RAGDOLL_JOINT_COUNT] = {
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_TORSO, {0.0, 0.0, 0.10}, {0.0, 0.0, -0.23}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_HEAD, {0.0, 0.0, 0.23}, {0.0, 0.0, -0.12}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_LEFT_UPPER_ARM, {-0.21, 0.0, 0.16}, {0.075, 0.0, 0.15}},
    {WALK_RAGDOLL_TORSO, WALK_RAGDOLL_RIGHT_UPPER_ARM, {0.21, 0.0, 0.16}, {-0.075, 0.0, 0.15}},
    {WALK_RAGDOLL_LEFT_UPPER_ARM, WALK_RAGDOLL_LEFT_FOREARM, {-0.04, 0.0, -0.15}, {0.01, 0.0, 0.135}},
    {WALK_RAGDOLL_RIGHT_UPPER_ARM, WALK_RAGDOLL_RIGHT_FOREARM, {0.04, 0.0, -0.15}, {-0.01, 0.0, 0.135}},
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_LEFT_THIGH, {-0.10, 0.0, -0.10}, {0.0, 0.0, 0.20}},
    {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_RIGHT_THIGH, {0.10, 0.0, -0.10}, {0.0, 0.0, 0.20}},
    {WALK_RAGDOLL_LEFT_THIGH, WALK_RAGDOLL_LEFT_SHIN, {0.0, 0.0, -0.20}, {0.0, 0.0, 0.19}},
    {WALK_RAGDOLL_RIGHT_THIGH, WALK_RAGDOLL_RIGHT_SHIN, {0.0, 0.0, -0.20}, {0.0, 0.0, 0.19}},
    {WALK_RAGDOLL_LEFT_SHIN, WALK_RAGDOLL_LEFT_FOOT, {0.0, 0.0, -0.19}, {0.0, -0.06, 0.06}},
    {WALK_RAGDOLL_RIGHT_SHIN, WALK_RAGDOLL_RIGHT_FOOT, {0.0, 0.0, -0.19}, {0.0, -0.06, 0.06}},
};

static inline bool WalkRagdollHasHumanoidTopology(const VoxelRagdoll *ragdoll)
{
    if (ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT ||
        ragdoll->jointCount != WALK_RAGDOLL_JOINT_COUNT ||
        ragdoll->rootBody != WALK_RAGDOLL_PELVIS)
        return false;
    for (uint32_t joint = 0u; joint < WALK_RAGDOLL_JOINT_COUNT; ++joint)
        if (ragdoll->joints[joint].bodyA != walkRagdollJoints[joint].bodyA ||
            ragdoll->joints[joint].bodyB != walkRagdollJoints[joint].bodyB)
            return false;
    return true;
}

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

/* One phase definition for the foot trajectory and the joint pose. The knee
 * bends during swing, while the planted leg extends through stance. */
static inline double WalkRagdollFootPhase(double gaitPhase, uint32_t foot)
{
    const double cycle = gaitPhase / 6.2831853071795864769 + (foot ? 0.5 : 0.0);
    return cycle - WalkMathFloor(cycle);
}

#define WALK_RAGDOLL_STANCE_FRACTION 0.60

static inline void WalkRagdollLegPose(double phase, double amount,
                                      double *hip, double *knee)
{
    if (phase < WALK_RAGDOLL_STANCE_FRACTION)
    {
        *hip = 0.28 * amount * (1.0 - 2.0 * phase / WALK_RAGDOLL_STANCE_FRACTION);
        *knee = 0.04 * amount;
    }
    else
    {
        const double swing = (phase - WALK_RAGDOLL_STANCE_FRACTION) /
                             (1.0 - WALK_RAGDOLL_STANCE_FRACTION);
        const double smooth = swing * swing * (3.0 - 2.0 * swing);
        *hip = 0.28 * amount * (2.0 * smooth - 1.0);
        *knee = amount * (0.04 + 0.52 * ScalarSin((float)(3.1415926535897932385 * swing)));
    }
}

static inline bool WalkRagdollLegTargets(const VoxelRagdoll *ragdoll, double yaw,
                                         double orientations[WALK_RAGDOLL_BODY_COUNT][4])
{
    const double forward[3] = {-ScalarSin((float)yaw), ScalarCos((float)yaw), 0.0};
    const double right[3] = {forward[1], -forward[0], 0.0};
    const double yawQuaternion[4] = {0.0, 0.0, ScalarSin((float)(yaw * 0.5)),
                                     ScalarCos((float)(yaw * 0.5))};
    double root[3];
    if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_PELVIS], root))
        return false;
    for (uint32_t leg = 0u; leg < 2u; ++leg)
    {
        const VoxelRagdollBallJointDefinition *hipJoint = &ragdoll->joints[6u + leg];
        const VoxelRagdollBallJointDefinition *kneeJoint = &ragdoll->joints[8u + leg];
        const VoxelRagdollBallJointDefinition *ankleJoint = &ragdoll->joints[10u + leg];
        const VoxelRigidBody *foot = &ragdoll->bodies[WALK_RAGDOLL_LEFT_FOOT + leg];
        double hip[3], ankle[3], ankleOffset[3];
        WalkRagdollRotateVector(orientations[WALK_RAGDOLL_PELVIS], hipJoint->anchorA, hip);
        WalkRagdollRotateVector(foot->orientation, ankleJoint->anchorB, ankleOffset);
        if (!VoxelRigidBodyLocalPosition(foot, ankle))
            return false;
        double axis[3], distanceSquared = 0.0;
        for (uint32_t coordinate = 0u; coordinate < 3u; ++coordinate)
        {
            hip[coordinate] += root[coordinate];
            ankle[coordinate] += ankleOffset[coordinate];
            axis[coordinate] = ankle[coordinate] - hip[coordinate];
            distanceSquared += axis[coordinate] * axis[coordinate];
        }
        const double distance = ScalarSqrtDouble(distanceSquared);
        if (distance < 0.10 || axis[2] > -0.10)
            continue; /* Fallen pose: let the recovery motors unfold the legs. */
        for (uint32_t coordinate = 0u; coordinate < 3u; ++coordinate)
            axis[coordinate] /= distance;
        const double upper = hipJoint->anchorB[2] - kneeJoint->anchorA[2];
        const double lower = kneeJoint->anchorB[2] - ankleJoint->anchorA[2];
        double reach = distance;
        if (reach > upper + lower - 0.0001)
            reach = upper + lower - 0.0001;
        const double minimumReach = WalkMathAbs(upper - lower) + 0.001;
        if (reach < minimumReach)
            reach = minimumReach;
        const double along = (upper * upper - lower * lower + reach * reach) / (2.0 * reach);
        const double bendSquared = upper * upper - along * along;
        const double bend = ScalarSqrtDouble(bendSquared > 0.0 ? bendSquared : 0.0);
        const double projection = forward[0] * axis[0] + forward[1] * axis[1];
        double pole[3], poleSquared = 0.0;
        for (uint32_t coordinate = 0u; coordinate < 3u; ++coordinate)
        {
            pole[coordinate] = forward[coordinate] - projection * axis[coordinate];
            poleSquared += pole[coordinate] * pole[coordinate];
        }
        const double poleLength = ScalarSqrtDouble(poleSquared);
        if (poleLength < 1.0e-6)
            continue;
        double knee[3], reachableAnkle[3];
        for (uint32_t coordinate = 0u; coordinate < 3u; ++coordinate)
        {
            knee[coordinate] = hip[coordinate] + along * axis[coordinate] +
                               bend * pole[coordinate] / poleLength;
            reachableAnkle[coordinate] = hip[coordinate] + reach * axis[coordinate];
        }
        for (uint32_t segment = 0u; segment < 2u; ++segment)
        {
            const double *top = segment == 0u ? hip : knee;
            const double *bottom = segment == 0u ? knee : reachableAnkle;
            const double length = segment == 0u ? upper : lower;
            double up[3];
            for (uint32_t coordinate = 0u; coordinate < 3u; ++coordinate)
                up[coordinate] = (top[coordinate] - bottom[coordinate]) / length;
            const double localX = up[0] * right[0] + up[1] * right[1];
            const double localY = up[0] * forward[0] + up[1] * forward[1];
            double tilt[4] = {-localY, localX, 0.0, 1.0 + up[2]};
            const double norm = ScalarSqrtDouble(tilt[0] * tilt[0] + tilt[1] * tilt[1] +
                                                 tilt[3] * tilt[3]);
            if (norm < 1.0e-6)
                continue;
            for (uint32_t coordinate = 0u; coordinate < 4u; ++coordinate)
                tilt[coordinate] /= norm;
            WalkRagdollQuaternionMultiply(yawQuaternion, tilt,
                orientations[(segment == 0u ? WALK_RAGDOLL_LEFT_THIGH : WALK_RAGDOLL_LEFT_SHIN) + leg]);
        }
    }
    return true;
}

static inline bool WalkRagdollPoseDriveWithSupport(VoxelRagdoll *ragdoll, double facingYaw,
                                        double gaitPhase, double gaitAmount,
                                        double deltaSeconds, const bool planted[2])
{
    if (!WalkRagdollHasHumanoidTopology(ragdoll) ||
        !WalkMathFinite(facingYaw) || !WalkMathFinite(gaitPhase) || !WalkMathFinite(gaitAmount) ||
        gaitAmount < 0.0 || gaitAmount > 1.0 ||
        !(deltaSeconds > 0.0) || deltaSeconds > 1.0 / 30.0)
        return false;

    const double gaitSin = ScalarSin((float)gaitPhase);
    double swing, oppositeSwing, leftKnee, rightKnee;
    WalkRagdollLegPose(WalkRagdollFootPhase(gaitPhase, 0u), gaitAmount,
                       &swing, &leftKnee);
    WalkRagdollLegPose(WalkRagdollFootPhase(gaitPhase, 1u), gaitAmount,
                       &oppositeSwing, &rightKnee);
    const double armSwing = 0.12 * gaitAmount * gaitSin;
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
        [WALK_RAGDOLL_LEFT_UPPER_ARM] = 0.0,
        [WALK_RAGDOLL_RIGHT_UPPER_ARM] = 0.0,
        [WALK_RAGDOLL_LEFT_FOREARM] = 0.0,
        [WALK_RAGDOLL_RIGHT_FOREARM] = 0.0,
    };
    const double yawSin = ScalarSin((float)(facingYaw * 0.5));
    const double yawCos = ScalarCos((float)(facingYaw * 0.5));
    double rootVelocity[3];
    if (!VoxelRigidBodyLinearVelocity(&ragdoll->bodies[WALK_RAGDOLL_PELVIS],
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
        double yawQuaternion[4] = {0.0, 0.0, yawSin, yawCos};
        if (body >= WALK_RAGDOLL_LEFT_FOOT && planted != NULL &&
            planted[body - WALK_RAGDOLL_LEFT_FOOT])
        {
            /* Do not rotate a planted sole through the other foot. It adopts
             * the new heading on its next swing, just like a turning step. */
            float rotation[9];
            VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[body], rotation);
            const double halfYaw = 0.5 * ScalarAtan2(rotation[1], rotation[0]);
            yawQuaternion[2] = ScalarSin((float)halfYaw);
            yawQuaternion[3] = ScalarCos((float)halfYaw);
        }
        double yawTilt[4];
        double target[4];
        WalkRagdollQuaternionMultiply(yawQuaternion, tiltQuaternion, yawTilt);
        WalkRagdollQuaternionMultiply(yawTilt, pitchQuaternion, target);
        memcpy(targetOrientations[body], target, sizeof(target));
    }
    /* Match leg orientation to the actual support locations. An independent
     * sine pose otherwise bends a knee one way while the foot motor pulls the
     * ankle another way, especially during turns and unequal foot heights. */
    if (planted != NULL && (planted[0] || planted[1]) &&
        !WalkRagdollLegTargets(ragdoll, facingYaw, targetOrientations))
        return false;
    float torsoRotation[9];
    VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[WALK_RAGDOLL_TORSO],
                                    torsoRotation);
    double recovery = (0.98 - (double)torsoRotation[8]) / 0.70;
    if (recovery < 0.0)
        recovery = 0.0;
    else if (recovery > 1.0)
        recovery = 1.0;
    for (uint32_t body = 0u; body < WALK_RAGDOLL_BODY_COUNT; ++body)
    {
        const bool controlledSole = planted != NULL && body >= WALK_RAGDOLL_LEFT_FOOT;
        double stiffness = controlledSole ? 180.0 :
                           body == WALK_RAGDOLL_PELVIS ? 85.0 :
                           (body <= WALK_RAGDOLL_HEAD ? 50.0 :
                            (body >= WALK_RAGDOLL_LEFT_THIGH ? 60.0 : 20.0));
        double damping = controlledSole ? 42.0 :
                         body == WALK_RAGDOLL_PELVIS ? 24.0 :
                         (body <= WALK_RAGDOLL_HEAD ? 16.0 :
                          (body >= WALK_RAGDOLL_LEFT_THIGH ? 18.0 : 8.0));
        /* A badly tilted torso needs extra righting torque. Fade it out before
         * the upright pose so normal walking keeps its softer balance motor. */
        if (body == WALK_RAGDOLL_PELVIS || body == WALK_RAGDOLL_TORSO)
        {
            stiffness *= 1.0 + 3.0 * recovery;
            damping *= 1.0 + recovery;
        }
        if (!WalkRagdollMotorBody(&ragdoll->bodies[body], targetOrientations[body],
                                  stiffness, damping, deltaSeconds))
            return false;
    }

    double rootPosition[3];
    if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[ragdoll->rootBody],
                                     rootPosition))
        return false;
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
        if (body == ragdoll->rootBody || body == WALK_RAGDOLL_LEFT_FOOT ||
            body == WALK_RAGDOLL_RIGHT_FOOT)
            continue;
        double position[3];
        double velocity[3];
        if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[body], position) ||
            !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[body], velocity))
            return false;
        double acceleration[3];
        double accelerationSquared = 0.0;
        double targetVelocity[3] = {rootVelocity[0], rootVelocity[1], rootVelocity[2]};
        if (planted != NULL && body >= WALK_RAGDOLL_LEFT_THIGH &&
            body <= WALK_RAGDOLL_RIGHT_SHIN)
        {
            const uint32_t leg = (body - WALK_RAGDOLL_LEFT_THIGH) & 1u;
            double footVelocity[3];
            if (!VoxelRigidBodyLinearVelocity(
                    &ragdoll->bodies[WALK_RAGDOLL_LEFT_FOOT + leg], footVelocity))
                return false;
            const double rootWeight = body <= WALK_RAGDOLL_RIGHT_THIGH ? 0.75 : 0.25;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                targetVelocity[axis] = rootWeight * rootVelocity[axis] +
                    (1.0 - rootWeight) * (planted[leg] ? 0.0 : footVelocity[axis]);
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            acceleration[axis] = 12.0 * (targetPositions[body][axis] - position[axis]) -
                                 6.0 * (velocity[axis] - targetVelocity[axis]);
            accelerationSquared += acceleration[axis] * acceleration[axis];
        }
        const double accelerationLength = ScalarSqrtDouble(accelerationSquared);
        if (accelerationLength > 18.0)
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                acceleration[axis] *= 18.0 / accelerationLength;
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

static inline bool WalkRagdollPoseDrive(VoxelRagdoll *ragdoll, double facingYaw,
                                        double gaitPhase, double gaitAmount,
                                        double deltaSeconds)
{
    return WalkRagdollPoseDriveWithSupport(ragdoll, facingYaw, gaitPhase,
                                          gaitAmount, deltaSeconds, NULL);
}

static inline bool WalkRagdollDrivePlanar(VoxelRagdoll *ragdoll,
                                          double directionX, double directionY,
                                          double targetSpeed,
                                          double maximumAcceleration,
                                          double deltaSeconds)
{
    if (ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT ||
        !WalkMathFinite(directionX) || !WalkMathFinite(directionY) ||
        !WalkMathFinite(targetSpeed) || !WalkMathFinite(maximumAcceleration) ||
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
    const double deltaLength = ScalarSqrtDouble(targetDelta[0] * targetDelta[0] +
                                                targetDelta[1] * targetDelta[1]);
    const double scale = deltaLength > maximumDelta ? maximumDelta / deltaLength : 1.0;
    const double delta[2] = {targetDelta[0] * scale, targetDelta[1] * scale};
    if (delta[0] == 0.0 && delta[1] == 0.0)
        return true;
    const double bodyDelta[3] = {delta[0], delta[1], 0.0};
    if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[ragdoll->rootBody],
                                         bodyDelta))
        return false;
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
        VoxelRigidBodyWake(&ragdoll->bodies[body]);
    return true;
}

static inline bool WalkRagdollFootContact(const VoxelRagdoll *ragdoll,
                                          const VoxelCollisionSource *collision,
                                          uint32_t footIndex, double maximumUpwardSpeed)
{
    if (ragdoll == NULL || !ragdoll->initialized || collision == NULL ||
        collision->queryBlockPhysics == NULL ||
        ragdoll->bodyCount != WALK_RAGDOLL_BODY_COUNT ||
        (footIndex != WALK_RAGDOLL_LEFT_FOOT &&
         footIndex != WALK_RAGDOLL_RIGHT_FOOT))
        return false;

    const VoxelRigidBody *foot = &ragdoll->bodies[footIndex];
    double center[3];
    double linearVelocity[3];
    float rotation[9];
    if (!VoxelRigidBodyLocalPosition(foot, center) ||
        !VoxelRigidBodyLinearVelocity(foot, linearVelocity) ||
        linearVelocity[2] > maximumUpwardSpeed)
        return false;
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
            if (!WalkRagdollCoordinateSafeForVoxelQuery(point[0]) ||
                !WalkRagdollCoordinateSafeForVoxelQuery(point[1]) ||
                !WalkRagdollCoordinateSafeForVoxelQuery(point[2]))
                continue;
            const int64_t blockX = (int64_t)WalkMathFloor(point[0]);
            const int64_t blockY = (int64_t)WalkMathFloor(point[1]);
            const int64_t nearestSurface = (int64_t)WalkMathFloor(point[2] + 0.5);
            for (int64_t offset = -1; offset <= 1; ++offset)
            {
                const int64_t surfaceZ = nearestSurface + offset;
                if (WalkMathAbs(point[2] - (double)surfaceZ) > 0.05)
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
    return false;
}

static inline bool WalkRagdollFootGrounded(const VoxelRagdoll *ragdoll,
                                          const VoxelCollisionSource *collision,
                                          uint32_t footIndex)
{
    return WalkRagdollFootContact(ragdoll, collision, footIndex, 0.15);
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
        if (WalkRagdollFootGrounded(ragdoll, collision, footIndex))
            return true;
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
