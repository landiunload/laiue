#include "mod/mod_block_policy.h"

bool ModHostApplyBlockMutation(const ModHostBindings *bindings, int64_t x, int64_t y, int64_t z,
                               uint8_t block)
{
    if (bindings == NULL || bindings->world == NULL || block > BLOCK_GRASS)
    {
        return false;
    }

    bool changed = false;
    if (bindings->blockMutationPolicy == MOD_HOST_BLOCK_MUTATION_LOCAL)
    {
        WorldSetBlock(bindings->world, x, y, z, (BlockType)block);
        changed = WorldGetBlock(bindings->world, x, y, z) == (BlockType)block;
    }
    else if (bindings->blockMutationPolicy == MOD_HOST_BLOCK_MUTATION_CALLBACK)
    {
        changed = bindings->mutateBlock != NULL &&
                  bindings->mutateBlock(bindings->blockMutationContext, x, y, z, block);
    }
    else
    {
        // DENY и неизвестные будущие значения всегда fail-closed.
        return false;
    }

    if (changed && bindings->invalidateBlock != NULL)
    {
        bindings->invalidateBlock(bindings->invalidateContext, x, y, z);
    }
    return changed;
}
