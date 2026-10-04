#pragma once

#include "physics/physics_service.h"

#include <stdbool.h>
#include <stdint.h>

/* Walk reaches rigid bodies only through the laiue.physics service table, so
 * the executable carries no load-time import of the physics library:
 * removing that artifact turns the ragdoll off instead of keeping the game
 * from starting.  The table is bound once after the module graph starts and
 * unbound before it stops.  Bind refuses a table without the body accessor
 * tail; while nothing is bound every accessor fails safely. */
bool WalkPhysicsBind(const LaiuePhysicsServiceV1 *service, uint32_t serviceSize);
void WalkPhysicsUnbind(void);
bool WalkPhysicsBound(void);

bool WalkBodyLocalPosition(const VoxelRigidBody *body, double outPosition[3]);
void WalkBodyOrientationMatrix(const VoxelRigidBody *body, float outMatrix[9]);
bool WalkBodyLinearVelocity(const VoxelRigidBody *body, double outVelocity[3]);
bool WalkBodyAngularVelocity(const VoxelRigidBody *body, double outVelocity[3]);
bool WalkBodyAddLinearVelocity(VoxelRigidBody *body, const double delta[3]);
bool WalkBodyAddAngularVelocity(VoxelRigidBody *body, const double delta[3]);
bool WalkBodyTranslateBlocks(VoxelRigidBody *body, const int64_t blockShift[3]);
void WalkBodyWake(VoxelRigidBody *body);
bool WalkRagdollJump(VoxelRagdoll *ragdoll, double upwardSpeed);
