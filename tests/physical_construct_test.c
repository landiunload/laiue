#include "construct/physical_construct.h"
#include "test_runtime.h"
#include "world/world.h"

static uint32_t g_checks;
static PhysicalConstructBlock g_parent_blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
static PhysicalConstructBlock g_import_blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
static PhysicalConstructBodyState g_states[PHYSICAL_CONSTRUCT_MAX_BODIES];
static PhysicalConstructBodyState g_replica_states[PHYSICAL_CONSTRUCT_MAX_BODIES];
static uint64_t g_split_ids[PHYSICAL_CONSTRUCT_MAX_BODIES];

static void Expect(bool condition, const char *name)
{
    ++g_checks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Проверка не пройдена: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(double left, double right, double epsilon)
{
    double difference = left - right;
    if (difference < 0.0)
    {
        difference = -difference;
    }
    return difference <= epsilon;
}

static uint32_t FindLever(const PhysicalConstructBlock *blocks, uint32_t count)
{
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (blocks[index].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
        {
            return index;
        }
    }
    return UINT32_MAX;
}

static void TestLeverModelAndChunks(void)
{
    const int8_t normal[3] = {0, 0, 1};
    PhysicalConstructModelPart model[PHYSICAL_CONSTRUCT_MODEL_PARTS];
    Expect(PhysicalConstructGetLeverModel(normal, model), "lever model");
    Expect(Near(model[0].localBounds.minimum[2], 0.0, 0.0) &&
               Near(model[0].localBounds.maximum[2], 2.0 / 16.0, 0.0) &&
               Near(model[1].localBounds.minimum[0], 7.0 / 16.0, 0.0) &&
               Near(model[2].localBounds.maximum[0], 10.0 / 16.0, 0.0) &&
               model[0].hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_BASE &&
               model[2].hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE,
           "lever geometry is shared sixteenth grid");

    static const int8_t normals[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
        {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    for (uint32_t direction = 0U; direction < 6U; ++direction)
    {
        Expect(PhysicalConstructGetLeverModel(normals[direction], model),
               "lever model supports every mount face");
        for (uint32_t part = 0U; part < PHYSICAL_CONSTRUCT_MODEL_PARTS; ++part)
        {
            for (uint32_t axis = 0U; axis < 3U; ++axis)
            {
                Expect(model[part].localBounds.minimum[axis] >= 0.0 &&
                           model[part].localBounds.maximum[axis] <= 1.0 &&
                           model[part].localBounds.minimum[axis] <
                               model[part].localBounds.maximum[axis],
                       "lever model remains visible inside its local cell");
            }
        }
    }

    const int32_t local[3] = {-1, -64, 64};
    int32_t chunk[3];
    uint32_t inChunk[3];
    PhysicalConstructLocalToChunk(local, chunk, inChunk);
    Expect(chunk[0] == -1 && inChunk[0] == 63U && chunk[1] == -1 && inChunk[1] == 0U &&
               chunk[2] == 1 && inChunk[2] == 0U,
           "negative local chunk floor division");
}

static void TestNaturalRootDoesNotCaptureTerrain(void)
{
    World *world = WorldCreate(101);
    Expect(world != NULL, "natural world create");
    PhysicalConstructSystem *constructs = PhysicalConstructSystemCreate(world, NULL);
    Expect(constructs != NULL, "natural construct system create");

    int64_t root[3] = {0, 0, WorldGetTerrainHeight(world, 0, 0)};
    const int8_t normal[3] = {0, 0, 1};
    uint64_t bodyId = 0U;
    Expect(PhysicalConstructActivateLever(constructs, root, normal, NULL, 0U, &bodyId) ==
               PHYSICAL_CONSTRUCT_OK,
           "activate lever on natural root");
    PhysicalConstructBodyState state;
    bool truncated = true;
    Expect(PhysicalConstructCopyBodyStates(constructs, &state, 1U, &truncated) == 1U &&
               !truncated && state.id == bodyId && state.blockCount == 2U,
           "natural terrain capture is root plus lever only");
    Expect(WorldGetBlock(world, root[0], root[1], root[2]) == BLOCK_AIR,
           "captured natural root removed from world");
    Expect(WorldGetBlock(world, root[0] + 1, root[1], root[2]) != BLOCK_AIR,
           "natural neighbour remains static");

    PhysicalConstructRaycastHit boundaryHit;
    const double boundaryOrigin[3] = {
        (double)root[0] + 1.0, (double)root[1] + 0.5,
        (double)root[2] + 0.5,
    };
    const double inward[3] = {-1.0, 0.0, 0.0};
    Expect(PhysicalConstructRaycast(constructs, boundaryOrigin, inward,
                                    2.0, &boundaryHit) &&
               Near(boundaryHit.distance, 0.0, 0.0) &&
               boundaryHit.hitPart == PHYSICAL_CONSTRUCT_HIT_VOXEL &&
               boundaryHit.normal[0] == 1 && boundaryHit.normal[1] == 0 &&
               boundaryHit.normal[2] == 0,
           "construct boundary ray reports the entering face normal");
    const double insideOrigin[3] = {
        (double)root[0] + 0.5, (double)root[1] + 0.5,
        (double)root[2] + 0.5,
    };
    const double outward[3] = {1.0, 0.0, 0.0};
    Expect(PhysicalConstructRaycast(constructs, insideOrigin, outward,
                                    2.0, &boundaryHit) &&
               Near(boundaryHit.distance, 0.0, 0.0) &&
               boundaryHit.normal[0] == 1,
           "construct inside ray reports its first exit face normal");
    const double zeroDirection[3] = {0.0, 0.0, 0.0};
    Expect(!PhysicalConstructRaycast(constructs, insideOrigin, zeroDirection,
                                     2.0, &boundaryHit),
           "construct raycast rejects a zero direction");

    double initialOriginZ = state.origin[2];
    PhysicalConstructSimulateFixedSteps(constructs, 240U);
    Expect(PhysicalConstructCopyBodyStates(constructs, &state, 1U, &truncated) == 1U &&
               state.origin[2] <= initialOriginZ && state.origin[2] >= initialOriginZ - 0.01 &&
               state.velocity[2] == 0.0,
           "fixed-step gravity rests on static voxel without tunnelling");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

static void TestMultiwaySplitCapacityAndAtomicity(void)
{
    World *world = WorldCreate(406);
    Expect(world != NULL, "multiway split world create");
    PhysicalConstructSystem *constructs = PhysicalConstructSystemCreate(world, NULL);
    Expect(constructs != NULL, "multiway split system create");

    PhysicalConstructBlock cross[7] = {
        {.local = {0, 0, 0}, .material = BLOCK_EARTH,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {-1, 0, 0}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {1, 0, 0}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {0, -1, 0}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {0, 1, 0}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {0, 0, -1}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
        {.local = {0, 0, 1}, .material = BLOCK_GRASS,
         .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL},
    };
    PhysicalConstructBodyState crossState = {
        .id = 41U,
        .topologyRevision = 5U,
        .origin = {100.0, 200.0, 3000.0},
        .velocity = {1.0, -2.0, 0.5},
        .blockCount = 7U,
    };
    Expect(PhysicalConstructImportBody(constructs, &crossState, cross, 7U) ==
               PHYSICAL_CONSTRUCT_OK,
           "import six-way split topology");

    PhysicalConstructBlock filler = {
        .local = {0, 0, 0},
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    for (uint64_t id = 100U; id <= 110U; ++id)
    {
        PhysicalConstructBodyState fillerState = {
            .id = id,
            .topologyRevision = 1U,
            .origin = {(double)id * 2.0, 500.0, 5000.0},
            .blockCount = 1U,
        };
        Expect(PhysicalConstructImportBody(constructs, &fillerState,
                                           &filler, 1U) == PHYSICAL_CONSTRUCT_OK,
               "fill body capacity before multiway split");
    }

    const int32_t center[3] = {0, 0, 0};
    uint32_t splitCount = UINT32_MAX;
    Expect(PhysicalConstructBreakBlock(constructs, 41U, center, NULL,
                                       g_split_ids, PHYSICAL_CONSTRUCT_MAX_BODIES,
                                       &splitCount) == PHYSICAL_CONSTRUCT_CAPACITY &&
               splitCount == 0U,
           "multiway split rejects insufficient body capacity");
    uint32_t unchangedCount = 0U;
    Expect(PhysicalConstructCopyBlocks(constructs, 41U, g_parent_blocks,
                                       PHYSICAL_CONSTRUCT_MAX_BLOCKS,
                                       &unchangedCount) && unchangedCount == 7U,
           "failed multiway split is atomic");

    Expect(PhysicalConstructRemoveBody(constructs, 100U) &&
               PhysicalConstructRemoveBody(constructs, 101U),
           "release capacity for multiway split");
    splitCount = 0U;
    PhysicalConstructBlock removed;
    Expect(PhysicalConstructBreakBlock(constructs, 41U, center, &removed,
                                       g_split_ids, PHYSICAL_CONSTRUCT_MAX_BODIES,
                                       &splitCount) == PHYSICAL_CONSTRUCT_OK &&
               splitCount == 6U && g_split_ids[0] == 41U &&
               removed.material == BLOCK_EARTH,
           "center removal creates all six components");

    bool truncated = true;
    uint32_t stateCount = PhysicalConstructCopyBodyStates(
        constructs, g_states, PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    bool matched[6] = {false, false, false, false, false, false};
    static const double expectedOrigins[6][3] = {
        {99.0, 200.0, 3000.0}, {101.0, 200.0, 3000.0},
        {100.0, 199.0, 3000.0}, {100.0, 201.0, 3000.0},
        {100.0, 200.0, 2999.0}, {100.0, 200.0, 3001.0},
    };
    for (uint32_t result = 0U; result < splitCount; ++result)
    {
        for (uint32_t index = 0U; index < stateCount; ++index)
        {
            if (g_states[index].id != g_split_ids[result]) continue;
            PhysicalConstructBlock onlyBlock;
            uint32_t onlyCount = 0U;
            Expect(g_states[index].blockCount == 1U &&
                       PhysicalConstructCopyBlocks(constructs,
                           g_states[index].id, &onlyBlock, 1U, &onlyCount) &&
                       onlyCount == 1U && onlyBlock.local[0] == 0 &&
                       onlyBlock.local[1] == 0 && onlyBlock.local[2] == 0,
                   "split component receives a bounded local origin");
            for (uint32_t expected = 0U; expected < 6U; ++expected)
            {
                if (!matched[expected] &&
                    Near(g_states[index].origin[0], expectedOrigins[expected][0], 0.0) &&
                    Near(g_states[index].origin[1], expectedOrigins[expected][1], 0.0) &&
                    Near(g_states[index].origin[2], expectedOrigins[expected][2], 0.0))
                {
                    matched[expected] = true;
                    break;
                }
            }
            break;
        }
    }
    bool allMatched = !truncated;
    for (uint32_t expected = 0U; expected < 6U; ++expected)
        allMatched = allMatched && matched[expected];
    Expect(allMatched, "split components preserve every world transform");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

static void TestActivationPlacementRaycastGrabAndSplit(void)
{
    World *world = WorldCreate(202);
    Expect(world != NULL, "edited world create");
    PhysicalConstructSystem *constructs = PhysicalConstructSystemCreate(world, NULL);
    Expect(constructs != NULL, "edited construct system create");

    const int64_t root[3] = {10, 10, 200};
    WorldSetBlock(world, 10, 10, 200, BLOCK_EARTH);
    WorldSetBlock(world, 11, 10, 200, BLOCK_GRASS);
    WorldSetBlock(world, 12, 10, 200, BLOCK_EARTH);
    const int8_t normal[3] = {0, 0, 1};
    uint64_t parentId = 0U;
    Expect(PhysicalConstructActivateLever(constructs, root, normal, NULL, 0U, &parentId) ==
               PHYSICAL_CONSTRUCT_OK,
           "activate edited line");

    PhysicalConstructBodyState parentState;
    bool truncated = true;
    Expect(PhysicalConstructCopyBodyStates(constructs, &parentState, 1U, &truncated) == 1U &&
               !truncated && parentState.blockCount == 4U && parentState.grabOwner == 0U,
           "body count includes lever attachment");
    uint32_t parentBlockCount = 0U;
    Expect(PhysicalConstructCopyBlocks(constructs, parentId, g_parent_blocks,
                                       PHYSICAL_CONSTRUCT_MAX_BLOCKS, &parentBlockCount) &&
               parentBlockCount == 4U,
           "copy activated blocks");
    uint32_t leverIndex = FindLever(g_parent_blocks, parentBlockCount);
    Expect(leverIndex != UINT32_MAX && g_parent_blocks[leverIndex].local[0] == 0 &&
               g_parent_blocks[leverIndex].local[1] == 0 &&
               g_parent_blocks[leverIndex].local[2] == 0,
           "lever is local origin");
    Expect(WorldGetBlock(world, 10, 10, 200) == BLOCK_AIR &&
               WorldGetBlock(world, 11, 10, 200) == BLOCK_AIR &&
               WorldGetBlock(world, 12, 10, 200) == BLOCK_AIR,
           "captured edited component removed");
    Expect(PhysicalConstructWorldCellOccupied(constructs, 10, 10, 201) &&
               PhysicalConstructWorldCellOccupied(constructs, 10, 10, 200),
           "composite cell query sees lever and voxel");

    const double rayOrigin[3] = {10.5, 10.5, 203.0};
    const double rayDirection[3] = {0.0, 0.0, -1.0};
    PhysicalConstructRaycastHit hit;
    Expect(PhysicalConstructRaycast(constructs, rayOrigin, rayDirection, 8.0, &hit) &&
               hit.bodyId == parentId && hit.blockKind == PHYSICAL_CONSTRUCT_BLOCK_LEVER &&
               hit.hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE,
           "raycast selects lever handle model");

    const int32_t placedLocal[3] = {-1, 0, -1};
    PhysicalConstructAabb blocker = {
        .minimum = {9.1, 10.1, 200.1},
        .maximum = {9.9, 10.9, 200.9},
    };
    Expect(PhysicalConstructPlaceBlock(constructs, parentId, placedLocal,
                                       (uint8_t)(BLOCK_GRASS + 1U), NULL, 0U) ==
               PHYSICAL_CONSTRUCT_INVALID_ARGUMENT,
           "placement rejects unknown construct material");
    Expect(PhysicalConstructPlaceBlock(constructs, parentId, placedLocal, BLOCK_GRASS, &blocker,
                                       1U) == PHYSICAL_CONSTRUCT_COLLISION,
           "placement rejects external collision");
    Expect(PhysicalConstructPlaceBlock(constructs, parentId, placedLocal, BLOCK_GRASS, NULL, 0U) ==
               PHYSICAL_CONSTRUCT_OK,
           "placement on physical model");

    const int32_t collidingLocal[3] = {0, 0, -2};
    WorldSetBlock(world, 10, 10, 199, BLOCK_EARTH);
    Expect(PhysicalConstructPlaceBlock(constructs, parentId, collidingLocal, BLOCK_GRASS, NULL,
                                       0U) == PHYSICAL_CONSTRUCT_COLLISION,
           "placement rejects static world collision");
    WorldSetBlock(world, 10, 10, 199, BLOCK_AIR);

    const double grabTarget[3] = {14.5, 10.5, 203.0};
    Expect(PhysicalConstructBeginGrab(constructs, parentId, 77U, grabTarget) ==
               PHYSICAL_CONSTRUCT_OK,
           "begin handle grab");
    PhysicalConstructStep(constructs);
    Expect(PhysicalConstructCopyBodyStates(constructs, &parentState, 1U, &truncated) == 1U &&
               parentState.grabbed && parentState.grabOwner == 77U && parentState.velocity[0] > 0.0,
           "grab spring accelerates body");
    Expect(PhysicalConstructEndGrab(constructs, parentId, 77U), "end handle grab");

    // Restore an exact grid-aligned state so the split transform can be
    // checked without depending on the preceding spring substep.
    PhysicalConstructBodyState restored = parentState;
    restored.origin[0] = 10.0;
    restored.origin[1] = 10.0;
    restored.origin[2] = 201.0;
    restored.velocity[0] = 1.25;
    restored.velocity[1] = -0.5;
    restored.velocity[2] = 0.75;
    restored.grabOwner = 0U;
    restored.grabbed = false;
    Expect(PhysicalConstructApplyBodyMotion(constructs, &restored), "restore motion before split");

    const int32_t bridge[3] = {1, 0, -1};
    PhysicalConstructBlock removed;
    uint32_t splitCount = 0U;
    Expect(PhysicalConstructBreakBlock(constructs, parentId, bridge, &removed, g_split_ids,
                                       PHYSICAL_CONSTRUCT_MAX_BODIES,
                                       &splitCount) == PHYSICAL_CONSTRUCT_OK &&
               removed.kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL && splitCount == 2U &&
               g_split_ids[0] == parentId,
           "bridge removal deterministically splits body");

    uint32_t stateCount = PhysicalConstructCopyBodyStates(
        constructs, g_states, PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    Expect(stateCount == 2U && !truncated && g_states[0].id == parentId &&
               g_states[0].blockCount == 3U && g_states[1].blockCount == 1U,
           "lever component preserves parent id");
    Expect(Near(g_states[1].origin[0], 12.0, 0.0) && Near(g_states[1].origin[1], 10.0, 0.0) &&
               Near(g_states[1].origin[2], 200.0, 0.0) &&
               Near(g_states[1].velocity[0], 1.25, 0.0) &&
               Near(g_states[1].velocity[1], -0.5, 0.0) && Near(g_states[1].velocity[2], 0.75, 0.0),
           "child rebase preserves world transform and velocity");
    PhysicalConstructBlock childBlock;
    uint32_t childCount = 0U;
    Expect(PhysicalConstructCopyBlocks(constructs, g_states[1].id, &childBlock, 1U, &childCount) &&
               childCount == 1U && childBlock.local[0] == 0 && childBlock.local[1] == 0 &&
               childBlock.local[2] == 0,
           "leverless child receives its own local origin");

    PhysicalConstructSystem *replica = PhysicalConstructSystemCreate(world, NULL);
    Expect(replica != NULL, "replica create");
    for (uint32_t index = 0U; index < stateCount; ++index)
    {
        uint32_t count = 0U;
        Expect(PhysicalConstructCopyBlocks(constructs, g_states[index].id, g_import_blocks,
                                           PHYSICAL_CONSTRUCT_MAX_BLOCKS, &count),
               "copy body for import");
        Expect(PhysicalConstructImportBody(replica, &g_states[index], g_import_blocks, count) ==
                   PHYSICAL_CONSTRUCT_OK,
               "import body topology and motion");
    }
    Expect(PhysicalConstructCopyBodyStates(replica, g_replica_states, PHYSICAL_CONSTRUCT_MAX_BODIES,
                                           &truncated) == 2U &&
               g_replica_states[0].blockCount == g_states[0].blockCount,
           "replica copy/import round trip");
    PhysicalConstructSystemReset(replica);
    Expect(PhysicalConstructCopyBodyStates(replica, g_replica_states, PHYSICAL_CONSTRUCT_MAX_BODIES,
                                           &truncated) == 0U,
           "full snapshot reset");

    PhysicalConstructSystemDestroy(replica);
    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

static void TestDenseChunkCaptureAndLeverConnectivity(void)
{
    World *world = WorldCreate(404);
    Expect(world != NULL, "dense world create");
    PhysicalConstructSystem *constructs = PhysicalConstructSystemCreate(world, NULL);
    Expect(constructs != NULL, "dense construct system create");

    const int64_t root[3] = {0, 0, 2000};
    uint32_t placed = 0U;
    for (int64_t y = 0; y < CHUNK_SIZE && placed < 300U; y += 2)
    {
        for (int64_t x = 0; x < CHUNK_SIZE && placed < 300U; x += 2)
        {
            WorldSetBlock(world, x, y, root[2], BLOCK_EARTH);
            ++placed;
        }
    }
    const int64_t denseChunk[3] = {0, 0, root[2] / CHUNK_SIZE};
    WorldChunkDelta limited[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    uint32_t required = 0U;
    uint64_t chunkRevision = 0U;
    Expect(!WorldCopyChunkDeltas(world, denseChunk, limited,
                                 PHYSICAL_CONSTRUCT_MAX_BLOCKS, &required, &chunkRevision) &&
               required >= 300U,
           "dense chunk precondition exceeds construct scratch");

    uint64_t beforeRevision = WorldGetRevision(world);
    const int8_t upward[3] = {0, 0, 1};
    uint64_t bodyId = 0U;
    Expect(PhysicalConstructActivateLever(constructs, root, upward, NULL, 0U, &bodyId) ==
               PHYSICAL_CONSTRUCT_OK,
           "per-cell provenance activates inside dense edited chunk");
    PhysicalConstructBodyState state;
    bool truncated = true;
    Expect(PhysicalConstructCopyBodyStates(constructs, &state, 1U, &truncated) == 1U &&
               !truncated && state.blockCount == 2U &&
               WorldGetRevision(world) == beforeRevision + 1U,
           "atomic extraction captures only root and publishes one mutation");
    const PhysicalConstructAabb colliderQuery = {
        .minimum = {0.0, 0.0, 2000.0},
        .maximum = {1.0, 1.0, 2002.0},
    };
    PhysicalConstructCollider colliders[4];
    truncated = true;
    Expect(PhysicalConstructCopyCollidersInAabb(constructs, &colliderQuery, colliders, 4U,
                                                &truncated) == 4U &&
               !truncated && colliders[0].bodyId == bodyId &&
               Near(colliders[0].bounds.minimum[2], 2000.0, 0.0) &&
               Near(colliders[0].bounds.maximum[2], 2001.0, 0.0) &&
               Near(colliders[0].velocity[0], 0.0, 0.0),
           "exact construct colliders copy in deterministic order");
    truncated = false;
    Expect(PhysicalConstructCopyCollidersInAabb(constructs, &colliderQuery, colliders, 1U,
                                                &truncated) == 1U &&
               truncated,
           "construct collider copy reports bounded truncation");
    Expect(WorldGetBlock(world, 2, 0, root[2]) == BLOCK_EARTH,
           "dense unrelated edit remains static");

    const int32_t leverSide[3] = {-1, 0, 0};
    Expect(PhysicalConstructPlaceBlock(constructs, bodyId, leverSide, BLOCK_EARTH, NULL, 0U) ==
               PHYSICAL_CONSTRUCT_NOT_CONNECTED,
           "lever non-mount faces are not structural voxel neighbours");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);

    world = WorldCreate(405);
    Expect(world != NULL, "lever split world create");
    constructs = PhysicalConstructSystemCreate(world, NULL);
    Expect(constructs != NULL, "lever split system create");
    const int64_t splitRoot[3] = {10, 10, 2000};
    WorldSetBlock(world, 10, 10, 2000, BLOCK_EARTH);
    WorldSetBlock(world, 11, 10, 2000, BLOCK_EARTH);
    WorldSetBlock(world, 11, 10, 2001, BLOCK_GRASS);
    Expect(PhysicalConstructActivateLever(constructs, splitRoot, upward, NULL, 0U, &bodyId) ==
               PHYSICAL_CONSTRUCT_OK,
           "activate lever bridge topology");
    const int32_t bridge[3] = {1, 0, -1};
    uint32_t splitCount = 0U;
    Expect(PhysicalConstructBreakBlock(constructs, bodyId, bridge, NULL, g_split_ids,
                                       PHYSICAL_CONSTRUCT_MAX_BODIES, &splitCount) ==
               PHYSICAL_CONSTRUCT_OK &&
               splitCount == 2U,
           "side voxel no longer remains connected through lever cell");
    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

static void TestSegmentedMotionDynamicBlockerAndCheckedRebase(void)
{
    World *world = WorldCreate(505);
    Expect(world != NULL, "motion world create");
    PhysicalConstructConfiguration configuration;
    PhysicalConstructGetDefaultConfiguration(&configuration);
    configuration.fixedStepSeconds = 1.0;
    configuration.gravity = 0.0;
    configuration.maximumSpeed = 4.0;
    PhysicalConstructSystem *constructs =
        PhysicalConstructSystemCreate(world, &configuration);
    Expect(constructs != NULL, "motion construct system create");

    PhysicalConstructBlock voxel = {
        .local = {0, 0, 0},
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    PhysicalConstructBodyState moving = {
        .id = 1U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 2000.0},
        .velocity = {1.0, 0.0, 0.0},
        .blockCount = 1U,
    };
    PhysicalConstructBlock invalidVoxel = voxel;
    invalidVoxel.material = (uint8_t)(BLOCK_GRASS + 1U);
    Expect(PhysicalConstructImportBody(constructs, &moving, &invalidVoxel, 1U) ==
               PHYSICAL_CONSTRUCT_INVALID_ARGUMENT,
           "import rejects unknown construct material");
    Expect(PhysicalConstructImportBody(constructs, &moving, &voxel, 1U) ==
               PHYSICAL_CONSTRUCT_OK,
           "import moving voxel");
    Expect(PhysicalConstructStepWithBlockers(constructs, NULL, 0U),
           "segmented free step accepted");
    bool truncated = true;
    Expect(PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               Near(moving.origin[0], 1.0, 1e-12) && Near(moving.velocity[0], 1.0, 1e-12),
           "segmented free movement preserves full displacement and velocity");

    moving.origin[0] = 0.0;
    moving.velocity[0] = 1.0;
    Expect(PhysicalConstructApplyBodyMotion(constructs, &moving),
           "restore motion before dynamic collision");
    PhysicalConstructDynamicBlocker player = {
        .bounds = {
            .minimum = {1.5, 0.0, 2000.0},
            .maximum = {2.5, 1.0, 2001.0},
        },
    };
    Expect(PhysicalConstructStepWithBlockers(constructs, &player, 1U),
           "dynamic blocker step accepted");
    Expect(PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               moving.origin[0] >= 0.49 && moving.origin[0] <= 0.502 &&
               moving.velocity[0] == 0.0,
           "player-like dynamic blocker stops swept construct");

    moving.origin[0] = 0.0;
    moving.velocity[0] = 1.0;
    Expect(PhysicalConstructApplyBodyMotion(constructs, &moving),
           "restore ignored blocker motion");
    player.ignoredBodyId = moving.id;
    Expect(PhysicalConstructStepWithBlockers(constructs, &player, 1U) &&
               PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               Near(moving.origin[0], 1.0, 1e-12),
           "dynamic blocker can ignore only its held body");

    const int64_t validShift[3] = {1, 0, 64};
    Expect(PhysicalConstructRebase(constructs, validShift) &&
               PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               Near(moving.origin[0], 0.0, 1e-12) && Near(moving.origin[2], 1936.0, 1e-12),
           "bounded checked rebase");
    PhysicalConstructBodyState beforeRejected = moving;
    const int64_t invalidShift[3] = {INT64_MIN, 0, 0};
    Expect(!PhysicalConstructRebase(constructs, invalidShift) &&
               PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               Near(moving.origin[0], beforeRejected.origin[0], 0.0) &&
               Near(moving.origin[2], beforeRejected.origin[2], 0.0),
           "out-of-range rebase is atomic no-op");

    PhysicalConstructSystemReset(constructs);
    moving = (PhysicalConstructBodyState){
        .id = 1U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 2000.0},
        .velocity = {1.0, 0.0, 0.0},
        .blockCount = 1U,
    };
    PhysicalConstructBodyState obstacle = {
        .id = 2U,
        .topologyRevision = 1U,
        .origin = {1.5, 0.0, 2000.0},
        .blockCount = 1U,
    };
    Expect(PhysicalConstructImportBody(constructs, &moving, &voxel, 1U) ==
               PHYSICAL_CONSTRUCT_OK &&
               PhysicalConstructImportBody(constructs, &obstacle, &voxel, 1U) ==
                   PHYSICAL_CONSTRUCT_OK &&
               PhysicalConstructStepWithBlockers(constructs, NULL, 0U),
           "body occupancy collision setup");
    Expect(PhysicalConstructCopyBodyStates(constructs, g_states,
                                           PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated) == 2U &&
               g_states[0].origin[0] >= 0.49 && g_states[0].origin[0] <= 0.502 &&
               g_states[0].velocity[0] == 0.0 && Near(g_states[1].origin[0], 1.5, 0.0),
           "bounded occupancy narrow phase stops body against body");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

static void TestMaximumColliderBatch(void)
{
    World *world = WorldCreate(606);
    Expect(world != NULL, "maximum collider world create");
    PhysicalConstructConfiguration configuration;
    PhysicalConstructGetDefaultConfiguration(&configuration);
    configuration.fixedStepSeconds = 1.0;
    configuration.gravity = 0.0;
    configuration.maximumSpeed = 4.0;
    PhysicalConstructSystem *constructs =
        PhysicalConstructSystemCreate(world, &configuration);
    Expect(constructs != NULL, "maximum collider construct system create");

    g_import_blocks[0] = (PhysicalConstructBlock){
        .local = {0, 0, 0},
        .mountNormal = {0, 0, 1},
        .kind = PHYSICAL_CONSTRUCT_BLOCK_LEVER,
    };
    for (uint32_t index = 1U; index < PHYSICAL_CONSTRUCT_MAX_BLOCKS; ++index)
    {
        g_import_blocks[index] = (PhysicalConstructBlock){
            .local = {(int32_t)(index - 1U), 0, -1},
            .material = BLOCK_EARTH,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        };
    }
    PhysicalConstructBodyState moving = {
        .id = 1U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 5001.0},
        .velocity = {1.0, 0.0, 0.0},
        .blockCount = PHYSICAL_CONSTRUCT_MAX_BLOCKS,
    };
    Expect(PhysicalConstructImportBody(constructs, &moving, g_import_blocks,
                                       PHYSICAL_CONSTRUCT_MAX_BLOCKS) ==
               PHYSICAL_CONSTRUCT_OK,
           "maximum construct import");
    bool truncated = true;
    Expect(PhysicalConstructStepWithBlockers(constructs, NULL, 0U) &&
               PhysicalConstructCopyBodyStates(constructs, &moving, 1U, &truncated) == 1U &&
               !truncated && Near(moving.origin[0], 1.0, 1e-12) &&
               Near(moving.velocity[0], 1.0, 1e-12),
           "maximum construct uses one bounded world range batch");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(PhysicalConstructTestEntryPoint)
{
    TestLeverModelAndChunks();
    TestNaturalRootDoesNotCaptureTerrain();
    TestActivationPlacementRaycastGrabAndSplit();
    TestDenseChunkCaptureAndLeverConnectivity();
    TestMultiwaySplitCapacityAndAtomicity();
    TestSegmentedMotionDynamicBlockerAndCheckedRebase();
    TestMaximumColliderBatch();
    LaiueTestRuntimeWrite("physical construct checks: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
