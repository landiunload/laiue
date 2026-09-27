#include "physics/ragdoll.h"
#include "physics/numeric_provider.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "../examples/walk/humanoid_ragdoll.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct RagdollHarness
{
    VoxelCollisionSource collision;
    VoxelRigidStepSettings rigidSettings;
    VoxelRagdollSettings ragdollSettings;
    void *scratch;
    uint32_t scratchBytes;
} RagdollHarness;

static void RagdollExpect(bool condition, const char *name)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Ragdoll check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static double RagdollAbs(double value)
{
    return value < 0.0 ? -value : value;
}

static void QueryRagdollFloor(void *context, int64_t x, int64_t y, int64_t z,
                              VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    outBlock->flags = z < 1 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.75f;
}

static void HarnessInitialize(RagdollHarness *harness)
{
    memset(harness, 0, sizeof(*harness));
    harness->collision.queryBlockPhysics = QueryRagdollFloor;
    VoxelRigidStepSettingsDefault(&harness->rigidSettings);
    harness->rigidSettings.gravity[0] = 0.0;
    harness->rigidSettings.gravity[1] = 0.0;
    harness->rigidSettings.gravity[2] = -9.81;
    harness->rigidSettings.solverIterations = 10u;
    VoxelRagdollSettingsDefault(&harness->ragdollSettings);
    harness->scratchBytes = VoxelRigidBodyStepScratchBytes(WALK_RAGDOLL_BODY_COUNT);
    harness->scratch = PlatformAllocate(harness->scratchBytes, false);
    RagdollExpect(harness->scratchBytes != 0u && harness->scratch != NULL,
                  "fixed-step scratch allocation");
}

static void HarnessRelease(RagdollHarness *harness)
{
    PlatformFree(harness->scratch);
    harness->scratch = NULL;
}

static bool InitializeHumanoid(VoxelRagdoll *ragdoll, uint64_t stableIdBase)
{
    const VoxelRagdollDefinition definition = {
        .stableIdBase = stableIdBase,
        .origin = {0.0, 0.0, 0.7},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    return VoxelRagdollInitialize(ragdoll, &definition);
}

static double JointErrorSquared(const VoxelRagdoll *ragdoll)
{
    double maximumSquared = 0.0;
    for (uint32_t index = 0u; index < ragdoll->jointCount; ++index)
    {
        const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[index];
        double point[2][3];
        const uint32_t bodies[2] = {joint->bodyA, joint->bodyB};
        const double *anchors[2] = {joint->anchorA, joint->anchorB};
        for (uint32_t endpoint = 0u; endpoint < 2u; ++endpoint)
        {
            float rotation[9];
            double center[3];
            VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[bodies[endpoint]], rotation);
            if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[bodies[endpoint]], center))
                return INFINITY;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                point[endpoint][axis] = center[axis] +
                    (double)rotation[axis] * anchors[endpoint][0] +
                    (double)rotation[3u + axis] * anchors[endpoint][1] +
                    (double)rotation[6u + axis] * anchors[endpoint][2];
        }
        double squared = 0.0;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double difference = point[0][axis] - point[1][axis];
            squared += difference * difference;
        }
        if (squared > maximumSquared)
            maximumSquared = squared;
    }
    return maximumSquared;
}

