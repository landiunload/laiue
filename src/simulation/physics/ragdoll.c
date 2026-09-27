#include "physics/ragdoll.h"

#include "physics/numeric_provider.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

#define RAGDOLL_STEP_SECONDS (1.0 / 128.0)
#define RAGDOLL_MAX_JOINT_ITERATIONS 64u
#define RAGDOLL_MAX_JOINT_IMPULSE 64.0
#define RAGDOLL_MAX_CORRECTION_SPEED 100.0

static bool Finite(double value)
{
    return value == value && value <= DBL_MAX && value >= -DBL_MAX;
}

static bool DefinitionValid(const VoxelRagdollDefinition *definition)
{
    if (definition == NULL || definition->stableIdBase == 0u ||
        definition->bodyCount == 0u || definition->bodyCount > VOXEL_RAGDOLL_MAX_BODIES ||
        definition->jointCount >= definition->bodyCount ||
        definition->jointCount > VOXEL_RAGDOLL_MAX_JOINTS || definition->bodies == NULL ||
        (definition->jointCount != 0u && definition->joints == NULL) ||
        definition->rootBody >= definition->bodyCount ||
        definition->stableIdBase > UINT64_MAX - (definition->bodyCount - 1u))
        return false;

    if (definition->jointCount != definition->bodyCount - 1u)
        return false;

    uint32_t parent[VOXEL_RAGDOLL_MAX_BODIES];
    for (uint32_t body = 0u; body < definition->bodyCount; ++body)
        parent[body] = body;

    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!Finite(definition->origin[axis]))
            return false;
    for (uint32_t body = 0u; body < definition->bodyCount; ++body)
    {
        const VoxelRagdollBodyDefinition *part = &definition->bodies[body];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!Finite(part->offset[axis]) || !Finite(part->halfExtent[axis]) ||
                !Finite(definition->origin[axis] + part->offset[axis]) ||
                !(part->halfExtent[axis] > 0.0))
                return false;
        if (!Finite(part->mass) || !(part->mass > 0.0) || !Finite(part->friction) ||
            part->friction < 0.0 || part->friction > 1.0 || !Finite(part->restitution) ||
            part->restitution < 0.0 || part->restitution > 1.0)
            return false;
    }
    for (uint32_t joint = 0u; joint < definition->jointCount; ++joint)
    {
        const VoxelRagdollBallJointDefinition *connection = &definition->joints[joint];
        if (connection->bodyA >= definition->bodyCount ||
            connection->bodyB >= definition->bodyCount ||
            connection->bodyA == connection->bodyB)
            return false;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!Finite(connection->anchorA[axis]) || !Finite(connection->anchorB[axis]) ||
                connection->anchorA[axis] < -definition->bodies[connection->bodyA].halfExtent[axis] ||
                connection->anchorA[axis] > definition->bodies[connection->bodyA].halfExtent[axis] ||
                connection->anchorB[axis] < -definition->bodies[connection->bodyB].halfExtent[axis] ||
                connection->anchorB[axis] > definition->bodies[connection->bodyB].halfExtent[axis])
                return false;

        uint32_t rootA = connection->bodyA;
        uint32_t rootB = connection->bodyB;
        while (parent[rootA] != rootA)
            rootA = parent[rootA];
        while (parent[rootB] != rootB)
            rootB = parent[rootB];
        if (rootA == rootB)
            return false;
        parent[rootB] = rootA;

        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double pointA = definition->origin[axis] +
                                  definition->bodies[connection->bodyA].offset[axis] +
                                  connection->anchorA[axis];
            const double pointB = definition->origin[axis] +
                                  definition->bodies[connection->bodyB].offset[axis] +
                                  connection->anchorB[axis];
            if (!Finite(pointA) || !Finite(pointB) || pointA - pointB > 1.0e-6 ||
                pointB - pointA > 1.0e-6)
                return false;
        }
    }
    return true;
}

void VoxelRagdollSettingsDefault(VoxelRagdollSettings *outSettings)
{
    if (outSettings == NULL)
        return;
    *outSettings = (VoxelRagdollSettings){
        .solverIterations = VOXEL_RAGDOLL_DEFAULT_JOINT_ITERATIONS,
        .errorCorrection = 0.2,
        .maximumCorrectionSpeed = 6.0,
    };
}

