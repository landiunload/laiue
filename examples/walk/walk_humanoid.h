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
    bool initialized;
} WalkHumanoidControllerState;

bool WalkHumanoidInitialize(const LaiuePhysicsServiceV1 *physics,
                           VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const double origin[3], uint64_t stableIdBase,
                           VoxelRigidStepSettings *outRigidSettings,
                           VoxelRagdollSettings *outRagdollSettings,
                           void **outScratch, uint32_t *outScratchBytes);
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
void WalkHumanoidRelease(const LaiuePhysicsServiceV1 *physics,
                         VoxelRagdoll *ragdoll, void **scratch);
