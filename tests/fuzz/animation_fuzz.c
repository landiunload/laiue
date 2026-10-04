#include "media/animation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int Decode(const uint8_t *bytes, uint32_t size, AnimationData *data)
{
    memset(data, 0, sizeof(*data));
    AnimationInfo info = {0};
    if (AnimationInspect(bytes, size, &info) != ANIMATION_OK)
        return 0;
    data->channels = malloc((size_t)info.channelCount * sizeof(AnimationChannel) + 1u);
    data->keys = malloc((size_t)info.keyCount * sizeof(AnimationKey) + 1u);
    data->joints = malloc((size_t)info.jointCount * sizeof(AnimationJoint) + 1u);
    if (data->channels == NULL || data->keys == NULL || data->joints == NULL)
        return 0;
    data->channelCount = info.channelCount;
    data->keyCount = info.keyCount;
    data->jointCount = info.jointCount;
    return AnimationDecode(bytes, size, &info, data) == ANIMATION_OK;
}

static void Release(AnimationData *data)
{
    free(data->channels);
    free(data->keys);
    free(data->joints);
}

int LLVMFuzzerTestOneInput(const uint8_t *bytes, size_t size)
{
    if (size > (1u << 20))
        return 0;
    AnimationData data = {0};
    if (!Decode(bytes, (uint32_t)size, &data))
    {
        Release(&data);
        return 0;
    }
    uint32_t encodedSize = 0u;
    if (AnimationEncodedBytes(&data, &encodedSize) != ANIMATION_OK)
        abort();
    uint8_t *encoded = malloc(encodedSize);
    if (encoded == NULL)
    {
        Release(&data);
        return 0;
    }
    uint32_t written = 0u;
    if (AnimationEncode(&data, encoded, encodedSize, &written) != ANIMATION_OK ||
        written != encodedSize || written != size || memcmp(bytes, encoded, size) != 0)
        abort();
    AnimationData again = {0};
    if (!Decode(encoded, encodedSize, &again) || again.channelCount != data.channelCount ||
        again.keyCount != data.keyCount || again.jointCount != data.jointCount ||
        again.duration != data.duration ||
        memcmp(again.channels, data.channels,
               (size_t)data.channelCount * sizeof(AnimationChannel)) != 0 ||
        memcmp(again.keys, data.keys, (size_t)data.keyCount * sizeof(AnimationKey)) != 0 ||
        memcmp(again.joints, data.joints, (size_t)data.jointCount * sizeof(AnimationJoint)) != 0)
        abort();
    Release(&again);
    free(encoded);
    Release(&data);
    return 0;
}
