#include "test_runtime.h"
#include "walk_visuals.h"
#include "humanoid_ragdoll.h"
#include "physics/numeric_provider.h"
#include "walk_physics_binding.h"
#include "platform/system.h"

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

#define CHUNK_TEST_RESOURCES 128u

typedef struct ChunkTestBuffer
{
    uint32_t usage;
    uint32_t sizeBytes;
    bool live;
    union
    {
        LaiueGraphicsVertexV2 vertices[48];
        uint32_t indices[288];
    } data;
} ChunkTestBuffer;

typedef struct ChunkTestDevice
{
    LaiueGraphicsDeviceV2 api;
    ChunkTestBuffer buffers[CHUNK_TEST_RESOURCES];
    uint32_t namespaceId;
    uint32_t created;
    uint32_t destroyed;
    uint32_t uploaded;
    uint32_t indexUploads;
    uint32_t capabilityQueries;
    uint32_t failCreateUsage;
    uint32_t failUploadUsage;
    bool nativeIndices;
} ChunkTestDevice;

static ChunkTestDevice chunkDevice, foreignChunkDevice;
static WalkVisualChunkSet chunkSet, chunkSnapshot;
static LaiueGraphicsDrawItemV2 chunkDraws[WALK_VISUAL_CHUNK_DRAW_COUNT];
static LaiueGraphicsVertexV2 legacyChunkVertices[WALK_VISUAL_TEXTURE_COUNT][36];
static uint32_t legacyChunkCounts[WALK_VISUAL_TEXTURE_COUNT];
static uint32_t chunkExtraQuads, chunkBuildCalls;
static bool failChunkMesh;
static const LaiueGraphicsHandle chunkTextures[3] = {101u, 102u, 103u};

static ChunkTestBuffer *ChunkBuffer(ChunkTestDevice *device, LaiueGraphicsHandle handle)
{
    Expect((uint32_t)(handle >> 32u) == device->namespaceId && (uint32_t)handle != 0u &&
               (uint32_t)handle <= device->created,
           "chunk graphics callbacks receive their own namespace and allocated slot");
    ChunkTestBuffer *buffer = &device->buffers[(uint32_t)handle - 1u];
    Expect(buffer->live, "chunk handles are live and never destroyed twice");
    return buffer;
}

static uint32_t CreateChunkBuffer(LaiueGraphicsDeviceV2 *api,
                                  const LaiueGraphicsBufferDescV1 *description,
                                  LaiueGraphicsHandle *outBuffer)
{
    ChunkTestDevice *device = (ChunkTestDevice *)api->context;
    Expect(description != NULL && description->structSize == sizeof(*description) &&
               outBuffer != NULL && description->sizeBytes != 0u &&
               description->sizeBytes <= sizeof(device->buffers[0].data) &&
               (description->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX ||
                description->usageFlags == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX),
           "chunk buffers declare exact vertex or index ownership and bounded size");
    if (description->usageFlags == device->failCreateUsage)
        return 0u;
    Expect(device->created < CHUNK_TEST_RESOURCES, "chunk mock resource capacity is sufficient");
    ChunkTestBuffer *buffer = &device->buffers[device->created++];
    buffer->usage = description->usageFlags;
    buffer->sizeBytes = (uint32_t)description->sizeBytes;
    buffer->live = true;
    *outBuffer = ((uint64_t)device->namespaceId << 32u) | device->created;
    return 1u;
}

static uint32_t UploadChunkBuffer(LaiueGraphicsDeviceV2 *api,
                                  const LaiueGraphicsBufferUploadV1 *upload)
{
    ChunkTestDevice *device = (ChunkTestDevice *)api->context;
    Expect(upload != NULL && upload->structSize == sizeof(*upload) && upload->data != NULL &&
               upload->offsetBytes == 0u,
           "chunk resource preparation uploads complete buffers before recording");
    ChunkTestBuffer *buffer = ChunkBuffer(device, upload->buffer);
    Expect(upload->sizeBytes == buffer->sizeBytes, "chunk uploads match their resource allocation");
    ++device->uploaded;
    if (buffer->usage == device->failUploadUsage)
        return 0u;
    memcpy(&buffer->data, upload->data, buffer->sizeBytes);
    if (buffer->usage == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX)
        ++device->indexUploads;
    return 1u;
}

