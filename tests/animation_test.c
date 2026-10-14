#include "animation/animation_service.h"
#include "media/animation.h"
#include "test_runtime.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static AnimationChannel g_channels[5];
static AnimationKey g_keys[10];
static AnimationJoint g_joints[2];
static uint8_t g_bytes[4096];
static LaiueAnimationTransformV1 g_locals[2];
static LaiueAnimationMatrixV1 g_globals[2];
static LaiueAnimationMatrixV1 g_palette[2];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation sampling: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Close(float a, float b)
{
    const float difference = a - b;
    return difference >= -0.00001f && difference <= 0.00001f;
}

static LaiueAnimationV1 *MakeClip(const LaiueAnimationServiceV1 *service)
{
    for (uint32_t joint = 0u; joint < 2u; ++joint)
    {
        g_joints[joint].parent = joint == 0u ? ANIMATION_NO_PARENT : 0u;
        g_joints[joint].bindLocal.rotation[3] = 1.0f;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            g_joints[joint].bindLocal.scale[axis] = 1.0f;
            g_joints[joint].inverseBind[axis * 4u + axis] = 1.0f;
        }
    }
    g_joints[1].bindLocal.translation[1] = 2.0f;
    g_joints[1].inverseBind[7] = -2.0f;
    for (uint32_t channel = 0u; channel < 5u; ++channel)
    {
        g_channels[channel].kind = channel + 1u;
        g_channels[channel].target = channel == 1u ? 1u : 0u;
        g_channels[channel].interpolation = channel == 4u ? ANIMATION_STEP : ANIMATION_LINEAR;
        g_channels[channel].firstKey = channel * 2u;
        g_channels[channel].keyCount = 2u;
        g_keys[channel * 2u + 1u].time = 2.0f;
    }
    g_keys[1].value[0] = 10.0f;
    g_keys[2].value[3] = 1.0f;
    g_keys[3].value[2] = 1.0f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        g_keys[4].value[axis] = 1.0f;
        g_keys[5].value[axis] = 3.0f;
    }
    g_keys[6].value[0] = 2.0f;
    g_keys[7].value[0] = 6.0f;
    g_keys[8].frame = 3u;
    g_keys[9].frame = UINT32_MAX;
    const AnimationData data = {g_channels, 5u, g_keys, 10u, g_joints, 2u, 2.0f};
    uint32_t written = 0u;
    Expect(AnimationEncode(&data, g_bytes, sizeof(g_bytes), &written) == ANIMATION_OK,
           "fixture encode");
    uint32_t status = 0u;
    LaiueAnimationV1 *clip = service->loadMemory(g_bytes, written, &status);
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_OK, "clip load");
    return clip;
}