static double RagdollDot(const double left[3], const double right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static double BoxProjectionRadius(const VoxelRigidBody *body, const float rotation[9],
                                 const double axis[3])
{
    double radius = 0.0;
    for (uint32_t localAxis = 0u; localAxis < 3u; ++localAxis)
    {
        const double bodyAxis[3] = {rotation[localAxis * 3u],
                                    rotation[localAxis * 3u + 1u],
                                    rotation[localAxis * 3u + 2u]};
        radius += body->halfExtent[localAxis] * RagdollAbs(RagdollDot(axis, bodyAxis));
    }
    return radius;
}

static bool PairPenetratesBeyond(const VoxelRigidBody *left,
                                 const VoxelRigidBody *right, double tolerance)
{
    float rotationLeft[9];
    float rotationRight[9];
    double centerLeft[3];
    double centerRight[3];
    if (!VoxelRigidBodyLocalPosition(left, centerLeft) ||
        !VoxelRigidBodyLocalPosition(right, centerRight))
        return true;
    VoxelRigidBodyOrientationMatrix(left, rotationLeft);
    VoxelRigidBodyOrientationMatrix(right, rotationRight);
    double axes[15][3];
    uint32_t axisCount = 0u;
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        for (uint32_t component = 0u; component < 3u; ++component)
        {
            axes[axisCount][component] = rotationLeft[index * 3u + component];
            axes[axisCount + 3u][component] = rotationRight[index * 3u + component];
        }
    }
    axisCount = 6u;
    for (uint32_t leftAxis = 0u; leftAxis < 3u; ++leftAxis)
        for (uint32_t rightAxis = 0u; rightAxis < 3u; ++rightAxis)
        {
            const double a[3] = {rotationLeft[leftAxis * 3u],
                                 rotationLeft[leftAxis * 3u + 1u],
                                 rotationLeft[leftAxis * 3u + 2u]};
            const double b[3] = {rotationRight[rightAxis * 3u],
                                 rotationRight[rightAxis * 3u + 1u],
                                 rotationRight[rightAxis * 3u + 2u]};
            axes[axisCount][0] = a[1] * b[2] - a[2] * b[1];
            axes[axisCount][1] = a[2] * b[0] - a[0] * b[2];
            axes[axisCount][2] = a[0] * b[1] - a[1] * b[0];
            ++axisCount;
        }
    const double centerDelta[3] = {centerRight[0] - centerLeft[0],
                                   centerRight[1] - centerLeft[1],
                                   centerRight[2] - centerLeft[2]};
    const double toleranceSquared = tolerance * tolerance;
    for (uint32_t index = 0u; index < axisCount; ++index)
    {
        const double axisSquared = RagdollDot(axes[index], axes[index]);
        if (axisSquared <= 1.0e-12)
            continue;
        const double overlap = BoxProjectionRadius(left, rotationLeft, axes[index]) +
                               BoxProjectionRadius(right, rotationRight, axes[index]) -
                               RagdollAbs(RagdollDot(centerDelta, axes[index]));
        if (overlap <= 0.0 || overlap * overlap <= toleranceSquared * axisSquared)
            return false;
    }
    return true;
}

static bool HasDeepSelfPenetration(const VoxelRagdoll *ragdoll)
{
    for (uint32_t left = 0u; left < ragdoll->bodyCount; ++left)
        for (uint32_t right = left + 1u; right < ragdoll->bodyCount; ++right)
            if (PairPenetratesBeyond(&ragdoll->bodies[left], &ragdoll->bodies[right], 0.03))
                return true;
    return false;
}