static void DestroyChunkBuffer(LaiueGraphicsDeviceV2 *api, LaiueGraphicsHandle handle)
{
    ChunkTestDevice *device = (ChunkTestDevice *)api->context;
    ChunkBuffer(device, handle)->live = false;
    ++device->destroyed;
}

static uint32_t ChunkCapabilities(const LaiueGraphicsDeviceV2 *api)
{
    ChunkTestDevice *device = (ChunkTestDevice *)api->context;
    Expect(api->structSize >= LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE,
           "a truncated capability tail is never called");
    ++device->capabilityQueries;
    return device->nativeIndices ? LAIUE_GRAPHICS_CAP_NATIVE_INDICES : 0u;
}

static void InitializeChunkDevice(ChunkTestDevice *device, uint32_t namespaceId, bool native)
{
    memset(device, 0, sizeof(*device));
    device->namespaceId = namespaceId;
    device->nativeIndices = native;
    device->api = (LaiueGraphicsDeviceV2){
        .structSize = sizeof(device->api),
        .abiVersion = LAIUE_GRAPHICS_DEVICE_V2_ABI_VERSION,
        .context = device,
        .createBuffer = CreateChunkBuffer,
        .uploadBuffer = UploadChunkBuffer,
        .destroyHandle = DestroyChunkBuffer,
        .getCapabilities = ChunkCapabilities,
    };
}

static uint64_t ChunkLiveBytes(const ChunkTestDevice *device)
{
    uint64_t bytes = 0u;
    for (uint32_t i = 0u; i < device->created; ++i)
        if (device->buffers[i].live)
            bytes += device->buffers[i].sizeBytes;
    return bytes;
}

static ChunkMesherScratch *CreateChunkScratch(void)
{
    return (ChunkMesherScratch *)&mesherScratchToken;
}

static uint8_t ChunkBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    return 2u;
}

static bool BuildTestChunk(const ChunkMesherWorldSource *source, ChunkMesherScratch *scratch,
                           int64_t x, int64_t y, int64_t z, ChunkQuad **outQuads,
                           uint32_t *outQuadCount)
{
    Expect(source != NULL && source->context != NULL && source->fillRegion != NULL &&
               scratch == (ChunkMesherScratch *)&mesherScratchToken,
           "chunk preparation uses the supplied world adapter and retained mesher scratch");
    ++chunkBuildCalls;
    *outQuads = NULL;
    *outQuadCount = 0u;
    if (y != 0 || z != 0)
        return true;
    if (failChunkMesh)
        return false;
    BlockType block = 0u;
    Expect(source->fillRegion(source->context, x * 64, 0, 0, 1, 1, 1, &block) ==
                   WORLD_REGION_MIXED &&
               block == 2u,
           "the chunk adapter forwards block coordinates to its world provider");
    const uint32_t count = 9u + chunkExtraQuads;
    ChunkQuad *quads = (ChunkQuad *)PlatformAllocate(count * (uint32_t)sizeof(*quads), false);
    Expect(quads != NULL, "small deterministic mesher fixtures allocate");
    for (uint32_t q = 0u; q < 6u + chunkExtraQuads; ++q)
        quads[q] = PackChunkQuad(2u + q, 3u, 4u, q % 6u, 1u, 2u, 3u, 4u);
    quads[6u + chunkExtraQuads] = PackChunkQuad(2u, 3u, 4u, 0u, 2u, 2u, 3u, 4u);
    quads[7u + chunkExtraQuads] = PackChunkQuad(2u, 3u, 4u, 5u, 2u, 2u, 3u, 4u);
    quads[8u + chunkExtraQuads] = PackChunkQuad(2u, 3u, 4u, 3u, 3u, 2u, 3u, 4u);
    *outQuads = quads;
    *outQuadCount = count;
    return true;
}

