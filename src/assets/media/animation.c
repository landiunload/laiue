#include "media/animation.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define LK_MAGIC UINT32_C(0x314b414c) /* LAK1 */
#define LK_CHANNEL_BYTES 20u
#define LK_KEY_BYTES 24u
#define LK_JOINT_BYTES 92u

_Static_assert(sizeof(float) == 4u && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "animation format requires binary32 floats");

static bool Overlap(const void *a, size_t aBytes, const void *b, size_t bBytes)
{
    if (aBytes == 0u || bBytes == 0u)
        return false;
    const uintptr_t left = (uintptr_t)a;
    const uintptr_t right = (uintptr_t)b;
    if (aBytes > UINTPTR_MAX - left || bBytes > UINTPTR_MAX - right)
        return true;
    return left < right + bBytes && right < left + aBytes;
}

static uint32_t ReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float ReadF32(const uint8_t *p)
{
    uint32_t bits = ReadU32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void WriteU32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
        p[i] = (uint8_t)(value >> (i * 8u));
}

static void WriteF32(uint8_t *p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    WriteU32(p, bits);
}

static bool Finite(float value)
{
    return value >= -FLT_MAX && value <= FLT_MAX;
}

static bool UnitQuaternion(const float *q)
{
    double length = 0.0;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!Finite(q[i]))
            return false;
        length += (double)q[i] * q[i];
    }
    return length >= 0.999 && length <= 1.001;
}

static bool ValidTransform(const AnimationTransform *t)
{
    for (uint32_t i = 0u; i < 3u; ++i)
        if (!Finite(t->translation[i]) || !Finite(t->scale[i]) || t->scale[i] == 0.0f)
            return false;
    return UnitQuaternion(t->rotation);
}

static AnimationChannel ReadChannel(const uint8_t *p)
{
    AnimationChannel channel = {ReadU32(p), ReadU32(p + 4), ReadU32(p + 8), ReadU32(p + 12),
                                ReadU32(p + 16)};
    return channel;
}

static AnimationKey ReadKey(const uint8_t *p)
{
    AnimationKey key = {0};
    key.time = ReadF32(p);
    for (uint32_t i = 0u; i < 4u; ++i)
        key.value[i] = ReadF32(p + 4u + i * 4u);
    key.frame = ReadU32(p + 20);
    return key;
}

static AnimationJoint ReadJoint(const uint8_t *p)
{
    AnimationJoint joint = {0};
    joint.parent = ReadU32(p);
    for (uint32_t i = 0u; i < 3u; ++i)
        joint.bindLocal.translation[i] = ReadF32(p + 4u + i * 4u);
    for (uint32_t i = 0u; i < 4u; ++i)
        joint.bindLocal.rotation[i] = ReadF32(p + 16u + i * 4u);
    for (uint32_t i = 0u; i < 3u; ++i)
        joint.bindLocal.scale[i] = ReadF32(p + 32u + i * 4u);
    for (uint32_t i = 0u; i < 12u; ++i)
        joint.inverseBind[i] = ReadF32(p + 44u + i * 4u);
    return joint;
}

/* One validator for decoded arrays and the wire representation. The wire
 * reader creates individual stack records, never aliases native structs. */
typedef struct Records
{
    const AnimationData *data;
    const uint8_t *channels;
    const uint8_t *keys;
    const uint8_t *joints;
} Records;

static AnimationChannel ChannelAt(const Records *r, uint32_t index)
{
    return r->data != NULL ? r->data->channels[index]
                           : ReadChannel(r->channels + (size_t)index * LK_CHANNEL_BYTES);
}

static AnimationKey KeyAt(const Records *r, uint32_t index)
{
    return r->data != NULL ? r->data->keys[index] : ReadKey(r->keys + (size_t)index * LK_KEY_BYTES);
}

static AnimationJoint JointAt(const Records *r, uint32_t index)
{
    return r->data != NULL ? r->data->joints[index]
                           : ReadJoint(r->joints + (size_t)index * LK_JOINT_BYTES);
}

