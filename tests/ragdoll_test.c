#include "physics/ragdoll.h"
#include "physics/numeric_provider.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "../examples/walk/humanoid_ragdoll.h"
#include "../examples/walk/walk_humanoid.h"
#include "walk_physics_binding.h"

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

static void QueryRagdollNoFloor(void *context, int64_t x, int64_t y, int64_t z,
                                VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = 0u;
    outBlock->friction = 0.0f;
}

static void QueryRagdollFloorEdge(void *context, int64_t x, int64_t y, int64_t z,
                                  VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)y;
    outBlock->flags = x < 0 && z < 1 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.75f;
}

static void QueryCameraWall(void *context, int64_t x, int64_t y, int64_t z,
                             VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)z;
    /* One metre thick wall behind the character: -2 <= y < -1. */
    outBlock->flags = y == -2 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.0f;
}

static void QueryCameraTargetWall(void *context, int64_t x, int64_t y, int64_t z,
                                  VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)z;
    /* The shoulder anchor is inside this solid column while the boom points
     * away from it. The camera must publish a clear sample, never the anchor. */
    outBlock->flags = y == 0 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.0f;
}

static void QueryCameraFullyBlocked(void *context, int64_t x, int64_t y, int64_t z,
                                    VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = VOXEL_BLOCK_PHYSICS_SOLID;
    outBlock->friction = 0.0f;
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
    harness->rigidSettings.penetrationSlop = 0.002;
    VoxelRagdollSettingsDefault(&harness->ragdollSettings);
    harness->ragdollSettings.solverIterations = 16u;
    harness->ragdollSettings.errorCorrection = 0.5;
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
        .origin = {0.0, 0.0, 1.0},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    return VoxelRagdollInitialize(ragdoll, &definition);
}

static void BodyVerticalBounds(const VoxelRigidBody *body, double *bottom, double *top)
{
    double position[3];
    float rotation[9];
    RagdollExpect(VoxelRigidBodyLocalPosition(body, position),
                  "metric height check reads a finite body position");
    VoxelRigidBodyOrientationMatrix(body, rotation);
    const double extent = body->halfExtent[0] * RagdollAbs(rotation[2]) +
                          body->halfExtent[1] * RagdollAbs(rotation[5]) +
                          body->halfExtent[2] * RagdollAbs(rotation[8]);
    *bottom = position[2] - extent;
    *top = position[2] + extent;
}

static void HumanoidSoleAndCrown(const VoxelRagdoll *ragdoll,
                                 double *sole, double *crown)
{
    double leftSole, rightSole, unused;
    BodyVerticalBounds(&ragdoll->bodies[WALK_RAGDOLL_LEFT_FOOT], &leftSole, &unused);
    BodyVerticalBounds(&ragdoll->bodies[WALK_RAGDOLL_RIGHT_FOOT], &rightSole, &unused);
    BodyVerticalBounds(&ragdoll->bodies[WALK_RAGDOLL_HEAD], &unused, crown);
    *sole = leftSole < rightSole ? leftSole : rightSole;
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
            if (PairPenetratesBeyond(&ragdoll->bodies[left], &ragdoll->bodies[right], 0.025))
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

static bool ControllerStatesEqual(const WalkHumanoidControllerState *first,
                                  const WalkHumanoidControllerState *second)
{
    if (first->gaitAmount != second->gaitAmount ||
        first->initialized != second->initialized ||
        first->previousJumpInput != second->previousJumpInput)
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (first->previousRootPosition[axis] != second->previousRootPosition[axis])
            return false;
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        if (first->footAnchorValid[foot] != second->footAnchorValid[foot] ||
            first->previousFootStance[foot] != second->previousFootStance[foot])
            return false;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (first->footAnchorRelativeToRoot[foot][axis] !=
                second->footAnchorRelativeToRoot[foot][axis])
                return false;
    }
    return true;
}

static void RunMetricStandingRegression(RagdollHarness *harness)
{
    VoxelRagdoll *ragdoll = PlatformAllocate(sizeof(*ragdoll), false);
    RagdollExpect(ragdoll != NULL, "metric standing test allocates a humanoid");
    memset(ragdoll, 0, sizeof(*ragdoll));
    RagdollExpect(InitializeHumanoid(ragdoll, 15000u),
                  "metric standing test initializes a humanoid on the one-metre floor");
    double sole, crown;
    HumanoidSoleAndCrown(ragdoll, &sole, &crown);
    RagdollExpect(RagdollAbs(sole - 1.0) < 1.0e-8 &&
                      RagdollAbs(crown - sole - 1.80) < 1.0e-8,
                  "rest geometry is exactly 1.80 metres from sole to crown");
    WalkHumanoidControllerState controller = {0};
    bool grounded = true;
    double facing = 0.0, phase = 0.0;
    double maximumHeightError = 0.0, maximumFloorGap = 0.0;
    for (uint32_t tick = 0u; tick < 512u; ++tick)
    {
        RagdollExpect(WalkHumanoidStep(&linkedWalkPhysicsContext, ragdoll, &controller,
                                       &harness->collision, &harness->rigidSettings,
                                       &harness->ragdollSettings, harness->scratch,
                                       harness->scratchBytes, 0.0, 0.0, false, false, 1.0 / 128.0,
                                       &grounded, &facing, &phase, NULL),
                      "the metric humanoid settles through fixed physics steps");
        if (tick < 384u)
            continue;
        HumanoidSoleAndCrown(ragdoll, &sole, &crown);
        const double heightError = RagdollAbs(crown - sole - 1.80);
        const double floorGap = RagdollAbs(sole - 1.0);
        if (heightError > maximumHeightError)
            maximumHeightError = heightError;
        if (floorGap > maximumFloorGap)
            maximumFloorGap = floorGap;
    }
    if (maximumHeightError > 0.03 || maximumFloorGap > 0.03)
    {
        LaiueTestRuntimeWrite("settled height/sole/max error cm ");
        RagdollWriteSignedHundredths(crown - sole);
        LaiueTestRuntimeWrite("/");
        RagdollWriteSignedHundredths(sole - 1.0);
        LaiueTestRuntimeWrite("/");
        RagdollWriteSignedHundredths(maximumHeightError);
        LaiueTestRuntimeWrite("\r\n");
        const uint32_t measuredBodies[] = {
            WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_TORSO, WALK_RAGDOLL_HEAD,
            WALK_RAGDOLL_LEFT_THIGH, WALK_RAGDOLL_LEFT_SHIN,
            WALK_RAGDOLL_RIGHT_THIGH, WALK_RAGDOLL_RIGHT_SHIN,
        };
        for (uint32_t index = 0u; index < sizeof(measuredBodies) / sizeof(measuredBodies[0]);
             ++index)
        {
            double position[3];
            float rotation[9];
            const uint32_t body = measuredBodies[index];
            (void)VoxelRigidBodyLocalPosition(&ragdoll->bodies[body], position);
            VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[body], rotation);
            LaiueTestRuntimeWrite("body/z cm/up percent ");
            RagdollWriteUint32(body);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(position[2]);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(rotation[8]);
            LaiueTestRuntimeWrite("\r\n");
        }
        LaiueTestRuntimeWrite("max joint gap cm ");
        RagdollWriteSignedHundredths(ScalarSqrtDouble(JointErrorSquared(ragdoll)));
        LaiueTestRuntimeWrite("\r\n");
    }
    RagdollExpect(grounded && maximumHeightError <= 0.03 && maximumFloorGap <= 0.03,
                  "settled standing keeps a 1.80m height and floor contact within 3cm");
    VoxelRagdollRelease(ragdoll);
    PlatformFree(ragdoll);
}

