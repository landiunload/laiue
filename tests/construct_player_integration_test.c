#include "construct/physical_construct.h"
#include "gameplay/player_controller.h"
#include "test_runtime.h"
#include "world/world.h"

typedef struct ConstructCollisionAdapter
{
    PhysicalConstructSystem* constructs;
    PhysicalConstructCollider scratch[VOXEL_DYNAMIC_COLLIDER_CAPACITY];
} ConstructCollisionAdapter;

static void Expect(bool condition, const char* name)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Construct/player integration failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(double left, double right, double tolerance)
{
    double difference = left - right;
    if (difference < 0.0)
        difference = -difference;
    return difference <= tolerance;
}

static void QueryAir(void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = 0U;
    outBlock->friction = 0.0f;
}

static bool QueryConstructs(void* context,
    const VoxelBodyBounds* queryBounds,
    VoxelDynamicCollider* output, uint32_t capacity,
    uint32_t* outCount)
{
    ConstructCollisionAdapter* adapter = context;
    *outCount = 0U;
    if (capacity > VOXEL_DYNAMIC_COLLIDER_CAPACITY)
        return false;

    PhysicalConstructAabb query;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        query.minimum[axis] = queryBounds->minimum[axis];
        query.maximum[axis] = queryBounds->maximum[axis];
    }
    bool truncated = false;
    uint32_t count = PhysicalConstructCopyCollidersInAabb(
        adapter->constructs, &query, adapter->scratch,
        capacity, &truncated);
    if (truncated)
        return false;

    for (uint32_t index = 0U; index < count; ++index)
    {
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            output[index].bounds.minimum[axis] =
                adapter->scratch[index].bounds.minimum[axis];
            output[index].bounds.maximum[axis] =
                adapter->scratch[index].bounds.maximum[axis];
            output[index].velocity[axis] =
                adapter->scratch[index].velocity[axis];
        }
        output[index].friction = 1.0f;
        output[index].stableId = (uint64_t)index + 1U;
    }
    *outCount = count;
    return true;
}

static void BuildPlayerBlocker(const PlayerController* player,
    const Camera* camera, PhysicalConstructDynamicBlocker* outBlocker)
{
    VoxelBodyShape shape;
    VoxelBodyBounds bounds;
    PlayerControllerGetBodyShape(player, &shape);
    VoxelBodyCalculateBounds(camera->position, &shape, &bounds);
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        outBlocker->bounds.minimum[axis] = bounds.minimum[axis];
        outBlocker->bounds.maximum[axis] = bounds.maximum[axis];
    }
    outBlocker->ignoredBodyId = 0U;
}

LAIUE_TEST_ENTRY(ConstructPlayerIntegrationTestEntryPoint)
{
    World* world = WorldCreate(711);
    Expect(world != NULL, "world create");

    PhysicalConstructConfiguration constructConfiguration;
    PhysicalConstructGetDefaultConfiguration(&constructConfiguration);
    constructConfiguration.gravity = 0.0;
    PhysicalConstructSystem* constructs = PhysicalConstructSystemCreate(
        world, &constructConfiguration);
    Expect(constructs != NULL, "construct system create");

    PhysicalConstructBlock blocks[2] = {
        {
            .local = {-1, 0, 0},
            .material = BLOCK_EARTH,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        },
        {
            .local = {0, 0, 0},
            .mountNormal = {1, 0, 0},
            .material = 0U,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_LEVER,
        },
    };
    PhysicalConstructBodyState body = {
        .id = 7U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 0.0},
        .velocity = {0.0, 1.2, 0.0},
        .blockCount = 2U,
    };
    Expect(PhysicalConstructImportBody(
            constructs, &body, blocks, 2U) == PHYSICAL_CONSTRUCT_OK,
        "import moving lever construct");

    ConstructCollisionAdapter adapter = {
        .constructs = constructs,
    };
    PlayerCollisionSource collision = {
        .context = &adapter,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryConstructs,
    };
    PlayerControllerConfig playerConfiguration;
    PlayerControllerGetDefaultConfig(&playerConfiguration);
    PlayerController player;
    PlayerControllerInit(&player, &playerConfiguration);
    Camera camera = {
        .position = {
            -0.5,
            0.5,
            1.0 + playerConfiguration.collisionEpsilon +
                playerConfiguration.standingEyeHeight,
        },
    };
    PlayerControllerCommand idle = {0};

    for (uint32_t substep = 0U; substep < 4U; ++substep)
    {
        PhysicalConstructDynamicBlocker blocker;
        BuildPlayerBlocker(&player, &camera, &blocker);
        Expect(PhysicalConstructStepWithBlockers(
                constructs, &blocker, 1U),
            "construct fixed step");
        (void)PlayerControllerSimulateFixedSteps(
            &player, &collision, &camera, &idle, 1U);
    }

    PhysicalConstructBodyState moved;
    bool truncated = true;
    Expect(PhysicalConstructCopyBodyStates(
            constructs, &moved, 1U, &truncated) == 1U && !truncated,
        "copy moved body");
    Expect(Near(moved.origin[1], 0.02, 1e-12),
        "construct advances four 240 Hz steps");
    Expect(PlayerControllerIsGrounded(&player),
        "player remains grounded on construct");
    Expect(Near(camera.position[1], 0.52, 1e-12),
        "player receives the same four platform steps");

    VoxelBodyShape probe = {
        .radius = 0.02,
        .height = 0.04,
        .eyeHeight = 0.02,
        .collisionEpsilon = 0.001,
    };
    double emptyLeverCorner[3] = {
        0.9, moved.origin[1] + 0.9, 0.9
    };
    Expect(!VoxelBodyCollides(
            &collision, emptyLeverCorner, &probe),
        "lever cell keeps empty fractional space");
    double insideRoot[3] = {
        -0.5, moved.origin[1] + 0.5, 0.5
    };
    Expect(VoxelBodyCollides(&collision, insideRoot, &probe),
        "construct voxel has an exact solid collider");

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
    LaiueTestRuntimeWrite(
        "Construct/player integration: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
