#include "walk_physics.h"

#include <stddef.h>

static const LaiuePhysicsServiceV1 *walkPhysics;

bool WalkPhysicsBind(const LaiuePhysicsServiceV1 *service, uint32_t serviceSize)
{
    walkPhysics = NULL;
    const size_t required =
        offsetof(LaiuePhysicsServiceV1, bodyTranslateBlocks) + sizeof(service->bodyTranslateBlocks);
    if (service == NULL || serviceSize < required || service->structSize < required ||
        service->bodyWake == NULL || service->ragdollJump == NULL ||
        service->bodyLocalPosition == NULL || service->bodyOrientationMatrix == NULL ||
        service->bodyLinearVelocity == NULL || service->bodyAngularVelocity == NULL ||
        service->bodyAddLinearVelocity == NULL || service->bodyAddAngularVelocity == NULL ||
        service->bodyTranslateBlocks == NULL)
        return false;
    walkPhysics = service;
    return true;
}

void WalkPhysicsUnbind(void)
{
    walkPhysics = NULL;
}

bool WalkPhysicsBound(void)
{
    return walkPhysics != NULL;
}

bool WalkBodyLocalPosition(const VoxelRigidBody *body, double outPosition[3])
{
    return walkPhysics != NULL && walkPhysics->bodyLocalPosition(body, outPosition);
}

void WalkBodyOrientationMatrix(const VoxelRigidBody *body, float outMatrix[9])
{
    if (walkPhysics != NULL)
    {
        walkPhysics->bodyOrientationMatrix(body, outMatrix);
        return;
    }
    for (uint32_t index = 0u; index < 9u; ++index)
        outMatrix[index] = index % 4u == 0u ? 1.0f : 0.0f;
}

bool WalkBodyLinearVelocity(const VoxelRigidBody *body, double outVelocity[3])
{
    return walkPhysics != NULL && walkPhysics->bodyLinearVelocity(body, outVelocity);
}

bool WalkBodyAngularVelocity(const VoxelRigidBody *body, double outVelocity[3])
{
    return walkPhysics != NULL && walkPhysics->bodyAngularVelocity(body, outVelocity);
}

bool WalkBodyAddLinearVelocity(VoxelRigidBody *body, const double delta[3])
{
    return walkPhysics != NULL && walkPhysics->bodyAddLinearVelocity(body, delta);
}

bool WalkBodyAddAngularVelocity(VoxelRigidBody *body, const double delta[3])
{
    return walkPhysics != NULL && walkPhysics->bodyAddAngularVelocity(body, delta);
}

bool WalkBodyTranslateBlocks(VoxelRigidBody *body, const int64_t blockShift[3])
{
    return walkPhysics != NULL && walkPhysics->bodyTranslateBlocks(body, blockShift);
}

void WalkBodyWake(VoxelRigidBody *body)
{
    if (walkPhysics != NULL)
        walkPhysics->bodyWake(body);
}

bool WalkRagdollJump(VoxelRagdoll *ragdoll, double upwardSpeed)
{
    return walkPhysics != NULL && walkPhysics->ragdollJump(ragdoll, upwardSpeed);
}