static void RunCameraRegression(void)
{
    VoxelRagdoll *ragdoll = PlatformAllocate(sizeof(*ragdoll), false);
    RagdollExpect(ragdoll != NULL, "camera test allocates a humanoid");
    memset(ragdoll, 0, sizeof(*ragdoll));
    RagdollExpect(InitializeHumanoid(ragdoll, 16000u),
                  "camera test initializes a metric humanoid");
    const VoxelCollisionSource clear = {.queryBlockPhysics = QueryRagdollNoFloor};
    const VoxelCollisionSource wall = {.queryBlockPhysics = QueryCameraWall};
    const VoxelCollisionSource embedded = {.queryBlockPhysics = QueryCameraTargetWall};
    const VoxelCollisionSource fullyBlocked = {.queryBlockPhysics = QueryCameraFullyBlocked};
    const float forward[3] = {0.0f, 1.0f, 0.0f};
    double eye[3], head[3], pelvis[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_HEAD], head) &&
                      VoxelRigidBodyLocalPosition(&ragdoll->bodies[WALK_RAGDOLL_PELVIS], pelvis),
                  "camera reads the physical head and pelvis");
    RagdollExpect(
        WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, forward, true, eye) &&
            eye[0] == head[0] && eye[1] == head[1] &&
            RagdollAbs(eye[2] - head[2] - 0.02) < 1.0e-8 &&
            RagdollAbs(eye[2] - 1.0 - 1.70) < 1.0e-8,
        "first-person eyes follow the head at 1.70m above the floor");
    RagdollExpect(
        WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, forward, false, eye) &&
            RagdollAbs(eye[0] - pelvis[0]) < 1.0e-8 &&
            RagdollAbs(eye[1] - pelvis[1] + 3.0) < 1.0e-8 &&
            RagdollAbs(eye[2] - pelvis[2] - 0.40) < 1.0e-8,
        "unobstructed third person keeps a three-metre shoulder boom");
    RagdollExpect(
        WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &wall, forward, false, eye) &&
            eye[1] > -0.86 && eye[1] < -0.84,
        "the camera boom stops 15cm before a wall instead of crossing it");
    RagdollExpect(
        WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &embedded, forward, false, eye) &&
            eye[1] < -1.15,
        "a camera target inside terrain advances to a clear point before publishing");
    eye[0] = 11.0;
    eye[1] = 12.0;
    eye[2] = 13.0;
    RagdollExpect(!WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &fullyBlocked, forward,
                                         false, eye) &&
                      eye[0] == 11.0 && eye[1] == 12.0 && eye[2] == 13.0,
                  "a fully obstructed camera preserves its last valid position for the caller");
    const double fallbackEye[3] = {9.0, 8.0, 7.0};
    double cachedThirdPersonEye[3] = {0.0, 0.0, 0.0};
    double resolvedEye[3];
    bool hasCachedThirdPersonEye = false;
    RagdollExpect(WalkHumanoidResolveCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, forward,
                                               false, fallbackEye, cachedThirdPersonEye,
                                               &hasCachedThirdPersonEye, resolvedEye) &&
                      hasCachedThirdPersonEye && RagdollAbs(resolvedEye[0] - pelvis[0]) < 1.0e-8 &&
                      RagdollAbs(resolvedEye[1] - pelvis[1] + 3.0) < 1.0e-8,
                  "camera resolution stores a successful third-person eye");
    const double thirdPersonEyeBeforeFirstPerson[3] = {
        cachedThirdPersonEye[0], cachedThirdPersonEye[1], cachedThirdPersonEye[2]};
    RagdollExpect(WalkHumanoidResolveCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, forward,
                                               true, fallbackEye, cachedThirdPersonEye,
                                               &hasCachedThirdPersonEye, resolvedEye) &&
                      hasCachedThirdPersonEye && RagdollAbs(resolvedEye[0] - head[0]) < 1.0e-8 &&
                      RagdollAbs(resolvedEye[1] - head[1]) < 1.0e-8 &&
                      RagdollAbs(resolvedEye[2] - head[2] - 0.02) < 1.0e-8 &&
                      cachedThirdPersonEye[0] == thirdPersonEyeBeforeFirstPerson[0] &&
                      cachedThirdPersonEye[1] == thirdPersonEyeBeforeFirstPerson[1] &&
                      cachedThirdPersonEye[2] == thirdPersonEyeBeforeFirstPerson[2],
                  "first-person camera uses the head without overwriting the TPP cache");
    RagdollExpect(WalkHumanoidResolveCameraEye(&linkedWalkPhysicsContext, ragdoll, &fullyBlocked,
                                               forward, false, fallbackEye, cachedThirdPersonEye,
                                               &hasCachedThirdPersonEye, resolvedEye) &&
                      RagdollAbs(resolvedEye[0] - thirdPersonEyeBeforeFirstPerson[0]) < 1.0e-8 &&
                      RagdollAbs(resolvedEye[1] - thirdPersonEyeBeforeFirstPerson[1]) < 1.0e-8 &&
                      RagdollAbs(resolvedEye[2] - thirdPersonEyeBeforeFirstPerson[2]) < 1.0e-8,
                  "blocked TPP after an FPS switch reuses its own prior clear eye");
    const float pitched[3] = {0.0f, 0.8f, 0.6f};
    RagdollExpect(
        WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, pitched, false, eye) &&
            RagdollAbs(eye[1] - pelvis[1] + 2.4) < 1.0e-6 &&
            RagdollAbs(eye[2] - pelvis[2] - 0.40 + 1.8) < 1.0e-6,
        "camera pitch moves around the shoulder through the complete 3D boom");
    const float scaled[3] = {0.0f, 8.0f, 6.0f};
    double scaledEye[3];
    RagdollExpect(WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, scaled, false,
                                        scaledEye) &&
                      RagdollAbs(eye[1] - scaledEye[1]) < 1.0e-6 &&
                      RagdollAbs(eye[2] - scaledEye[2]) < 1.0e-6,
                  "camera distance is independent of forward-vector magnitude");
    const float invalid[3] = {0.0f, NAN, 0.0f};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const double previousEye[3] = {eye[0], eye[1], eye[2]};
    RagdollExpect(
        !WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, invalid, false, eye) &&
            !WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, zero, true, eye) &&
            eye[0] == previousEye[0] && eye[1] == previousEye[1] && eye[2] == previousEye[2],
        "invalid camera input fails without publishing a corrupt eye position");
    /* TranslateBlocks is the floating-origin rebase API: it subtracts shift. */
    const int64_t headShift[3] = {0, 0, -1};
    RagdollExpect(
        VoxelRigidBodyTranslateBlocks(&ragdoll->bodies[WALK_RAGDOLL_HEAD], headShift) &&
            WalkHumanoidCameraEye(&linkedWalkPhysicsContext, ragdoll, &clear, forward, true, eye) &&
            RagdollAbs(eye[2] - head[2] - 1.02) < 1.0e-8,
        "first person follows a moving physical head rather than a fixed pelvis offset");
    VoxelRagdollRelease(ragdoll);
    PlatformFree(ragdoll);
}

