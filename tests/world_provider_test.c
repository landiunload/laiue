#include "world/world.h"
#include "test_runtime.h"

#include <limits.h>

typedef struct ProviderContext
{
    int64_t origin[3];
    uint32_t getCalls;
    uint32_t fillCalls;
    uint32_t rebaseCalls;
    bool rejectRebase;
} ProviderContext;

static uint32_t worldProviderChecks;

static void ProviderExpect(bool condition, const char *name)
{
    ++worldProviderChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("World provider check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0U;
    while (left[index] != L'\0' || right[index] != L'\0')
    {
        if (left[index] != right[index])
        {
            return false;
        }
        ++index;
    }
    return true;
}

static BlockType ProviderPattern(const ProviderContext *context, int64_t x, int64_t y, int64_t z)
{
    int64_t absoluteX = context->origin[0] + x;
    int64_t absoluteY = context->origin[1] + y;
    int64_t absoluteZ = context->origin[2] + z;
    int64_t sum = absoluteX + absoluteY * 3 + absoluteZ * 5;
    return sum % 4 == 0 ? BLOCK_AIR : (BlockType)9U;
}

static BlockType ProviderGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->getCalls;
    return ProviderPattern(context, x, y, z);
}

static WorldRegionContents ProviderFillRegion(void *rawContext, int64_t minBlockX,
                                              int64_t minBlockY, int64_t minBlockZ, int32_t sizeX,
                                              int32_t sizeY, int32_t sizeZ, BlockType *outBlocks)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->fillCalls;
    bool anyAir = false;
    bool anySolid = false;
    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                size_t index =
                    (((size_t)y * (size_t)sizeX) + (size_t)x) * (size_t)sizeZ + (size_t)z;
                BlockType block =
                    ProviderPattern(context, minBlockX + x, minBlockY + y, minBlockZ + z);
                outBlocks[index] = block;
                anyAir |= block == BLOCK_AIR;
                anySolid |= block != BLOCK_AIR;
            }
        }
    }
    if (anyAir && anySolid)
    {
        return WORLD_REGION_MIXED;
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}

static bool ProviderRebase(void *rawContext, int64_t blockShiftX, int64_t blockShiftY,
                           int64_t blockShiftZ)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->rebaseCalls;
    if (context->rejectRebase)
    {
        return false;
    }
    context->origin[0] += blockShiftX;
    context->origin[1] += blockShiftY;
    context->origin[2] += blockShiftZ;
    return true;
}

static void TestEmptyWorld(void)
{
    World *world = WorldCreate(NULL);
    ProviderExpect(world != NULL, "NULL provider did not create a world");
    ProviderExpect(WorldGetBlock(world, -9000000, 17, 9000000) == BLOCK_AIR,
                   "empty world returned a material");

    BlockType region[8];
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        region[index] = (BlockType)0xffU;
    }
    ProviderExpect(WorldFillRegion(world, -1, -1, -1, 2, 2, 2, region) == WORLD_REGION_ALL_AIR,
                   "empty region classification is not all-air");
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        ProviderExpect(region[index] == BLOCK_AIR, "empty provider did not clear the region");
    }

    BlockType guard = (BlockType)0x7fU;
    ProviderExpect(WorldFillRegion(world, 0, 0, 0, 1, 0, 1, &guard) == WORLD_REGION_ALL_AIR &&
                       guard == (BlockType)0x7fU,
                   "invalid region changed its output buffer");
    WorldDestroy(world);

    WorldBaseProvider invalid = {0};
    ProviderExpect(WorldCreate(&invalid) == NULL, "provider without getBlock was accepted");
}

static void TestProviderAndMutations(void)
{
    ProviderContext context = {0};
    WorldBaseProvider provider = {
        .context = &context,
        .getBlock = ProviderGetBlock,
        .fillRegion = ProviderFillRegion,
        .rebase = ProviderRebase,
    };
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "provider world was not created");

    BlockType base = ProviderPattern(&context, 1, 2, 3);
    ProviderExpect(WorldGetBlock(world, 1, 2, 3) == base && context.getCalls == 1U,
                   "getBlock provider was not used");
    ProviderExpect(WorldGetRevision(world) == 0U, "new world revision is not zero");

    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, (BlockType)255U) &&
                       WorldGetBlock(world, 1, 2, 3) == (BlockType)255U &&
                       WorldGetRevision(world) == 1U,
                   "application material override was not published");
    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, base) &&
                       WorldGetBlock(world, 1, 2, 3) == base && WorldGetRevision(world) == 2U,
                   "restoring the provider value failed");
    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, base) && WorldGetRevision(world) == 2U,
                   "no-op mutation advanced revision");

    BlockType region[8];
    WorldRegionContents contents = WorldFillRegion(world, -1, 2, 3, 2, 2, 2, region);
    ProviderExpect(context.fillCalls == 1U, "fillRegion provider was not used");
    bool anyAir = false;
    bool anySolid = false;
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 2; ++x)
        {
            for (int32_t z = 0; z < 2; ++z)
            {
                size_t index = (((size_t)y * 2U) + (size_t)x) * 2U + (size_t)z;
                BlockType expected = ProviderPattern(&context, -1 + x, 2 + y, 3 + z);
                ProviderExpect(region[index] == expected,
                               "fillRegion layout or provider value is wrong");
                anyAir |= expected == BLOCK_AIR;
                anySolid |= expected != BLOCK_AIR;
            }
        }
    }
    WorldRegionContents expectedContents =
        anyAir && anySolid ? WORLD_REGION_MIXED
                           : (anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR);
    ProviderExpect(contents == expectedContents, "filled region classification is wrong");

    const int64_t first[3] = {17, 4, -2};
    const int64_t second[3] = {CHUNK_SIZE + 17, 4, -2};
    BlockType firstBase = ProviderPattern(&context, first[0], first[1], first[2]);
    BlockType secondBase = ProviderPattern(&context, second[0], second[1], second[2]);
    WorldBlockMutation batch[2] = {
        {
            .block = {first[0], first[1], first[2]},
            .expected = firstBase,
            .replacement = (BlockType)21U,
        },
        {
            .block = {second[0], second[1], second[2]},
            .expected = secondBase,
            .replacement = (BlockType)22U,
        },
    };
    uint64_t beforeBatch = WorldGetRevision(world);
    ProviderExpect(WorldApplyBlockBatch(world, batch, 2U) &&
                       WorldGetRevision(world) == beforeBatch + 2U &&
                       WorldGetBlock(world, first[0], first[1], first[2]) == (BlockType)21U &&
                       WorldGetBlock(world, second[0], second[1], second[2]) == (BlockType)22U,
                   "atomic batch did not publish all values");

    WorldBlockMutation rejected[2] = {
        {
            .block = {first[0], first[1], first[2]},
            .expected = (BlockType)21U,
            .replacement = (BlockType)31U,
        },
        {
            .block = {second[0], second[1], second[2]},
            .expected = (BlockType)99U,
            .replacement = (BlockType)32U,
        },
    };
    uint64_t beforeRejected = WorldGetRevision(world);
    ProviderExpect(!WorldApplyBlockBatch(world, rejected, 2U) &&
                       WorldGetRevision(world) == beforeRejected &&
                       WorldGetBlock(world, first[0], first[1], first[2]) == (BlockType)21U &&
                       WorldGetBlock(world, second[0], second[1], second[2]) == (BlockType)22U,
                   "rejected batch changed world state");
    ProviderExpect(WorldApplyBlockBatch(world, NULL, 0U) &&
                       WorldGetRevision(world) == beforeRejected,
                   "empty batch was not a no-op");

    WorldDestroy(world);
}