LAIUE_TEST_ENTRY(AnimationTestEntryPoint)
{
    const LaiueAnimationServiceV1 *service = LaiueAnimationGetStaticServiceV1();
    Expect(service->abiVersion == 1u && service->structSize == sizeof(*service), "versioned table");
    LaiueAnimationV1 *clip = MakeClip(service);
    LaiueAnimationViewV1 view = {.structSize = sizeof(view)};
    Expect(service->getView(clip, &view) != 0u && view.duration == 2.0f && view.jointCount == 2u,
           "view");
    LaiueAnimationChannelV1 channel = {0};
    Expect(service->getChannel(clip, 3u, &channel) != 0u &&
               channel.kind == LAIUE_ANIMATION_SCALAR && channel.target == 0u,
           "application target metadata");
    LaiueAnimationValueV1 value = {0};
    Expect(service->sampleChannel(clip, 3u, 1.0f, 0u, &value) != 0u && value.value[0] == 4.0f,
           "linear scalar");
    Expect(service->sampleChannel(clip, 3u, -1.0f, 0u, &value) != 0u && value.value[0] == 2.0f,
           "negative clamp");
    Expect(service->sampleChannel(clip, 3u, 9.0f, 0u, &value) != 0u && value.value[0] == 6.0f,
           "end clamp");
    Expect(service->sampleChannel(clip, 3u, -0.5f, 1u, &value) != 0u && value.value[0] == 5.0f,
           "negative loop wraps backwards");
    Expect(service->sampleChannel(clip, 3u, 2.0f, 1u, &value) != 0u && value.value[0] == 2.0f,
           "loop excludes endpoint");
    Expect(service->sampleChannel(clip, 4u, 1.999f, 0u, &value) != 0u && value.frame == 3u,
           "flipbook holds old integer frame");
    Expect(service->sampleChannel(clip, 4u, 2.0f, 0u, &value) != 0u && value.frame == UINT32_MAX,
           "flipbook exact timestamp selects next integer frame");
    Expect(service->sampleChannel(clip, 1u, 1.0f, 0u, &value) != 0u &&
               Close(value.value[2], 0.70710678f) && Close(value.value[3], 0.70710678f),
           "quaternion normalized midpoint");
    LaiueAnimationValueV1 again = {0};
    Expect(service->sampleChannel(clip, 1u, 1.0f, 0u, &again) != 0u &&
               memcmp(&value, &again, sizeof(value)) == 0,
           "repeat sampling is deterministic");
    Expect(service->sampleChannel(clip, 3u, FLT_MAX, 1u, &value) != 0u && value.value[0] >= 2.0f &&
               value.value[0] <= 6.0f,
           "huge finite loop time avoids integer overflow");
    Expect(service->sampleChannel(clip, 5u, 0.0f, 0u, &value) == 0u, "channel bounds");
    const uint32_t nanBits = UINT32_C(0x7fc00000);
    float notFinite;
    memcpy(&notFinite, &nanBits, sizeof(notFinite));
    Expect(service->sampleChannel(clip, 3u, notFinite, 1u, &value) == 0u,
           "nonfinite sampling time rejected");
    Expect(service->samplePose(clip, 1.0f, 0u, g_locals, g_globals, g_palette, 1u) == 0u,
           "pose capacity");
    Expect(service->samplePose(clip, 1.0f, 0u, g_locals, (LaiueAnimationMatrixV1 *)(void *)g_locals,
                               g_palette, 2u) == 0u,
           "pose buffers with different strides cannot alias");
    Expect(service->samplePose(clip, 1.0f, 0u, g_locals, g_globals, g_globals + 1u, 2u) == 0u,
           "partial pose matrix overlap rejected");
    Expect(service->samplePose(clip, 1.0f, 0u, g_locals, g_globals, g_palette, 2u) != 0u &&
               g_globals[0].m[3] == 5.0f && g_globals[1].m[7] == 4.0f &&
               Close(g_globals[1].m[1], -2.0f) && Close(g_palette[1].m[3], 9.0f),
           "parent scale, child rotation, inverse bind order");
    LaiueAnimationMatrixV1 saved[2];
    memcpy(saved, g_palette, sizeof(saved));
    Expect(service->samplePose(clip, 1.0f, 0u, g_locals, g_globals, g_palette, 2u) != 0u &&
               memcmp(saved, g_palette, sizeof(saved)) == 0,
           "repeat pose is deterministic");
    service->release(clip);

    g_keys[3].value[2] = 0.0f;
    g_keys[3].value[3] = -1.0f;
    const AnimationData antipodal = {g_channels, 5u, g_keys, 10u, g_joints, 2u, 2.0f};
    uint32_t written = 0u;
    Expect(AnimationEncode(&antipodal, g_bytes, sizeof(g_bytes), &written) == ANIMATION_OK,
           "antipodal fixture");
    clip = service->loadMemory(g_bytes, written, NULL);
    Expect(clip != NULL && service->sampleChannel(clip, 1u, 1.0f, 0u, &value) != 0u &&
               value.value[2] == 0.0f && value.value[3] == 1.0f,
           "antipodal quaternions use shortest arc without zero division");
    service->release(clip);

    const uint32_t tinyBits = 1u;
    float tinyDuration;
    memcpy(&tinyDuration, &tinyBits, sizeof(tinyDuration));
    for (uint32_t key = 1u; key < 10u; key += 2u)
        g_keys[key].time = tinyDuration;
    const AnimationData tiny = {g_channels, 5u, g_keys, 10u, g_joints, 2u, tinyDuration};
    Expect(AnimationEncode(&tiny, g_bytes, sizeof(g_bytes), &written) == ANIMATION_OK,
           "smallest positive duration encodes");
    clip = service->loadMemory(g_bytes, written, NULL);
    Expect(clip != NULL && service->sampleChannel(clip, 3u, -FLT_MAX, 1u, &value) != 0u &&
               value.value[0] == 2.0f,
           "full float exponent range wraps without overflow or hanging");
    service->release(clip);
    AnimationChannel staticChannel = {ANIMATION_SCALAR, 1u, ANIMATION_STEP, 0u, 1u};
    AnimationKey staticKey = {0.0f, {9.0f, 0.0f, 0.0f, 0.0f}, 0u};
    const AnimationData constant = {&staticChannel, 1u, &staticKey, 1u, NULL, 0u, 0.0f};
    Expect(AnimationEncode(&constant, g_bytes, sizeof(g_bytes), &written) == ANIMATION_OK,
           "zero-duration static channel encodes");
    clip = service->loadMemory(g_bytes, written, NULL);
    Expect(clip != NULL && service->sampleChannel(clip, 0u, FLT_MAX, 1u, &value) != 0u &&
               value.value[0] == 9.0f,
           "zero-duration channel samples at zero");
    service->release(clip);

    LaiueAnimationMatrixV1 matrices[2] = {
        {{2.0f, 0.0f, 0.0f, 10.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}}};
    LaiueAnimationSkinVertexV1 vertices[2] = {
        {{1.0f, 2.0f, 3.0f}, {1.0f, 1.0f, 0.0f}, {0u, 0u, 0u, 0u}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0u, 1u, 0u, 0u}, {0.5f, 0.5f, 0.0f, 0.0f}}};
    LaiueAnimationSkinnedVertexV1 outputs[2] = {0};
    Expect(service->skinVertices(vertices, 2u, matrices, 2u,
                                 (LaiueAnimationSkinnedVertexV1 *)(void *)(vertices + 1u),
                                 2u) == 0u &&
               vertices[1].normal[2] == 1.0f,
           "skinning partial input/output overlap rejected before either pass");
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) != 0u &&
               outputs[0].position[0] == 12.0f && outputs[0].position[1] == 2.0f &&
               Close(outputs[0].normal[0], 0.4472136f) && Close(outputs[0].normal[1], 0.8944272f) &&
               outputs[1].position[0] == 5.0f && outputs[1].normal[2] == 1.0f,
           "weighted skinning and inverse transpose normals");
    LaiueAnimationSkinnedVertexV1 previous[2];
    memcpy(previous, outputs, sizeof(previous));
    vertices[1].joints[3] = 2u;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) == 0u &&
               memcmp(previous, outputs, sizeof(previous)) == 0,
           "late invalid joint, even zero weight, never partially publishes output");
    vertices[1].joints[3] = 0u;
    vertices[1].weights[0] = -0.5f;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) == 0u,
           "negative weight rejected");
    vertices[1].weights[0] = 0.5f;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 1u) == 0u,
           "skinning output bounds");
    matrices[0].m[0] = 0.0f;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) == 0u,
           "singular normal transform rejected");
    matrices[0].m[0] = notFinite;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) == 0u,
           "nonfinite palette rejected");
    matrices[0].m[0] = FLT_MAX;
    vertices[0].position[0] = FLT_MAX;
    Expect(service->skinVertices(vertices, 2u, matrices, 2u, outputs, 2u) == 0u &&
               memcmp(previous, outputs, sizeof(previous)) == 0,
           "finite input with overflowing output never partially publishes vertices");
    Expect(service->skinVertices(NULL, 0u, NULL, 0u, NULL, 0u) != 0u, "zero vertices is no-op");
    LaiueTestRuntimeWrite("Animation sampling checks passed\n");
    LAIUE_TEST_SUCCESS();
}