static void RunHumanoidTopologyRegression(RagdollHarness *harness)
{
    VoxelRagdoll *ragdoll = PlatformAllocate(sizeof(*ragdoll), false);
    RagdollExpect(ragdoll != NULL, "topology test allocates a humanoid");
    memset(ragdoll, 0, sizeof(*ragdoll));
    RagdollExpect(InitializeHumanoid(ragdoll, 17000u),
                  "topology test initializes the canonical humanoid");
    const uint32_t leftBody = ragdoll->joints[6].bodyB;
    ragdoll->joints[6].bodyB = ragdoll->joints[7].bodyB;
    ragdoll->joints[7].bodyB = leftBody;
    const uint64_t originalBodyHash = HashBodyState(ragdoll);
    RagdollExpect(
        !WalkRagdollHasHumanoidTopology(ragdoll) &&
            !WalkRagdollPoseDrive(&linkedWalkPhysicsContext, ragdoll, 0.0, 0.0, 0.0, 1.0 / 128.0),
        "pose motors reject a ragdoll with a mismatched leg-joint topology");
    WalkHumanoidControllerState controller = {0};
    const WalkHumanoidControllerState originalController = controller;
    bool grounded = true;
    double facing = 0.0;
    double phase = 0.0;
    WalkHumanoidStepFailure failure = WALK_HUMANOID_STEP_OK;
    RagdollExpect(!WalkHumanoidStep(&linkedWalkPhysicsContext, ragdoll, &controller,
                                    &harness->collision, &harness->rigidSettings,
                                    &harness->ragdollSettings, harness->scratch,
                                    harness->scratchBytes, 0.0, 0.0, false, false, 1.0 / 128.0,
                                    &grounded, &facing, &phase, &failure) &&
                      failure == WALK_HUMANOID_STEP_INVALID_STATE &&
                      HashBodyState(ragdoll) == originalBodyHash &&
                      ControllerStatesEqual(&controller, &originalController) && grounded &&
                      facing == 0.0 && phase == 0.0,
                  "the fixed-step controller rejects a mismatched topology before mutating it");
    VoxelRagdollRelease(ragdoll);
    PlatformFree(ragdoll);
}

static void RunHumanoidReplay(RagdollHarness *harness, VoxelRagdoll *first,
                              VoxelRagdoll *second)
{
    WalkHumanoidControllerState controllerA = {0};
    WalkHumanoidControllerState controllerB = {0};
    bool groundedA = WalkRagdollGrounded(&linkedWalkPhysicsContext, first, &harness->collision);
    bool groundedB = groundedA;
    double facingA = 0.0;
    double facingB = 0.0;
    double phaseA = 0.0;
    double phaseB = 0.0;
    const double deltaSeconds = 1.0 / 128.0;
    double startingPelvis[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &first->bodies[WALK_RAGDOLL_PELVIS], startingPelvis),
                  "walking replay reads the initial pelvis position");
    double segmentStart[3];
    memcpy(segmentStart, startingPelvis, sizeof(segmentStart));
    double settledPelvis[3] = {0.0, 0.0, 0.0};
    for (uint32_t tick = 0u; tick < 1024u; ++tick)
    {
        const double directionX = tick >= 256u && tick < 512u ? 1.0 : 0.0;
        const double directionY = tick < 256u ? 1.0 : (tick >= 768u ? -1.0 : 0.0);
        const double previousFacing = facingA;
        WalkHumanoidStepFailure failure = WALK_HUMANOID_STEP_OK;
        const bool advanced =
            WalkHumanoidStep(&linkedWalkPhysicsContext, first, &controllerA, &harness->collision,
                             &harness->rigidSettings, &harness->ragdollSettings, harness->scratch,
                             harness->scratchBytes, directionX, directionY, false, false,
                             deltaSeconds, &groundedA, &facingA, &phaseA, &failure) &&
            WalkHumanoidStep(&linkedWalkPhysicsContext, second, &controllerB, &harness->collision,
                             &harness->rigidSettings, &harness->ragdollSettings, harness->scratch,
                             harness->scratchBytes, directionX, directionY, false, false,
                             deltaSeconds, &groundedB, &facingB, &phaseB, &failure);
        if (!advanced)
        {
            LaiueTestRuntimeWrite("advance failure tick/code ");
            RagdollWriteUint32(tick);
            LaiueTestRuntimeWrite("/");
            RagdollWriteUint32((uint32_t)failure);
            LaiueTestRuntimeWrite("\r\n");
        }
        RagdollExpect(advanced, "fixed-step stance, swing and rigid contacts advance");
        RagdollExpect(HashBodyState(first) == HashBodyState(second),
                      "identical inputs produce identical body state");
        RagdollExpect(ControllerStatesEqual(&controllerA, &controllerB) &&
                          groundedA == groundedB && facingA == facingB &&
                          phaseA == phaseB,
                      "turns, stops and restarts replay the complete controller state");
        RagdollExpect(RagdollAbs(facingA - previousFacing) < 0.0873,
                      "direction changes turn smoothly without snapping the body");
        if (JointErrorSquared(first) >= 0.0025)
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
        if (pelvisPositionOkay && (pelvisPosition[2] <= startingPelvis[2] - 0.20 ||
                                   pelvisPosition[2] >= startingPelvis[2] + 0.20))
        {
            LaiueTestRuntimeWrite("pelvis tick/start/current ");
            RagdollWriteUint32(tick);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(startingPelvis[2]);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(pelvisPosition[2]);
            LaiueTestRuntimeWrite("\r\n");
        }
        RagdollExpect(pelvisPositionOkay &&
                          pelvisPosition[2] > startingPelvis[2] - 0.20 &&
                          pelvisPosition[2] < startingPelvis[2] + 0.20,
                      "active gait keeps the pelvis above the planted feet");
        float pelvisRotation[9];
        VoxelRigidBodyOrientationMatrix(&first->bodies[WALK_RAGDOLL_PELVIS],
                                        pelvisRotation);
        RagdollExpect(pelvisRotation[8] > 0.7f,
                      "active gait keeps the pelvis upright beneath the torso");
        if (tick == 255u || tick == 511u || tick == 1023u)
        {
            const double progress =
                (pelvisPosition[0] - segmentStart[0]) * directionX +
                (pelvisPosition[1] - segmentStart[1]) * directionY;
            RagdollExpect(progress > 0.8,
                          "starting, turning and restarting all move in the requested direction");
            RagdollExpect(torsoRotation[3] * directionX +
                              torsoRotation[4] * directionY > 0.8,
                          "the torso faces the travel direction after completing a turn");
        }
        if (tick == 703u)
            memcpy(settledPelvis, pelvisPosition, sizeof(settledPelvis));
        if (tick == 767u)
        {
            double velocity[3];
            RagdollExpect(VoxelRigidBodyLinearVelocity(
                              &first->bodies[WALK_RAGDOLL_PELVIS], velocity),
                          "stopped controller reports a finite pelvis velocity");
            const double driftX = pelvisPosition[0] - settledPelvis[0];
            const double driftY = pelvisPosition[1] - settledPelvis[1];
            RagdollExpect(driftX * driftX + driftY * driftY < 0.01 &&
                              velocity[0] * velocity[0] + velocity[1] * velocity[1] < 0.04,
                          "released input stops the character without persistent skating");
        }
        if (tick == 255u || tick == 511u || tick == 767u)
            memcpy(segmentStart, pelvisPosition, sizeof(segmentStart));
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
    RagdollExpect(WalkRagdollGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision),
                  "feet detect the top surface of the solid floor");
    RagdollExpect(
        WalkRagdollJumpIfGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision, 5.0),
        "jump is accepted while a foot supports the character");
    RagdollExpect(
        !WalkRagdollJumpIfGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision, 5.0),
        "an upward-moving ragdoll cannot jump again before landing");
    bool becameAirborne = false;
    bool landed = false;
    for (uint32_t tick = 0u; tick < 300u; ++tick)
    {
        const double deltaSeconds = 1.0 / 128.0;
        RagdollExpect(
            WalkRagdollPoseDrive(&linkedWalkPhysicsContext, ragdoll, 0.0, 0.0, 0.0, deltaSeconds) &&
                VoxelRagdollStep(ragdoll, &harness->collision, &harness->rigidSettings,
                                 &harness->ragdollSettings, harness->scratch, harness->scratchBytes,
                                 NULL),
            "jump arc advances through the fixed physics step");
        const bool grounded =
            WalkRagdollGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision);
        becameAirborne = becameAirborne || (tick > 4u && !grounded);
        landed = landed || (tick > 128u && grounded);
        if (landed)
            break;
    }
    RagdollExpect(becameAirborne && landed,
                  "jump follows one arc and regains grounded state on landing");
    RagdollExpect(
        WalkRagdollJumpIfGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision, 5.0),
        "a grounded landing restores one valid jump");
    VoxelRagdollRelease(ragdoll);
}