bool VoxelRagdollInitialize(VoxelRagdoll *ragdoll,
                            const VoxelRagdollDefinition *definition)
{
    if (ragdoll == NULL)
        return false;
    memset(ragdoll, 0, sizeof(*ragdoll));
    if (!DefinitionValid(definition))
        return false;

    for (uint32_t index = 0u; index < definition->bodyCount; ++index)
    {
        const VoxelRagdollBodyDefinition *part = &definition->bodies[index];
        VoxelRigidBodyDescription bodyDescription = {
            .halfExtent = {part->halfExtent[0], part->halfExtent[1], part->halfExtent[2]},
            .position = {definition->origin[0] + part->offset[0],
                         definition->origin[1] + part->offset[1],
                         definition->origin[2] + part->offset[2]},
            .mass = part->mass,
            .restitution = part->restitution,
            .friction = part->friction,
        };
        if (!VoxelRigidBodyInitialize(&ragdoll->bodies[index],
                                      definition->stableIdBase + index,
                                      &bodyDescription))
        {
            VoxelRigidBodyRelease(&ragdoll->bodies[index]);
            for (uint32_t release = index; release > 0u; --release)
                VoxelRigidBodyRelease(&ragdoll->bodies[release - 1u]);
            memset(ragdoll, 0, sizeof(*ragdoll));
            return false;
        }
        ragdoll->bodyCount = index + 1u;
    }
    if (definition->jointCount != 0u)
        memcpy(ragdoll->joints, definition->joints,
               (size_t)definition->jointCount * sizeof(ragdoll->joints[0]));
    ragdoll->jointCount = definition->jointCount;
    ragdoll->rootBody = definition->rootBody;
    ragdoll->initialized = true;
    return true;
}

void VoxelRagdollRelease(VoxelRagdoll *ragdoll)
{
    if (ragdoll == NULL)
        return;
    const uint32_t bodyCount = ragdoll->bodyCount <= VOXEL_RAGDOLL_MAX_BODIES
                                   ? ragdoll->bodyCount
                                   : VOXEL_RAGDOLL_MAX_BODIES;
    for (uint32_t index = bodyCount; index > 0u; --index)
        VoxelRigidBodyRelease(&ragdoll->bodies[index - 1u]);
    memset(ragdoll, 0, sizeof(*ragdoll));
}

static void Cross(const double left[3], const double right[3], double out[3])
{
    out[0] = left[1] * right[2] - left[2] * right[1];
    out[1] = left[2] * right[0] - left[0] * right[2];
    out[2] = left[0] * right[1] - left[1] * right[0];
}

