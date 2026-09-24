#include "voxel/raycast.h"
#include "world/numeric_provider.h"
#include "test_runtime.h"

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Voxel raycast core check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Equal(const int64_t value[3], int64_t x, int64_t y, int64_t z)
{
    return value[0] == x && value[1] == y && value[2] == z;
}

static BlockType ReadWorld(void *context, int64_t x, int64_t y, int64_t z)
{
    return WorldGetBlock((World *)context, x, y, z);
}

LAIUE_TEST_ENTRY(VoxelRaycastCoreTestEntryPoint)
{
    WorldSetNumericService(LaiueNumericGetStaticServiceV1());
    World *world = WorldCreate(NULL);
    Expect(world != NULL, "empty world was not created");

    const double origin[3] = {0.5, 0.5, 0.5};
    const float positiveX[3] = {1.0f, 0.0f, 0.0f};
    VoxelRaycastHit hit;
    Expect(!VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX, 10.0f, &hit),
           "empty world produced a hit");
    Expect(WorldTrySetBlock(world, 3, 0, 0, (BlockType)7U),
           "positive target block was not created");
    Expect(VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX, 10.0f, &hit) &&
               Equal(hit.block, 3, 0, 0) && Equal(hit.previousBlock, 2, 0, 0) &&
               hit.normal[0] == -1 && hit.normal[1] == 0 && hit.normal[2] == 0 &&
               hit.distance == 2.5,
           "positive-axis hit data is wrong");

    const float negativeX[3] = {-1.0f, 0.0f, 0.0f};
    Expect(WorldTrySetBlock(world, -2, 0, 0, (BlockType)8U),
           "negative target block was not created");
    Expect(VoxelRaycastWithBlockQuery(world, ReadWorld, origin, negativeX, 10.0f, &hit) &&
               Equal(hit.block, -2, 0, 0) && Equal(hit.previousBlock, -1, 0, 0) &&
               hit.normal[0] == 1 && hit.distance == 1.5,
           "negative-axis hit data is wrong");

    const float invalidDirection[3] = {1.5f, 0.0f, 0.0f};
    Expect(!VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX, 0.0f, &hit) &&
               !VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX,
                             VOXEL_RAYCAST_MAX_DISTANCE + 1.0f, &hit) &&
               !VoxelRaycastWithBlockQuery(world, ReadWorld, origin, invalidDirection,
                                           10.0f, &hit),
           "invalid ray parameters were accepted");

    WorldDestroy(world);
    LaiueTestRuntimeWrite("Voxel raycast core tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