static void RunPlanarIsotropyRegression(void)
{
    VoxelRagdoll *cardinal = PlatformAllocate(sizeof(*cardinal), false);
    VoxelRagdoll *diagonal = PlatformAllocate(sizeof(*diagonal), false);
    RagdollExpect(cardinal != NULL && diagonal != NULL,
                  "planar motor test allocates cardinal and diagonal ragdolls");
    memset(cardinal, 0, sizeof(*cardinal));
    memset(diagonal, 0, sizeof(*diagonal));
    RagdollExpect(InitializeHumanoid(cardinal, 14000u) &&
                      InitializeHumanoid(diagonal, 14000u),
                  "planar motor test initializes matching ragdolls");
    const double verticalVelocity[3] = {0.0, 0.0, 2.0};
    RagdollExpect(VoxelRigidBodyAddLinearVelocity(
                      &cardinal->bodies[WALK_RAGDOLL_PELVIS], verticalVelocity) &&
                      VoxelRigidBodyAddLinearVelocity(
                          &diagonal->bodies[WALK_RAGDOLL_PELVIS], verticalVelocity),
                  "planar motor test begins with independent vertical motion");
    const double diagonalComponent = ScalarSqrtDouble(0.5);
    const double maximumDelta = 3.5 / 128.0;
    double previousCardinal[3] = {0.0, 0.0, 2.0};
    double previousDiagonal[3] = {0.0, 0.0, 2.0};
    for (uint32_t tick = 0u; tick < 192u; ++tick)
    {
        const double direction = tick < 64u ? 1.0 : (tick < 128u ? 0.0 : -1.0);
        RagdollExpect(WalkRagdollDrivePlanar(&linkedWalkPhysicsContext, cardinal, direction, 0.0,
                                             1.2, 3.5, 1.0 / 128.0) &&
                          WalkRagdollDrivePlanar(
                              &linkedWalkPhysicsContext, diagonal, direction * diagonalComponent,
                              direction * diagonalComponent, 1.2, 3.5, 1.0 / 128.0),
                      "cardinal and diagonal motors accelerate, brake and reverse");
        double cardinalVelocity[3], diagonalVelocity[3];
        RagdollExpect(VoxelRigidBodyLinearVelocity(
                          &cardinal->bodies[WALK_RAGDOLL_PELVIS], cardinalVelocity) &&
                          VoxelRigidBodyLinearVelocity(
                              &diagonal->bodies[WALK_RAGDOLL_PELVIS], diagonalVelocity),
                      "planar motor velocities remain finite");
        const double cardinalSpeed = RagdollAbs(cardinalVelocity[0]);
        const double diagonalSpeed = ScalarSqrtDouble(
            diagonalVelocity[0] * diagonalVelocity[0] +
            diagonalVelocity[1] * diagonalVelocity[1]);
        RagdollExpect(RagdollAbs(cardinalSpeed - diagonalSpeed) < 1.0e-6 &&
                          RagdollAbs(diagonalVelocity[0] - diagonalVelocity[1]) < 1.0e-8,
                      "diagonal input gains no acceleration or braking advantage");
        const double deltaX = diagonalVelocity[0] - previousDiagonal[0];
        const double deltaY = diagonalVelocity[1] - previousDiagonal[1];
        RagdollExpect(RagdollAbs(cardinalVelocity[0] - previousCardinal[0]) <=
                          maximumDelta + 1.0e-8 &&
                          deltaX * deltaX + deltaY * deltaY <=
                              maximumDelta * maximumDelta + 1.0e-8 &&
                          cardinalVelocity[1] == 0.0 && cardinalVelocity[2] == 2.0 &&
                          diagonalVelocity[2] == 2.0,
                      "the acceleration budget is radial and never changes vertical motion");
        if (tick == 63u || tick == 191u)
            RagdollExpect(cardinalVelocity[0] * direction > 1.19,
                          "both travel directions reach the requested speed");
        if (tick == 127u)
            RagdollExpect(cardinalSpeed < 1.0e-6 && diagonalSpeed < 1.0e-6,
                          "released cardinal and diagonal input both brake to rest");
        memcpy(previousCardinal, cardinalVelocity, sizeof(previousCardinal));
        memcpy(previousDiagonal, diagonalVelocity, sizeof(previousDiagonal));
    }
    VoxelRagdollRelease(diagonal);
    VoxelRagdollRelease(cardinal);
    PlatformFree(diagonal);
    PlatformFree(cardinal);
}

