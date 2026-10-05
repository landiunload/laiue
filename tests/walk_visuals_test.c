#include "test_runtime.h"
#include "walk_visuals.h"
#include "humanoid_ragdoll.h"
#include "physics/numeric_provider.h"
#include "walk_physics_binding.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct VisualDevice
{
    LaiueGraphicsDeviceV2 api;
    uint64_t capacity;
    uint32_t uploads;
    uint32_t destroys;
    bool failCreate;
    bool failUpload;
    LaiueGraphicsVertexV2 uploaded[WALK_RAGDOLL_VISUAL_VERTEX_COUNT];
} VisualDevice;

typedef struct GuardedScratch
{
    uint64_t before;
    WalkRagdollVisualScratch scratch;
    uint64_t after;
} GuardedScratch;

static VisualDevice testDevice;
static VoxelRagdoll testRagdoll;
static GuardedScratch guardedScratch;
static LaiueGraphicsVertexV2 referenceVertices[WALK_RAGDOLL_VISUAL_VERTEX_COUNT];
static uint64_t mesherScratchToken;
static uint32_t mesherScratchReleases;

static void DestroyMesherScratch(ChunkMesherScratch *scratch)
{
    if (scratch == (ChunkMesherScratch *)&mesherScratchToken)
        ++mesherScratchReleases;
}

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Walk visuals check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t CreateBuffer(LaiueGraphicsDeviceV2 *api,
                              const LaiueGraphicsBufferDescV1 *description,
                              LaiueGraphicsHandle *outBuffer)
{
    VisualDevice *device = (VisualDevice *)api->context;
    Expect(description != NULL && outBuffer != NULL &&
               description->structSize == sizeof(*description) &&
               description->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
           "ragdoll buffer uses the portable vertex-buffer contract");
    Expect(description->sizeBytes == sizeof(device->uploaded),
           "ragdoll allocation matches the public mesh vertex capacity");
    if (device->failCreate)
        return 0u;
    device->capacity = description->sizeBytes;
    *outBuffer = 17u;
    return 1u;
}

static uint32_t UploadBuffer(LaiueGraphicsDeviceV2 *api,
                              const LaiueGraphicsBufferUploadV1 *upload)
{
    VisualDevice *device = (VisualDevice *)api->context;
    Expect(upload != NULL && upload->structSize == sizeof(*upload) &&
               upload->buffer == 17u && upload->offsetBytes == 0u &&
               upload->data != NULL && upload->sizeBytes == device->capacity &&
               upload->sizeBytes == sizeof(device->uploaded),
           "mesh upload fills the allocated buffer without exceeding capacity");
    ++device->uploads;
    if (device->failUpload)
        return 0u;
    memcpy(device->uploaded, upload->data, sizeof(device->uploaded));
    return 1u;
}

static void DestroyHandle(LaiueGraphicsDeviceV2 *api, LaiueGraphicsHandle handle)
{
    VisualDevice *device = (VisualDevice *)api->context;
    Expect(handle == 17u, "visual cleanup destroys its own buffer handle");
    ++device->destroys;
}

static void CheckScratchGuards(void)
{
    Expect(guardedScratch.before == UINT64_C(0x23456789ABCDEF01) &&
               guardedScratch.after == UINT64_C(0xFEDCBA9876543210),
           "mesh generation stays within the caller-provided scratch buffer");
}

static void ExpectRejected(LaiueGraphicsDeviceV2 *device,
                            LaiueGraphicsHandle buffer,
                            const VoxelRagdoll *ragdoll,
                            const double origin[3], const char *message)
{
    const uint32_t uploads = testDevice.uploads;
    Expect(!WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, device, buffer, ragdoll,
                                           origin, &guardedScratch.scratch),
           message);
    Expect(testDevice.uploads == uploads,
           "invalid visual input cannot submit a partially built mesh");
    CheckScratchGuards();
}

