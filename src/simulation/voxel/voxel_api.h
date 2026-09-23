#pragma once

/* Optional sparse-world provider. This contract knows blocks and collision,
 * but never imports a renderer or a graphics/backend header. */

#include <stdint.h>

#define LAIUE_VOXEL_ABI_VERSION_1 1u

typedef struct LaiueVoxelCoordV1
{
    int64_t x;
    int64_t y;
    int32_t z;
} LaiueVoxelCoordV1;

typedef struct LaiueVoxelAabbV1
{
    LaiueVoxelCoordV1 minimum;
    LaiueVoxelCoordV1 maximum;
} LaiueVoxelAabbV1;

typedef struct LaiueVoxelBlockV1
{
    uint32_t material;
    uint32_t flags;
} LaiueVoxelBlockV1;

typedef struct LaiueVoxelMeshRangeV1
{
    uint64_t vertexOffset;
    uint64_t vertexCount;
    uint64_t indexOffset;
    uint64_t indexCount;
} LaiueVoxelMeshRangeV1;

typedef struct LaiueVoxelProviderV1 LaiueVoxelProviderV1;
typedef uint32_t (*LaiueVoxelGetBlockFn)(const LaiueVoxelProviderV1 *,
                                         const LaiueVoxelCoordV1 *,
                                         LaiueVoxelBlockV1 *outBlock);
typedef uint32_t (*LaiueVoxelGetBlockStateFn)(const LaiueVoxelProviderV1 *,
                                              const LaiueVoxelCoordV1 *,
                                              LaiueVoxelBlockV1 *outBlock,
                                              uint32_t *outExplicit);
typedef uint32_t (*LaiueVoxelEnumerateSolidFn)(const LaiueVoxelProviderV1 *,
                                               const LaiueVoxelAabbV1 *,
                                               uint32_t (*visitor)(void *,
                                                                   const LaiueVoxelCoordV1 *,
                                                                   const LaiueVoxelBlockV1 *),
                                               void *visitorContext);
typedef uint32_t (*LaiueVoxelBuildMeshFn)(const LaiueVoxelProviderV1 *, const LaiueVoxelAabbV1 *,
                                          void *vertexOutput, uint64_t vertexCapacity,
                                          void *indexOutput, uint64_t indexCapacity,
                                          LaiueVoxelMeshRangeV1 *outRange);

struct LaiueVoxelProviderV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void *context;
    LaiueVoxelGetBlockFn getBlock;
    LaiueVoxelGetBlockStateFn getBlockState;
    LaiueVoxelEnumerateSolidFn enumerateSolid;
    LaiueVoxelBuildMeshFn buildMesh;
    uintptr_t reserved[7];
};
