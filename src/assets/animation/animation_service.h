#pragma once

/* Standalone animation service: no renderer/model/world dependency. The
 * application binds scalar/texture targets and joint indices to its assets.
 * .lkp directories live under animations/, with .lk clips inside. */
#include "api.h"
#include "content/content_catalog.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_ANIMATION_SERVICE_NAME "laiue.animation"
#define LAIUE_ANIMATION_SERVICE_ABI_VERSION_1 1u
#define LAIUE_ANIMATION_TRANSLATION 1u
#define LAIUE_ANIMATION_ROTATION 2u
#define LAIUE_ANIMATION_SCALE 3u
#define LAIUE_ANIMATION_SCALAR 4u
#define LAIUE_ANIMATION_TEXTURE_FRAME 5u
#define LAIUE_ANIMATION_STEP 0u
#define LAIUE_ANIMATION_LINEAR 1u

#define LAIUE_ANIMATION_LOAD_NOT_ATTEMPTED 0u
#define LAIUE_ANIMATION_LOAD_OK 1u
#define LAIUE_ANIMATION_LOAD_NO_ACTIVE_PACK 2u
#define LAIUE_ANIMATION_LOAD_NOT_FOUND 3u
#define LAIUE_ANIMATION_LOAD_INVALID_CLIP 4u
#define LAIUE_ANIMATION_LOAD_IO_ERROR 5u
#define LAIUE_ANIMATION_LOAD_OUT_OF_MEMORY 6u
#define LAIUE_ANIMATION_LOAD_NO_CATALOG 7u
#define LAIUE_ANIMATION_FLAG_PLACEHOLDER (UINT32_C(1) << 31)

typedef struct LaiueAnimationV1 LaiueAnimationV1;

typedef struct LaiueAnimationViewV1
{
    uint32_t structSize;
    uint32_t flags;
    float duration;
    uint32_t channelCount;
    uint32_t keyCount;
    uint32_t jointCount;
} LaiueAnimationViewV1;

typedef struct LaiueAnimationChannelV1
{
    uint32_t kind;
    uint32_t target;
    uint32_t interpolation;
    uint32_t keyCount;
} LaiueAnimationChannelV1;

typedef struct LaiueAnimationValueV1
{
    float value[4];
    uint32_t frame;
} LaiueAnimationValueV1;

typedef struct LaiueAnimationTransformV1
{
    float translation[3];
    float rotation[4]; /* x,y,z,w */
    float scale[3];
} LaiueAnimationTransformV1;

/* Affine row-major 3x4, column vectors. Translation is m[3],m[7],m[11].
 * This layout is deliberately independent of any renderer's matrix ABI. */
typedef struct LaiueAnimationMatrixV1
{
    float m[12];
} LaiueAnimationMatrixV1;

typedef struct LaiueAnimationSkinVertexV1
{
    float position[3];
    float normal[3];
    uint32_t joints[4];
    float weights[4]; /* nonnegative, sum within 0.001 of one */
} LaiueAnimationSkinVertexV1;

typedef struct LaiueAnimationSkinnedVertexV1
{
    float position[3];
    float normal[3];
} LaiueAnimationSkinnedVertexV1;

typedef struct LaiueAnimationNameV1
{
    wchar_t name[LAIUE_CONTENT_NAME_CAPACITY];
    uint32_t active;
} LaiueAnimationNameV1;

typedef struct LaiueAnimationListV1
{
    LaiueAnimationNameV1 *entries;
    uint32_t count;
} LaiueAnimationListV1;

typedef struct LaiueAnimationServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t (*enumeratePacks)(LaiueContentCatalog *catalog, LaiueAnimationListV1 *outList);
    uint32_t (*activatePack)(LaiueContentCatalog *catalog, const wchar_t *name);
    uint32_t (*enumerateClips)(LaiueContentCatalog *catalog, LaiueAnimationListV1 *outList);
    void (*releaseList)(LaiueAnimationListV1 *list);
    /* Missing/broken pack resources yield an empty neutral placeholder with
     * the reason in outStatus. Unsafe resource names or OOM return NULL.
     * Explicit file/memory failure returns NULL, never a placeholder. */
    LaiueAnimationV1 *(*loadFrom)(LaiueContentCatalog *catalog, const wchar_t *name,
                                  uint32_t *outStatus);
    LaiueAnimationV1 *(*loadFile)(const wchar_t *path, uint32_t *outStatus);
    LaiueAnimationV1 *(*loadMemory)(const void *bytes, uint32_t sizeBytes, uint32_t *outStatus);
    uint32_t (*getView)(const LaiueAnimationV1 *clip, LaiueAnimationViewV1 *outView);
    uint32_t (*getChannel)(const LaiueAnimationV1 *clip, uint32_t channel,
                           LaiueAnimationChannelV1 *outChannel);
    void (*release)(LaiueAnimationV1 *clip);
    /* Clamp time, or wrap to [0,duration) when loop != 0. Negative time
     * wraps backwards; zero-duration clips always sample at zero.
     * LINEAR rotations use normalized shortest-arc lerp. No allocations. */
    uint32_t (*sampleChannel)(const LaiueAnimationV1 *clip, uint32_t channel, float time,
                              uint32_t loop, LaiueAnimationValueV1 *outValue);
    /* Caller owns all three arrays with jointCapacity entries. Local TRS
     * starts from bind pose; TRS channels replace individual components.
     * globals = parent * local, palette = globals * inverseBind.
     * Numeric overflow returns 0; output arrays are then unspecified. */
    uint32_t (*samplePose)(const LaiueAnimationV1 *clip, float time, uint32_t loop,
                           LaiueAnimationTransformV1 *locals, LaiueAnimationMatrixV1 *globals,
                           LaiueAnimationMatrixV1 *palette, uint32_t jointCapacity);
    /* No allocations. Outputs use weighted affine position transforms and
     * inverse-transpose normal transforms, then normalize normals. Validates
     * all bounds/finite values before writing any output. Input/output arrays
     * must not overlap (overlap is rejected). Zero vertices is a successful no-op. */
    uint32_t (*skinVertices)(const LaiueAnimationSkinVertexV1 *vertices, uint32_t vertexCount,
                             const LaiueAnimationMatrixV1 *palette, uint32_t jointCount,
                             LaiueAnimationSkinnedVertexV1 *outVertices, uint32_t vertexCapacity);
    uintptr_t reserved[8];
} LaiueAnimationServiceV1;

struct LaiueContentServiceV1;
LAIUE_ANIMATION_API const LaiueModuleApiV1 *LaiueAnimationGetStaticModuleApiV1(void);
LAIUE_ANIMATION_API const LaiueAnimationServiceV1 *LaiueAnimationGetStaticServiceV1(void);
/* Set before catalog operations; keep the provider alive until all users
 * stop. Module hosts install the optional provider at start and clear it
 * on stop. Only one hosted animation module instance may be active. */
LAIUE_ANIMATION_API void LaiueAnimationSetContentService(
    const struct LaiueContentServiceV1 *content);
