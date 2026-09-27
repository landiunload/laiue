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

static uint32_t ragdollPenetratingLeft;
static uint32_t ragdollPenetratingRight;
static double ragdollLastPenetrationDepth;

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

static void RagdollWriteUint32(uint32_t value)
{
    char digits[10];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u)
    {
        const char digit[2] = {digits[--count], '\0'};
        LaiueTestRuntimeWrite(digit);
    }
}

static void RagdollWriteSignedHundredths(double value)
{
    if (value < 0.0)
    {
        LaiueTestRuntimeWrite("-");
        value = -value;
    }
    RagdollWriteUint32((uint32_t)(value * 100.0));
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
    harness->rigidSettings.solverIterations = 16u;
    harness->rigidSettings.penetrationCorrection = 0.55;
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
    double minimumDepth = INFINITY;
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
        const double depth = overlap / ScalarSqrtDouble(axisSquared);
        if (depth < minimumDepth)
            minimumDepth = depth;
    }
    ragdollLastPenetrationDepth = minimumDepth;
    return minimumDepth > tolerance;
}

static bool HasDeepSelfPenetration(const VoxelRagdoll *ragdoll)
{
    for (uint32_t left = 0u; left < ragdoll->bodyCount; ++left)
        for (uint32_t right = left + 1u; right < ragdoll->bodyCount; ++right)
        {
            bool directlyJoined = false;
            for (uint32_t joint = 0u; joint < ragdoll->jointCount; ++joint)
                if ((ragdoll->joints[joint].bodyA == left &&
                     ragdoll->joints[joint].bodyB == right) ||
                    (ragdoll->joints[joint].bodyA == right &&
                     ragdoll->joints[joint].bodyB == left))
                {
                    directlyJoined = true;
                    break;
                }
            if (directlyJoined)
                continue;
            if (PairPenetratesBeyond(&ragdoll->bodies[left], &ragdoll->bodies[right], 0.06))
            {
                ragdollPenetratingLeft = left;
                ragdollPenetratingRight = right;
                return true;
            }
        }
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
    double startingPelvis[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &first->bodies[WALK_RAGDOLL_PELVIS], startingPelvis),
                  "walking replay reads the initial pelvis position");
    for (uint32_t tick = 0u; tick < 720u; ++tick)
    {
        const double directionX = tick < 240u ? 0.6 : (tick < 480u ? 0.0 : -0.4);
        const double directionY = tick < 240u ? 0.8 : 0.0;
        RagdollExpect(WalkRagdollDrivePlanar(first, directionX, directionY, 1.5, 2.0,
                                             1.0 / 128.0) &&
                          WalkRagdollDrivePlanar(second, directionX, directionY, 1.5,
                                                 2.0, 1.0 / 128.0),
                      "bounded stick motor");
        const double gaitPhase = (double)tick * (1.0 / 128.0) * 3.0;
        RagdollExpect(WalkRagdollPoseDrive(first, 0.0, gaitPhase, 0.7,
                                           1.0 / 128.0) &&
                          WalkRagdollPoseDrive(second, 0.0, gaitPhase, 0.7,
                                               1.0 / 128.0),
                      "upright and alternating limb motors accept fixed-step targets");
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
        if (JointErrorSquared(first) >= 0.0225)
        {
            LaiueTestRuntimeWrite("joint anchor error at tick ");
            RagdollWriteUint32(tick);
            LaiueTestRuntimeWrite("\r\n");
            RagdollExpect(false, "joint anchors remain bounded");
        }
        if (HasDeepSelfPenetration(first))
        {
            LaiueTestRuntimeWrite("body overlap at tick ");
            RagdollWriteUint32(tick);
            LaiueTestRuntimeWrite(": ");
            RagdollWriteUint32(ragdollPenetratingLeft);
            LaiueTestRuntimeWrite(" / ");
            RagdollWriteUint32(ragdollPenetratingRight);
            LaiueTestRuntimeWrite(" >");
            RagdollWriteUint32((uint32_t)(ragdollLastPenetrationDepth * 100.0));
            LaiueTestRuntimeWrite(" cm; centers cm ");
            for (uint32_t body = 0u; body < 2u; ++body)
            {
                const uint32_t bodyIndex = body == 0u ? ragdollPenetratingLeft
                                                      : ragdollPenetratingRight;
                double position[3];
                (void)VoxelRigidBodyLocalPosition(&first->bodies[bodyIndex], position);
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    RagdollWriteSignedHundredths(position[axis]);
                    LaiueTestRuntimeWrite(" ");
                }
                LaiueTestRuntimeWrite(" / ");
            }
            LaiueTestRuntimeWrite("\r\n");
            RagdollExpect(false, "colliding body parts do not deeply penetrate");
        }
        float torsoRotation[9];
        VoxelRigidBodyOrientationMatrix(&first->bodies[WALK_RAGDOLL_TORSO],
                                        torsoRotation);
        RagdollExpect(torsoRotation[8] > 0.7f,
                      "active ragdoll motor keeps torso upright while walking");
        double pelvisPosition[3];
        const bool pelvisPositionOkay = VoxelRigidBodyLocalPosition(
            &first->bodies[WALK_RAGDOLL_PELVIS], pelvisPosition);
        RagdollExpect(pelvisPositionOkay && pelvisPosition[2] > 1.7 &&
                          pelvisPosition[2] < 4.5,
                      "active gait keeps the pelvis above the planted feet");
        float pelvisRotation[9];
        VoxelRigidBodyOrientationMatrix(&first->bodies[WALK_RAGDOLL_PELVIS],
                                        pelvisRotation);
        RagdollExpect(pelvisRotation[8] > 0.7f,
                      "active gait keeps the pelvis upright beneath the torso");
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
    double finalPelvis[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &first->bodies[WALK_RAGDOLL_PELVIS], finalPelvis) &&
                      RagdollAbs(finalPelvis[0] - startingPelvis[0]) +
                          RagdollAbs(finalPelvis[1] - startingPelvis[1]) > 1.0,
                  "analog locomotion translates the complete ragdoll");
}