static void RunJumpInputRegression(RagdollHarness *harness)
{
    VoxelRagdoll *held = PlatformAllocate(sizeof(*held), false);
    VoxelRagdoll *pressed = PlatformAllocate(sizeof(*pressed), false);
    RagdollExpect(held != NULL && pressed != NULL,
                  "jump input test allocates paired ragdolls");
    memset(held, 0, sizeof(*held));
    memset(pressed, 0, sizeof(*pressed));
    RagdollExpect(InitializeHumanoid(held, 12000u) &&
                      InitializeHumanoid(pressed, 12000u),
                  "jump input test initializes identical ragdolls");
    WalkHumanoidControllerState heldController = {0};
    WalkHumanoidControllerState pressedController = {0};
    bool heldGrounded = true;
    bool pressedGrounded = true;
    double heldFacing = 0.0, pressedFacing = 0.0;
    double heldPhase = 0.0, pressedPhase = 0.0;
    double startingPelvis[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &held->bodies[WALK_RAGDOLL_PELVIS], startingPelvis),
                  "jump input test reads the starting pelvis");
    double peakHeight = startingPelvis[2];
    uint32_t rejectedAirborneRequests = 0u;
    bool becameAirborne = false;
    bool landed = false;
    for (uint32_t tick = 0u; tick < 416u; ++tick)
    {
        const bool airborneRequest = !pressedGrounded && tick >= 16u &&
                                     tick < 80u && tick % 8u == 0u;
        if (airborneRequest)
            ++rejectedAirborneRequests;
        const bool heldJump = tick < 320u || tick == 384u;
        const bool pressedJump = tick == 0u || airborneRequest || tick == 384u;
        RagdollExpect(
            WalkHumanoidStep(&linkedWalkPhysicsContext, held, &heldController, &harness->collision,
                             &harness->rigidSettings, &harness->ragdollSettings, harness->scratch,
                             harness->scratchBytes, 0.0, 0.0, false, heldJump, 1.0 / 128.0,
                             &heldGrounded, &heldFacing, &heldPhase, NULL) &&
                WalkHumanoidStep(&linkedWalkPhysicsContext, pressed, &pressedController,
                                 &harness->collision, &harness->rigidSettings,
                                 &harness->ragdollSettings, harness->scratch, harness->scratchBytes,
                                 0.0, 0.0, false, pressedJump, 1.0 / 128.0, &pressedGrounded,
                                 &pressedFacing, &pressedPhase, NULL),
            "held and repeated airborne jump inputs advance normally");
        RagdollExpect(HashBodyState(held) == HashBodyState(pressed) &&
                          heldGrounded == pressedGrounded,
                      "holding jump and pressing it in the air add no extra impulse");
        double pelvis[3];
        RagdollExpect(VoxelRigidBodyLocalPosition(
                          &held->bodies[WALK_RAGDOLL_PELVIS], pelvis),
                      "jump input test keeps a finite pelvis");
        if (tick < 320u && pelvis[2] > peakHeight)
            peakHeight = pelvis[2];
        becameAirborne = becameAirborne || (tick < 128u && !heldGrounded);
        landed = landed || (tick > 64u && tick < 320u && heldGrounded);
        if (tick == 383u)
            RagdollExpect(heldGrounded,
                          "one held press lands and remains ready for a new press");
        if (tick == 384u)
        {
            double velocity[3];
            RagdollExpect(VoxelRigidBodyLinearVelocity(
                              &held->bodies[WALK_RAGDOLL_PELVIS], velocity) &&
                              velocity[2] > 2.0 && !heldGrounded,
                          "releasing and pressing after landing starts a second jump");
        }
    }
    RagdollExpect(becameAirborne && landed && rejectedAirborneRequests >= 3u &&
                      peakHeight > startingPelvis[2] + 0.30 &&
                      peakHeight < startingPelvis[2] + 1.50,
                  "a held jump follows one bounded arc and rejects midair requests");
    VoxelRagdollRelease(pressed);
    VoxelRagdollRelease(held);
    PlatformFree(pressed);
    PlatformFree(held);
}

static void RunFloorEdgeSupportRegression(RagdollHarness *harness)
{
    VoxelRagdoll *ragdoll = PlatformAllocate(sizeof(*ragdoll), false);
    RagdollExpect(ragdoll != NULL, "ledge support test allocates a ragdoll");
    memset(ragdoll, 0, sizeof(*ragdoll));
    RagdollExpect(InitializeHumanoid(ragdoll, 13000u),
                  "ledge support test initializes a straddling ragdoll");
    const VoxelCollisionSource edge = {.queryBlockPhysics = QueryRagdollFloorEdge};
    RagdollExpect(WalkRagdollFootGrounded(&linkedWalkPhysicsContext, ragdoll, &edge,
                                          WALK_RAGDOLL_LEFT_FOOT) &&
                      !WalkRagdollFootGrounded(&linkedWalkPhysicsContext, ragdoll, &edge,
                                               WALK_RAGDOLL_RIGHT_FOOT),
                  "only the foot overlapping a real floor can provide support");
    WalkHumanoidControllerState controller = {0};
    bool grounded = true;
    double facing = 0.0, phase = 0.0;
    RagdollExpect(WalkHumanoidStep(&linkedWalkPhysicsContext, ragdoll, &controller, &edge,
                                   &harness->rigidSettings, &harness->ragdollSettings,
                                   harness->scratch, harness->scratchBytes, 0.0, 0.0, false, false,
                                   1.0 / 128.0, &grounded, &facing, &phase, NULL),
                  "a one-foot ledge contact advances the shared controller");
    RagdollExpect(controller.footAnchorValid[0] && !controller.footAnchorValid[1],
                  "the unsupported foot does not inherit the other foot's planted anchor");
    RagdollExpect(WalkRagdollFootGrounded(&linkedWalkPhysicsContext, ragdoll, &harness->collision,
                                          WALK_RAGDOLL_RIGHT_FOOT) &&
                      WalkHumanoidStep(&linkedWalkPhysicsContext, ragdoll, &controller,
                                       &harness->collision, &harness->rigidSettings,
                                       &harness->ragdollSettings, harness->scratch,
                                       harness->scratchBytes, 0.0, 0.0, false, false, 1.0 / 128.0,
                                       &grounded, &facing, &phase, NULL) &&
                      controller.footAnchorValid[1],
                  "a foot landing during stance acquires its new support immediately");
    VoxelRagdollRelease(ragdoll);
    PlatformFree(ragdoll);
}

