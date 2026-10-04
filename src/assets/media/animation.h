#pragma once

#include <stdint.h>

/* Internal, allocation-free .lk codec. .lkp is a content directory, not an
 * archive. Records are serialized field by field in little-endian order.
 * Inspect validates every record before the caller allocates its buffers. */
#define ANIMATION_LK_VERSION 1u
#define ANIMATION_LK_HEADER_BYTES 48u
#define ANIMATION_MAX_CHANNELS 4096u
#define ANIMATION_MAX_KEYS (UINT32_C(1) << 20)
#define ANIMATION_MAX_JOINTS 1024u
#define ANIMATION_MAX_FILE_BYTES (UINT32_C(64) << 20)
#define ANIMATION_NO_PARENT UINT32_MAX

typedef enum AnimationStatus
{
    ANIMATION_OK = 0,
    ANIMATION_INVALID_ARGUMENT,
    ANIMATION_NOT_RECOGNISED,
    ANIMATION_TRUNCATED,
    ANIMATION_CORRUPT,
    ANIMATION_UNSUPPORTED_VERSION,
    ANIMATION_TOO_LARGE,
    ANIMATION_BUFFER_TOO_SMALL,
} AnimationStatus;

typedef enum AnimationChannelKind
{
    ANIMATION_TRANSLATION = 1u,
    ANIMATION_ROTATION,
    ANIMATION_SCALE,
    ANIMATION_SCALAR,
    ANIMATION_TEXTURE_FRAME,
} AnimationChannelKind;

typedef enum AnimationInterpolation
{
    ANIMATION_STEP = 0u,
    ANIMATION_LINEAR = 1u, /* shortest-arc normalized lerp for rotations */
} AnimationInterpolation;

typedef struct AnimationTransform
{
    float translation[3];
    float rotation[4]; /* unit quaternion x,y,z,w */
    float scale[3];
} AnimationTransform;

typedef struct AnimationJoint
{
    uint32_t parent; /* NO_PARENT, or an earlier joint: cycles are impossible */
    AnimationTransform bindLocal;
    float inverseBind[12]; /* affine row-major 3x4; column vectors */
} AnimationJoint;

typedef struct AnimationChannel
{
    uint32_t kind;
    uint32_t target; /* joint index for TRS; application id otherwise */
    uint32_t interpolation;
    uint32_t firstKey;
    uint32_t keyCount;
} AnimationChannel;

typedef struct AnimationKey
{
    float time;
    float value[4];
    uint32_t frame; /* exact integer, never converted through float */
} AnimationKey;

typedef struct AnimationInfo
{
    uint32_t channelCount;
    uint32_t keyCount;
    uint32_t jointCount;
    float duration;
} AnimationInfo;

typedef struct AnimationData
{
    AnimationChannel *channels;
    uint32_t channelCount;
    AnimationKey *keys;
    uint32_t keyCount;
    AnimationJoint *joints;
    uint32_t jointCount;
    float duration;
} AnimationData;

AnimationStatus AnimationInspect(const void *bytes, uint32_t sizeBytes, AnimationInfo *outInfo);
/* Counts in inOutData are caller capacities on entry, decoded counts on
 * success. info must match a fresh inspection. Failure writes no records.
 * Input, metadata and the three caller arrays must not overlap. */
AnimationStatus AnimationDecode(const void *bytes, uint32_t sizeBytes, const AnimationInfo *info,
                                AnimationData *inOutData);
AnimationStatus AnimationValidate(const AnimationData *data);
AnimationStatus AnimationEncodedBytes(const AnimationData *data, uint32_t *outBytes);
AnimationStatus AnimationEncode(const AnimationData *data, void *outBytes, uint32_t capacityBytes,
                                uint32_t *outWritten);
const char *AnimationStatusText(AnimationStatus status);
