#include "construct/physical_construct.h"
#include "interaction/voxel_interaction.h"
#include "interaction/voxel_raycast.h"
#include "test_runtime.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <string.h>

static void InteractionExpect(bool condition, const char* message)
{
    if (condition) return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(VoxelInteractionTestEntryPoint)
{
    World* world = WorldCreate(303);
    InteractionExpect(world != NULL, "interaction world create failed");
    PhysicalConstructSystem* constructs =
        PhysicalConstructSystemCreate(world, NULL);
    InteractionExpect(constructs != NULL,
        "interaction construct system create failed");

    const int64_t root[3] = { 10, 10, 200 };
    WorldSetBlock(world, root[0], root[1], root[2], BLOCK_EARTH);
    const double topOrigin[3] = { 10.5, 10.5, 204.0 };
    const float downward[3] = { 0.0f, 0.0f, -1.0f };
    VoxelEdit edit;
    InteractionExpect(VoxelInteractionTryCreateEdit(world, constructs,
            topOrigin, downward, NULL, 0U, false, true,
            VOXEL_INTERACTION_PHYSICS_LEVER_ITEM, 8.0f, &edit) &&
        edit.type == VOXEL_EDIT_ACTIVATE_LEVER &&
        edit.space == VOXEL_EDIT_SPACE_WORLD &&
        memcmp(edit.block, root, sizeof(root)) == 0 &&
        edit.mountNormal[0] == 0 && edit.mountNormal[1] == 0 &&
        edit.mountNormal[2] == 1,
        "lever placement did not target the static block face");

    uint64_t bodyId = 0U;
    InteractionExpect(PhysicalConstructActivateLever(constructs,
            edit.block, edit.mountNormal, NULL, 0U, &bodyId) ==
            PHYSICAL_CONSTRUCT_OK && bodyId != 0U,
        "lever edit did not create a physical body");

    VoxelSceneHit sceneHit;
    InteractionExpect(VoxelInteractionRaycastScene(world, constructs,
            topOrigin, downward, 8.0f, &sceneHit) &&
        sceneHit.kind == VOXEL_SCENE_HIT_LEVER_HANDLE &&
        sceneHit.bodyId == bodyId,
        "scene raycast did not prefer the lever handle");
    InteractionExpect(VoxelInteractionTryCreateEdit(world, constructs,
            topOrigin, downward, NULL, 0U, true, false,
            0U, 8.0f, &edit) && edit.type == VOXEL_EDIT_BREAK &&
        edit.space == VOXEL_EDIT_SPACE_CONSTRUCT &&
        edit.bodyId == bodyId && edit.localBlock[0] == 0 &&
        edit.localBlock[1] == 0 && edit.localBlock[2] == 0 &&
        edit.original == BLOCK_EARTH &&
        BlockGetProperties(edit.original).breakSeconds > 0.0f,
        "left click on the lever handle did not create a construct break");
    InteractionExpect(!VoxelInteractionTryCreateEdit(world, constructs,
            topOrigin, downward, NULL, 0U, false, true,
            BLOCK_GRASS, 8.0f, &edit),
        "right click placement was allowed through the lever handle");

    const double baseOrigin[3] = { 7.0, 10.5, 201.0625 };
    const float right[3] = { 1.0f, 0.0f, 0.0f };
    InteractionExpect(VoxelInteractionRaycastScene(world, constructs,
            baseOrigin, right, 8.0f, &sceneHit) &&
        sceneHit.kind == VOXEL_SCENE_HIT_LEVER_BASE &&
        sceneHit.bodyId == bodyId,
        "scene raycast did not distinguish the lever base");
    InteractionExpect(VoxelInteractionTryCreateEdit(world, constructs,
            baseOrigin, right, NULL, 0U, true, false,
            0U, 8.0f, &edit) && edit.type == VOXEL_EDIT_BREAK &&
        edit.space == VOXEL_EDIT_SPACE_CONSTRUCT && edit.bodyId == bodyId &&
        edit.original == BLOCK_EARTH &&
        BlockGetProperties(edit.original).breakSeconds > 0.0f,
        "left click on the lever base did not create a construct break");
    VoxelEdit leverBreakEdit = edit;
    InteractionExpect(!VoxelInteractionTryCreateEdit(world, constructs,
            baseOrigin, right, NULL, 0U, false, true,
            BLOCK_GRASS, 8.0f, &edit),
        "lever base incorrectly entered the handle grab/place path");

    const double sideOrigin[3] = { 7.0, 10.5, 200.5 };
    InteractionExpect(VoxelInteractionTryCreateEdit(world, constructs,
            sideOrigin, right, NULL, 0U, false, true,
            BLOCK_GRASS, 8.0f, &edit) &&
        edit.type == VOXEL_EDIT_PLACE &&
        edit.space == VOXEL_EDIT_SPACE_CONSTRUCT &&
        edit.bodyId == bodyId,
        "block placement did not target construct-local coordinates");

    PhysicalConstructBodyState state;
    bool truncated = true;
    InteractionExpect(PhysicalConstructCopyBodyStates(constructs,
            &state, 1U, &truncated) == 1U && !truncated,
        "construct state copy failed");
    PhysicalConstructAabb blocker = {
        .minimum = {
            state.origin[0] + edit.localBlock[0],
            state.origin[1] + edit.localBlock[1],
            state.origin[2] + edit.localBlock[2],
        },
        .maximum = {
            state.origin[0] + edit.localBlock[0] + 1.0,
            state.origin[1] + edit.localBlock[1] + 1.0,
            state.origin[2] + edit.localBlock[2] + 1.0,
        },
    };
    InteractionExpect(PhysicalConstructPlaceBlock(constructs, bodyId,
            edit.localBlock, edit.replacement, &blocker, 1U) ==
            PHYSICAL_CONSTRUCT_COLLISION,
        "construct placement ignored an external collider");
    InteractionExpect(PhysicalConstructPlaceBlock(constructs, bodyId,
            edit.localBlock, edit.replacement, NULL, 0U) ==
            PHYSICAL_CONSTRUCT_OK,
        "collision-free construct placement was rejected");

    InteractionExpect(!VoxelInteractionTryCreateEdit(world, constructs,
            sideOrigin, right, NULL, 0U, false, true,
            VOXEL_INTERACTION_PHYSICS_LEVER_ITEM, 8.0f, &edit),
        "a second lever was attached to a physical body");

    InteractionExpect(PhysicalConstructCopyBodyStates(constructs,
            &state, 1U, &truncated) == 1U && !truncated,
        "moving placement state copy failed");
    state.origin[0] += 0.375;
    state.origin[1] -= 0.25;
    state.origin[2] += 0.125;
    state.velocity[0] = 0.0;
    state.velocity[1] = 0.0;
    state.velocity[2] = 0.0;
    InteractionExpect(PhysicalConstructApplyBodyMotion(constructs, &state),
        "moving placement transform apply failed");
    const double movingSideOrigin[3] = {
        state.origin[0] - 3.0,
        state.origin[1] + 0.5,
        state.origin[2] - 0.5,
    };
    InteractionExpect(VoxelInteractionTryCreateEdit(world, constructs,
            movingSideOrigin, right, NULL, 0U, false, true,
            BLOCK_EARTH, 8.0f, &edit) &&
        edit.type == VOXEL_EDIT_PLACE &&
        edit.space == VOXEL_EDIT_SPACE_CONSTRUCT &&
        edit.bodyId == bodyId && edit.localBlock[0] == -2 &&
        edit.localBlock[1] == 0 && edit.localBlock[2] == -1,
        "fractional moving construct lost its local face normal");
    blocker = (PhysicalConstructAabb){
        .minimum = {
            state.origin[0] + edit.localBlock[0],
            state.origin[1] + edit.localBlock[1],
            state.origin[2] + edit.localBlock[2],
        },
        .maximum = {
            state.origin[0] + edit.localBlock[0] + 1.0,
            state.origin[1] + edit.localBlock[1] + 1.0,
            state.origin[2] + edit.localBlock[2] + 1.0,
        },
    };
    InteractionExpect(PhysicalConstructPlaceBlock(constructs, bodyId,
            edit.localBlock, edit.replacement, &blocker, 1U) ==
            PHYSICAL_CONSTRUCT_COLLISION,
        "moving construct placement ignored a fractional external collision");
    InteractionExpect(PhysicalConstructPlaceBlock(constructs, bodyId,
            edit.localBlock, edit.replacement, NULL, 0U) ==
            PHYSICAL_CONSTRUCT_OK,
        "collision-free moving construct placement was rejected");

    PhysicalConstructBlock removedLever;
    uint64_t remainingBodyId = 0U;
    uint32_t remainingBodyCount = 0U;
    InteractionExpect(PhysicalConstructBreakBlock(constructs,
            leverBreakEdit.bodyId, leverBreakEdit.localBlock,
            &removedLever, &remainingBodyId, 1U,
            &remainingBodyCount) == PHYSICAL_CONSTRUCT_OK &&
        removedLever.kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER &&
        remainingBodyCount == 1U && remainingBodyId == bodyId,
        "lever break edit did not remove the construct attachment");

    const double outOfRangeOrigin[3] = {
        9223372036854775808.0, 0.0, 0.0,
    };
    VoxelRaycastHit rayHit;
    InteractionExpect(!VoxelRaycast(world, outOfRangeOrigin, right,
            8.0f, &rayHit),
        "raycast accepted an origin outside the int64 world");
    InteractionExpect(!VoxelRaycast(world, sideOrigin, right,
            2048.0f, &rayHit),
        "raycast accepted an unbounded traversal distance");

    union { uint32_t bits; float value; } floatNan = { 0x7fc00000U };
    union { uint64_t bits; double value; } doubleNan = {
        UINT64_C(0x7ff8000000000000),
    };
    const float invalidDirection[3] = { floatNan.value, 0.0f, 0.0f };
    const double invalidOrigin[3] = { doubleNan.value, 0.0, 0.0 };
    InteractionExpect(!VoxelInteractionRaycastScene(world, constructs,
            sideOrigin, invalidDirection, 8.0f, &sceneHit) &&
        !VoxelInteractionRaycastScene(world, constructs,
            sideOrigin, right, floatNan.value, &sceneHit) &&
        !VoxelInteractionRaycastScene(world, constructs,
            invalidOrigin, right, 8.0f, &sceneHit),
        "scene raycast accepted NaN input");
    PhysicalConstructAabb invalidBlocker = blocker;
    invalidBlocker.minimum[0] = doubleNan.value;
    InteractionExpect(!VoxelInteractionTryCreateEdit(world, constructs,
            sideOrigin, right, &invalidBlocker, 1U, false, true,
            BLOCK_EARTH, 8.0f, &edit) &&
        !VoxelInteractionTryCreateEdit(world, constructs,
            sideOrigin, right, &blocker,
            PHYSICAL_CONSTRUCT_MAX_DYNAMIC_BLOCKERS + 1U,
            false, true, BLOCK_EARTH, 8.0f, &edit),
        "interaction accepted invalid blocker bounds or capacity");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);

    world = WorldCreate(304);
    InteractionExpect(world != NULL, "tie world create failed");
    constructs = PhysicalConstructSystemCreate(world, NULL);
    InteractionExpect(constructs != NULL, "tie construct create failed");
    WorldSetBlock(world, 0, 0, 200, BLOCK_GRASS);
    PhysicalConstructBlock tiedBlock = {
        .local = { 0, 0, 0 },
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    PhysicalConstructBodyState tiedState = {
        .id = 1U,
        .topologyRevision = 1U,
        .origin = { 0.0, 0.0, 200.0 },
        .blockCount = 1U,
    };
    InteractionExpect(PhysicalConstructImportBody(constructs,
            &tiedState, &tiedBlock, 1U) == PHYSICAL_CONSTRUCT_OK,
        "tie construct import failed");
    const double tieOrigin[3] = { -2.0, 0.5, 200.5 };
    InteractionExpect(VoxelInteractionRaycastScene(world, constructs,
            tieOrigin, right, 8.0f, &sceneHit) &&
        sceneHit.kind == VOXEL_SCENE_HIT_WORLD &&
        sceneHit.material == BLOCK_GRASS && sceneHit.distance == 2.0,
        "equal-distance world/construct tie is not world-first");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
    LaiueTestRuntimeWrite("Voxel construct interaction: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