static void RunGroundedJumpRegression(RagdollHarness *harness, VoxelRagdoll *ragdoll)
{
    RagdollExpect(InitializeHumanoid(ragdoll, 5000u),
                  "grounded jump ragdoll initializes");
    RagdollExpect(WalkRagdollGrounded(ragdoll, &harness->collision),
                  "feet detect the top surface of the solid floor");
    RagdollExpect(WalkRagdollJumpIfGrounded(ragdoll, &harness->collision, 5.0),
                  "jump is accepted while a foot supports the character");
    RagdollExpect(!WalkRagdollJumpIfGrounded(ragdoll, &harness->collision, 5.0),
                  "an upward-moving ragdoll cannot jump again before landing");
    bool becameAirborne = false;
    bool landed = false;
    for (uint32_t tick = 0u; tick < 300u; ++tick)
    {
        const double deltaSeconds = 1.0 / 128.0;
        RagdollExpect(WalkRagdollPoseDrive(ragdoll, 0.0, 0.0, 0.0,
                                           deltaSeconds) &&
                          VoxelRagdollStep(ragdoll, &harness->collision,
                                           &harness->rigidSettings,
                                           &harness->ragdollSettings,
                                           harness->scratch,
                                           harness->scratchBytes, NULL),
                      "jump arc advances through the fixed physics step");
        const bool grounded = WalkRagdollGrounded(ragdoll, &harness->collision);
        becameAirborne = becameAirborne || (tick > 4u && !grounded);
        landed = landed || (tick > 128u && grounded);
        if (landed)
            break;
    }
    RagdollExpect(becameAirborne && landed,
                  "jump follows one arc and regains grounded state on landing");
    RagdollExpect(WalkRagdollJumpIfGrounded(ragdoll, &harness->collision, 5.0),
                  "a grounded landing restores one valid jump");
    VoxelRagdollRelease(ragdoll);
}

