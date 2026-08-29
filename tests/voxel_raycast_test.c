#include "scene/voxel_raycast.h"
#include "test_runtime.h"

static uint32_t raycastChecks;

static void RaycastExpect(bool condition, const char *name)
{
    ++raycastChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Voxel raycast check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool BlockEquals(const int64_t value[3], int64_t x, int64_t y, int64_t z)
{
    return value[0] == x && value[1] == y && value[2] == z;
}

LAIUE_TEST_ENTRY(VoxelRaycastTestEntryPoint)
{
    World *world = WorldCreate(NULL);
    RaycastExpect(world != NULL, "empty world was not created");

    const double origin[3] = {0.5, 0.5, 0.5};
    const float positiveX[3] = {1.0f, 0.0f, 0.0f};
    VoxelRaycastHit hit;
    RaycastExpect(!VoxelRaycast(world, origin, positiveX, 10.0f, &hit),
                  "empty world produced a hit");

    RaycastExpect(WorldTrySetBlock(world, 3, 0, 0, (BlockType)7U),
                  "positive target block was not created");
    RaycastExpect(VoxelRaycast(world, origin, positiveX, 10.0f, &hit) &&
                      BlockEquals(hit.block, 3, 0, 0) && BlockEquals(hit.previousBlock, 2, 0, 0) &&
                      hit.normal[0] == -1 && hit.normal[1] == 0 && hit.normal[2] == 0 &&
                      hit.distance == 2.5,
                  "positive-axis hit data is wrong");

    RaycastExpect(WorldTrySetBlock(world, -2, 0, 0, (BlockType)8U),
                  "negative target block was not created");
    const float negativeX[3] = {-1.0f, 0.0f, 0.0f};
    RaycastExpect(
        VoxelRaycast(world, origin, negativeX, 10.0f, &hit) && BlockEquals(hit.block, -2, 0, 0) &&
            BlockEquals(hit.previousBlock, -1, 0, 0) && hit.normal[0] == 1 && hit.distance == 1.5,
        "negative-axis hit data is wrong");

    const float invalidDirection[3] = {1.5f, 0.0f, 0.0f};
    RaycastExpect(
        !VoxelRaycast(world, origin, positiveX, 0.0f, &hit) &&
            !VoxelRaycast(world, origin, positiveX, VOXEL_RAYCAST_MAX_DISTANCE + 1.0f, &hit) &&
            !VoxelRaycast(world, origin, invalidDirection, 10.0f, &hit),
        "invalid ray parameters were accepted");

    WorldDestroy(world);
    LaiueTestRuntimeWrite("Voxel raycast tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