static const LaiueMesherServiceV1 chunkMesher = {
    .structSize = sizeof(chunkMesher),
    .abiVersion = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
    .scratchCreate = CreateChunkScratch,
    .scratchDestroy = DestroyMesherScratch,
    .buildChunkMesh = BuildTestChunk,
};

static void CheckChunkFallback(uint32_t prefixBytes, bool callback, bool native, bool saveReference)
{
    const int64_t center[3] = {0, 0, 0};
    InitializeChunkDevice(&chunkDevice, 43u, native);
    chunkDevice.api.structSize = prefixBytes;
    if (!callback)
        chunkDevice.api.getCapabilities = NULL;
    /* The compatibility fixture physically ends at its advertised prefix. */
    LaiueGraphicsDeviceV2 *api = (LaiueGraphicsDeviceV2 *)PlatformAllocate(prefixBytes, false);
    Expect(api != NULL, "an actual compatibility table prefix allocates");
    memcpy(api, &chunkDevice.api, prefixBytes);
    Expect(WalkVisualsCreateChunkSet(&chunkMesher, &chunkSet) &&
               WalkVisualsUpdateChunkSet(api, &chunkMesher, &chunkSet, ChunkBlock, NULL, center),
           "old or unavailable native-index capabilities retain complete chunk geometry");
    Expect(chunkSet.indexBuffer == 0u && chunkDevice.indexUploads == 0u &&
               ChunkLiveBytes(&chunkDevice) == 3u * 9u * 6u * sizeof(LaiueGraphicsVertexV2),
           "fallback owns exactly six vertices per quad and no unused topology resource");
    Expect(chunkDevice.capabilityQueries ==
               (callback && prefixBytes >= LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE ? 1u : 0u),
           "capability probing respects both physical prefix length and missing callbacks");
    const uint32_t count =
        WalkVisualsBuildChunkDraws(&chunkSet, center, chunkTextures, 104u, chunkDraws);
    Expect(count == 9u, "three populated chunks produce one draw per material");
    for (uint32_t i = 0u; i < count; ++i)
    {
        const uint32_t material = (uint32_t)(chunkDraws[i].texture - chunkTextures[0]);
        ChunkTestBuffer *buffer = ChunkBuffer(&chunkDevice, chunkDraws[i].vertexBuffer);
        Expect(chunkDraws[i].indexBuffer == 0u && chunkDraws[i].sampler == 104u &&
                   chunkDraws[i].indexCount * sizeof(LaiueGraphicsVertexV2) == buffer->sizeBytes,
               "fallback draws retain exact triangle counts and material bindings");
        if (saveReference)
        {
            legacyChunkCounts[material] = chunkDraws[i].indexCount;
            memcpy(legacyChunkVertices[material], buffer->data.vertices, buffer->sizeBytes);
        }
    }
    WalkVisualsDestroyChunkSet(api, &chunkMesher, &chunkSet);
    Expect(ChunkLiveBytes(&chunkDevice) == 0u && chunkDevice.created == chunkDevice.destroyed,
           "fallback teardown releases every prepared VB once");
    PlatformFree(api);
}

