/* The shipped assets are the fixture. Expected arm motion is derived from
 * segment lengths and independent scalar trigonometry, not sampled matrices. */
#include "animation/animation_service.h"
#include "math/scalar.h"
#include "model/model_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum
{
    JOINTS = 3u,
    MAX_VERTICES = 65536u
};

static wchar_t g_directory[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t g_path[LAIUE_PLATFORM_PATH_CAPACITY];
static LaiueAnimationTransformV1 g_locals[JOINTS];
static LaiueAnimationMatrixV1 g_globals[JOINTS];
static LaiueAnimationMatrixV1 g_palette[JOINTS];
static double g_scratch[(73u * JOINTS + 7u) / 8u];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation model check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(double actual, double expected)
{
    const double difference = actual - expected;
    return difference >= -3e-5 && difference <= 3e-5;
}

static bool Path(const wchar_t *relative)
{
    uint32_t count = 0u;
    while (g_directory[count] != L'\0')
    {
        if (count + 2u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return false;
        g_path[count] = g_directory[count];
        ++count;
    }
    if (count != 0u && g_path[count - 1u] != L'/' && g_path[count - 1u] != L'\\')
        g_path[count++] = L'/';
    for (uint32_t index = 0u; relative[index] != L'\0'; ++index)
    {
        if (count + 1u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return false;
        g_path[count++] = relative[index];
    }
    g_path[count] = L'\0';
    return true;
}

static uint64_t Hash(const void *data, size_t bytes)
{
    const uint8_t *source = data;
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t index = 0u; index < bytes; ++index)
        value = (value ^ source[index]) * UINT64_C(1099511628211);
    return value;
}

static bool HasScratch(const LaiueAnimationServiceV1 *service)
{
    return service->structSize >= LAIUE_ANIMATION_SERVICE_V1_SCRATCH_SIZE &&
           service->skinningScratchBytes != NULL && service->skinVerticesWithScratch != NULL;
}

static void Pose(const LaiueAnimationServiceV1 *service, const LaiueAnimationV1 *clip,
                 const LaiueAnimationSkinVertexV1 *source, uint32_t count, float time,
                 LaiueAnimationSkinnedVertexV1 *out)
{
    Expect(service->samplePose(clip, time, 1u, g_locals, g_globals, g_palette, JOINTS) != 0u,
           "real clip samples its complete bound skeleton");
    Expect(service->skinVertices(source, count, g_palette, JOINTS, out, count) != 0u,
           "real model weights deform through the public animation service");
    for (uint32_t vertex = 0u; vertex < count; ++vertex)
    {
        const float *normal = out[vertex].normal;
        const double squared = (double)normal[0] * normal[0] + (double)normal[1] * normal[1] +
                               (double)normal[2] * normal[2];
        Expect(Near(squared, 1.0), "deformed surface normals stay finite and unit length");
    }
}

static void CachedMatches(const LaiueAnimationServiceV1 *service,
                          const LaiueAnimationSkinVertexV1 *source, uint32_t count,
                          const LaiueAnimationSkinnedVertexV1 *expected,
                          LaiueAnimationSkinnedVertexV1 *temporary)
{
    if (!HasScratch(service))
        return;
    const uint32_t bytes = service->skinningScratchBytes(JOINTS);
    Expect(bytes != 0u && bytes <= sizeof(g_scratch), "three-joint workspace is bounded");
    Expect(service->skinVerticesWithScratch(source, count, g_palette, JOINTS, temporary, count,
                                            g_scratch, bytes) != 0u &&
               memcmp(temporary, expected, (size_t)count * sizeof(*expected)) == 0,
           "optional cached deformation matches the real asset's legacy output exactly");
}

LAIUE_TEST_ENTRY(AnimationModelTestEntryPoint)
{
    const LaiueModelServiceV1 *models = LaiueModelGetStaticServiceV1();
    const LaiueAnimationServiceV1 *animation = LaiueAnimationGetStaticServiceV1();
    Expect(models != NULL && models->abiVersion == LAIUE_MODEL_SERVICE_ABI_VERSION_1 &&
               models->structSize >= LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE &&
               LAIUE_MODEL_SERVICE_HAS_SKIN_V1(models),
           "model provider exposes its optional binding view safely");
    Expect(animation != NULL && animation->abiVersion == LAIUE_ANIMATION_SERVICE_ABI_VERSION_1 &&
               animation->structSize >= LAIUE_ANIMATION_SERVICE_V1_BASE_SIZE,
           "animation provider exposes the original sampling/deformation prefix");
    Expect(PlatformExecutableDirectory(g_directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "test executable directory resolves");
    Expect(Path(L"assets_animation/models/Demo.lop/robot.lo"), "real model path fits");
    uint32_t status = LAIUE_MODEL_LOAD_NOT_ATTEMPTED;
    LaiueModelV1 *model = models->loadFile(g_path, &status);
    Expect(model != NULL && status == LAIUE_MODEL_LOAD_OK, "real robot model loads explicitly");
    LaiueModelViewV1 mesh = {.structSize = sizeof(mesh)};
    LaiueModelSkinViewV1 skin = {.structSize = sizeof(skin)};
    Expect(models->getView(model, &mesh) != 0u &&
               (mesh.flags & LAIUE_MODEL_FLAG_PLACEHOLDER) == 0u && mesh.vertexCount != 0u &&
               mesh.vertexCount <= MAX_VERTICES && models->getSkinView(model, &skin) != 0u &&
               skin.jointCount == JOINTS && skin.vertexCount == mesh.vertexCount &&
               skin.vertices != NULL,
           "real skin binding corresponds one-to-one with the static geometry");
    Expect(Path(L"assets_animation/animations/Demo.lkp/wave.lk"), "real clip path fits");
    status = LAIUE_ANIMATION_LOAD_NOT_ATTEMPTED;
    LaiueAnimationV1 *clip = animation->loadFile(g_path, &status);
    LaiueAnimationViewV1 clipView = {.structSize = sizeof(clipView)};
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_OK &&
               animation->getView(clip, &clipView) != 0u &&
               (clipView.flags & LAIUE_ANIMATION_FLAG_PLACEHOLDER) == 0u &&
               clipView.jointCount == skin.jointCount && clipView.duration == 2.0f,
           "real two-second wave clip has the matching three-joint rig");
    const uint32_t count = mesh.vertexCount;
    const size_t outputBytes = (size_t)count * sizeof(LaiueAnimationSkinnedVertexV1);
    const size_t inputBytes = (size_t)count * sizeof(LaiueAnimationSkinVertexV1);
    uint8_t *allocation = PlatformAllocate(inputBytes + outputBytes * 3u, false);
    Expect(allocation != NULL, "bounded caller-owned deformation arrays allocate");
    LaiueAnimationSkinVertexV1 *source = (LaiueAnimationSkinVertexV1 *)allocation;
    LaiueAnimationSkinnedVertexV1 *bind =
        (LaiueAnimationSkinnedVertexV1 *)(void *)(allocation + inputBytes);
    LaiueAnimationSkinnedVertexV1 *wave =
        (LaiueAnimationSkinnedVertexV1 *)(void *)(allocation + inputBytes + outputBytes);
    LaiueAnimationSkinnedVertexV1 *temporary =
        (LaiueAnimationSkinnedVertexV1 *)(void *)(allocation + inputBytes + outputBytes * 2u);
    const uint64_t meshHash = Hash(mesh.vertices, (size_t)count * sizeof(*mesh.vertices));
    const uint64_t indexHash = Hash(mesh.indices, (size_t)mesh.indexCount * sizeof(*mesh.indices));
    const uint64_t skinHash = Hash(skin.vertices, (size_t)count * sizeof(*skin.vertices));
    for (uint32_t vertex = 0u; vertex < count; ++vertex)
    {
        memcpy(source[vertex].position, mesh.vertices[vertex].position,
               sizeof(source[vertex].position));
        memcpy(source[vertex].normal, mesh.vertices[vertex].normal, sizeof(source[vertex].normal));
        memcpy(source[vertex].joints, skin.vertices[vertex].joints, sizeof(source[vertex].joints));
        memcpy(source[vertex].weights, skin.vertices[vertex].weights,
               sizeof(source[vertex].weights));
    }
    const uint64_t sourceHash = Hash(source, inputBytes);
    Pose(animation, clip, source, count, 0.0f, bind);
    CachedMatches(animation, source, count, bind, temporary);
    for (uint32_t vertex = 0u; vertex < count; ++vertex)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            Expect(Near(bind[vertex].position[axis], mesh.vertices[vertex].position[axis]),
                   "inverse binds reproduce the authored geometry at time zero");
    Pose(animation, clip, source, count, 1.0f, wave);
    CachedMatches(animation, source, count, wave, temporary);
    uint32_t bodyVertices = 0u;
    uint32_t tipVertices = 0u;
    double tip[3] = {0.0, 0.0, 0.0};
    for (uint32_t vertex = 0u; vertex < count; ++vertex)
    {
        if (source[vertex].joints[0] == 0u && source[vertex].weights[0] == 1.0f)
        {
            ++bodyVertices;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                Expect(wave[vertex].position[axis] == mesh.vertices[vertex].position[axis],
                       "body vertices assigned to the stationary root never move");
        }
        if (Near(mesh.vertices[vertex].position[0], -1.05))
        {
            ++tipVertices;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                tip[axis] += wave[vertex].position[axis];
        }
    }
    Expect(bodyVertices != 0u && tipVertices == 8u, "body and complete sleeve-tip groups exist");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        tip[axis] /= tipVertices;
    const double expectedX = -0.25 - 0.4 * ScalarCos(1.2f) - 0.4 * ScalarCos(-0.2f);
    const double expectedZ = 1.3 + 0.4 * ScalarSin(1.2f) + 0.4 * ScalarSin(-0.2f);
    Expect(Near(tip[0], expectedX) && Near(tip[1], 0.0) && Near(tip[2], expectedZ),
           "sleeve tip follows the independently computed shoulder and elbow chain");
    Expect(memcmp(bind, wave, outputBytes) != 0,
           "the actual mesh deforms between zero and one second");
    const float repeatedTimes[] = {1.0f, 3.0f, -1.0f};
    for (uint32_t sample = 0u; sample < 3u; ++sample)
    {
        Pose(animation, clip, source, count, repeatedTimes[sample], temporary);
        Expect(memcmp(wave, temporary, outputBytes) == 0,
               "repeat and forward/backward looping reproduce the exact same mesh");
    }
    memset(temporary, 0xa5, outputBytes);
    const uint64_t unchangedOutput = Hash(temporary, outputBytes);
    Expect(animation->skinVertices(source, count, g_palette, JOINTS - 1u, temporary, count) == 0u &&
               Hash(temporary, outputBytes) == unchangedOutput,
           "a too-short palette rejects the bound mesh without publishing any vertex");
    if (HasScratch(animation))
    {
        const uint32_t bytes = animation->skinningScratchBytes(JOINTS - 1u);
        Expect(animation->skinVerticesWithScratch(source, count, g_palette, JOINTS - 1u, temporary,
                                                  count, g_scratch, bytes) == 0u &&
                   Hash(temporary, outputBytes) == unchangedOutput,
               "cached short-palette failure has the same whole-output transaction");
    }
    Expect(Hash(mesh.vertices, (size_t)count * sizeof(*mesh.vertices)) == meshHash &&
               Hash(mesh.indices, (size_t)mesh.indexCount * sizeof(*mesh.indices)) == indexHash &&
               Hash(skin.vertices, (size_t)count * sizeof(*skin.vertices)) == skinHash &&
               Hash(source, inputBytes) == sourceHash,
           "deformation leaves borrowed geometry, UV/color, indices, weights and inputs immutable");
    PlatformFree(allocation);
    animation->release(clip);
    LaiueModelSkinViewV1 retained = {.structSize = sizeof(retained)};
    Expect(models->getSkinView(model, &retained) != 0u && retained.vertices == skin.vertices &&
               Hash(retained.vertices, (size_t)count * sizeof(*retained.vertices)) == skinHash,
           "borrowed binding stays valid until model release independently of clip lifetime");
    models->release(model);
    LaiueTestRuntimeWrite("Animation model binding checks passed\n");
    LAIUE_TEST_SUCCESS();
}