static void RunGroundedWalkingRegression(RagdollHarness *harness)
{
    VoxelRagdoll *first = PlatformAllocate(sizeof(*first), false);
    VoxelRagdoll *second = PlatformAllocate(sizeof(*second), false);
    RagdollExpect(first != NULL && second != NULL,
                  "walking controller allocates paired ragdolls");
    memset(first, 0, sizeof(*first));
    memset(second, 0, sizeof(*second));
    RagdollExpect(InitializeHumanoid(first, 9000u) &&
                      InitializeHumanoid(second, 9000u),
                  "walking controller initializes deterministic ragdolls");
    WalkHumanoidControllerState controllerA = {0};
    WalkHumanoidControllerState controllerB = {0};
    bool groundedA = WalkRagdollGrounded(&linkedWalkPhysicsContext, first, &harness->collision);
    bool groundedB = groundedA;
    double facingA = 0.0;
    double facingB = 0.0;
    double phaseA = 0.0;
    double phaseB = 0.0;
    double startingRoot[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &first->bodies[WALK_RAGDOLL_PELVIS], startingRoot),
                  "walking controller reads the starting pelvis position");
    double startingFootHeight[2] = {0.0, 0.0};
    double peakFootHeight[2] = {-INFINITY, -INFINITY};
    double maximumPlantedFootError[2] = {0.0, 0.0};
    uint32_t maximumSlipTick[2] = {0u, 0u};
    double minimumSoleHeight[2] = {INFINITY, INFINITY};
    uint32_t minimumSoleTick[2] = {0u, 0u};
    double minimumSoleVelocity[2][3] = {{0.0}};
    double minimumSoleAngular[2][3] = {{0.0}};
    uint32_t supportedFootTicks[2] = {0u, 0u};
    uint32_t stanceTransitions[2] = {0u, 0u};
    bool previousStance[2] = {false, false};
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        double position[3];
        RagdollExpect(VoxelRigidBodyLocalPosition(
                          &first->bodies[WALK_RAGDOLL_LEFT_FOOT + foot], position),
                      "walking controller reads both starting feet");
        startingFootHeight[foot] = position[2];
        peakFootHeight[foot] = position[2];
    }
    double minimumHeight = startingRoot[2];
    double maximumHeight = startingRoot[2];
    uint32_t minimumHeightTick = 0u;
    double minimumHeightVelocity = 0.0;
    double minimumFootHeight[2] = {0.0, 0.0};
    bool minimumFootGrounded[2] = {false, false};
    uint32_t groundedTicks = 0u;
    const double deltaSeconds = 1.0 / 128.0;
    for (uint32_t tick = 0u; tick < 768u; ++tick)
    {
        const double moveY = tick < 512u ? 1.0 : 0.0;
        RagdollExpect(WalkHumanoidStep(&linkedWalkPhysicsContext, first, &controllerA,
                                       &harness->collision, &harness->rigidSettings,
                                       &harness->ragdollSettings, harness->scratch,
                                       harness->scratchBytes, 0.0, moveY, false, false,
                                       deltaSeconds, &groundedA, &facingA, &phaseA, NULL) &&
                          WalkHumanoidStep(&linkedWalkPhysicsContext, second, &controllerB,
                                           &harness->collision, &harness->rigidSettings,
                                           &harness->ragdollSettings, harness->scratch,
                                           harness->scratchBytes, 0.0, moveY, false, false,
                                           deltaSeconds, &groundedB, &facingB, &phaseB, NULL),
                      "fixed-step stance and swing controller advances");
        if (tick < 512u && groundedA)
            ++groundedTicks;
        if ((tick & 15u) == 0u)
            RagdollExpect(HashBodyState(first) == HashBodyState(second),
                          "procedural footsteps replay deterministically");
        double root[3];
        RagdollExpect(VoxelRigidBodyLocalPosition(
                          &first->bodies[WALK_RAGDOLL_PELVIS], root),
                      "walking controller keeps a finite pelvis");
        for (uint32_t foot = 0u; foot < 2u; ++foot)
        {
            double footPosition[3];
            const uint32_t bodyIndex = WALK_RAGDOLL_LEFT_FOOT + foot;
            RagdollExpect(VoxelRigidBodyLocalPosition(
                              &first->bodies[bodyIndex], footPosition),
                          "walking controller keeps both feet finite");
            if (footPosition[2] > peakFootHeight[foot])
                peakFootHeight[foot] = footPosition[2];
            double soleHeight, unused;
            BodyVerticalBounds(&first->bodies[bodyIndex], &soleHeight, &unused);
            if (soleHeight < minimumSoleHeight[foot])
            {
                minimumSoleHeight[foot] = soleHeight;
                minimumSoleTick[foot] = tick;
                (void)VoxelRigidBodyLinearVelocity(&first->bodies[bodyIndex], minimumSoleVelocity[foot]);
                (void)VoxelRigidBodyAngularVelocity(&first->bodies[bodyIndex], minimumSoleAngular[foot]);
            }
            if (tick < 512u && WalkRagdollFootGrounded(&linkedWalkPhysicsContext, first,
                                                       &harness->collision, bodyIndex))
                ++supportedFootTicks[foot];
            if (controllerA.previousFootStance[foot] != previousStance[foot])
                ++stanceTransitions[foot];
            previousStance[foot] = controllerA.previousFootStance[foot];
            if (controllerA.footAnchorValid[foot] &&
                WalkRagdollFootGrounded(&linkedWalkPhysicsContext, first, &harness->collision,
                                        bodyIndex))
            {
                const double errorX = footPosition[0] -
                    (controllerA.previousRootPosition[0] +
                     controllerA.footAnchorRelativeToRoot[foot][0]);
                const double errorY = footPosition[1] -
                    (controllerA.previousRootPosition[1] +
                     controllerA.footAnchorRelativeToRoot[foot][1]);
                const double error = ScalarSqrtDouble(errorX * errorX + errorY * errorY);
                if (error > maximumPlantedFootError[foot])
                {
                    maximumPlantedFootError[foot] = error;
                    maximumSlipTick[foot] = tick;
                }
            }
        }
        if (root[2] < minimumHeight)
        {
            minimumHeight = root[2];
            minimumHeightTick = tick;
            double rootVelocity[3];
            (void)VoxelRigidBodyLinearVelocity(
                &first->bodies[WALK_RAGDOLL_PELVIS], rootVelocity);
            minimumHeightVelocity = rootVelocity[2];
            for (uint32_t foot = 0u; foot < 2u; ++foot)
            {
                double footPosition[3];
                (void)VoxelRigidBodyLocalPosition(
                    &first->bodies[WALK_RAGDOLL_LEFT_FOOT + foot], footPosition);
                minimumFootHeight[foot] = footPosition[2];
                minimumFootGrounded[foot] =
                    WalkRagdollFootGrounded(&linkedWalkPhysicsContext, first, &harness->collision,
                                            WALK_RAGDOLL_LEFT_FOOT + foot);
            }
        }
        if (root[2] > maximumHeight)
            maximumHeight = root[2];
    }
    double finalRoot[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &first->bodies[WALK_RAGDOLL_PELVIS], finalRoot),
                  "walking controller reads its final pelvis position");
    if (finalRoot[1] - startingRoot[1] <= 2.0)
    {
        LaiueTestRuntimeWrite("walk stats y ");
        RagdollWriteSignedHundredths(startingRoot[1]);
        LaiueTestRuntimeWrite(" -> ");
        RagdollWriteSignedHundredths(finalRoot[1]);
        LaiueTestRuntimeWrite(" z ");
        RagdollWriteSignedHundredths(minimumHeight);
        LaiueTestRuntimeWrite(" .. ");
        RagdollWriteSignedHundredths(maximumHeight);
        LaiueTestRuntimeWrite(" grounded ");
        RagdollWriteUint32(groundedTicks);
        LaiueTestRuntimeWrite(" minTick ");
        RagdollWriteUint32(minimumHeightTick);
        LaiueTestRuntimeWrite(" vz ");
        RagdollWriteSignedHundredths(minimumHeightVelocity);
        LaiueTestRuntimeWrite(" feet ");
        RagdollWriteSignedHundredths(minimumFootHeight[0]);
        LaiueTestRuntimeWrite("/");
        RagdollWriteSignedHundredths(minimumFootHeight[1]);
        LaiueTestRuntimeWrite(" supports ");
        RagdollWriteUint32(minimumFootGrounded[0] ? 1u : 0u);
        RagdollWriteUint32(minimumFootGrounded[1] ? 1u : 0u);
        LaiueTestRuntimeWrite("\r\n");
    }
    RagdollExpect(finalRoot[1] - startingRoot[1] > 2.0,
                  "stance-driven steps translate the character forward");
    RagdollExpect(groundedTicks > 220u,
                  "walking remains supported by the floor for most moving ticks");
    if (!(minimumHeight > startingRoot[2] - 0.20 &&
          maximumHeight < startingRoot[2] + 0.20))
    {
        LaiueTestRuntimeWrite("walk height range ");
        RagdollWriteSignedHundredths(minimumHeight);
        LaiueTestRuntimeWrite(" .. ");
        RagdollWriteSignedHundredths(maximumHeight);
        LaiueTestRuntimeWrite(" final ");
        RagdollWriteSignedHundredths(finalRoot[2]);
        LaiueTestRuntimeWrite(" minTick ");
        RagdollWriteUint32(minimumHeightTick);
        LaiueTestRuntimeWrite(" vz ");
        RagdollWriteSignedHundredths(minimumHeightVelocity);
        LaiueTestRuntimeWrite(" feet ");
        RagdollWriteSignedHundredths(minimumFootHeight[0]);
        LaiueTestRuntimeWrite("/");
        RagdollWriteSignedHundredths(minimumFootHeight[1]);
        LaiueTestRuntimeWrite(" supports ");
        RagdollWriteUint32(minimumFootGrounded[0] ? 1u : 0u);
        RagdollWriteUint32(minimumFootGrounded[1] ? 1u : 0u);
        LaiueTestRuntimeWrite("\r\n");
    }
    RagdollExpect(minimumHeight > startingRoot[2] - 0.20 &&
                      maximumHeight < startingRoot[2] + 0.20,
                  "walking controller does not collapse or launch the pelvis");
    for (uint32_t foot = 0u; foot < 2u; ++foot)
    {
        if (peakFootHeight[foot] <= startingFootHeight[foot] + 0.04 ||
            supportedFootTicks[foot] < 40u ||
            maximumPlantedFootError[foot] > 0.10)
        {
            LaiueTestRuntimeWrite("foot gait stats ");
            RagdollWriteUint32(foot);
            LaiueTestRuntimeWrite(" z0/peak ");
            RagdollWriteSignedHundredths(startingFootHeight[foot]);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(peakFootHeight[foot]);
            LaiueTestRuntimeWrite(" support/slip ");
            RagdollWriteUint32(supportedFootTicks[foot]);
            LaiueTestRuntimeWrite("/");
            RagdollWriteSignedHundredths(maximumPlantedFootError[foot]);
            LaiueTestRuntimeWrite(" tick ");
            RagdollWriteUint32(maximumSlipTick[foot]);
            LaiueTestRuntimeWrite("\r\n");
        }
        RagdollExpect(peakFootHeight[foot] > startingFootHeight[foot] + 0.04,
                      "each swing foot visibly lifts from its starting height");
        RagdollExpect(supportedFootTicks[foot] >= 40u,
                      "each foot makes repeated grounded stance contact");
        RagdollExpect(stanceTransitions[foot] >= 6u,
                      "each foot alternates through repeated stance and swing phases");
        if (minimumSoleHeight[foot] < 1.0 - 0.015)
        {
            LaiueTestRuntimeWrite("sole minimum millimetres ");
            RagdollWriteUint32((uint32_t)(minimumSoleHeight[foot] * 1000.0));
            LaiueTestRuntimeWrite(" tick/velocity/angular ");
            RagdollWriteUint32(minimumSoleTick[foot]);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                LaiueTestRuntimeWrite(" ");
                RagdollWriteSignedHundredths(minimumSoleVelocity[foot][axis]);
                LaiueTestRuntimeWrite("/");
                RagdollWriteSignedHundredths(minimumSoleAngular[foot][axis]);
            }
            LaiueTestRuntimeWrite("\r\n");
        }
        RagdollExpect(minimumSoleHeight[foot] >= 1.0 - 0.015,
                      "both oriented soles penetrate the voxel floor by at most 15mm");
        RagdollExpect(maximumPlantedFootError[foot] <= 0.10,
                      "planted feet stay near their world-space support anchors");
    }
    VoxelRagdollRelease(second);
    VoxelRagdollRelease(first);
    PlatformFree(second);
    PlatformFree(first);
}

