#pragma once

#include "physics/physics_service.h"

#include <stdbool.h>
#include <stdint.h>

bool WalkHumanoidInitialize(const LaiuePhysicsServiceV1 *physics,
                           VoxelRagdoll *ragdoll,
                           const VoxelCollisionSource *collision,
                           const double origin[3], uint64_t stableIdBase,
                           VoxelRigidStepSettings *outRigidSettings,
                           VoxelRagdollSettings *outRagdollSettings,
                           void **outScratch, uint32_t *outScratchBytes);
bool WalkHumanoidStep(const LaiuePhysicsServiceV1 *physics,
                      VoxelRagdoll *ragdoll,
                      const VoxelCollisionSource *collision,
                      const VoxelRigidStepSettings *rigidSettings,
                      const VoxelRagdollSettings *ragdollSettings,
                      void *scratch, uint32_t scratchBytes,
                      double moveX, double moveY, bool sprint, bool jump,
                      double deltaSeconds, bool *inOutGrounded,
                      double *inOutFacingYaw, double *inOutGaitPhase);
bool WalkHumanoidIsGrounded(const VoxelRagdoll *ragdoll,
                            const VoxelCollisionSource *collision);
void WalkHumanoidRelease(const LaiuePhysicsServiceV1 *physics,
                         VoxelRagdoll *ragdoll, void **scratch);
