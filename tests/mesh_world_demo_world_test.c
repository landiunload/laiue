#include "mesh_world_demo_world.h"
#include "test_runtime.h"

#include <string.h>

static void Check(uint32_t condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static void ProviderCoordinates(void)
{
    MeshDemoProvider provider = {
        .groundModel = MESH_DEMO_MODEL_GROUND,
        .treeModel = MESH_DEMO_MODEL_TREE,
    };
    LaiueMeshInstanceDescV1 first[3] = {0}, rebased[3] = {0};
    LaiueMeshCellV1 cell = {-1234567890123LL, 2345678901234LL, 0};
    uint32_t count = 0u;
    Check(MeshDemoPopulate(&provider, &cell, first, 3u, &count) && count == 3u,
          "negative and distant cells generate ground and two trees");
    provider.origin = (LaiueMeshCellV1){cell.x, cell.y, 9};
    cell = (LaiueMeshCellV1){0, 0, -9};
    Check(MeshDemoPopulate(&provider, &cell, rebased, 3u, &count) && count == 3u &&
              memcmp(first, rebased, sizeof(first)) == 0,
          "rebased provider reproduces identical absolute terrain");
    cell.z = 0;
    Check(MeshDemoPopulate(&provider, &cell, NULL, 0u, &count) && count == 0u,
          "empty vertical cell is a successful provider result");
    cell.z = -9;
    Check(!MeshDemoPopulate(&provider, &cell, rebased, 2u, &count) && count == 0u,
          "insufficient capacity refuses partial terrain");
}

static void MovementAndRebase(void)
{
    const LaiueMeshWorldServiceV1 *service = LaiueMeshWorldGetStaticServiceV1();
    LaiueMeshWorldV1 *world = NULL;
    const LaiueMeshWorldConfigV1 config = {
        .structSize = sizeof(config),
        .cellSize = MESH_DEMO_CELL_SIZE,
        .maximumInstances = 32u,
    };
    Check(service->create(&config, &world), "create demonstration world");
    const LaiueMeshShapeV1 ground = {
        .structSize = sizeof(ground),
        .flags = LAIUE_MESH_SHAPE_COLLIDE_BOUNDS,
        .boundsMin = {0.0f, 0.0f, -0.25f},
        .boundsMax = {16.0f, 16.0f, 0.0f},
    };
    const LaiueMeshShapeV1 trunk = {
        .structSize = sizeof(trunk),
        .flags = LAIUE_MESH_SHAPE_COLLIDE_BOUNDS,
        .boundsMin = {-0.25f, -0.25f, 0.0f},
        .boundsMax = {0.25f, 0.25f, 2.5f},
    };
    Check(service->registerShape(world, 1u, &ground) && service->registerShape(world, 2u, &trunk),
          "register demonstration colliders");
    LaiueMeshInstanceDescV1 instance = {
        .structSize = sizeof(instance),
        .model = 1u,
        .transform = {.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .scale = {1.0f, 1.0f, 1.0f}},
        .flags = LAIUE_MESH_INSTANCE_COLLIDABLE,
    };
    LaiueMeshInstanceV1 handle = 0u;
    Check(service->add(world, &instance, &handle), "place ground");
    instance.model = 2u;
    instance.transform.position.local[0] = 4.0f;
    instance.transform.position.local[1] = 4.0f;
    Check(service->add(world, &instance, &handle), "place tree collider");
    LaiueMeshPositionV1 player = {.local = {2.0f, 3.0f, 1.2f}};
    const float halfExtent[3] = {0.3f, 0.3f, 0.9f};
    const float falling[3] = {0.0f, 0.0f, -4.0f};
    LaiueMeshMoveResultV1 result = {0};
    Check(service->moveBox(world, &player, halfExtent, falling, &result) && result.grounded &&
              result.position.local[2] > 0.89f,
          "player rests on ground");
    player = result.position;
    const float walking[3] = {4.0f, 2.0f, 0.0f};
    Check(service->moveBox(world, &player, halfExtent, walking, &result) && result.collided &&
              result.position.local[0] < 3.5f && result.position.local[1] > 4.9f,
          "moveBox slides along the tree instead of stopping all motion");
    player = result.position;
    player.cell = (LaiueMeshCellV1){1000000000000LL, -1000000000000LL, 7};
    MeshDemoProvider provider = {.origin = {5, -8, 2}};
    Check(MeshDemoRebase(service, world, &provider, &player) && player.cell.x == 0 &&
              player.cell.y == 0 && player.cell.z == 0 && provider.origin.x == 1000000000005LL &&
              provider.origin.y == -1000000000008LL && provider.origin.z == 9,
          "application rebases the player and absolute provider origin together");
    LaiueMeshInstanceInfoV1 info = {0};
    Check(service->get(world, handle, &info) &&
              info.transform.position.cell.x == -1000000000000LL &&
              info.transform.position.cell.y == 1000000000000LL &&
              info.transform.position.cell.z == -7,
          "world placements preserve absolute coordinates after application rebase");
    service->destroy(world);
}

LAIUE_TEST_ENTRY(MeshWorldDemoWorldTestEntryPoint)
{
    ProviderCoordinates();
    MovementAndRebase();
    LaiueTestRuntimeWrite("mesh world demo provider, movement and rebase passed\n");
    LAIUE_TEST_SUCCESS();
}