static AnimationStatus ValidateRecords(const AnimationInfo *info, const Records *r)
{
    if (!Finite(info->duration) || info->duration < 0.0f)
        return ANIMATION_CORRUPT;
    for (uint32_t index = 0u; index < info->jointCount; ++index)
    {
        const AnimationJoint joint = JointAt(r, index);
        if ((joint.parent != ANIMATION_NO_PARENT && joint.parent >= index) ||
            !ValidTransform(&joint.bindLocal))
            return ANIMATION_CORRUPT;
        for (uint32_t i = 0u; i < 12u; ++i)
            if (!Finite(joint.inverseBind[i]))
                return ANIMATION_CORRUPT;
    }
    uint32_t cursor = 0u;
    for (uint32_t index = 0u; index < info->channelCount; ++index)
    {
        const AnimationChannel channel = ChannelAt(r, index);
        if (channel.kind < ANIMATION_TRANSLATION || channel.kind > ANIMATION_TEXTURE_FRAME ||
            channel.interpolation > ANIMATION_LINEAR || channel.firstKey != cursor ||
            channel.keyCount == 0u || channel.keyCount > info->keyCount - cursor ||
            (channel.kind <= ANIMATION_SCALE && channel.target >= info->jointCount) ||
            (channel.kind == ANIMATION_TEXTURE_FRAME && channel.interpolation != ANIMATION_STEP))
            return ANIMATION_CORRUPT;
        for (uint32_t earlier = 0u; earlier < index; ++earlier)
        {
            const AnimationChannel other = ChannelAt(r, earlier);
            if (other.kind == channel.kind && other.target == channel.target)
                return ANIMATION_CORRUPT;
        }
        float previous = -1.0f;
        for (uint32_t offset = 0u; offset < channel.keyCount; ++offset)
        {
            const AnimationKey key = KeyAt(r, cursor + offset);
            if (!Finite(key.time) || key.time < 0.0f || key.time > info->duration ||
                key.time <= previous)
                return ANIMATION_CORRUPT;
            previous = key.time;
            for (uint32_t component = 0u; component < 4u; ++component)
                if (!Finite(key.value[component]))
                    return ANIMATION_CORRUPT;
            if (channel.kind == ANIMATION_ROTATION && !UnitQuaternion(key.value))
                return ANIMATION_CORRUPT;
            if (channel.kind == ANIMATION_SCALE)
                for (uint32_t component = 0u; component < 3u; ++component)
                    if (key.value[component] == 0.0f)
                        return ANIMATION_CORRUPT;
        }
        cursor += channel.keyCount;
    }
    return cursor == info->keyCount ? ANIMATION_OK : ANIMATION_CORRUPT;
}

static AnimationStatus Counts(const AnimationInfo *info, uint32_t *outBytes)
{
    if (info->channelCount > ANIMATION_MAX_CHANNELS || info->keyCount > ANIMATION_MAX_KEYS ||
        info->jointCount > ANIMATION_MAX_JOINTS)
        return ANIMATION_TOO_LARGE;
    const uint64_t size =
        ANIMATION_LK_HEADER_BYTES + (uint64_t)info->channelCount * LK_CHANNEL_BYTES +
        (uint64_t)info->keyCount * LK_KEY_BYTES + (uint64_t)info->jointCount * LK_JOINT_BYTES;
    if (size > ANIMATION_MAX_FILE_BYTES)
        return ANIMATION_TOO_LARGE;
    *outBytes = (uint32_t)size;
    return ANIMATION_OK;
}

AnimationStatus AnimationInspect(const void *bytes, uint32_t sizeBytes, AnimationInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL)
        return ANIMATION_INVALID_ARGUMENT;
    if (sizeBytes > ANIMATION_MAX_FILE_BYTES)
        return ANIMATION_TOO_LARGE;
    if (sizeBytes < 4u)
        return ANIMATION_TRUNCATED;
    const uint8_t *p = (const uint8_t *)bytes;
    if (ReadU32(p) != LK_MAGIC)
        return ANIMATION_NOT_RECOGNISED;
    if (sizeBytes < ANIMATION_LK_HEADER_BYTES)
        return ANIMATION_TRUNCATED;
    if (ReadU32(p + 4) != ANIMATION_LK_VERSION)
        return ANIMATION_UNSUPPORTED_VERSION;
    if (ReadU32(p + 8) != ANIMATION_LK_HEADER_BYTES || ReadU32(p + 44) != 0u)
        return ANIMATION_CORRUPT;
    const AnimationInfo info = {ReadU32(p + 16), ReadU32(p + 20), ReadU32(p + 24), ReadF32(p + 28)};
    uint32_t expected = 0u;
    const AnimationStatus status = Counts(&info, &expected);
    if (status != ANIMATION_OK)
        return status;
    if (ReadU32(p + 12) != expected)
        return ANIMATION_CORRUPT;
    if (sizeBytes < expected)
        return ANIMATION_TRUNCATED;
    if (sizeBytes != expected || ReadU32(p + 32) != ANIMATION_LK_HEADER_BYTES ||
        ReadU32(p + 36) != ANIMATION_LK_HEADER_BYTES + info.channelCount * LK_CHANNEL_BYTES ||
        ReadU32(p + 40) != ANIMATION_LK_HEADER_BYTES + info.channelCount * LK_CHANNEL_BYTES +
                               info.keyCount * LK_KEY_BYTES)
        return ANIMATION_CORRUPT;
    const Records records = {NULL, p + ReadU32(p + 32), p + ReadU32(p + 36), p + ReadU32(p + 40)};
    const AnimationStatus validated = ValidateRecords(&info, &records);
    if (validated == ANIMATION_OK)
        *outInfo = info;
    return validated;
}