static void CheckFiniteMesh(void)
{
    for (uint32_t vertex = 0u; vertex < WALK_RAGDOLL_VISUAL_VERTEX_COUNT; ++vertex)
    {
        const LaiueGraphicsVertexV2 *value = &testDevice.uploaded[vertex];
        Expect(WalkMathFinite(value->position[0]) && WalkMathFinite(value->position[1]) &&
                   WalkMathFinite(value->position[2]) && WalkMathFinite(value->uv[0]) &&
                   WalkMathFinite(value->uv[1]),
               "every uploaded position and texture coordinate is finite");
        Expect((value->colorRGBA >> 24u) == 255u,
               "the complete opaque character mesh has initialized colors");
    }
    CheckScratchGuards();
}

static void TriangleNormal(uint32_t first, double normal[3])
{
    double edgeA[3], edgeB[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        edgeA[axis] = (double)testDevice.uploaded[first + 1u].position[axis] -
                      testDevice.uploaded[first].position[axis];
        edgeB[axis] = (double)testDevice.uploaded[first + 2u].position[axis] -
                      testDevice.uploaded[first].position[axis];
    }
    normal[0] = edgeA[1] * edgeB[2] - edgeA[2] * edgeB[1];
    normal[1] = edgeA[2] * edgeB[0] - edgeA[0] * edgeB[2];
    normal[2] = edgeA[0] * edgeB[1] - edgeA[1] * edgeB[0];
}

static void CheckTriangleGeometry(void)
{
    Expect(WALK_RAGDOLL_VISUAL_VERTEX_COUNT % 3u == 0u,
           "the mesh capacity contains complete triangles");
    for (uint32_t first = 0u; first < WALK_RAGDOLL_VISUAL_VERTEX_COUNT; first += 3u)
    {
        double normal[3];
        TriangleNormal(first, normal);
        Expect(normal[0] * normal[0] + normal[1] * normal[1] +
                   normal[2] * normal[2] > 1.0e-15,
               "the mesh has no collapsed triangles, including sphere poles");
    }
    /* Exercise both independent primitive generators: a convex pelvis loft
     * (80 triangles), then the head ellipsoid after pelvis and torso. These
     * ranges intentionally exclude the separately attached facial details. */
    const uint32_t starts[2] = {0u, 480u};
    const uint32_t counts[2] = {240u, 504u};
    const uint32_t bodies[2] = {WALK_RAGDOLL_PELVIS, WALK_RAGDOLL_HEAD};
    for (uint32_t part = 0u; part < 2u; ++part)
    {
        double center[3];
        Expect(VoxelRigidBodyLocalPosition(&testRagdoll.bodies[bodies[part]], center),
               "the winding test reads each primitive's physics center");
        for (uint32_t first = starts[part]; first < starts[part] + counts[part]; first += 3u)
        {
            double normal[3];
            double outward = 0.0;
            TriangleNormal(first, normal);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                outward += normal[axis] *
                    ((double)testDevice.uploaded[first].position[axis] - center[axis]);
            Expect(outward > 1.0e-9,
                   "loft and ellipsoid triangles face outward without inverted caps");
        }
    }
}

