#include "test_runtime.h"
#include "walk_runtime.h"

#include <stdbool.h>

typedef struct TestTerrain
{
    bool floor;
    bool wall;
    bool ceiling;
} TestTerrain;

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

    LAIUE_TEST_SUCCESS();
}
