#include "animation/animation_internal.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(double) == 8u && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "animation normalization requires binary64 doubles");
_Static_assert(ANIMATION_MAX_JOINTS == LAIUE_ANIMATION_MAX_SKIN_JOINTS,
               "public and codec joint limits match");
_Static_assert(_Alignof(double) <= LAIUE_ANIMATION_SKIN_SCRATCH_ALIGNMENT,
               "the portable scratch alignment covers double matrices");

#include "animation_numeric.inc"
#define ANIMATION_SKIN_CACHED
#include "animation_skin_vertex.inc"
#undef ANIMATION_SKIN_CACHED

uint32_t AnimationSkinningScratchBytes(uint32_t jointCount)
{
    if (jointCount > ANIMATION_MAX_JOINTS)
        return 0u;
    /* A dense double matrix array followed by byte validity flags: no
     * per-joint padding and no allocations in either publication pass. */
    return jointCount * (9u * (uint32_t)sizeof(double) + (uint32_t)sizeof(uint8_t));
}

uint32_t AnimationSkinVerticesWithScratch(const LaiueAnimationSkinVertexV1 *vertices,
                                          uint32_t vertexCount,
                                          const LaiueAnimationMatrixV1 *palette,
                                          uint32_t jointCount,
                                          LaiueAnimationSkinnedVertexV1 *outVertices,
                                          uint32_t vertexCapacity, void *scratch,
                                          uint32_t scratchBytes)
{
    if (vertexCount > vertexCapacity || jointCount > ANIMATION_MAX_JOINTS)
        return 0u;
    if (vertexCount == 0u)
        return 1u;
    const uint32_t required = AnimationSkinningScratchBytes(jointCount);
    if (vertices == NULL || palette == NULL || outVertices == NULL || required == 0u ||
        scratch == NULL || scratchBytes < required ||
        ((uintptr_t)scratch & (LAIUE_ANIMATION_SKIN_SCRATCH_ALIGNMENT - 1u)) != 0u)
        return 0u;
    const uint64_t inputBytes = (uint64_t)vertexCount * sizeof(*vertices);
    const uint64_t outputBytes = (uint64_t)vertexCount * sizeof(*outVertices);
    const uint64_t paletteBytes = (uint64_t)jointCount * sizeof(*palette);
    if (Overlap(vertices, inputBytes, outVertices, outputBytes) ||
        Overlap(palette, paletteBytes, outVertices, outputBytes) ||
        Overlap(scratch, scratchBytes, vertices, inputBytes) ||
        Overlap(scratch, scratchBytes, palette, paletteBytes) ||
        Overlap(scratch, scratchBytes, outVertices, outputBytes))
        return 0u;
    for (uint32_t joint = 0u; joint < jointCount; ++joint)
        for (uint32_t component = 0u; component < 12u; ++component)
            if (!Finite(palette[joint].m[component]))
                return 0u;
    double *normalMatrices = (double *)scratch;
    uint8_t *normalValid = (uint8_t *)(normalMatrices + (size_t)(jointCount * 9u));
    for (uint32_t joint = 0u; joint < jointCount; ++joint)
        normalValid[joint] =
            (uint8_t)NormalMatrix(palette[joint].m, normalMatrices + (size_t)(joint * 9u));
    /* A singular unused joint is permitted, just as on the legacy path. */
    LaiueAnimationSkinnedVertexV1 temporary;
    for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
        if (!SkinOneCached(&vertices[vertex], palette, jointCount, normalMatrices, normalValid,
                           &temporary))
            return 0u;
    for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
        (void)SkinOneCached(&vertices[vertex], palette, jointCount, normalMatrices, normalValid,
                            &outVertices[vertex]);
    return 1u;
}