static void CheckNativeChunkDraws(const int64_t center[3], bool compareLegacy)
{
    const uint32_t count =
        WalkVisualsBuildChunkDraws(&chunkSet, center, chunkTextures, 104u, chunkDraws);
    Expect(count == 9u, "native indexing preserves all material and chunk draws");
    ChunkTestBuffer *indices = ChunkBuffer(&chunkDevice, chunkSet.indexBuffer);
    Expect(indices->usage == LAIUE_GRAPHICS_BUFFER_USAGE_INDEX &&
               indices->sizeBytes == chunkSet.indexQuadCapacity * 6u * sizeof(uint32_t),
           "one shared topology resource is sized to the largest material chunk");
    static const uint32_t corners[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (uint32_t i = 0u; i < chunkSet.indexQuadCapacity * 6u; ++i)
        Expect(indices->data.indices[i] == i / 6u * 4u + corners[i % 6u],
               "shared uint32 index bytes preserve both triangles and outward quad winding");
    for (uint32_t d = 0u; d < count; ++d)
    {
        const LaiueGraphicsDrawItemV2 *draw = &chunkDraws[d];
        ChunkTestBuffer *vertices = ChunkBuffer(&chunkDevice, draw->vertexBuffer);
        const uint32_t material = (uint32_t)(draw->texture - chunkTextures[0]);
        Expect(draw->indexBuffer == chunkSet.indexBuffer && draw->firstIndex == 0u &&
                   draw->vertexOffset == 0 && draw->sampler == 104u &&
                   vertices->sizeBytes ==
                       draw->indexCount / 6u * 4u * sizeof(LaiueGraphicsVertexV2),
               "all independent four-corner VBs reuse the same topology and material bindings");
        if (compareLegacy)
        {
            Expect(draw->indexCount == legacyChunkCounts[material],
                   "native and fallback paths report the same number of rendered triangles");
            for (uint32_t i = 0u; i < draw->indexCount; ++i)
                Expect(memcmp(&vertices->data.vertices[indices->data.indices[i]],
                              &legacyChunkVertices[material][i],
                              sizeof(LaiueGraphicsVertexV2)) == 0,
                       "native corners reconstruct fallback positions, repeated UVs, colors and "
                       "winding");
        }
    }
}

static void CheckChunkVisuals(void)
{
    chunkExtraQuads = 0u;
    CheckChunkFallback(sizeof(LaiueGraphicsDeviceV2), true, false, true);
    CheckChunkFallback(sizeof(LaiueGraphicsDeviceV2), false, true, false);
    CheckChunkFallback((uint32_t)offsetof(LaiueGraphicsDeviceV2, getCapabilities), true, true,
                       false);
    CheckChunkFallback(LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE - 1u, true, true, false);
    const int64_t center[3] = {0, 0, 0};
    const int64_t movedCenter[3] = {128, 0, 0};
    InitializeChunkDevice(&chunkDevice, 44u, true);
    InitializeChunkDevice(&foreignChunkDevice, 45u, true);
    const uint32_t scratchReleases = mesherScratchReleases;
    Expect(WalkVisualsCreateChunkSet(&chunkMesher, &chunkSet) &&
               WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock,
                                         NULL, center),
           "native chunk geometry prepares before beginFrame");
    Expect(chunkSet.indexQuadCapacity == 6u && chunkDevice.indexUploads == 1u &&
               ChunkLiveBytes(&chunkDevice) ==
                   3u * 9u * 4u * sizeof(LaiueGraphicsVertexV2) + 6u * 6u * sizeof(uint32_t),
           "native storage is four vertices per quad plus one largest-material topology IB");
    CheckNativeChunkDraws(center, true);
    const uint32_t created = chunkDevice.created, uploaded = chunkDevice.uploaded;
    const uint32_t builds = chunkBuildCalls;
    Expect(WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock, NULL,
                                     center) &&
               chunkDevice.created == created && chunkDevice.uploaded == uploaded &&
               chunkBuildCalls == builds,
           "unchanged chunks reuse their VBs, topology and mesher result without uploads");
    chunkSnapshot = chunkSet;
    Expect(!WalkVisualsUpdateChunkSet(&foreignChunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock,
                                      NULL, center),
           "a foreign device cannot adopt another device's chunk handles");
    WalkVisualsInvalidateBlock(&chunkSet, &foreignChunkDevice.api, 0, 0, 0);
    WalkVisualsInvalidateChunkSet(&foreignChunkDevice.api, &chunkSet);
    WalkVisualsDestroyChunkSet(&foreignChunkDevice.api, &chunkMesher, &chunkSet);
    Expect(memcmp(&chunkSet, &chunkSnapshot, sizeof(chunkSet)) == 0 &&
               foreignChunkDevice.created == 0u && foreignChunkDevice.destroyed == 0u &&
               mesherScratchReleases == scratchReleases,
           "foreign-device cleanup leaves the owner's geometry, topology and scratch intact");
    chunkExtraQuads = 2u;
    const uint64_t liveBytes = ChunkLiveBytes(&chunkDevice);
    const uint32_t failures[3] = {LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
                                  LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
                                  LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX};
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt)
    {
        chunkDevice.failCreateUsage = attempt == 0u ? failures[attempt] : 0u;
        chunkDevice.failUploadUsage = attempt != 0u ? failures[attempt] : 0u;
        const uint32_t beforeCreates = chunkDevice.created, beforeDestroys = chunkDevice.destroyed;
        Expect(!WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock,
                                          NULL, movedCenter),
               "failed VB or shared-IB growth propagates");
        Expect(memcmp(&chunkSet, &chunkSnapshot, sizeof(chunkSet)) == 0 &&
                   ChunkLiveBytes(&chunkDevice) == liveBytes &&
                   chunkDevice.created - beforeCreates == chunkDevice.destroyed - beforeDestroys,
               "failed preparation releases only candidates and preserves every prior draw and "
               "center");
        CheckNativeChunkDraws(center, true);
    }
    chunkDevice.failCreateUsage = chunkDevice.failUploadUsage = 0u;
    failChunkMesh = true;
    Expect(!WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock, NULL,
                                      movedCenter) &&
               memcmp(&chunkSet, &chunkSnapshot, sizeof(chunkSet)) == 0,
           "mesher failure also leaves the complete previous chunk set published");
    failChunkMesh = false;
    const LaiueGraphicsHandle oldIndices = chunkSet.indexBuffer;
    Expect(WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock, NULL,
                                     movedCenter) &&
               chunkSet.indexQuadCapacity == 8u && chunkSet.indexBuffer != oldIndices &&
               chunkDevice.indexUploads == 2u &&
               !chunkDevice.buffers[(uint32_t)oldIndices - 1u].live,
           "successful growth publishes one larger topology and releases the previous IB once");
    CheckNativeChunkDraws(movedCenter, false);
    const LaiueGraphicsHandle retainedIndices = chunkSet.indexBuffer;
    WalkVisualsInvalidateChunkSet(&chunkDevice.api, &chunkSet);
    WalkVisualsInvalidateChunkSet(&chunkDevice.api, &chunkSet);
    Expect(chunkSet.indexBuffer == retainedIndices && chunkSet.indexQuadCapacity == 8u &&
               chunkSet.scratch == (ChunkMesherScratch *)&mesherScratchToken &&
               ChunkLiveBytes(&chunkDevice) == 8u * 6u * sizeof(uint32_t) &&
               mesherScratchReleases == scratchReleases,
           "rebase invalidation releases VBs while retaining topology and mesher scratch exactly "
           "once");
    chunkExtraQuads = 0u;
    Expect(WalkVisualsUpdateChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet, ChunkBlock, NULL,
                                     center) &&
               chunkSet.indexBuffer == retainedIndices && chunkDevice.indexUploads == 2u,
           "post-rebase rebuild reuses its already sufficient topology allocation");
    CheckNativeChunkDraws(center, true);
    WalkVisualsDestroyChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet);
    WalkVisualsDestroyChunkSet(&chunkDevice.api, &chunkMesher, &chunkSet);
    Expect(ChunkLiveBytes(&chunkDevice) == 0u && chunkDevice.created == chunkDevice.destroyed &&
               mesherScratchReleases == scratchReleases + 1u && chunkSet.indexBuffer == 0u &&
               chunkSet.scratch == NULL,
           "final teardown releases the retained index buffer, every VB and scratch exactly once");
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
    CheckChunkVisuals();
    LAIUE_TEST_SUCCESS();
}
