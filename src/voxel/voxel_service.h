#pragma once

/* Service table for a renderer/physics-independent sparse voxel store. */

#include "mod/module_api.h"
#include "voxel/voxel_api.h"

#include <stdint.h>

#define LAIUE_VOXEL_SERVICE_NAME "laiue.voxel"
#define LAIUE_VOXEL_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueVoxelWorldV1 LaiueVoxelWorldV1;

typedef struct LaiueVoxelWorldConfigV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueVoxelBlockV1 defaultBlock;
    uintptr_t reserved[4];
} LaiueVoxelWorldConfigV1;

typedef uint32_t (*LaiueVoxelWorldCreateFn)(
    const LaiueVoxelWorldConfigV1 *config,
    LaiueVoxelWorldV1 **outWorld);
typedef void (*LaiueVoxelWorldDestroyFn)(LaiueVoxelWorldV1 *world);
typedef uint32_t (*LaiueVoxelWorldGetProviderFn)(
    LaiueVoxelWorldV1 *world,
    LaiueVoxelProviderV1 *outProvider);
typedef uint32_t (*LaiueVoxelWorldSetBlockFn)(
    LaiueVoxelWorldV1 *world,
    const LaiueVoxelCoordV1 *coordinate,
    const LaiueVoxelBlockV1 *block);
typedef uint64_t (*LaiueVoxelWorldGetRevisionFn)(
    const LaiueVoxelWorldV1 *world);

typedef struct LaiueVoxelServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueVoxelWorldCreateFn create;
    LaiueVoxelWorldDestroyFn destroy;
    LaiueVoxelWorldGetProviderFn getProvider;
    LaiueVoxelWorldSetBlockFn setBlock;
    LaiueVoxelWorldGetRevisionFn getRevision;
    uintptr_t reserved[8];
} LaiueVoxelServiceV1;

const LaiueModuleApiV1 *LaiueVoxelGetStaticModuleApiV1(void);
