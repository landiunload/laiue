#include "animation/animation_internal.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(double) == 8u && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "animation normalization requires binary64 doubles");

static bool Finite(double value)
{
    return value >= -FLT_MAX && value <= FLT_MAX;
}

static bool Overlap(const void *a, uint64_t aBytes, const void *b, uint64_t bBytes)
{
    if (aBytes == 0u || bBytes == 0u)
        return false;
    const uintptr_t left = (uintptr_t)a;
    const uintptr_t right = (uintptr_t)b;
    if (aBytes > UINTPTR_MAX - left || bBytes > UINTPTR_MAX - right)
        return true;
    return left < right + bBytes && right < left + aBytes;
}

/* No libm or platform dependency. All normalization inputs are bounded
 * doubles built from finite floats; exponent-based initial estimate also
 * handles tiny values without a data-dependent halving loop. */
static double SquareRoot(double value)
{
    if (!(value > 0.0))
        return 0.0;
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits = (bits >> 1) + UINT64_C(0x1ff8000000000000);
    double estimate;
    memcpy(&estimate, &bits, sizeof(estimate));
    for (uint32_t step = 0u; step < 8u; ++step)
        estimate = 0.5 * (estimate + value / estimate);
    return estimate;
}

static float ClipTime(float time, float duration, uint32_t loop)
{
    if (!(duration > 0.0f))
        return 0.0f;
    if (loop == 0u)
        return time < 0.0f ? 0.0f : (time > duration ? duration : time);
    /* Remainder via binary long division: at most 278 iterations across
     * the full finite binary32 range, no overflowing float->integer cast. */
    double remaining = time < 0.0f ? -(double)time : (double)time;
    double divisor = duration;
    while (divisor <= remaining * 0.5)
        divisor *= 2.0;
    while (divisor >= (double)duration)
    {
        if (remaining >= divisor)
            remaining -= divisor;
        divisor *= 0.5;
    }
    if (time < 0.0f && remaining != 0.0)
        remaining = (double)duration - remaining;
    float wrapped = (float)remaining;
    /* Rounding a tiny backwards remainder can produce duration. */
    return wrapped < duration ? wrapped : 0.0f;
}

static void Sample(const AnimationData *data, const AnimationChannel *channel, float time,
                   LaiueAnimationValueV1 *out)
{
    const AnimationKey *keys = data->keys + channel->firstKey;
    uint32_t low = 0u;
    uint32_t high = channel->keyCount;
    /* upper_bound(time): an exact key timestamp selects the new frame. */
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        if (keys[middle].time <= time)
            low = middle + 1u;
        else
            high = middle;
    }
    const uint32_t index = low == 0u ? 0u : low - 1u;
    memcpy(out->value, keys[index].value, sizeof(out->value));
    out->frame = keys[index].frame;
    if (channel->interpolation == ANIMATION_STEP || low == 0u || low == channel->keyCount)
        return;
    const AnimationKey *a = &keys[index];
    const AnimationKey *b = &keys[index + 1u];
    const double factor = ((double)time - a->time) / ((double)b->time - a->time);
    double sign = 1.0;
    if (channel->kind == ANIMATION_ROTATION)
    {
        double dot = 0.0;
        for (uint32_t component = 0u; component < 4u; ++component)
            dot += (double)a->value[component] * b->value[component];
        if (dot < 0.0)
            sign = -1.0;
    }
    const uint32_t components = channel->kind == ANIMATION_SCALAR     ? 1u
                                : channel->kind == ANIMATION_ROTATION ? 4u
                                                                      : 3u;
    double value[4] = {0.0, 0.0, 0.0, 0.0};
    double length = 0.0;
    for (uint32_t component = 0u; component < components; ++component)
    {
        value[component] =
            (1.0 - factor) * a->value[component] + factor * sign * b->value[component];
        length += value[component] * value[component];
    }
    const double divisor = channel->kind == ANIMATION_ROTATION ? SquareRoot(length) : 1.0;
    for (uint32_t component = 0u; component < components; ++component)
        out->value[component] = (float)(value[component] / divisor);
}

uint32_t AnimationSampleChannel(const LaiueAnimationV1 *clip, uint32_t channel, float time,
                                uint32_t loop, LaiueAnimationValueV1 *outValue)
{
    if (clip == NULL || outValue == NULL || channel >= clip->data.channelCount || !Finite(time))
        return 0u;
    Sample(&clip->data, &clip->data.channels[channel], ClipTime(time, clip->data.duration, loop),
           outValue);
    return 1u;
}

