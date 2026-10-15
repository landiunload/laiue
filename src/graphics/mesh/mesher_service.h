#pragma once

#include "mesh/chunk_mesher.h"
#include "mesh/surface_mesher.h"
#include "mod/module_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_MESHER_SERVICE_NAME "laiue.mesher"
#define LAIUE_MESHER_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueMesherServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    ChunkMesherScratch *(*scratchCreate)(void);
    void (*scratchDestroy)(ChunkMesherScratch *scratch);
    bool (*buildChunkMesh)(const ChunkMesherWorldSource *source,
                           ChunkMesherScratch *scratch,
                           int64_t chunkX, int64_t chunkY, int64_t chunkZ,
                           ChunkQuad **outQuads, uint32_t *outQuadCount);
    /* Optional tail: present when both the published size and structSize
     * reach LAIUE_MESHER_SERVICE_V1_SURFACE_SIZE. */
    uint32_t (*buildSurfaceMesh)(LaiueSurfaceMeshRequestV1 *request);
} LaiueMesherServiceV1;

#define LAIUE_MESHER_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueMesherServiceV1, buildSurfaceMesh))
#define LAIUE_MESHER_SERVICE_V1_SURFACE_SIZE ((uint32_t)sizeof(LaiueMesherServiceV1))

LAIUE_MESHER_API const LaiueMesherServiceV1 *LaiueMesherGetStaticServiceV1(void);
const LaiueModuleApiV1 *LaiueMesherGetStaticModuleApiV1(void);
