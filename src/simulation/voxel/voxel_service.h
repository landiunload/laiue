#pragma once

/* Service table for a renderer/physics-independent sparse voxel store. */

#include "mod/module_api.h"
#include "voxel/voxel_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_VOXEL_SERVICE_NAME "laiue.voxel"
#define LAIUE_VOXEL_SERVICE_ABI_VERSION_1 1u
#define LAIUE_VOXEL_SERVICE_NAME_V2 "laiue.voxel.v2"
#define LAIUE_VOXEL_SERVICE_ABI_VERSION_2 2u

typedef struct LaiueVoxelWorldV1 LaiueVoxelWorldV1;

/* Move the world's local origin by whole blocks while preserving absolute
 * coordinates.  The call is transactional: a zero result leaves the world
 * unchanged. */
typedef uint32_t (*LaiueVoxelWorldRebaseFn)(
    LaiueVoxelWorldV1 *world, int64_t blockShiftX, int64_t blockShiftY,
    int64_t blockShiftZ);

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
/* New callers pass the provider context explicitly so the voxel module can
 * use the selected world technology without importing its DLL. */
typedef uint32_t (*LaiueVoxelWorldCreateWithContextFn)(
    void *serviceContext, const LaiueVoxelWorldConfigV1 *config,
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
    LaiueVoxelWorldCreateWithContextFn createWithContext;
    void *context;
    LaiueVoxelWorldRebaseFn rebase;
} LaiueVoxelServiceV1;

#define LAIUE_VOXEL_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueVoxelServiceV1, createWithContext))
#define LAIUE_VOXEL_SERVICE_V1_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueVoxelServiceV1, context) + sizeof(void *)))
#define LAIUE_VOXEL_SERVICE_V1_REBASE_OFFSET \
    ((uint32_t)offsetof(LaiueVoxelServiceV1, rebase))

/* The V2 service is a distinct name because its coordinate contract is not
 * layout-compatible with V1.  It shares the same opaque world instance and
 * lifecycle; only the coordinate/provider view is widened. */
typedef LaiueVoxelWorldV1 LaiueVoxelWorldV2;
typedef uint32_t (*LaiueVoxelWorldGetProviderV2Fn)(
    LaiueVoxelWorldV2 *world, LaiueVoxelProviderV2 *outProvider);
typedef uint32_t (*LaiueVoxelWorldSetBlockV2Fn)(
    LaiueVoxelWorldV2 *world, const LaiueVoxelCoordV2 *coordinate,
    const LaiueVoxelBlockV1 *block);

typedef struct LaiueVoxelServiceV2
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueVoxelWorldCreateWithContextFn createWithContext;
    LaiueVoxelWorldDestroyFn destroy;
    LaiueVoxelWorldGetProviderV2Fn getProvider;
    LaiueVoxelWorldSetBlockV2Fn setBlock;
    LaiueVoxelWorldGetRevisionFn getRevision;
    uintptr_t reserved[8];
    void *context;
    LaiueVoxelWorldRebaseFn rebase;
} LaiueVoxelServiceV2;

#define LAIUE_VOXEL_SERVICE_V2_REBASE_OFFSET \
    ((uint32_t)offsetof(LaiueVoxelServiceV2, rebase))

const LaiueModuleApiV1 *LaiueVoxelGetStaticModuleApiV1(void);
