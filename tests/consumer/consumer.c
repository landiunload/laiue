#include "world/world.h"

int main(void)
{
    World* world = WorldCreate(NULL);
    if (world == NULL)
    {
        return 1;
    }
    bool succeeded = WorldGetBlock(world, 0, 0, 0) == BLOCK_AIR
        && WorldTrySetBlock(world, 0, 0, 0, (BlockType)17U)
        && WorldGetBlock(world, 0, 0, 0) == (BlockType)17U;
    WorldDestroy(world);
    return succeeded ? 0 : 2;
}
