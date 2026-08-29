#include "content/content_format.h"
#include "mod/mod_manifest.h"
#include "mod/mod_types.h"
#include "physics/voxel_body.h"
#include "world/world.h"

#if defined(LAIUE_CONSUMER_HAS_GRAPHICS)
#include "render/shader_pack.h"
#endif

int main(void)
{
    World* world = WorldCreate(NULL);
    if (world == NULL)
    {
        return 1;
    }
    bool succeeded =
        WorldGetBlock(world, 0, 0, 0) == BLOCK_AIR &&
        WorldTrySetBlock(world, 0, 0, 0, (BlockType)17U) &&
        WorldGetBlock(world, 0, 0, 0) == (BlockType)17U && LaiueContentNameIsSafe(L"Example.lsp") &&
        LaiueModPackNameIsSafe(L"example.lmp") && LaiueModStatusString(LAIUE_MOD_STATUS_OK) != NULL;
    const VoxelBodyShape shape = {
        .radius = 0.3,
        .height = 1.8,
        .eyeHeight = 1.75,
        .collisionEpsilon = 0.001,
    };
    const double position[3] = {0.5, 0.5, 2.751};
    succeeded = succeeded && VoxelBodyLocalRangeIsResolved(position, &shape);
#if defined(LAIUE_CONSUMER_HAS_GRAPHICS)
    LaiueShaderSet shaderSet;
    LaiueShaderSetInitialize(&shaderSet);
    succeeded = succeeded && LaiueShaderSetIsValid(&shaderSet);
#endif
    WorldDestroy(world);
    return succeeded ? 0 : 2;
}