static double Dot(const double left[3], const double right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static void TransformOffset(const float rotation[9], const double local[3], double world[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        world[axis] = (double)rotation[axis] * local[0] +
                      (double)rotation[3u + axis] * local[1] +
                      (double)rotation[6u + axis] * local[2];
}

static void ApplyInverseInertia(const VoxelRigidBody *body, const float rotation[9],
                                const double torque[3], double response[3])
{
    double local[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        local[axis] = body->inverseInertia[axis] *
                      ((double)rotation[axis * 3u] * torque[0] +
                       (double)rotation[axis * 3u + 1u] * torque[1] +
                       (double)rotation[axis * 3u + 2u] * torque[2]);
    TransformOffset(rotation, local, response);
}

static bool JointAnchor(const VoxelRigidBody *body, const double localAnchor[3],
                        double outLever[3], double outPoint[3], float outRotation[9])
{
    double center[3];
    if (!VoxelRigidBodyLocalPosition(body, center))
        return false;
    VoxelRigidBodyOrientationMatrix(body, outRotation);
    TransformOffset(outRotation, localAnchor, outLever);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        outPoint[axis] = center[axis] + outLever[axis];
        if (!Finite(outPoint[axis]))
            return false;
    }
    return true;
}

static double Clamp(double value, double minimum, double maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static bool SolveJointAxis(VoxelRagdoll *ragdoll,
                           const VoxelRagdollBallJointDefinition *joint,
                           const double error[3], const double leverA[3],
                           const double leverB[3], const float rotationA[9],
                           const float rotationB[9], const double pointA[3],
                           const double pointB[3], const VoxelRagdollSettings *settings,
                           uint32_t axis)
{
    VoxelRigidBody *bodyA = &ragdoll->bodies[joint->bodyA];
    VoxelRigidBody *bodyB = &ragdoll->bodies[joint->bodyB];
    double direction[3] = {0.0, 0.0, 0.0};
    direction[axis] = 1.0;
    double torqueA[3];
    double torqueB[3];
    Cross(leverA, direction, torqueA);
    Cross(leverB, direction, torqueB);
    double responseA[3];
    double responseB[3];
    ApplyInverseInertia(bodyA, rotationA, torqueA, responseA);
    ApplyInverseInertia(bodyB, rotationB, torqueB, responseB);
    const double effectiveMass = bodyA->inverseMass + bodyB->inverseMass +
                                 Dot(torqueA, responseA) + Dot(torqueB, responseB);
    if (!(effectiveMass > 0.0) || !Finite(effectiveMass))
        return false;

    double velocityA[3];
    double velocityB[3];
    if (!VoxelRigidBodyPointVelocity(bodyA, pointA, velocityA) ||
        !VoxelRigidBodyPointVelocity(bodyB, pointB, velocityB))
        return false;
    const double relativeSpeed = velocityA[axis] - velocityB[axis];
    const double bias = Clamp(settings->errorCorrection * error[axis] / RAGDOLL_STEP_SECONDS,
                              -settings->maximumCorrectionSpeed,
                              settings->maximumCorrectionSpeed);
    const double impulse = Clamp(-(relativeSpeed + bias) / effectiveMass,
                                 -RAGDOLL_MAX_JOINT_IMPULSE,
                                 RAGDOLL_MAX_JOINT_IMPULSE);
    if (!Finite(impulse))
        return false;

    double linearA[3];
    double linearB[3];
    double angularA[3];
    double angularB[3];
    for (uint32_t component = 0u; component < 3u; ++component)
    {
        linearA[component] = direction[component] * impulse * bodyA->inverseMass;
        linearB[component] = -direction[component] * impulse * bodyB->inverseMass;
        angularA[component] = responseA[component] * impulse;
        angularB[component] = -responseB[component] * impulse;
    }
    if (!VoxelRigidBodyAddLinearVelocity(bodyA, linearA) ||
        !VoxelRigidBodyAddAngularVelocity(bodyA, angularA) ||
        !VoxelRigidBodyAddLinearVelocity(bodyB, linearB) ||
        !VoxelRigidBodyAddAngularVelocity(bodyB, angularB))
        return false;
    if (impulse != 0.0)
    {
        VoxelRigidBodyWake(bodyA);
        VoxelRigidBodyWake(bodyB);
    }
    return true;
}

static bool SolveJoints(VoxelRagdoll *ragdoll, const VoxelRagdollSettings *settings)
{
    VoxelPhysicsConfigureThread();
    for (uint32_t iteration = 0u; iteration < settings->solverIterations; ++iteration)
    {
        for (uint32_t index = 0u; index < ragdoll->jointCount; ++index)
        {
            const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[index];
            double leverA[3];
            double leverB[3];
            double pointA[3];
            double pointB[3];
            float rotationA[9];
            float rotationB[9];
            if (!JointAnchor(&ragdoll->bodies[joint->bodyA], joint->anchorA,
                             leverA, pointA, rotationA) ||
                !JointAnchor(&ragdoll->bodies[joint->bodyB], joint->anchorB,
                             leverB, pointB, rotationB))
                return false;
            double error[3];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                error[axis] = pointA[axis] - pointB[axis];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                if (!SolveJointAxis(ragdoll, joint, error, leverA, leverB,
                                    rotationA, rotationB, pointA, pointB,
                                    settings, axis))
                    return false;
        }
    }
    return true;
}

bool VoxelRagdollStep(VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
                      const VoxelRigidStepSettings *rigidSettings,
                      const VoxelRagdollSettings *settings, void *scratch,
                      uint32_t scratchBytes, const VoxelRigidStepOptions *options)
{
    if (ragdoll == NULL || !ragdoll->initialized || ragdoll->bodyCount == 0u ||
        ragdoll->bodyCount > VOXEL_RAGDOLL_MAX_BODIES ||
        ragdoll->jointCount > VOXEL_RAGDOLL_MAX_JOINTS ||
        collision == NULL || collision->queryBlockPhysics == NULL ||
        rigidSettings == NULL || settings == NULL || settings->solverIterations == 0u ||
        settings->solverIterations > RAGDOLL_MAX_JOINT_ITERATIONS ||
        !Finite(settings->errorCorrection) || settings->errorCorrection < 0.0 ||
        settings->errorCorrection > 0.5 || !Finite(settings->maximumCorrectionSpeed) ||
        !(settings->maximumCorrectionSpeed > 0.0) ||
        settings->maximumCorrectionSpeed > RAGDOLL_MAX_CORRECTION_SPEED)
        return false;
    VoxelRigidStepOptions filteredOptions = {
        .structSize = sizeof(filteredOptions),
        .solverOrder = VOXEL_RIGID_SOLVER_CANONICAL,
    };
    const size_t legacyOptionsSize = offsetof(VoxelRigidStepOptions, bodyPairExclusions);
    const size_t exclusionFieldsEnd =
        offsetof(VoxelRigidStepOptions, bodyPairExclusionCount) +
        sizeof(filteredOptions.bodyPairExclusionCount);
    uint32_t excludedPairs[VOXEL_RAGDOLL_MAX_BODIES] = {0u};
    if (options != NULL)
    {
        if (options->structSize < legacyOptionsSize ||
            (options->structSize > legacyOptionsSize &&
             options->structSize < exclusionFieldsEnd))
            return false;
        memcpy(&filteredOptions, options, legacyOptionsSize);
        filteredOptions.structSize = sizeof(filteredOptions);
        if (options->structSize >= exclusionFieldsEnd)
        {
            if ((options->bodyPairExclusions == NULL &&
                 options->bodyPairExclusionCount != 0u) ||
                (options->bodyPairExclusions != NULL &&
                 options->bodyPairExclusionCount != ragdoll->bodyCount))
                return false;
            if (options->bodyPairExclusions != NULL)
                for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
                    excludedPairs[body] = options->bodyPairExclusions[body];
        }
    }
    for (uint32_t index = 0u; index < ragdoll->jointCount; ++index)
    {
        const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[index];
        excludedPairs[joint->bodyA] |= UINT32_C(1) << joint->bodyB;
        excludedPairs[joint->bodyB] |= UINT32_C(1) << joint->bodyA;
    }
    filteredOptions.bodyPairExclusions = excludedPairs;
    filteredOptions.bodyPairExclusionCount = ragdoll->bodyCount;
    // Joint impulses are applied before integration/contact resolution so the
    // rigid solver can prevent connected limbs from crossing other parts in
    // the same tick. Solving them after contacts lets the final joint impulse
    // push a limb through an already-solved collision surface.
    return SolveJoints(ragdoll, settings) &&
           VoxelRigidBodyStepEx(ragdoll->bodies, ragdoll->bodyCount,
                                collision, rigidSettings, scratch,
                                scratchBytes, &filteredOptions);
}

bool VoxelRagdollDrive(VoxelRagdoll *ragdoll, double directionX, double directionY,
                       double targetSpeed, double maximumAcceleration)
{
    if (ragdoll == NULL || !ragdoll->initialized || ragdoll->rootBody >= ragdoll->bodyCount ||
        !Finite(directionX) || !Finite(directionY) || !Finite(targetSpeed) ||
        !Finite(maximumAcceleration) || targetSpeed < 0.0 || !(maximumAcceleration > 0.0) ||
        directionX * directionX + directionY * directionY > 1.000001)
        return false;
    VoxelRigidBody *root = &ragdoll->bodies[ragdoll->rootBody];
    double velocity[3];
    if (!VoxelRigidBodyLinearVelocity(root, velocity))
        return false;
    const double maximumDelta = maximumAcceleration * RAGDOLL_STEP_SECONDS;
    double delta[3] = {
        Clamp(directionX * targetSpeed - velocity[0], -maximumDelta, maximumDelta),
        Clamp(directionY * targetSpeed - velocity[1], -maximumDelta, maximumDelta),
        0.0,
    };
    if (delta[0] == 0.0 && delta[1] == 0.0)
        return true;
    if (!VoxelRigidBodyAddLinearVelocity(root, delta))
        return false;
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
        VoxelRigidBodyWake(&ragdoll->bodies[body]);
    return true;
}

bool VoxelRagdollJump(VoxelRagdoll *ragdoll, double upwardSpeed)
{
    if (ragdoll == NULL || !ragdoll->initialized || ragdoll->bodyCount == 0u ||
        ragdoll->bodyCount > VOXEL_RAGDOLL_MAX_BODIES || !Finite(upwardSpeed) ||
        !(upwardSpeed > 0.0))
        return false;
    const double delta[3] = {0.0, 0.0, upwardSpeed};
    for (uint32_t index = 0u; index < ragdoll->bodyCount; ++index)
    {
        if (!VoxelRigidBodyAddLinearVelocity(&ragdoll->bodies[index], delta))
            return false;
        VoxelRigidBodyWake(&ragdoll->bodies[index]);
    }
    return true;
}