AnimationStatus AnimationValidate(const AnimationData *data)
{
    if (data == NULL || (data->channelCount != 0u && data->channels == NULL) ||
        (data->keyCount != 0u && data->keys == NULL) ||
        (data->jointCount != 0u && data->joints == NULL))
        return ANIMATION_INVALID_ARGUMENT;
    const AnimationInfo info = {data->channelCount, data->keyCount, data->jointCount,
                                data->duration};
    uint32_t size = 0u;
    const AnimationStatus status = Counts(&info, &size);
    if (status != ANIMATION_OK)
        return status;
    const Records records = {data, NULL, NULL, NULL};
    return ValidateRecords(&info, &records);
}

AnimationStatus AnimationDecode(const void *bytes, uint32_t sizeBytes, const AnimationInfo *info,
                                AnimationData *inOutData)
{
    if (info == NULL || inOutData == NULL)
        return ANIMATION_INVALID_ARGUMENT;
    AnimationInfo actual = {0};
    const AnimationStatus status = AnimationInspect(bytes, sizeBytes, &actual);
    if (status != ANIMATION_OK)
        return status;
    if (info->channelCount != actual.channelCount || info->keyCount != actual.keyCount ||
        info->jointCount != actual.jointCount || info->duration != actual.duration)
        return ANIMATION_INVALID_ARGUMENT;
    if (inOutData->channelCount < actual.channelCount || inOutData->keyCount < actual.keyCount ||
        inOutData->jointCount < actual.jointCount)
        return ANIMATION_BUFFER_TOO_SMALL;
    if ((actual.channelCount != 0u && inOutData->channels == NULL) ||
        (actual.keyCount != 0u && inOutData->keys == NULL) ||
        (actual.jointCount != 0u && inOutData->joints == NULL))
        return ANIMATION_INVALID_ARGUMENT;
    const size_t channelBytes = (size_t)actual.channelCount * sizeof(AnimationChannel);
    const size_t keyBytes = (size_t)actual.keyCount * sizeof(AnimationKey);
    const size_t jointBytes = (size_t)actual.jointCount * sizeof(AnimationJoint);
    if (Overlap(bytes, sizeBytes, inOutData, sizeof(*inOutData)) ||
        Overlap(bytes, sizeBytes, inOutData->channels, channelBytes) ||
        Overlap(bytes, sizeBytes, inOutData->keys, keyBytes) ||
        Overlap(bytes, sizeBytes, inOutData->joints, jointBytes) ||
        Overlap(inOutData, sizeof(*inOutData), inOutData->channels, channelBytes) ||
        Overlap(inOutData, sizeof(*inOutData), inOutData->keys, keyBytes) ||
        Overlap(inOutData, sizeof(*inOutData), inOutData->joints, jointBytes) ||
        Overlap(inOutData->channels, channelBytes, inOutData->keys, keyBytes) ||
        Overlap(inOutData->channels, channelBytes, inOutData->joints, jointBytes) ||
        Overlap(inOutData->keys, keyBytes, inOutData->joints, jointBytes))
        return ANIMATION_INVALID_ARGUMENT;
    const uint8_t *p = (const uint8_t *)bytes;
    const uint32_t channelOffset = ReadU32(p + 32);
    const uint32_t keyOffset = ReadU32(p + 36);
    const uint32_t jointOffset = ReadU32(p + 40);
    for (uint32_t i = 0u; i < actual.channelCount; ++i)
        inOutData->channels[i] = ReadChannel(p + channelOffset + (size_t)i * LK_CHANNEL_BYTES);
    for (uint32_t i = 0u; i < actual.keyCount; ++i)
        inOutData->keys[i] = ReadKey(p + keyOffset + (size_t)i * LK_KEY_BYTES);
    for (uint32_t i = 0u; i < actual.jointCount; ++i)
        inOutData->joints[i] = ReadJoint(p + jointOffset + (size_t)i * LK_JOINT_BYTES);
    inOutData->channelCount = actual.channelCount;
    inOutData->keyCount = actual.keyCount;
    inOutData->jointCount = actual.jointCount;
    inOutData->duration = actual.duration;
    return ANIMATION_OK;
}

AnimationStatus AnimationEncodedBytes(const AnimationData *data, uint32_t *outBytes)
{
    if (outBytes == NULL)
        return ANIMATION_INVALID_ARGUMENT;
    const AnimationStatus status = AnimationValidate(data);
    if (status != ANIMATION_OK)
        return status;
    const AnimationInfo info = {data->channelCount, data->keyCount, data->jointCount,
                                data->duration};
    return Counts(&info, outBytes);
}

