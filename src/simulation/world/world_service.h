#pragma once

#include "world/world.h"
#include "mod/module_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_WORLD_SERVICE_NAME "laiue.world"
#define LAIUE_WORLD_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueWorldServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    World *(*create)(const WorldBaseProvider *provider);
    void (*destroy)(World *world);
    bool (*rebase)(World *world, int64_t blockShiftX,
                   int64_t blockShiftY, int64_t blockShiftZ);
    BlockType (*getBlock)(World *world, int64_t x, int64_t y, int64_t z);
    bool (*trySetBlock)(World *world, int64_t x, int64_t y, int64_t z,
                        BlockType block);
    void (*setBlock)(World *world, int64_t x, int64_t y, int64_t z,
                     BlockType block);
    bool (*applyBlockBatch)(World *world, const WorldBlockMutation *mutations,
                            uint32_t count);
    uint64_t (*getRevision)(World *world);
    WorldRegionContents (*fillRegion)(World *world, int64_t minBlockX,
                                      int64_t minBlockY, int64_t minBlockZ,
                                      int32_t sizeX, int32_t sizeY,
                                      int32_t sizeZ, BlockType *outBlocks);
    bool (*getBlockState)(World *world, int64_t x, int64_t y, int64_t z,
                          BlockType *outBlock, bool *outExplicit);
    bool (*enumerateOverrides)(World *world, int64_t minimumX, int64_t minimumY,
                               int64_t minimumZ, int64_t maximumX,
                               int64_t maximumY, int64_t maximumZ,
                               WorldOverrideVisitor visitor, void *context);
    bool (*trySetBlockExplicit)(World *world, int64_t x, int64_t y, int64_t z,
                                BlockType block);
    /* Optional tail: binds World creation to the owning provider instance. */
    World *(*createWithContext)(void *moduleContext,
                                const WorldBaseProvider *provider);
    void *context;
} LaiueWorldServiceV1;

#define LAIUE_WORLD_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueWorldServiceV1, createWithContext))
#define LAIUE_WORLD_SERVICE_V1_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueWorldServiceV1, context) + sizeof(void *)))

LAIUE_WORLD_API const LaiueWorldServiceV1 *LaiueWorldGetStaticServiceV1(void);
LAIUE_WORLD_API const LaiueModuleApiV1 *LaiueWorldGetStaticModuleApiV1(void);