static void RunAirborneLocomotionRegression(RagdollHarness *harness)
{
    VoxelRagdoll *ragdoll = PlatformAllocate(sizeof(*ragdoll), false);
    RagdollExpect(ragdoll != NULL, "airborne gait allocates a ragdoll");
    memset(ragdoll, 0, sizeof(*ragdoll));
    RagdollExpect(InitializeHumanoid(ragdoll, 11000u),
                  "airborne gait ragdoll initializes");
    const VoxelCollisionSource noFloor = {
        .queryBlockPhysics = QueryRagdollNoFloor,
    };
    WalkHumanoidControllerState controller = {0};
    bool grounded = false;
    double facingYaw = 0.0;
    double gaitPhase = 0.0;
    double startingRoot[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &ragdoll->bodies[WALK_RAGDOLL_PELVIS], startingRoot),
                  "airborne gait records its launch-free starting pelvis");
    for (uint32_t tick = 0u; tick < 64u; ++tick)
        RagdollExpect(WalkHumanoidStep(&linkedWalkPhysicsContext, ragdoll, &controller, &noFloor,
                                       &harness->rigidSettings, &harness->ragdollSettings,
                                       harness->scratch, harness->scratchBytes, 0.0, 1.0, false,
                                       tick % 8u == 0u, 1.0 / 128.0, &grounded, &facingYaw,
                                       &gaitPhase, NULL),
                      "airborne input advances with gravity and limited steering");
    double finalRoot[3];
    RagdollExpect(VoxelRigidBodyLocalPosition(
                      &ragdoll->bodies[WALK_RAGDOLL_PELVIS], finalRoot),
                  "airborne gait records its final pelvis");
    const double horizontalTravel = ScalarSqrtDouble(
        (finalRoot[0] - startingRoot[0]) * (finalRoot[0] - startingRoot[0]) +
        (finalRoot[1] - startingRoot[1]) * (finalRoot[1] - startingRoot[1]));
    RagdollExpect(horizontalTravel < 0.20 &&
                      finalRoot[2] < startingRoot[2] - 0.50,
                  "airborne movement and repeated jump requests cannot prevent falling");
    VoxelRagdollRelease(ragdoll);
    PlatformFree(ragdoll);
}