static bool LocalMatrix(const LaiueAnimationTransformV1 *local, float *out)
{
    const double x = local->rotation[0], y = local->rotation[1], z = local->rotation[2],
                 w = local->rotation[3];
    const double norm = x * x + y * y + z * z + w * w;
    const double s = 2.0 / norm;
    const double matrix[9] = {
        1.0 - s * (y * y + z * z), s * (x * y - z * w),       s * (x * z + y * w),
        s * (x * y + z * w),       1.0 - s * (x * x + z * z), s * (y * z - x * w),
        s * (x * z - y * w),       s * (y * z + x * w),       1.0 - s * (x * x + y * y)};
    for (uint32_t row = 0u; row < 3u; ++row)
    {
        for (uint32_t column = 0u; column < 3u; ++column)
        {
            const double value = matrix[row * 3u + column] * local->scale[column];
            if (!Finite(value))
                return false;
            out[row * 4u + column] = (float)value;
        }
        out[row * 4u + 3u] = local->translation[row];
    }
    return true;
}

static bool Multiply(const float *a, const float *b, float *out)
{
    for (uint32_t row = 0u; row < 3u; ++row)
        for (uint32_t column = 0u; column < 4u; ++column)
        {
            double value = column == 3u ? a[row * 4u + 3u] : 0.0;
            for (uint32_t component = 0u; component < 3u; ++component)
                value += (double)a[row * 4u + component] * b[component * 4u + column];
            if (!Finite(value))
                return false;
            out[row * 4u + column] = (float)value;
        }
    return true;
}

uint32_t AnimationSamplePose(const LaiueAnimationV1 *clip, float time, uint32_t loop,
                             LaiueAnimationTransformV1 *locals, LaiueAnimationMatrixV1 *globals,
                             LaiueAnimationMatrixV1 *palette, uint32_t jointCapacity)
{
    if (clip == NULL || !Finite(time) || jointCapacity < clip->data.jointCount)
        return 0u;
    if (clip->data.jointCount == 0u)
        return 1u;
    if (locals == NULL || globals == NULL || palette == NULL)
        return 0u;
    const uint64_t localBytes = (uint64_t)clip->data.jointCount * sizeof(*locals);
    const uint64_t matrixBytes = (uint64_t)clip->data.jointCount * sizeof(*globals);
    if (Overlap(locals, localBytes, globals, matrixBytes) ||
        Overlap(locals, localBytes, palette, matrixBytes) ||
        Overlap(globals, matrixBytes, palette, matrixBytes))
        return 0u;
    const AnimationData *data = &clip->data;
    for (uint32_t joint = 0u; joint < data->jointCount; ++joint)
    {
        memcpy(locals[joint].translation, data->joints[joint].bindLocal.translation,
               sizeof(locals[joint].translation));
        memcpy(locals[joint].rotation, data->joints[joint].bindLocal.rotation,
               sizeof(locals[joint].rotation));
        memcpy(locals[joint].scale, data->joints[joint].bindLocal.scale,
               sizeof(locals[joint].scale));
    }
    const float at = ClipTime(time, data->duration, loop);
    for (uint32_t channel = 0u; channel < data->channelCount; ++channel)
    {
        const AnimationChannel *record = &data->channels[channel];
        if (record->kind > ANIMATION_SCALE)
            continue;
        LaiueAnimationValueV1 value = {0};
        Sample(data, record, at, &value);
        LaiueAnimationTransformV1 *local = &locals[record->target];
        if (record->kind == ANIMATION_TRANSLATION)
            memcpy(local->translation, value.value, sizeof(local->translation));
        else if (record->kind == ANIMATION_ROTATION)
            memcpy(local->rotation, value.value, sizeof(local->rotation));
        else
            memcpy(local->scale, value.value, sizeof(local->scale));
    }
    for (uint32_t joint = 0u; joint < data->jointCount; ++joint)
    {
        float matrix[12];
        if (!LocalMatrix(&locals[joint], matrix))
            return 0u;
        const uint32_t parent = data->joints[joint].parent;
        if (parent == ANIMATION_NO_PARENT)
            memcpy(globals[joint].m, matrix, sizeof(matrix));
        else if (!Multiply(globals[parent].m, matrix, globals[joint].m))
            return 0u;
        if (!Multiply(globals[joint].m, data->joints[joint].inverseBind, palette[joint].m))
            return 0u;
    }
    return 1u;
}

