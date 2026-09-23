#pragma once

#include "mesh/chunk_mesher.h"

#include <stdint.h>

#define LAIUE_MESHER_SERVICE_NAME "laiue.mesher"
#define LAIUE_MESHER_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueMesherServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    ChunkMesherScratch *(*scratchCreate)(void);
    void (*scratchDestroy)(ChunkMesherScratch *scratch);
    bool (*buildChunkMesh)(World *world, ChunkMesherScratch *scratch,
                           int64_t chunkX, int64_t chunkY, int64_t chunkZ,
                           ChunkQuad **outQuads, uint32_t *outQuadCount);
} LaiueMesherServiceV1;
