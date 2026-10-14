#include "media/animation.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint8_t g_encoded[4096];
static uint8_t g_bad[4096];
static AnimationChannel g_channels[5];
static AnimationKey g_keys[10];
static AnimationJoint g_joints[2];
static AnimationChannel g_decodedChannels[5];
static AnimationKey g_decodedKeys[10];
static AnimationJoint g_decodedJoints[2];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation codec: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void PutU32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8u));
}

static void Corrupt(uint32_t size, uint32_t offset, uint32_t value, AnimationStatus expected)
{
    memcpy(g_bad, g_encoded, size);
    PutU32(g_bad + offset, value);
    AnimationInfo info = {0};
    Expect(AnimationInspect(g_bad, size, &info) == expected, "malformed record is rejected");
}

LAIUE_TEST_ENTRY(AnimationCodecTestEntryPoint)
{
    for (uint32_t joint = 0u; joint < 2u; ++joint)
    {
        g_joints[joint].parent = joint == 0u ? ANIMATION_NO_PARENT : 0u;
        g_joints[joint].bindLocal.rotation[3] = 1.0f;
        g_joints[joint].bindLocal.scale[0] = 1.0f;
        g_joints[joint].bindLocal.scale[1] = 1.0f;
        g_joints[joint].bindLocal.scale[2] = 1.0f;
        g_joints[joint].inverseBind[0] = 1.0f;
        g_joints[joint].inverseBind[5] = 1.0f;
        g_joints[joint].inverseBind[10] = 1.0f;
    }
    for (uint32_t channel = 0u; channel < 5u; ++channel)
    {
        g_channels[channel].kind = channel + 1u;
        g_channels[channel].target = 0u;
        g_channels[channel].interpolation = channel == 4u ? ANIMATION_STEP : ANIMATION_LINEAR;
        g_channels[channel].firstKey = channel * 2u;
        g_channels[channel].keyCount = 2u;
        g_keys[channel * 2u + 1u].time = 2.0f;
    }
    g_keys[2].value[3] = g_keys[3].value[3] = 1.0f;
    for (uint32_t component = 0u; component < 3u; ++component)
        g_keys[4].value[component] = g_keys[5].value[component] = 1.0f;
    g_keys[7].value[0] = 20.0f;
    g_keys[9].frame = UINT32_MAX;
    AnimationData source = {g_channels, 5u, g_keys, 10u, g_joints, 2u, 2.0f};
    uint32_t size = 0u;
    Expect(AnimationEncodedBytes(&source, &size) == ANIMATION_OK && size == 572u,
           "independent record size calculation");
    uint32_t written = 1u;
    Expect(AnimationEncode(&source, g_encoded, size - 1u, &written) == ANIMATION_BUFFER_TOO_SMALL &&
               written == 0u,
           "encode checks capacity before writing");
    Expect(AnimationEncode(&source, g_encoded, sizeof(g_encoded), &written) == ANIMATION_OK &&
               written == size,
           "encode");
    Expect(g_encoded[0] == 'L' && g_encoded[1] == 'A' && g_encoded[2] == 'K' &&
               g_encoded[3] == '1' && g_encoded[4] == 1u && g_encoded[5] == 0u &&
               g_encoded[8] == 48u && g_encoded[32] == 48u,
           "wire is versioned little endian");
    AnimationInfo info = {0};
    Expect(AnimationInspect(g_encoded, size, &info) == ANIMATION_OK && info.channelCount == 5u &&
               info.keyCount == 10u && info.jointCount == 2u && info.duration == 2.0f,
           "inspection");
    AnimationData decoded = {g_decodedChannels, 5u, g_decodedKeys, 10u, g_decodedJoints, 2u, 0.0f};
    Expect(AnimationDecode(g_encoded, size, &info, &decoded) == ANIMATION_OK &&
               decoded.keys[9].frame == UINT32_MAX && decoded.joints[1].parent == 0u &&
               decoded.keys[7].value[0] == 20.0f,
           "decode preserves channels, hierarchy and integer frames");
    Expect(AnimationEncode(&decoded, g_bad, sizeof(g_bad), NULL) == ANIMATION_OK &&
               memcmp(g_bad, g_encoded, size) == 0,
           "round trip produces identical canonical bytes");
    for (uint32_t cut = 0u; cut < size; ++cut)
        Expect(AnimationInspect(g_encoded, cut, &info) != ANIMATION_OK,
               "every truncated prefix rejected");
    Expect(AnimationInspect(g_encoded, size + 1u, &info) == ANIMATION_CORRUPT,
           "trailing data rejected");
    Expect(AnimationInspect(NULL, size, &info) == ANIMATION_INVALID_ARGUMENT,
           "null input rejected");
    Corrupt(size, 0u, 0u, ANIMATION_NOT_RECOGNISED);
    Corrupt(size, 4u, 2u, ANIMATION_UNSUPPORTED_VERSION);
    Corrupt(size, 8u, 47u, ANIMATION_CORRUPT);
    Corrupt(size, 12u, UINT32_MAX, ANIMATION_CORRUPT);
    Corrupt(size, 16u, UINT32_MAX, ANIMATION_TOO_LARGE);
    Corrupt(size, 20u, UINT32_MAX, ANIMATION_TOO_LARGE);
    Corrupt(size, 24u, UINT32_MAX, ANIMATION_TOO_LARGE);
    Corrupt(size, 28u, UINT32_C(0x7fc00000), ANIMATION_CORRUPT);
    Corrupt(size, 32u, UINT32_MAX, ANIMATION_CORRUPT);
    Corrupt(size, 36u, 0u, ANIMATION_CORRUPT);
    Corrupt(size, 40u, 48u, ANIMATION_CORRUPT);
    Corrupt(size, 44u, 1u, ANIMATION_CORRUPT);
    Corrupt(size, 48u, 0u, ANIMATION_CORRUPT); /* unknown channel */
    Corrupt(size, 52u, 2u, ANIMATION_CORRUPT); /* joint target */
    Corrupt(size, 56u, 2u, ANIMATION_CORRUPT); /* interpolation */
    Corrupt(size, 60u, 1u, ANIMATION_CORRUPT); /* noncanonical firstKey */
    Corrupt(size, 64u, UINT32_MAX, ANIMATION_CORRUPT);
    Corrupt(size, 136u, ANIMATION_LINEAR, ANIMATION_CORRUPT);     /* linear flipbook */
    Corrupt(size, 148u, UINT32_C(0x7f800000), ANIMATION_CORRUPT); /* infinite key time */
    Corrupt(size, 172u, 0u, ANIMATION_CORRUPT);                   /* duplicate time */
    Corrupt(size, 152u, UINT32_C(0xff800000), ANIMATION_CORRUPT); /* infinite value */
    Corrupt(size, 212u, 0u, ANIMATION_CORRUPT);                   /* zero quaternion w */
    Corrupt(size, 248u, 0u, ANIMATION_CORRUPT);                   /* zero scale x */
    Corrupt(size, 388u, 0u, ANIMATION_CORRUPT);                   /* root points to itself */
    Corrupt(size, 480u, 1u, ANIMATION_CORRUPT);                   /* child points to itself */
    Corrupt(size, 432u, UINT32_C(0x7fc00000), ANIMATION_CORRUPT); /* inverse bind NaN */

    Expect(AnimationInspect(g_encoded, size, &info) == ANIMATION_OK, "restore info");
    AnimationData aliases = {
        g_decodedChannels, 5u, (AnimationKey *)(void *)g_encoded, 10u, g_decodedJoints, 2u, 0.0f};
    Expect(AnimationDecode(g_encoded, size, &info, &aliases) == ANIMATION_INVALID_ARGUMENT &&
               g_encoded[0] == 'L',
           "decode input/output overlap rejected before writing");
    Expect(AnimationEncode(&source, g_keys, sizeof(g_keys), NULL) == ANIMATION_BUFFER_TOO_SMALL,
           "encoder checks true capacity before overlap");
    decoded.channelCount = 4u;
    decoded.channels[0].kind = 99u;
    Expect(AnimationDecode(g_encoded, size, &info, &decoded) == ANIMATION_BUFFER_TOO_SMALL &&
               decoded.channels[0].kind == 99u,
           "decode capacity failure writes nothing");
    decoded.channelCount = 5u;
    ++info.keyCount;
    Expect(AnimationDecode(g_encoded, size, &info, &decoded) == ANIMATION_INVALID_ARGUMENT,
           "forged inspect result rejected");
    g_channels[1].kind = ANIMATION_TRANSLATION;
    Expect(AnimationValidate(&source) == ANIMATION_CORRUPT, "duplicate channel targets rejected");
    g_channels[1].kind = ANIMATION_ROTATION;
    const AnimationData empty = {0};
    Expect(AnimationEncode(&empty, g_bad, sizeof(g_bad), &written) == ANIMATION_OK &&
               written == 48u,
           "neutral empty clip encodes");
    Expect(AnimationInspect(g_bad, written, &info) == ANIMATION_OK && info.keyCount == 0u,
           "neutral empty clip decodes");
    LaiueTestRuntimeWrite("Animation codec checks passed\n");
    LAIUE_TEST_SUCCESS();
}
