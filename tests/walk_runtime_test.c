#include "test_runtime.h"
#include "walk_runtime.h"

#include <stdbool.h>
#include <string.h>

typedef struct TestTerrain
{
    bool floor;
    bool wall;
    bool ceiling;
} TestTerrain;

typedef struct RebaseProbe
{
    LaiueCharacterPositionV1 position;
    bool worldAccepts;
    uint32_t worldCalls;
    uint32_t characterCalls;
} RebaseProbe;

static RebaseProbe g_rebaseProbe;

static uint32_t ProbeGetPosition(const LaiueCharacterControllerV1 *controller,
                                 LaiueCharacterPositionV1 *outPosition)
{
    (void)controller;
    if (outPosition == NULL)
        return 0u;
    *outPosition = g_rebaseProbe.position;
    return 1u;
}

static uint32_t ProbeCharacterRebase(LaiueCharacterControllerV1 *controller,
                                     int64_t cellDeltaX, int64_t cellDeltaY,
                                     int64_t localDeltaX, int64_t localDeltaY)
{
    (void)controller;
    ++g_rebaseProbe.characterCalls;
    g_rebaseProbe.position.cellX += cellDeltaX;
    g_rebaseProbe.position.cellY += cellDeltaY;
    g_rebaseProbe.position.localX += localDeltaX;
    g_rebaseProbe.position.localY += localDeltaY;
    return 1u;
}

static uint32_t ProbeVoxelRebase(LaiueVoxelWorldV1 *world,
                                 int64_t blockShiftX, int64_t blockShiftY,
                                 int64_t blockShiftZ)
{
    (void)world;
    (void)blockShiftX;
    (void)blockShiftY;
    (void)blockShiftZ;
    ++g_rebaseProbe.worldCalls;
    return g_rebaseProbe.worldAccepts ? 1u : 0u;
}

static uint32_t TestGetBlock(const LaiueVoxelProviderV1 *provider,
                             const LaiueVoxelCoordV1 *coordinate,
                             LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL)
        return 0u;
    const TestTerrain *terrain = (const TestTerrain *)provider->context;
    if (terrain == NULL)
        return 0u;
    bool solid = terrain->floor && coordinate->z == 0;
    solid = solid || (terrain->wall && coordinate->x == 3 &&
                      coordinate->z >= 0 && coordinate->z <= 1);
    solid = solid || (terrain->ceiling && coordinate->x == 0 &&
                      coordinate->y == 0 && coordinate->z == 3);
    outBlock->material = solid ? 1u : 0u;
    outBlock->flags = 0u;
    return 1u;
}

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static LaiueCharacterCollisionV1 MakeCollision(
    LaiueVoxelProviderV1 *provider)
{
    LaiueCharacterCollisionV1 collision = {
        .structSize = sizeof(collision),
        .abiVersion = LAIUE_CHARACTER_ABI_VERSION_1,
        .context = provider,
        .sweepAabb = WalkSweepAabb,
    };
    return collision;
}

LAIUE_TEST_ENTRY(WalkRuntimeTestEntryPoint)
{
    TestTerrain terrain = {.wall = true};
    LaiueVoxelProviderV1 provider = {
        .structSize = sizeof(provider),
        .abiVersion = LAIUE_VOXEL_ABI_VERSION_1,
        .context = &terrain,
        .getBlock = TestGetBlock,
    };
    LaiueCharacterCollisionV1 collision = MakeCollision(&provider);
    LaiueCharacterPositionV1 start = {
        .cellX = 0,
        .cellY = 0,
        .localX = 100,
        .localY = 500,
        .localZ = 1400,
    };
    LaiueCharacterPositionV1 result = {0};
    uint32_t grounded = 0u;
    Expect(WalkSweepAabb(&collision, &start, 400, 5000, 0, 0, &result,
                        &grounded) != 0u,
           "long horizontal sweep succeeds");
    Expect(result.localX == 2600 && grounded == 0u,
           "long horizontal sweep stops at the first wall instead of tunnelling");

    terrain = (TestTerrain){.ceiling = true};
    start.localX = 100;
    start.localY = 500;
    start.localZ = 1400;
    result = (LaiueCharacterPositionV1){0};
    Expect(WalkSweepAabb(&collision, &start, 400, 0, 0, 3000, &result,
                        &grounded) != 0u,
           "vertical ceiling sweep succeeds");
    Expect(result.localZ == 2600 && grounded == 0u,
           "vertical sweep resolves a ceiling contact");

    terrain = (TestTerrain){.floor = true};
    start.localZ = 1400;
    result = (LaiueCharacterPositionV1){0};
    Expect(WalkSweepAabb(&collision, &start, 400, 0, 0, -3000, &result,
                        &grounded) != 0u,
           "vertical floor sweep succeeds");
    Expect(result.localZ == 1400 && grounded != 0u,
           "vertical sweep resolves a floor contact and grounds the body");

    LaiueVoxelServiceV1 voxelService = {
        .structSize = sizeof(voxelService),
        .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        .rebase = ProbeVoxelRebase,
    };
    LaiueCharacterServiceV1 characterService = {
        .structSize = sizeof(characterService),
        .abiVersion = LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        .getPosition = ProbeGetPosition,
        .rebaseOrigin = ProbeCharacterRebase,
    };
    LaiueCharacterPositionV1 original = {
        .cellX = INT64_C(1) << 40,
        .cellY = -(INT64_C(1) << 39),
        .localX = 0,
        .localY = 0,
        .localZ = 1400,
    };
    g_rebaseProbe = (RebaseProbe){
        .position = original,
        .worldAccepts = false,
    };
    Expect(WalkRebaseWorldAndCharacter(
               &voxelService, sizeof(voxelService),
               (LaiueVoxelWorldV1 *)(uintptr_t)1u, &characterService,
               sizeof(characterService),
               (LaiueCharacterControllerV1 *)(uintptr_t)1u) == 0u,
           "world rebase rejection is reported");
    Expect(g_rebaseProbe.worldCalls == 1u && g_rebaseProbe.characterCalls == 2u &&
               memcmp(&g_rebaseProbe.position, &original, sizeof(original)) == 0,
           "failed world rebase rolls the character origin back");

    g_rebaseProbe = (RebaseProbe){
        .position = original,
        .worldAccepts = true,
    };
    Expect(WalkRebaseWorldAndCharacter(
               &voxelService, sizeof(voxelService),
               (LaiueVoxelWorldV1 *)(uintptr_t)1u, &characterService,
               sizeof(characterService),
               (LaiueCharacterControllerV1 *)(uintptr_t)1u) != 0u &&
               g_rebaseProbe.worldCalls == 1u && g_rebaseProbe.characterCalls == 1u &&
               g_rebaseProbe.position.cellX == 0 &&
               g_rebaseProbe.position.cellY == 0,
           "accepted rebase commits both sides once");

    g_rebaseProbe = (RebaseProbe){.position = original, .worldAccepts = true};
    Expect(WalkRebaseWorldAndCharacter(
               &voxelService, LAIUE_VOXEL_SERVICE_V1_REBASE_OFFSET,
               (LaiueVoxelWorldV1 *)(uintptr_t)1u, &characterService,
               sizeof(characterService),
               (LaiueCharacterControllerV1 *)(uintptr_t)1u) != 0u &&
               g_rebaseProbe.worldCalls == 0u && g_rebaseProbe.characterCalls == 0u,
           "short voxel tables do not call an absent rebase tail");

    LAIUE_TEST_SUCCESS();
}