static void TestRebaseAndFormatting(void)
{
    ProviderContext context = {0};
    WorldBaseProvider provider = {
        .context = &context,
        .getBlock = ProviderGetBlock,
        .fillRegion = ProviderFillRegion,
        .rebase = ProviderRebase,
    };
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "rebase world was not created");

    const int64_t original[3] = {5, 6, 7};
    ProviderExpect(WorldTrySetBlock(world, original[0], original[1], original[2], (BlockType)42U),
                   "global override was not created");
    wchar_t before[32];
    WorldFormatAbsoluteBlockCoordinate(world, 0, original[0], before, 32U);
    ProviderExpect(WideEquals(before, L"5"), "initial coordinate formatting is wrong");

    ProviderExpect(WorldRebase(world, CHUNK_SIZE, -2 * CHUNK_SIZE, 3 * CHUNK_SIZE) &&
                       context.rebaseCalls == 1U,
                   "chunk-aligned rebase failed");
    const int64_t rebased[3] = {
        original[0] - CHUNK_SIZE,
        original[1] + 2 * CHUNK_SIZE,
        original[2] - 3 * CHUNK_SIZE,
    };
    ProviderExpect(WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "override did not retain its absolute coordinate");
    ProviderExpect(ProviderPattern(&context, rebased[0] + 1, rebased[1], rebased[2]) ==
                       WorldGetBlock(world, rebased[0] + 1, rebased[1], rebased[2]),
                   "provider did not follow the rebased local origin");
    wchar_t after[32];
    WorldFormatAbsoluteBlockCoordinate(world, 0, rebased[0], after, 32U);
    ProviderExpect(WideEquals(after, L"5"), "absolute coordinate changed after rebase");

    uint32_t callsBeforeUnaligned = context.rebaseCalls;
    ProviderExpect(!WorldRebase(world, 1, 0, 0) && context.rebaseCalls == callsBeforeUnaligned &&
                       WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "unaligned rebase was not rejected atomically");

    context.rejectRebase = true;
    wchar_t beforeRejected[32];
    WorldFormatAbsoluteBlockCoordinate(world, 1, rebased[1], beforeRejected, 32U);
    uint64_t revisionBeforeRejected = WorldGetRevision(world);
    ProviderExpect(!WorldRebase(world, CHUNK_SIZE, 0, 0) &&
                       context.rebaseCalls == callsBeforeUnaligned + 1U &&
                       WorldGetRevision(world) == revisionBeforeRejected &&
                       WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "provider-rejected rebase changed world state");
    wchar_t afterRejected[32];
    WorldFormatAbsoluteBlockCoordinate(world, 1, rebased[1], afterRejected, 32U);
    ProviderExpect(WideEquals(beforeRejected, afterRejected),
                   "provider-rejected rebase changed coordinate origin");
    WorldDestroy(world);

    World *farWorld = WorldCreate(NULL);
    const int64_t hugeAlignedShift = INT64_MAX - (CHUNK_SIZE - 1);
    ProviderExpect(farWorld != NULL && WorldRebase(farWorld, hugeAlignedShift, 0, 0) &&
                       WorldRebase(farWorld, hugeAlignedShift, 0, 0) &&
                       WorldRebase(farWorld, hugeAlignedShift, 0, 0),
                   "infinite origin did not grow beyond 64 bits");
    wchar_t farText[32];
    WorldFormatAbsoluteBlockCoordinate(farWorld, 0, 0, farText, 32U);
    ProviderExpect(WideEquals(farText, L"~2^64"), "far coordinate formatting is wrong");
    WorldDestroy(farWorld);
}

LAIUE_TEST_ENTRY(WorldProviderTestEntryPoint)
{
    TestEmptyWorld();
    TestProviderAndMutations();
    TestRebaseAndFormatting();
    LaiueTestRuntimeWrite("World provider tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
