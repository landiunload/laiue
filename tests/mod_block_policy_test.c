#include "mod/mod_block_policy.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct BlockPolicyProbe
{
    World *world;
    uint32_t mutationCalls;
    uint32_t invalidationCalls;
    bool acceptMutation;
} BlockPolicyProbe;

static uint32_t g_blockPolicyChecks;

static void BlockPolicyExpect(bool condition, const char *name)
{
    ++g_blockPolicyChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool ProbeMutation(void *context, int64_t x, int64_t y, int64_t z, uint8_t block)
{
    BlockPolicyProbe *probe = context;
    ++probe->mutationCalls;
    if (!probe->acceptMutation)
    {
        return false;
    }
    WorldSetBlock(probe->world, x, y, z, (BlockType)block);
    return WorldGetBlock(probe->world, x, y, z) == (BlockType)block;
}

static void ProbeInvalidation(void *context, int64_t x, int64_t y, int64_t z)
{
    BlockPolicyProbe *probe = context;
    (void)x;
    (void)y;
    (void)z;
    ++probe->invalidationCalls;
}

static uint8_t DifferentBlock(BlockType block)
{
    return block == BLOCK_AIR ? BLOCK_EARTH : BLOCK_AIR;
}

LAIUE_TEST_ENTRY(ModBlockPolicyTestEntryPoint)
{
    World *world = WorldCreate(INT64_C(0x102030405060708));
    BlockPolicyExpect(world != NULL, "policy test world creation");

    const int64_t denied[3] = {3, 7, 96};
    BlockType deniedBefore = WorldGetBlock(world, denied[0], denied[1], denied[2]);
    uint8_t deniedReplacement = DifferentBlock(deniedBefore);
    BlockPolicyProbe probe = {
        .world = world,
        .acceptMutation = true,
    };
    ModHostBindings bindings = {
        .world = world,
        .blockMutationPolicy = MOD_HOST_BLOCK_MUTATION_DENY,
        .blockMutationContext = &probe,
        .mutateBlock = ProbeMutation,
        .invalidateContext = &probe,
        .invalidateBlock = ProbeInvalidation,
    };
    BlockPolicyExpect(
        !ModHostApplyBlockMutation(&bindings, denied[0], denied[1], denied[2], deniedReplacement) &&
            WorldGetBlock(world, denied[0], denied[1], denied[2]) == deniedBefore &&
            probe.mutationCalls == 0U && probe.invalidationCalls == 0U,
        "deny policy changed the client world");

    const int64_t local[3] = {4, 7, 96};
    uint8_t localReplacement = DifferentBlock(WorldGetBlock(world, local[0], local[1], local[2]));
    bindings.blockMutationPolicy = MOD_HOST_BLOCK_MUTATION_LOCAL;
    BlockPolicyExpect(
        ModHostApplyBlockMutation(&bindings, local[0], local[1], local[2], localReplacement) &&
            WorldGetBlock(world, local[0], local[1], local[2]) == (BlockType)localReplacement &&
            probe.mutationCalls == 0U && probe.invalidationCalls == 1U,
        "local policy did not mutate offline world");

    const int64_t authoritative[3] = {5, 7, 96};
    uint8_t authoritativeReplacement =
        DifferentBlock(WorldGetBlock(world, authoritative[0], authoritative[1], authoritative[2]));
    bindings.blockMutationPolicy = MOD_HOST_BLOCK_MUTATION_CALLBACK;
    BlockPolicyExpect(ModHostApplyBlockMutation(&bindings, authoritative[0], authoritative[1],
                                                authoritative[2], authoritativeReplacement) &&
                          WorldGetBlock(world, authoritative[0], authoritative[1],
                                        authoritative[2]) == (BlockType)authoritativeReplacement &&
                          probe.mutationCalls == 1U && probe.invalidationCalls == 2U,
                      "callback policy bypassed authoritative callback");

    const int64_t rejected[3] = {6, 7, 96};
    BlockType rejectedBefore = WorldGetBlock(world, rejected[0], rejected[1], rejected[2]);
    probe.acceptMutation = false;
    BlockPolicyExpect(!ModHostApplyBlockMutation(&bindings, rejected[0], rejected[1], rejected[2],
                                                 DifferentBlock(rejectedBefore)) &&
                          WorldGetBlock(world, rejected[0], rejected[1], rejected[2]) ==
                              rejectedBefore &&
                          probe.mutationCalls == 2U && probe.invalidationCalls == 2U,
                      "rejected callback mutation invalidated or changed world");

    bindings.mutateBlock = NULL;
    BlockPolicyExpect(!ModHostApplyBlockMutation(&bindings, rejected[0], rejected[1], rejected[2],
                                                 DifferentBlock(rejectedBefore)),
                      "callback policy accepted a missing authority");

    bindings.blockMutationPolicy = (ModHostBlockMutationPolicy)99;
    BlockPolicyExpect(!ModHostApplyBlockMutation(&bindings, rejected[0], rejected[1], rejected[2],
                                                 DifferentBlock(rejectedBefore)),
                      "unknown policy was not fail-closed");

    bindings.blockMutationPolicy = MOD_HOST_BLOCK_MUTATION_LOCAL;
    BlockPolicyExpect(!ModHostApplyBlockMutation(&bindings, rejected[0], rejected[1], rejected[2],
                                                 (uint8_t)(BLOCK_GRASS + 1)),
                      "invalid block escaped policy validation");

    WorldDestroy(world);
    BlockPolicyExpect(g_blockPolicyChecks == 8U, "all mod block policy checks executed");
    LaiueTestRuntimeWrite("Mod block policy: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