AnimationStatus AnimationEncode(const AnimationData *data, void *outBytes, uint32_t capacityBytes,
                                uint32_t *outWritten)
{
    if (outWritten != NULL)
        *outWritten = 0u;
    if (outBytes == NULL)
        return ANIMATION_INVALID_ARGUMENT;
    uint32_t size = 0u;
    const AnimationStatus status = AnimationEncodedBytes(data, &size);
    if (status != ANIMATION_OK)
        return status;
    if (capacityBytes < size)
        return ANIMATION_BUFFER_TOO_SMALL;
    if (Overlap(outBytes, size, data, sizeof(*data)) ||
        Overlap(outBytes, size, data->channels,
                (size_t)data->channelCount * sizeof(AnimationChannel)) ||
        Overlap(outBytes, size, data->keys, (size_t)data->keyCount * sizeof(AnimationKey)) ||
        Overlap(outBytes, size, data->joints, (size_t)data->jointCount * sizeof(AnimationJoint)))
        return ANIMATION_INVALID_ARGUMENT;
    uint8_t *p = (uint8_t *)outBytes;
    const uint32_t keyOffset = ANIMATION_LK_HEADER_BYTES + data->channelCount * LK_CHANNEL_BYTES;
    const uint32_t jointOffset = keyOffset + data->keyCount * LK_KEY_BYTES;
    WriteU32(p, LK_MAGIC);
    WriteU32(p + 4, ANIMATION_LK_VERSION);
    WriteU32(p + 8, ANIMATION_LK_HEADER_BYTES);
    WriteU32(p + 12, size);
    WriteU32(p + 16, data->channelCount);
    WriteU32(p + 20, data->keyCount);
    WriteU32(p + 24, data->jointCount);
    WriteF32(p + 28, data->duration);
    WriteU32(p + 32, ANIMATION_LK_HEADER_BYTES);
    WriteU32(p + 36, keyOffset);
    WriteU32(p + 40, jointOffset);
    WriteU32(p + 44, 0u);
    for (uint32_t i = 0u; i < data->channelCount; ++i)
    {
        uint8_t *record = p + ANIMATION_LK_HEADER_BYTES + (size_t)i * LK_CHANNEL_BYTES;
        const AnimationChannel *channel = &data->channels[i];
        WriteU32(record, channel->kind);
        WriteU32(record + 4, channel->target);
        WriteU32(record + 8, channel->interpolation);
        WriteU32(record + 12, channel->firstKey);
        WriteU32(record + 16, channel->keyCount);
    }
    for (uint32_t i = 0u; i < data->keyCount; ++i)
    {
        uint8_t *record = p + keyOffset + (size_t)i * LK_KEY_BYTES;
        WriteF32(record, data->keys[i].time);
        for (uint32_t component = 0u; component < 4u; ++component)
            WriteF32(record + 4u + component * 4u, data->keys[i].value[component]);
        WriteU32(record + 20, data->keys[i].frame);
    }
    for (uint32_t i = 0u; i < data->jointCount; ++i)
    {
        uint8_t *record = p + jointOffset + (size_t)i * LK_JOINT_BYTES;
        const AnimationJoint *joint = &data->joints[i];
        WriteU32(record, joint->parent);
        for (uint32_t component = 0u; component < 3u; ++component)
            WriteF32(record + 4u + component * 4u, joint->bindLocal.translation[component]);
        for (uint32_t component = 0u; component < 4u; ++component)
            WriteF32(record + 16u + component * 4u, joint->bindLocal.rotation[component]);
        for (uint32_t component = 0u; component < 3u; ++component)
            WriteF32(record + 32u + component * 4u, joint->bindLocal.scale[component]);
        for (uint32_t component = 0u; component < 12u; ++component)
            WriteF32(record + 44u + component * 4u, joint->inverseBind[component]);
    }
    if (outWritten != NULL)
        *outWritten = size;
    return ANIMATION_OK;
}

const char *AnimationStatusText(AnimationStatus status)
{
    switch (status)
    {
    case ANIMATION_OK:
        return "ok";
    case ANIMATION_INVALID_ARGUMENT:
        return "invalid argument";
    case ANIMATION_NOT_RECOGNISED:
        return "not recognised";
    case ANIMATION_TRUNCATED:
        return "truncated";
    case ANIMATION_CORRUPT:
        return "corrupt";
    case ANIMATION_UNSUPPORTED_VERSION:
        return "unsupported version";
    case ANIMATION_TOO_LARGE:
        return "too large";
    case ANIMATION_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown status";
    }
}
