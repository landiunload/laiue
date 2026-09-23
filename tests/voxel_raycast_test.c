#include "voxel_render/chunk_streaming.h"
#include "voxel/raycast.h"
#include "test_runtime.h"

static uint32_t raycastChecks;

// Точечный обход дальней грани куба в ChunkStreamingSetCenter проверяется
// без рендерера: рабочие потоки останавливаются ChunkStreamingPause, меши
// не создаются вовсе, а число заявок считается детерминированно.
static uint8_t StreamingSolid(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    return 1u;
}

#if defined(_WIN32)
// Разреженный мир: ровно один блок в локальном нуле каждого чанка. Так
// каждая ячейка куба даёт крошечный непустой меш, и проверка бюджета
// загрузок упирается в предел очереди рендера, а не в число заявок.
static uint8_t StreamingSparse(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    const int64_t localX = ((x % 64) + 64) % 64;
    const int64_t localY = ((y % 64) + 64) % 64;
    const int64_t localZ = ((z % 64) + 64) % 64;
    return (localX == 0 && localY == 0 && localZ == 0) ? 1u : 0u;
}
#endif

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

static BlockType ReadWorld(void *context, int64_t x, int64_t y, int64_t z)
{
    return WorldGetBlock((World *)context, x, y, z);
}

LAIUE_TEST_ENTRY(VoxelRaycastTestEntryPoint)
{
    World *world = WorldCreate(NULL);
    RaycastExpect(world != NULL, "empty world was not created");

    const double origin[3] = {0.5, 0.5, 0.5};
    const float positiveX[3] = {1.0f, 0.0f, 0.0f};
    VoxelRaycastHit hit;
    RaycastExpect(!VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX,
                                              10.0f, &hit),
                  "empty world produced a hit");

    RaycastExpect(WorldTrySetBlock(world, 3, 0, 0, (BlockType)7U),
                  "positive target block was not created");
    RaycastExpect(VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX,
                                             10.0f, &hit) &&
                      BlockEquals(hit.block, 3, 0, 0) && BlockEquals(hit.previousBlock, 2, 0, 0) &&
                      hit.normal[0] == -1 && hit.normal[1] == 0 && hit.normal[2] == 0 &&
                      hit.distance == 2.5,
                  "positive-axis hit data is wrong");

    RaycastExpect(WorldTrySetBlock(world, -2, 0, 0, (BlockType)8U),
                  "negative target block was not created");
    const float negativeX[3] = {-1.0f, 0.0f, 0.0f};
    RaycastExpect(
        VoxelRaycastWithBlockQuery(world, ReadWorld, origin, negativeX, 10.0f, &hit) &&
            BlockEquals(hit.block, -2, 0, 0) &&
            BlockEquals(hit.previousBlock, -1, 0, 0) && hit.normal[0] == 1 && hit.distance == 1.5,
        "negative-axis hit data is wrong");

    const float invalidDirection[3] = {1.5f, 0.0f, 0.0f};
    RaycastExpect(
        !VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX, 0.0f, &hit) &&
            !VoxelRaycastWithBlockQuery(world, ReadWorld, origin, positiveX,
                                        VOXEL_RAYCAST_MAX_DISTANCE + 1.0f, &hit) &&
            !VoxelRaycastWithBlockQuery(world, ReadWorld, origin, invalidDirection,
                                        10.0f, &hit),
        "invalid ray parameters were accepted");

    WorldDestroy(world);

    // Стриминг чанков: за первый вызов в таблицу входит весь куб радиуса R,
    // а каждый следующий шаг ровно на соседний чанк добавляет только дальнюю
    // грань. При диагональном шаге это две грани без общего ребра. Счёт
    // заявок детерминирован и служит эталоном для точечного обхода.
    {
        const int32_t radius = 3;
        const uint64_t side = (uint64_t)(2 * radius + 1);
        const uint64_t face = side * side;
        const uint64_t first = face * side;

        WorldBaseProvider provider;
        provider.context = NULL;
        provider.getBlock = StreamingSolid;
        provider.fillRegion = NULL;
        provider.rebase = NULL;
        World *streamWorld = WorldCreate(&provider);
        RaycastExpect(streamWorld != NULL, "streaming world was not created");

        ChunkStreaming *streaming = ChunkStreamingCreate(
            streamWorld, (Renderer *)&raycastChecks, radius);
        RaycastExpect(streaming != NULL, "streaming was not created");
        RaycastExpect(ChunkStreamingPause(streaming), "streaming was not paused");

        ChunkStreamingStats stats;
        ChunkStreamingSetCenter(streaming, 0, 0, 0);
        ChunkStreamingGetStats(streaming, &stats);
        RaycastExpect(stats.queuedRequests == first,
            "the first streaming cube must request every chunk inside the radius");

        // Диагональ (0,0,0)->(1,1,0): две грани по (2R+1)^2 без общего ребра.
        ChunkStreamingSetCenter(streaming, 1, 1, 0);
        ChunkStreamingGetStats(streaming, &stats);
        RaycastExpect(stats.queuedRequests == first + 2u * face - side,
            "a diagonal step must request exactly the two leading faces");

        // Обычный шаг по одной оси: одна грань.
        ChunkStreamingSetCenter(streaming, 2, 1, 0);
        ChunkStreamingGetStats(streaming, &stats);
        RaycastExpect(stats.queuedRequests == first + 3u * face - side,
            "a one-axis step must request exactly one leading face");

        ChunkStreamingDestroy(streaming);
        WorldDestroy(streamWorld);
        LaiueTestRuntimeWrite("Chunk streaming leading-face checks passed.\r\n");
    }

    // Отказ рендера не должен перезаказывать сборку. Фиктивный рендерер —
    // обнулённый буфер: worldReady = false, поэтому RendererCreateMesh
    // всегда возвращает NULL. Раньше это выставляло hasUnqueuedPending и
    // заказывало чанк заново (двойная сборка); теперь готовый результат
    // придерживается до следующего кадра. Проверка говорит о конкретном
    // контракте D3D12-рендера, поэтому только Windows.
