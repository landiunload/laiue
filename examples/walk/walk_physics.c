#include "walk_physics.h"

#include <stddef.h>

bool WalkPhysicsBind(WalkPhysicsContext *context, const LaiuePhysicsServiceV1 *service,
                     uint32_t serviceSize)
{

    const size_t prefix = offsetof(LaiuePhysicsServiceV1, abiVersion) + sizeof(uint32_t);
    const size_t required =
        offsetof(LaiuePhysicsServiceV1, bodyTranslateBlocks) + sizeof(service->bodyTranslateBlocks);
    if (context == NULL || service == NULL || serviceSize < prefix ||
        service->abiVersion != LAIUE_PHYSICS_SERVICE_ABI_VERSION_1 || serviceSize < required ||
        service->structSize < required || service->configureThread == NULL ||
        service->stepScratchBytes == NULL || service->ragdollInitialize == NULL ||
        service->ragdollRelease == NULL || service->ragdollSettingsDefault == NULL ||
        service->ragdollStep == NULL || service->bodyWake == NULL || service->ragdollJump == NULL ||
        service->bodyLocalPosition == NULL || service->bodyOrientationMatrix == NULL ||
        service->bodyLinearVelocity == NULL || service->bodyAngularVelocity == NULL ||
        service->bodyAddLinearVelocity == NULL || service->bodyAddAngularVelocity == NULL ||
        service->bodyTranslateBlocks == NULL)
        return false;
    context->service = service;
    context->serviceSize = serviceSize;
    return true;
}

void WalkPhysicsUnbind(WalkPhysicsContext *context)
{
    if (context != NULL)
    {
        context->service = NULL;
        context->serviceSize = 0u;
    }
}

bool WalkPhysicsBound(const WalkPhysicsContext *context)
{
    return context != NULL && context->service != NULL;
}

bool WalkBodyLocalPosition(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                           double outPosition[3])
{
    return WalkPhysicsBound(context) && context->service->bodyLocalPosition(body, outPosition);
}

void WalkBodyOrientationMatrix(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                               float outMatrix[9])
{
    if (WalkPhysicsBound(context))
    {
        context->service->bodyOrientationMatrix(body, outMatrix);
        return;
    }
    for (uint32_t index = 0u; index < 9u; ++index)
        outMatrix[index] = index % 4u == 0u ? 1.0f : 0.0f;
}

bool WalkBodyLinearVelocity(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                            double outVelocity[3])
{
    return WalkPhysicsBound(context) && context->service->bodyLinearVelocity(body, outVelocity);
}

bool WalkBodyAngularVelocity(const WalkPhysicsContext *context, const VoxelRigidBody *body,
                             double outVelocity[3])
{
    return WalkPhysicsBound(context) && context->service->bodyAngularVelocity(body, outVelocity);
}

bool WalkBodyAddLinearVelocity(const WalkPhysicsContext *context, VoxelRigidBody *body,
                               const double delta[3])
{
    return WalkPhysicsBound(context) && context->service->bodyAddLinearVelocity(body, delta);
}

bool WalkBodyAddAngularVelocity(const WalkPhysicsContext *context, VoxelRigidBody *body,
                                const double delta[3])
{
    return WalkPhysicsBound(context) && context->service->bodyAddAngularVelocity(body, delta);
}

bool WalkBodyTranslateBlocks(const WalkPhysicsContext *context, VoxelRigidBody *body,
                             const int64_t blockShift[3])
{
    return WalkPhysicsBound(context) && context->service->bodyTranslateBlocks(body, blockShift);
}

void WalkBodyWake(const WalkPhysicsContext *context, VoxelRigidBody *body)
{
    if (WalkPhysicsBound(context))
        context->service->bodyWake(body);
}

bool WalkRagdollJump(const WalkPhysicsContext *context, VoxelRagdoll *ragdoll, double upwardSpeed)
{
    return WalkPhysicsBound(context) && context->service->ragdollJump(ragdoll, upwardSpeed);
}
