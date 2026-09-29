#pragma once

#include "physics/physics_service.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum WalkHumanoidStepFailure
{
    WALK_HUMANOID_STEP_OK = 0,
    WALK_HUMANOID_STEP_INVALID_STATE,
    WALK_HUMANOID_STEP_PLANAR_DRIVE,
    WALK_HUMANOID_STEP_POSE_DRIVE,
    WALK_HUMANOID_STEP_PHYSICS,
} WalkHumanoidStepFailure;

typedef struct WalkHumanoidControllerState
{
    double previousRootPosition[3];
    double footAnchorRelativeToRoot[2][3];
    bool footAnchorValid[2];
    bool previousFootStance[2];
    bool previousJumpInput;
    double gaitAmount;
    bool initialized;
} WalkHumanoidControllerState;

bool WalkHumanoidInitialize(const LaiuePhysicsServiceV1 *physics,
                           VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const double origin[3], uint64_t stableIdBase,
                           VoxelRigidStepSettings *outRigidSettings,
                           VoxelRagdollSettings *outRagdollSettings,
                           void **outScratch, uint32_t *outScratchBytes);
/* Jump accepts either a one-tick press or a held button. Release is required
 * before another jump; holding it through a landing never repeats the jump. */
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
                      WalkHumanoidStepFailure *outFailure);
void WalkHumanoidControllerRebase(WalkHumanoidControllerState *controllerState,
                                  const int64_t blockShift[3]);
bool WalkHumanoidIsGrounded(const VoxelRagdoll *ragdoll,
                            const VoxelCollisionSource *collision);
/* Shared head/shoulder camera for every platform. Third-person boom is swept
 * against voxel terrain with near-plane clearance, so walls cannot hide the
 * character by passing through the camera. Returns false without changing
 * outEye when inputs are invalid or the entire boom is blocked; callers can
 * retain their last valid eye. outEye uses ragdoll coordinates. */
bool WalkHumanoidCameraEye(const VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const float forward[3], bool firstPerson,
                           double outEye[3]);
/* Resolve camera failures without reusing third-person positions in first
 * person. A successful first-person query leaves the TPP cache untouched. */
bool WalkHumanoidResolveCameraEye(
    const VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
    const float forward[3], bool firstPerson, const double fallbackEye[3],
    double lastSafeThirdPersonEye[3], bool *hasLastSafeThirdPersonEye,
    double outEye[3]);
void WalkHumanoidRelease(const LaiuePhysicsServiceV1 *physics,
                         VoxelRagdoll *ragdoll, void **scratch);