#if defined(_WIN32)
    {
        const int32_t holdRadius = 3;
        const uint64_t holdFirst = 7u * 7u * 7u;

        static unsigned char fakeRenderer[65536];
        WorldBaseProvider holdProvider;
        holdProvider.context = NULL;
        holdProvider.getBlock = StreamingSparse;
        holdProvider.fillRegion = NULL;
        holdProvider.rebase = NULL;
        World *holdWorld = WorldCreate(&holdProvider);
        RaycastExpect(holdWorld != NULL, "hold world was not created");

        ChunkStreaming *hold = ChunkStreamingCreate(
            holdWorld, (Renderer *)fakeRenderer, holdRadius);
        RaycastExpect(hold != NULL, "hold streaming was not created");

        ChunkStreamingSetCenter(hold, 0, 0, 0);

        ChunkStreamingStats holdStats;
        for (int32_t spin = 0; spin < 4000; ++spin)
        {
            ChunkStreamingGetStats(hold, &holdStats);
            if (holdStats.pendingRequests == 0u
                && holdStats.queuedRequests == holdStats.completedBuilds)
            {
                break;
            }
            Sleep(1);
        }
        RaycastExpect(holdStats.queuedRequests == holdFirst,
            "the hold cube must request every chunk exactly once");

        // Рендер отказывает всем мешам: разбор очереди останавливается на
        // придержанном результате и повторных заявок не появляется.
        for (int32_t pump = 0; pump < 64; ++pump)
        {
            ChunkStreamingPump(hold);
        }
        ChunkStreamingGetStats(hold, &holdStats);
        RaycastExpect(holdStats.uploadedMeshes == 0,
            "a renderer that refuses meshes must upload nothing");
        RaycastExpect(holdStats.queuedRequests == holdFirst,
            "a refused mesh must not be requested for a second build");

        ChunkStreamingDestroy(hold);
        WorldDestroy(holdWorld);
    }
#endif

    LaiueTestRuntimeWrite("Voxel raycast tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
