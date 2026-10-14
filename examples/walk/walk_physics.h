#pragma once

#include "physics/physics_service.h"

#include <stdbool.h>
#include <stdint.h>

/* Every game binds its own borrowed table. A failed Bind leaves it unchanged. */
typedef struct WalkPhysicsContext
{
    const LaiuePhysicsServiceV1 *service;
    uint32_t serviceSize;
} WalkPhysicsContext;
bool WalkPhysicsBind(WalkPhysicsContext *context, const LaiuePhysicsServiceV1 *service,
                     uint32_t serviceSize);
void WalkPhysicsUnbind(WalkPhysicsContext *context);
bool WalkPhysicsBound(const WalkPhysicsContext *context);

bool WalkBodyLocalPosition(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                           double outPosition[3]);
void WalkBodyOrientationMatrix(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                               float outMatrix[9]);
bool WalkBodyLinearVelocity(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                            double outVelocity[3]);
bool WalkBodyAngularVelocity(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                             double outVelocity[3]);
bool WalkBodyAddLinearVelocity(const WalkPhysicsContext *context, VoxelRigidBody *body,
                               const double delta[3]);
bool WalkBodyAddAngularVelocity(const WalkPhysicsContext *context, VoxelRigidBody *body,
                                const double delta[3]);
bool WalkBodyTranslateBlocks(const WalkPhysicsContext *context, VoxelRigidBody *body,
                             const int64_t blockShift[3]);
void WalkBodyWake(const WalkPhysicsContext *context, VoxelRigidBody *body);
bool WalkRagdollJump(const WalkPhysicsContext *context, VoxelRagdoll *ragdoll, double upwardSpeed);
