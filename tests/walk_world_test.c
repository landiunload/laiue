#include "test_runtime.h"
#include "walk_world.h"

#include <stdint.h>
#include <string.h>

typedef struct TestBlock
{
    int64_t x;
    int64_t y;
    int64_t z;
    uint8_t material;
} TestBlock;

typedef struct TestWorld
{
    TestBlock blocks[4];
    uint32_t count;
    TestBlock lastWrite;
    uint32_t writes;
} TestWorld;

static uint8_t GetBlock(void *opaque, int64_t x, int64_t y, int64_t z)
{
    TestWorld *world = (TestWorld *)opaque;
    for (uint32_t i = 0u; i < world->count; ++i)
        if (world->blocks[i].x == x && world->blocks[i].y == y &&
            world->blocks[i].z == z)
            return world->blocks[i].material;
    return 0u;
}

static bool SetBlock(void *opaque, int64_t x, int64_t y, int64_t z,
                     uint8_t material)
{
    TestWorld *world = (TestWorld *)opaque;
    world->lastWrite = (TestBlock){x, y, z, material};
    ++world->writes;
    return true;
}

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Walk world check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(WalkWorldTestEntryPoint)
{
    TestWorld world = {0};
    const int64_t farX = (INT64_C(1) << 54) + 320;
    const int64_t offset[3] = {farX, -(INT64_C(1) << 53), 7};
    world.blocks[0] = (TestBlock){farX + 5, offset[1], 7, 2u};
    world.count = 1u;
    const double localOrigin[3] = {0.5, 0.5, 0.5};
    const float forward[3] = {1.0f, 0.0f, 0.0f};
    WalkWorldBlockHit hit;
    Expect(WalkWorldRaycast(GetBlock, &world, localOrigin, offset, forward,
                            8.0f, &hit) && hit.block[0] == farX + 5,
           "raycast keeps sub-block precision at large world coordinates");

    memset(&world, 0, sizeof(world));
    world.blocks[0] = (TestBlock){4, 0, 0, 1u};
    world.blocks[1] = (TestBlock){3, 0, 0, 1u};
    world.count = 2u;
    const double boundaryOrigin[3] = {4.0, 0.5, 0.5};
    const float backward[3] = {-1.0f, 0.0f, 0.0f};
    const int64_t zeroOffset[3] = {0, 0, 0};
    Expect(WalkWorldRaycast(GetBlock, &world, boundaryOrigin, zeroOffset,
                            backward, 4.0f, &hit) && hit.block[0] == 3,
           "negative ray starts in the cell it enters at an exact boundary");

    memset(&world, 0, sizeof(world));
    world.blocks[0] = (TestBlock){1, 0, 0, 1u};
    world.blocks[1] = (TestBlock){1, 1, 0, 3u};
    world.count = 2u;
    const float diagonal[3] = {1.0f, 1.0f, 0.0f};
    Expect(WalkWorldRaycast(GetBlock, &world, localOrigin, zeroOffset,
                            diagonal, 2.0f, &hit) && hit.block[0] == 1 &&
               hit.block[1] == 1 && hit.previousBlock[0] == 0 &&
               hit.previousBlock[1] == 0,
           "corner crossings advance every tied axis together");
    Expect(WalkWorldEditHit(SetBlock, &world, &hit, true, 2u) &&
               world.lastWrite.x == 0 && world.lastWrite.y == 0 &&
               world.lastWrite.material == 2u,
           "placing writes the adjacent air cell");
    Expect(WalkWorldEditHit(SetBlock, &world, &hit, false, 0u) &&
               world.lastWrite.x == 1 && world.lastWrite.y == 1 &&
               world.lastWrite.material == 0u && world.writes == 2u,
           "breaking clears the hit cell");
    Expect(!WalkWorldEditHit(SetBlock, &world, &hit, true, 0u),
           "air cannot be selected as a placement material");
    LAIUE_TEST_SUCCESS();
}