static void CheckNeutralScale(void)
{
    double minimumZ = INFINITY;
    double maximumZ = -INFINITY;
    for (uint32_t vertex = 0u; vertex < WALK_RAGDOLL_VISUAL_VERTEX_COUNT; ++vertex)
    {
        const double z = testDevice.uploaded[vertex].position[2];
        if (z < minimumZ)
            minimumZ = z;
        if (z > maximumZ)
            maximumZ = z;
    }
    Expect(WalkMathAbs(maximumZ - minimumZ - 1.8) < 2.0e-5,
           "the neutral visible character is 1.8 meters from sole to crown");
    double head[3], leftFoot[3], rightFoot[3];
    Expect(VoxelRigidBodyLocalPosition(&testRagdoll.bodies[WALK_RAGDOLL_HEAD], head) &&
               VoxelRigidBodyLocalPosition(&testRagdoll.bodies[WALK_RAGDOLL_LEFT_FOOT], leftFoot) &&
               VoxelRigidBodyLocalPosition(&testRagdoll.bodies[WALK_RAGDOLL_RIGHT_FOOT], rightFoot),
           "scale validation reads the neutral head and feet physics centers");
    const double leftSole = leftFoot[2] -
        testRagdoll.bodies[WALK_RAGDOLL_LEFT_FOOT].halfExtent[2];
    const double rightSole = rightFoot[2] -
        testRagdoll.bodies[WALK_RAGDOLL_RIGHT_FOOT].halfExtent[2];
    const double crown = head[2] + testRagdoll.bodies[WALK_RAGDOLL_HEAD].halfExtent[2];
    Expect(WalkMathAbs(leftSole - minimumZ) < 2.0e-5 &&
               WalkMathAbs(rightSole - minimumZ) < 2.0e-5 &&
               WalkMathAbs(crown - maximumZ) < 2.0e-5,
           "rendered soles and crown match the one-block-per-meter collision geometry");
}