static void RunFallBraceResponseRegression(void)
{
    VoxelRagdoll *normal = PlatformAllocate(sizeof(*normal), false);
    VoxelRagdoll *falling = PlatformAllocate(sizeof(*falling), false);
    RagdollExpect(normal != NULL && falling != NULL,
                  "brace response allocates fixed-capacity humanoids");
    memset(normal, 0, sizeof(*normal));
    memset(falling, 0, sizeof(*falling));
    RagdollExpect(InitializeHumanoid(normal, 7000u) &&
                      InitializeHumanoid(falling, 8000u),
                  "brace response initializes two deterministic humanoids");
    const double downwardVelocity[3] = {0.0, 0.0, -6.0};
    RagdollExpect(VoxelRigidBodyAddLinearVelocity(
                      &falling->bodies[WALK_RAGDOLL_PELVIS], downwardVelocity),
                  "brace response receives a fixed downward impact velocity");
    RagdollExpect(
        WalkRagdollPoseDrive(&linkedWalkPhysicsContext, normal, 0.0, 0.0, 0.0, 1.0 / 128.0) &&
            WalkRagdollPoseDrive(&linkedWalkPhysicsContext, falling, 0.0, 0.0, 0.0, 1.0 / 128.0),
        "pose motors accept both calm and falling states");
    double angularDifferenceSquared = 0.0;
    for (uint32_t body = WALK_RAGDOLL_LEFT_UPPER_ARM;
         body <= WALK_RAGDOLL_RIGHT_FOREARM; ++body)
    {
        double normalVelocity[3];
        double fallingVelocity[3];
        RagdollExpect(VoxelRigidBodyAngularVelocity(&normal->bodies[body],
                                                     normalVelocity) &&
                          VoxelRigidBodyAngularVelocity(&falling->bodies[body],
                                                       fallingVelocity),
                      "brace response reads finite arm motor velocities");
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double difference = fallingVelocity[axis] - normalVelocity[axis];
            angularDifferenceSquared += difference * difference;
        }
    }
    RagdollExpect(angularDifferenceSquared > 1.0e-8,
                  "fast falling deterministically moves the arms into a brace pose");
    VoxelRagdollRelease(falling);
    VoxelRagdollRelease(normal);
    PlatformFree(falling);
    PlatformFree(normal);
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
        RagdollExpect(
            WalkRagdollPoseDrive(&linkedWalkPhysicsContext, ragdoll, 0.0, 0.0, 0.0, deltaSeconds) &&
                VoxelRagdollStep(ragdoll, &harness->collision, &harness->rigidSettings,
                                 &harness->ragdollSettings, harness->scratch, harness->scratchBytes,
                                 NULL),
            "active ragdoll remains valid while recovering from a stumble");
    }
    float torsoRotation[9];
    VoxelRigidBodyOrientationMatrix(&ragdoll->bodies[WALK_RAGDOLL_TORSO],
                                    torsoRotation);
    if (!(torsoRotation[8] > 0.7f && JointErrorSquared(ragdoll) < 0.0025))
    {
        LaiueTestRuntimeWrite("recovery torsoUp/jointcm ");
        RagdollWriteSignedHundredths(torsoRotation[8]);
        LaiueTestRuntimeWrite("/");
        RagdollWriteSignedHundredths(ScalarSqrtDouble(JointErrorSquared(ragdoll)));
        LaiueTestRuntimeWrite("\r\n");
    }
    RagdollExpect(torsoRotation[8] > 0.7f && JointErrorSquared(ragdoll) < 0.0025,
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
    RagdollExpect(BindLinkedWalkPhysics(), "walk binds the linked physics table");
    RagdollExpect(WalkRagdollCoordinateSafeForVoxelQuery(0.0) &&
                      WalkRagdollCoordinateSafeForVoxelQuery(1.0e9) &&
                      !WalkRagdollCoordinateSafeForVoxelQuery(0x1p62) &&
                      !WalkRagdollCoordinateSafeForVoxelQuery(-0x1p62) &&
                      !WalkRagdollCoordinateSafeForVoxelQuery(0x1p63) &&
                      !WalkRagdollCoordinateSafeForVoxelQuery(-0x1p63) &&
                      !WalkRagdollCoordinateSafeForVoxelQuery(INFINITY),
                  "voxel query coordinates stay safely inside int64 limits");
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
    RunCameraRegression();
    RunHumanoidTopologyRegression(&harness);
    RunMetricStandingRegression(&harness);
    RunPlanarIsotropyRegression();
    RunJumpInputRegression(&harness);
    RunFloorEdgeSupportRegression(&harness);
    RunFallBraceResponseRegression();
    RunHumanoidReplay(&harness, first, second);
    RunGroundedWalkingRegression(&harness);
    RunAirborneLocomotionRegression(&harness);
    // Повторная инициализация живой ragdoll теряла бы bigint-скорости её
    // тел: сначала владелец освобождает прежнее состояние.
    VoxelRagdollRelease(second);
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
        .origin = {0.0, 0.0, 1.0},
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
