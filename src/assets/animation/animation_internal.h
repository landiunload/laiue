#pragma once

#include "animation/animation_service.h"
#include "media/animation.h"

struct LaiueAnimationV1
{
    AnimationData data;
    uint32_t flags;
};

uint32_t AnimationSampleChannel(const LaiueAnimationV1 *clip, uint32_t channel, float time,
                                uint32_t loop, LaiueAnimationValueV1 *outValue);
uint32_t AnimationSamplePose(const LaiueAnimationV1 *clip, float time, uint32_t loop,
                             LaiueAnimationTransformV1 *locals, LaiueAnimationMatrixV1 *globals,
                             LaiueAnimationMatrixV1 *palette, uint32_t jointCapacity);
uint32_t AnimationSkinVertices(const LaiueAnimationSkinVertexV1 *vertices, uint32_t vertexCount,
                               const LaiueAnimationMatrixV1 *palette, uint32_t jointCount,
                               LaiueAnimationSkinnedVertexV1 *outVertices, uint32_t vertexCapacity);