LAIUE_TEST_ENTRY(WalkVisualsTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    Expect(BindLinkedWalkPhysics(), "walk binds the linked physics table");
    testDevice.api = (LaiueGraphicsDeviceV2){
        .structSize = sizeof(testDevice.api),
        .abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION,
        .context = &testDevice,
        .createBuffer = CreateBuffer,
        .uploadBuffer = UploadBuffer,
        .destroyHandle = DestroyHandle,
    };
    guardedScratch.before = UINT64_C(0x23456789ABCDEF01);
    guardedScratch.after = UINT64_C(0xFEDCBA9876543210);
    const VoxelRagdollDefinition definition = {
        .stableIdBase = 12000u,
        .origin = {3.0, -4.0, 2.0},
        .bodies = walkRagdollBodies,
        .bodyCount = WALK_RAGDOLL_BODY_COUNT,
        .joints = walkRagdollJoints,
        .jointCount = WALK_RAGDOLL_JOINT_COUNT,
        .rootBody = WALK_RAGDOLL_PELVIS,
    };
    Expect(VoxelRagdollInitialize(&testRagdoll, &definition),
           "the real humanoid definition initializes for mesh verification");
    LaiueGraphicsHandle buffer = 0u;
    Expect(WalkVisualsCreateRagdollBuffer(&testDevice.api, &buffer) && buffer == 17u,
           "shared visuals create a buffer through a provider callback");
    const double origin[3] = {0.0, 0.0, 0.0};
    Expect(WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, &testDevice.api, buffer,
                                          &testRagdoll, origin, &guardedScratch.scratch),
           "the complete shared humanoid mesh uploads successfully");
    CheckFiniteMesh();
    CheckTriangleGeometry();
    CheckNeutralScale();
    memcpy(referenceVertices, testDevice.uploaded, sizeof(referenceVertices));

    const double shiftedOrigin[3] = {2.0, -3.0, 1.0};
    Expect(WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, &testDevice.api, buffer,
                                          &testRagdoll, shiftedOrigin, &guardedScratch.scratch),
           "mesh generation accepts a camera-relative render origin");
    for (uint32_t vertex = 0u; vertex < WALK_RAGDOLL_VISUAL_VERTEX_COUNT; ++vertex)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            Expect(WalkMathAbs((double)testDevice.uploaded[vertex].position[axis] -
                         ((double)referenceVertices[vertex].position[axis] -
                          shiftedOrigin[axis])) < 2.0e-5,
                   "every body, joint, and detail follows the render-origin shift");

    const int64_t translation[3] = {5, -6, 2};
    double movedCenter[3];
    double oldCenter[3];
    Expect(VoxelRigidBodyLocalPosition(&testRagdoll.bodies[0], oldCenter),
           "the original physics body center is readable");
    for (uint32_t body = 0u; body < testRagdoll.bodyCount; ++body)
        Expect(VoxelRigidBodyTranslateBlocks(&testRagdoll.bodies[body], translation),
               "all real physics body positions can be rebased");
    Expect(VoxelRigidBodyLocalPosition(&testRagdoll.bodies[0], movedCenter),
           "the translated physics body center is readable");
    Expect(WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, &testDevice.api, buffer,
                                          &testRagdoll, origin, &guardedScratch.scratch),
           "visual geometry follows the translated physics pose");
    for (uint32_t vertex = 0u; vertex < WALK_RAGDOLL_VISUAL_VERTEX_COUNT; ++vertex)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            Expect(WalkMathAbs((double)testDevice.uploaded[vertex].position[axis] -
                         ((double)referenceVertices[vertex].position[axis] +
                          movedCenter[axis] - oldCenter[axis])) < 2.0e-5,
                   "the whole mesh follows physics positions without stale transforms");
    CheckFiniteMesh();

    memcpy(referenceVertices, testDevice.uploaded, sizeof(referenceVertices));
    testRagdoll.bodies[WALK_RAGDOLL_PELVIS].orientation[2] = 0.7071067811865475;
    testRagdoll.bodies[WALK_RAGDOLL_PELVIS].orientation[3] = 0.7071067811865475;
    Expect(WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, &testDevice.api, buffer,
                                          &testRagdoll, origin, &guardedScratch.scratch),
           "visual geometry accepts an independently rotated physics body");
    for (uint32_t vertex = 0u; vertex < 240u; ++vertex)
    {
        const double x = (double)referenceVertices[vertex].position[0] - movedCenter[0];
        const double y = (double)referenceVertices[vertex].position[1] - movedCenter[1];
        Expect(WalkMathAbs((double)testDevice.uploaded[vertex].position[0] -
                     (movedCenter[0] - y)) < 2.0e-5 &&
                   WalkMathAbs((double)testDevice.uploaded[vertex].position[1] -
                        (movedCenter[1] + x)) < 2.0e-5 &&
                   WalkMathAbs((double)testDevice.uploaded[vertex].position[2] -
                        referenceVertices[vertex].position[2]) < 2.0e-5,
               "the pelvis mesh rotates around the physical center with its quaternion");
    }
    testRagdoll.bodies[WALK_RAGDOLL_PELVIS].orientation[2] = 0.0;
    testRagdoll.bodies[WALK_RAGDOLL_PELVIS].orientation[3] = 1.0;

    ExpectRejected(NULL, buffer, &testRagdoll, origin, "missing device is rejected");
    ExpectRejected(&testDevice.api, 0u, &testRagdoll, origin,
                   "missing graphics buffer is rejected");
    ExpectRejected(&testDevice.api, buffer, NULL, origin, "missing ragdoll is rejected");
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, NULL,
                   "missing render origin is rejected");
    const double invalidOrigin[3] = {NAN, 0.0, 0.0};
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, invalidOrigin,
                   "nonfinite render origin cannot reach the graphics device");
    testRagdoll.initialized = false;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "an uninitialized ragdoll is rejected");
    testRagdoll.initialized = true;
    testRagdoll.bodyCount = VOXEL_RAGDOLL_MAX_BODIES + 1u;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "invalid body count is rejected before array access");
    testRagdoll.bodyCount = WALK_RAGDOLL_BODY_COUNT;
    testRagdoll.jointCount = VOXEL_RAGDOLL_MAX_JOINTS + 1u;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "excess joint count cannot overflow the mesh scratch buffer");
    testRagdoll.jointCount = WALK_RAGDOLL_JOINT_COUNT;
    const uint32_t bodyA = testRagdoll.joints[0].bodyA;
    testRagdoll.joints[0].bodyA = VOXEL_RAGDOLL_MAX_BODIES;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "invalid joint body references are rejected before array access");
    testRagdoll.joints[0].bodyA = bodyA;
    const double extent = testRagdoll.bodies[0].halfExtent[0];
    testRagdoll.bodies[0].halfExtent[0] = NAN;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "nonfinite body dimensions cannot produce corrupt geometry");
    testRagdoll.bodies[0].halfExtent[0] = 0.0;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "zero body dimensions cannot produce degenerate geometry");
    testRagdoll.bodies[0].halfExtent[0] = extent;
    testRagdoll.bodies[0].orientation[3] = NAN;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "nonfinite physics orientation is rejected");
    testRagdoll.bodies[0].orientation[3] = 1.0;

    testDevice.api.structSize = (uint32_t)offsetof(LaiueGraphicsDeviceV2, uploadBuffer);
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "truncated graphics providers are rejected without reading the tail");
    testDevice.api.structSize = sizeof(testDevice.api);
    testDevice.api.uploadBuffer = NULL;
    ExpectRejected(&testDevice.api, buffer, &testRagdoll, origin,
                   "missing upload callback fails cleanly");
    testDevice.api.uploadBuffer = UploadBuffer;
    testDevice.failUpload = true;
    Expect(!WalkVisualsUpdateRagdollBuffer(&linkedWalkPhysicsContext, &testDevice.api, buffer,
                                           &testRagdoll, origin, &guardedScratch.scratch),
           "a graphics-provider upload failure propagates to the caller");
    testDevice.failUpload = false;
    testDevice.failCreate = true;
    LaiueGraphicsHandle failedBuffer = 123u;
    Expect(!WalkVisualsCreateRagdollBuffer(&testDevice.api, &failedBuffer) &&
               failedBuffer == 0u,
           "failed buffer allocation clears the output handle");
    WalkVisualsDestroyBuffer(&testDevice.api, &buffer);
    Expect(buffer == 0u && testDevice.destroys == 1u,
           "mesh cleanup releases its graphics allocation exactly once");
    WalkVisualsDestroyBuffer(&testDevice.api, &buffer);
    Expect(testDevice.destroys == 1u, "repeated mesh cleanup is harmless");
    static WalkVisualChunkSet chunks;
    chunks.scratch = (ChunkMesherScratch *)&mesherScratchToken;
    chunks.centerValid = true;
    chunks.chunks[0].buffers[0] = 17u;
    chunks.chunks[0].ready = true;
    chunks.chunks[WALK_VISUAL_CHUNK_COUNT - 1u].ready = true;
    WalkVisualsInvalidateChunkSet(&testDevice.api, &chunks);
    Expect(chunks.scratch == (ChunkMesherScratch *)&mesherScratchToken && !chunks.centerValid &&
               !chunks.chunks[0].ready && !chunks.chunks[WALK_VISUAL_CHUNK_COUNT - 1u].ready &&
               testDevice.destroys == 2u && mesherScratchReleases == 0u,
           "provider-frame invalidation releases GPU geometry but retains mesher scratch");
    WalkVisualsInvalidateChunkSet(&testDevice.api, &chunks);
    Expect(testDevice.destroys == 2u && mesherScratchReleases == 0u,
           "repeated invalidation does not release scratch or duplicate GPU destruction");
    const LaiueMesherServiceV1 mesher = {
        .structSize = sizeof(mesher),
        .abiVersion = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
        .scratchDestroy = DestroyMesherScratch,
    };
    WalkVisualsDestroyChunkSet(&testDevice.api, &mesher, &chunks);
    WalkVisualsDestroyChunkSet(&testDevice.api, &mesher, &chunks);
    Expect(chunks.scratch == NULL && mesherScratchReleases == 1u,
           "final chunk cleanup releases retained scratch exactly once");
    CheckScratchGuards();
    VoxelRagdollRelease(&testRagdoll);
    LAIUE_TEST_SUCCESS();
}
