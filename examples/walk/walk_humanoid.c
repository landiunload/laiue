#include "walk_humanoid.h"

#include "humanoid_ragdoll.h"
#include "platform/system.h"

#include <math.h>
#include <stddef.h>

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
                      const VoxelCollisionSource *collision,
                      const VoxelRigidStepSettings *rigidSettings,
                      const VoxelRagdollSettings *ragdollSettings,
                      void *scratch, uint32_t scratchBytes,
                      double moveX, double moveY, bool sprint, bool jump,
                      double deltaSeconds, bool *inOutGrounded,
                      double *inOutFacingYaw, double *inOutGaitPhase)
{
    if (physics == NULL || physics->ragdollStep == NULL || ragdoll == NULL ||
        collision == NULL || rigidSettings == NULL || ragdollSettings == NULL ||
        scratch == NULL || scratchBytes == 0u || inOutGrounded == NULL ||
        inOutFacingYaw == NULL || inOutGaitPhase == NULL || !isfinite(moveX) ||
        !isfinite(moveY) || !isfinite(deltaSeconds) || !(deltaSeconds > 0.0) ||
        deltaSeconds > 1.0 / 30.0 || !isfinite(*inOutFacingYaw) ||
        !isfinite(*inOutGaitPhase))
        return false;
    const double magnitudeSquared = moveX * moveX + moveY * moveY;
    if (!isfinite(magnitudeSquared))
        return false;
    double magnitude = ScalarSqrtDouble(magnitudeSquared);
    if (magnitude > 1.0)
    {
        moveX /= magnitude;
        moveY /= magnitude;
        magnitude = 1.0;
    }
    const double targetSpeed = sprint ? 4.2 : 2.6;
    if (magnitude > 1.0e-3)
        *inOutFacingYaw = ScalarAtan2((float)-moveX, (float)moveY);
    *inOutGaitPhase += targetSpeed * magnitude * deltaSeconds * 4.8;
    if (*inOutGaitPhase >= 6.2831853071795864769)
        *inOutGaitPhase -= 6.2831853071795864769;
    if (jump)
        (void)WalkRagdollJumpIfGrounded(ragdoll, collision, 5.0);
    if (!WalkRagdollDrivePlanar(ragdoll, moveX, moveY, targetSpeed,
                                *inOutGrounded ? 8.0 : 2.5, deltaSeconds) ||
        !WalkRagdollPoseDrive(ragdoll, *inOutFacingYaw, *inOutGaitPhase,
                              magnitude, deltaSeconds) ||
        !physics->ragdollStep(ragdoll, collision, rigidSettings, ragdollSettings,
                              scratch, scratchBytes, NULL))
        return false;
    *inOutGrounded = WalkRagdollGrounded(ragdoll, collision);
    return true;
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