static bool NormalMatrix(const float *m, double *out)
{
    /* Cofactors are the inverse transpose multiplied by determinant. */
    out[0] = (double)m[5] * m[10] - (double)m[6] * m[9];
    out[1] = (double)m[6] * m[8] - (double)m[4] * m[10];
    out[2] = (double)m[4] * m[9] - (double)m[5] * m[8];
    out[3] = (double)m[2] * m[9] - (double)m[1] * m[10];
    out[4] = (double)m[0] * m[10] - (double)m[2] * m[8];
    out[5] = (double)m[1] * m[8] - (double)m[0] * m[9];
    out[6] = (double)m[1] * m[6] - (double)m[2] * m[5];
    out[7] = (double)m[2] * m[4] - (double)m[0] * m[6];
    out[8] = (double)m[0] * m[5] - (double)m[1] * m[4];
    const double determinant =
        (double)m[0] * out[0] + (double)m[1] * out[1] + (double)m[2] * out[2];
    if (determinant == 0.0)
        return false;
    for (uint32_t i = 0u; i < 9u; ++i)
        out[i] /= determinant;
    return true;
}

static bool SkinOne(const LaiueAnimationSkinVertexV1 *vertex, const LaiueAnimationMatrixV1 *palette,
                    uint32_t jointCount, LaiueAnimationSkinnedVertexV1 *out)
{
    double total = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
        if (!Finite(vertex->position[component]) || !Finite(vertex->normal[component]))
            return false;
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
        if (vertex->joints[influence] >= jointCount || !Finite(vertex->weights[influence]) ||
            vertex->weights[influence] < 0.0f)
            return false;
        total += vertex->weights[influence];
    }
    if (total < 0.999 || total > 1.001)
        return false;
    double position[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 0.0};
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
        if (vertex->weights[influence] == 0.0f)
            continue;
        const double weight = (double)vertex->weights[influence] / total;
        const float *matrix = palette[vertex->joints[influence]].m;
        double normalMatrix[9];
        if (!NormalMatrix(matrix, normalMatrix))
            return false;
        for (uint32_t row = 0u; row < 3u; ++row)
        {
            double p = matrix[row * 4u + 3u];
            double n = 0.0;
            for (uint32_t component = 0u; component < 3u; ++component)
            {
                p += (double)matrix[row * 4u + component] * vertex->position[component];
                n += normalMatrix[row * 3u + component] * vertex->normal[component];
            }
            position[row] += p * weight;
            normal[row] += n * weight;
        }
    }
    const double length =
        SquareRoot(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    for (uint32_t component = 0u; component < 3u; ++component)
    {
        const double n = length > 0.0 ? normal[component] / length : 0.0;
        if (!Finite(position[component]) || !Finite(n))
            return false;
        out->position[component] = (float)position[component];
        out->normal[component] = (float)n;
    }
    return true;
}

uint32_t AnimationSkinVertices(const LaiueAnimationSkinVertexV1 *vertices, uint32_t vertexCount,
                               const LaiueAnimationMatrixV1 *palette, uint32_t jointCount,
                               LaiueAnimationSkinnedVertexV1 *outVertices, uint32_t vertexCapacity)
{
    if (vertexCount > vertexCapacity || jointCount > ANIMATION_MAX_JOINTS)
        return 0u;
    if (vertexCount == 0u)
        return 1u;
    if (vertices == NULL || palette == NULL || outVertices == NULL || jointCount == 0u)
        return 0u;
    const uint64_t inputBytes = (uint64_t)vertexCount * sizeof(*vertices);
    const uint64_t outputBytes = (uint64_t)vertexCount * sizeof(*outVertices);
    const uint64_t paletteBytes = (uint64_t)jointCount * sizeof(*palette);
    if (Overlap(vertices, inputBytes, outVertices, outputBytes) ||
        Overlap(palette, paletteBytes, outVertices, outputBytes))
        return 0u;
    for (uint32_t joint = 0u; joint < jointCount; ++joint)
        for (uint32_t component = 0u; component < 12u; ++component)
            if (!Finite(palette[joint].m[component]))
                return 0u;
    /* Validate the complete result before publishing any vertex: a late
     * invalid influence or overflow cannot expose a half-skinned mesh. */
    LaiueAnimationSkinnedVertexV1 temporary;
    for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
        if (!SkinOne(&vertices[vertex], palette, jointCount, &temporary))
            return 0u;
    for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
        (void)SkinOne(&vertices[vertex], palette, jointCount, &outVertices[vertex]);
    return 1u;
}
