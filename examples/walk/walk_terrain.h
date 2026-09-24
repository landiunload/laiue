#pragma once

#include "render/chunk_geometry.h"

/* The example owns its starter terrain. Keep the data definition in one
 * place so desktop and NativeActivity use the same visual/collision contract. */
static inline uint32_t WalkBuildTerrainQuads(ChunkQuad outQuads[5])
{
    if (outQuads == NULL)
        return 0u;
    outQuads[0] = PackChunkQuad(0u, 0u, 0u, 4u, 1u, 64u, 64u, 1u);
    outQuads[1] = PackChunkQuad(0u, 0u, 0u, 0u, 2u, 1u, 64u, 4u);
    outQuads[2] = PackChunkQuad(63u, 0u, 0u, 1u, 2u, 1u, 64u, 4u);
    outQuads[3] = PackChunkQuad(0u, 0u, 0u, 2u, 2u, 64u, 1u, 4u);
    outQuads[4] = PackChunkQuad(0u, 63u, 0u, 3u, 2u, 64u, 1u, 4u);
    return 5u;
}