static uint64_t HashBodyState(const VoxelRagdoll *ragdoll)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        double values[13];
        if (!VoxelRigidBodyLocalPosition(&ragdoll->bodies[body], &values[0]) ||
            !VoxelRigidBodyLinearVelocity(&ragdoll->bodies[body], &values[3]) ||
            !VoxelRigidBodyAngularVelocity(&ragdoll->bodies[body], &values[6]))
            return 0u;
        for (uint32_t axis = 0u; axis < 4u; ++axis)
            values[9u + axis] = ragdoll->bodies[body].orientation[axis];
        for (uint32_t value = 0u; value < 13u; ++value)
        {
            union
            {
                double value;
                uint64_t bits;
            } view = {values[value]};
            for (uint32_t byte = 0u; byte < 8u; ++byte)
            {
                hash ^= (view.bits >> (byte * 8u)) & UINT64_C(0xff);
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

static void RunHumanoidReplay(RagdollHarness *harness, VoxelRagdoll *first,
                              VoxelRagdoll *second)
{
    for (uint32_t tick = 0u; tick < 720u; ++tick)
    {
        const double directionX = tick < 240u ? 0.6 : (tick < 480u ? 0.0 : -0.4);
        const double directionY = tick < 240u ? 0.8 : 0.0;
        RagdollExpect(VoxelRagdollDrive(first, directionX, directionY, 2.0, 12.0) &&
                          VoxelRagdollDrive(second, directionX, directionY, 2.0, 12.0),
                      "bounded stick motor");
        if (tick == 300u)
            RagdollExpect(VoxelRagdollJump(first, 5.0) &&
                              VoxelRagdollJump(second, 5.0),
                          "whole-body jump impulse");
        RagdollExpect(VoxelRagdollStep(first, &harness->collision,
                                       &harness->rigidSettings,
                                       &harness->ragdollSettings, harness->scratch,
                                       harness->scratchBytes, NULL) &&
                          VoxelRagdollStep(second, &harness->collision,
                                           &harness->rigidSettings,
                                           &harness->ragdollSettings, harness->scratch,
                                           harness->scratchBytes, NULL),
                      "fixed-step rigid contacts and ball joints");
        RagdollExpect(HashBodyState(first) == HashBodyState(second),
                      "identical inputs produce identical body state");
        RagdollExpect(JointErrorSquared(first) < 0.0225,
                      "joint anchors remain bounded");
        RagdollExpect(!HasDeepSelfPenetration(first),
                      "colliding body parts do not deeply penetrate");
        for (uint32_t body = 0u; body < first->bodyCount; ++body)
        {
            double position[3];
            RagdollExpect(VoxelRigidBodyLocalPosition(&first->bodies[body], position) &&
                              RagdollAbs(position[0]) < 12.0 &&
                              RagdollAbs(position[1]) < 12.0 &&
                              position[2] > -1.0 && position[2] < 12.0,
                          "colliding articulated body remains in the test world");
        }
    }
}

LAIUE_TEST_ENTRY(RagdollTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    RagdollHarness harness;
    HarnessInitialize(&harness);
    VoxelRagdoll *first = PlatformAllocate(sizeof(*first), false);
    VoxelRagdoll *second = PlatformAllocate(sizeof(*second), false);
    VoxelRagdoll *invalid = PlatformAllocate(sizeof(*invalid), false);
    RagdollExpect(first != NULL && second != NULL && invalid != NULL,
                  "ragdoll test state allocation");
    memset(first, 0, sizeof(*first));
    memset(second, 0, sizeof(*second));
    memset(invalid, 0, sizeof(*invalid));
    RagdollExpect(InitializeHumanoid(first, 1000u) &&
                      InitializeHumanoid(second, 1000u),
                  "humanoid fixed-capacity entity initializes");
    RagdollExpect(first->bodyCount == WALK_RAGDOLL_BODY_COUNT &&
                      first->jointCount == WALK_RAGDOLL_JOINT_COUNT &&
                      JointErrorSquared(first) < 1.0e-18,
                  "body parts begin at satisfied joint anchors");
    RunHumanoidReplay(&harness, first, second);
    VoxelRagdollRelease(second);
    VoxelRagdollRelease(first);

    const VoxelRagdollDefinition badDefinition = {
        .stableIdBase = 1u,
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_BODY_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    RagdollExpect(!VoxelRagdollInitialize(invalid, &badDefinition) &&
                      !invalid->initialized && invalid->bodyCount == 0u,
                  "invalid non-tree joint capacity fails closed");
    VoxelRagdollDefinition disconnectedDefinition = badDefinition;
    disconnectedDefinition.jointCount = WALK_RAGDOLL_JOINT_COUNT - 1u;
    RagdollExpect(!VoxelRagdollInitialize(invalid, &disconnectedDefinition),
                  "disconnected body sets fail closed");
    VoxelRagdollBallJointDefinition misalignedJoints[WALK_RAGDOLL_JOINT_COUNT];
    memcpy(misalignedJoints, walkRagdollJoints, sizeof(misalignedJoints));
    misalignedJoints[0].anchorB[2] += 0.01;
    VoxelRagdollDefinition misalignedDefinition = {
        .stableIdBase = 1u,
        .origin = {0.0, 0.0, 0.7},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = misalignedJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    RagdollExpect(!VoxelRagdollInitialize(invalid, &misalignedDefinition),
                  "unsatisfied initial joint anchors fail closed");
    PlatformFree(invalid);
    PlatformFree(second);
    PlatformFree(first);
    HarnessRelease(&harness);
    LAIUE_TEST_SUCCESS();
}