static void RunUprightRecoveryRegression(RagdollHarness *harness,
                                         VoxelRagdoll *ragdoll)
{
    RagdollExpect(InitializeHumanoid(ragdoll, 6000u),
                  "recovery ragdoll initializes");
    const double pelvisImpulse[3] = {2.0, 0.0, 0.0};
    const double torsoImpulse[3] = {-1.5, 0.0, 0.0};
    RagdollExpect(VoxelRigidBodyAddAngularVelocity(
                      &ragdoll->bodies[WALK_RAGDOLL_PELVIS], pelvisImpulse) &&
                      VoxelRigidBodyAddAngularVelocity(
                          &ragdoll->bodies[WALK_RAGDOLL_TORSO], torsoImpulse),
                  "recovery test applies a deterministic stumble");
    for (uint32_t tick = 0u; tick < 480u; ++tick)
    {
        const double deltaSeconds = 1.0 / 128.0;
        RagdollExpect(WalkRagdollPoseDrive(ragdoll, 0.0, 0.0, 0.0,
                                           deltaSeconds) &&
                          VoxelRagdollStep(ragdoll, &harness->collision,
                                           &harness->rigidSettings,
                                           &harness->ragdollSettings,
                                           harness->scratch,
                                           harness->scratchBytes, NULL),
                      "active ragdoll remains valid while recovering from a stumble");
    }
    float torsoRotation[9];
    VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[WALK_RAGDOLL_TORSO],
                                    torsoRotation);
    RagdollExpect(torsoRotation[8] > 0.7f && JointErrorSquared(ragdoll) < 0.04,
                  "active ragdoll recovers upright with connected joints");
}

static void RunIdleDriveSleepRegression(RagdollHarness *harness, VoxelRagdoll *ragdoll)
{
    const VoxelRagdollDefinition definition = {
        .stableIdBase = 3000u,
        .origin = {0.0, 0.0, 10.0},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    RagdollExpect(VoxelRagdollInitialize(ragdoll, &definition),
                  "idle ragdoll initializes for sleep regression");
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        ragdoll->bodies[body].sleeping = true;
        ragdoll->bodies[body].sleepCounter = 17u;
    }

    for (uint32_t tick = 0u; tick < 8u; ++tick)
    {
        RagdollExpect(VoxelRagdollDrive(ragdoll, 0.0, 0.0, 2.6, 16.0),
                      "zero input is a valid idle motor command");
        for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
            RagdollExpect(ragdoll->bodies[body].sleeping &&
                              ragdoll->bodies[body].sleepCounter == 17u,
                          "zero input preserves an idle sleeping island");
    }

    VoxelRigidStepStats stats;
    RagdollExpect(VoxelRigidBodyStep(ragdoll->bodies, ragdoll->bodyCount,
                                    &harness->collision, &harness->rigidSettings,
                                    harness->scratch, harness->scratchBytes) &&
                      VoxelRigidBodyReadStepStats(harness->scratch, ragdoll->bodyCount,
                                                  harness->scratchBytes, &stats) &&
                      stats.awakeBodyCount == 0u,
                  "idle drive leaves the physics fast path fully asleep");

    RagdollExpect(VoxelRagdollDrive(ragdoll, 1.0, 0.0, 2.6, 16.0),
                  "nonzero input is accepted by a sleeping ragdoll");
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
        RagdollExpect(!ragdoll->bodies[body].sleeping,
                      "movement input wakes the articulated island");
    VoxelRagdollRelease(ragdoll);
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
                      first->bodyCount == 13u &&
                      JointErrorSquared(first) < 1.0e-18,
                  "13-part humanoid with feet begins at satisfied joint anchors");
    RunGroundedJumpRegression(&harness, invalid);
    RunHumanoidReplay(&harness, first, second);
    RunUprightRecoveryRegression(&harness, second);
    VoxelRagdollRelease(second);
    VoxelRagdollRelease(first);
    RunIdleDriveSleepRegression(&harness, first);

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
