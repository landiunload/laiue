// Ручной benchmark навигации: в ALL не входит, в CTest не регистрируется.
//
// Город — решётка 1000 × 1000 перекрёстков (миллион узлов, четыре миллиона
// направленных рёбер) со случайными стоимостями. Меряются: подготовка графа,
// одиночный A* между случайными перекрёстками, тот же пакет на всех ядрах,
// A* по сетке свободного движения и построение поля потока. Из повторов
// берётся минимум. Контрольная сумма стоимостей не должна меняться между
// сборками: это проверка детерминизма.

#include "navigation/navigation.h"
#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

#define SIDE 1000u
#define SPACING 100
#define QUERIES 256u
#define SAMPLES 3u

static uint32_t randomState = 0x9e3779b9u;
static LaiueNavNode nodes[SIDE * SIDE];
static LaiueNavBatchItem items[QUERIES];
static LaiueNavEdge pathStorage[QUERIES][8192];

static uint32_t NextRandom(void)
{
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 17u;
    randomState ^= randomState << 5u;
    return randomState;
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - 1u - index];
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void Report(const char *label, double seconds, uint64_t operations, const char *unit)
{
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(": ");
    WriteUnsigned((uint64_t)(seconds * 1000000.0));
    LaiueTestRuntimeWrite(" us");
    if (operations != 0u && seconds > 0.0)
    {
        LaiueTestRuntimeWrite(", ");
        WriteUnsigned((uint64_t)((double)operations / seconds));
        LaiueTestRuntimeWrite(" ");
        LaiueTestRuntimeWrite(unit);
        LaiueTestRuntimeWrite("/s");
    }
    LaiueTestRuntimeWrite("\n");
}

static void Fail(const char *message)
{
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static LaiueNavGraph *BuildGraph(void)
{
    LaiueNavGraphDesc desc = {sizeof(desc), SIDE * SIDE, SIDE * SIDE * 4u};
    LaiueNavGraph *graph = NULL;
    if (LaiueNavGraphCreate(&desc, &graph) != LAIUE_NAV_OK)
        Fail("graph create");
    for (uint32_t y = 0u; y < SIDE; ++y)
        for (uint32_t x = 0u; x < SIDE; ++x)
        {
            const LaiueNavPosition position = {(int64_t)x * SPACING, (int64_t)y * SPACING, 0};
            if (LaiueNavGraphAddNode(graph, &position, 0u, &nodes[y * SIDE + x]) != LAIUE_NAV_OK)
                Fail("add node");
        }
    for (uint32_t y = 0u; y < SIDE; ++y)
        for (uint32_t x = 0u; x < SIDE; ++x)
        {
            const uint32_t here = y * SIDE + x;
            LaiueNavEdgeDesc edge = {sizeof(edge), 0u, 1u, 0u};
            LaiueNavEdge handle;
            if (x + 1u < SIDE)
            {
                edge.cost = SPACING + NextRandom() % 200u;
                (void)LaiueNavGraphAddEdge(graph, nodes[here], nodes[here + 1u], &edge, &handle);
                edge.cost = SPACING + NextRandom() % 200u;
                (void)LaiueNavGraphAddEdge(graph, nodes[here + 1u], nodes[here], &edge, &handle);
            }
            if (y + 1u < SIDE)
            {
                edge.cost = SPACING + NextRandom() % 200u;
                (void)LaiueNavGraphAddEdge(graph, nodes[here], nodes[here + SIDE], &edge, &handle);
                edge.cost = SPACING + NextRandom() % 200u;
                (void)LaiueNavGraphAddEdge(graph, nodes[here + SIDE], nodes[here], &edge, &handle);
            }
        }
    return graph;
}

static void FillItems(uint32_t maximumDistance)
{
    for (uint32_t index = 0u; index < QUERIES; ++index)
    {
        const uint32_t startX = NextRandom() % SIDE;
        const uint32_t startY = NextRandom() % SIDE;
        uint32_t goalX = startX + NextRandom() % maximumDistance;
        uint32_t goalY = startY + NextRandom() % maximumDistance;
        goalX = goalX >= SIDE ? SIDE - 1u : goalX;
        goalY = goalY >= SIDE ? SIDE - 1u : goalY;
        LaiueNavBatchItem *item = &items[index];
        memset(item, 0, sizeof(*item));
        item->request.structSize = sizeof(item->request);
        item->request.start = nodes[startY * SIDE + startX];
        item->request.goal = nodes[goalY * SIDE + goalX];
        item->request.modes = 1u;
        item->request.heuristicNumerator = 1u;
        item->request.heuristicDenominator = 1u;
        item->edges = pathStorage[index];
        item->capacity = 8192u;
    }
}

static uint64_t Checksum(void)
{
    uint64_t sum = 0u;
    for (uint32_t index = 0u; index < QUERIES; ++index)
    {
        if (items[index].result.pathStatus != LAIUE_NAV_PATH_FOUND)
            Fail("batch path not found");
        sum = sum * 31u + items[index].result.cost;
    }
    return sum;
}

LAIUE_TEST_ENTRY(NavigationBenchmarkEntryPoint)
{
    double begin = PlatformMonotonicSeconds();
    LaiueNavGraph *graph = BuildGraph();
    Report("build 1M nodes / 4M edges", PlatformMonotonicSeconds() - begin, 0u, "");
    begin = PlatformMonotonicSeconds();
    if (LaiueNavGraphPrepare(graph) != LAIUE_NAV_OK)
        Fail("prepare");
    Report("prepare", PlatformMonotonicSeconds() - begin, 0u, "");

    const uint32_t threads =
        LaiueTaskLogicalProcessorCount() > 64u ? 64u : LaiueTaskLogicalProcessorCount();
    LaiueTaskPool *pool = LaiueTaskPoolCreate(threads);
    LaiueTaskExecutor executor;
    memset(&executor, 0, sizeof(executor));
    executor.structSize = sizeof(executor);
    if (pool == NULL || !LaiueTaskPoolGetExecutor(pool, &executor))
        Fail("pool");
    LaiueNavQuery *queries[64];
    for (uint32_t index = 0u; index < threads; ++index)
        if (LaiueNavQueryCreate(&queries[index]) != LAIUE_NAV_OK)
            Fail("query");

    static const uint32_t distances[] = {20u, 100u, 400u};
    static const char *const labels[][2] = {
        {"A* local (<=20 blocks), 1 thread", "A* local (<=20 blocks), all threads"},
        {"A* district (<=100 blocks), 1 thread", "A* district (<=100 blocks), all threads"},
        {"A* city (<=400 blocks), 1 thread", "A* city (<=400 blocks), all threads"},
    };
    for (uint32_t distance = 0u; distance < 3u; ++distance)
    {
        FillItems(distances[distance]);
        double serial = 1e30;
        double parallel = 1e30;
        uint64_t serialSum = 0u;
        uint64_t parallelSum = 0u;
        for (uint32_t sample = 0u; sample < SAMPLES; ++sample)
        {
            begin = PlatformMonotonicSeconds();
            (void)LaiueNavFindPathsBatch(graph, NULL, queries, 1u, items, QUERIES);
            const double one = PlatformMonotonicSeconds() - begin;
            serial = one < serial ? one : serial;
            serialSum = Checksum();
            begin = PlatformMonotonicSeconds();
            (void)LaiueNavFindPathsBatch(graph, &executor, queries, threads, items, QUERIES);
            const double all = PlatformMonotonicSeconds() - begin;
            parallel = all < parallel ? all : parallel;
            parallelSum = Checksum();
        }
        if (serialSum != parallelSum)
            Fail("batch result depends on thread count");
        Report(labels[distance][0], serial, QUERIES, "paths");
        Report(labels[distance][1], parallel, QUERIES, "paths");
        LaiueTestRuntimeWrite("  checksum ");
        WriteUnsigned(serialSum);
        LaiueTestRuntimeWrite("\n");
    }

    LaiueNavGridDesc gridDesc = {sizeof(gridDesc), 1u};
    LaiueNavGrid *grid = NULL;
    if (LaiueNavGridCreate(&gridDesc, &grid) != LAIUE_NAV_OK)
        Fail("grid");
    for (uint32_t wall = 0u; wall < 2000u; ++wall)
    {
        const LaiueNavRect rect = {(int64_t)(NextRandom() % 1024u) - 512,
                                   (int64_t)(NextRandom() % 1024u) - 512, 1u + NextRandom() % 40u,
                                   1u + NextRandom() % 3u};
        (void)LaiueNavGridFillRect(grid, &rect, 0u);
    }
    static LaiueNavCell cells[1u << 16];
    LaiueNavGridPathRequest request;
    memset(&request, 0, sizeof(request));
    request.structSize = sizeof(request);
    request.start.x = -400;
    request.start.y = -400;
    request.goal.x = 400;
    request.goal.y = 400;
    request.margin = 100u;
    (void)LaiueNavGridSetCell(grid, -400, -400, 1u);
    (void)LaiueNavGridSetCell(grid, 400, 400, 1u);
    double best = 1e30;
    LaiueNavPathResult result;
    memset(&result, 0, sizeof(result));
    result.structSize = sizeof(result);
    for (uint32_t sample = 0u; sample < SAMPLES; ++sample)
    {
        begin = PlatformMonotonicSeconds();
        LaiueNavGridFindPath(grid, queries[0], &request, cells, 1u << 16, &result);
        const double elapsed = PlatformMonotonicSeconds() - begin;
        best = elapsed < best ? elapsed : best;
    }
    if (result.pathStatus != LAIUE_NAV_PATH_FOUND)
        Fail("grid path");
    Report("grid A* 800x800 diagonal with walls", best, result.expansions, "cells");

    LaiueNavFlowField *field = NULL;
    if (LaiueNavFlowFieldCreate(&field) != LAIUE_NAV_OK)
        Fail("flow");
    const LaiueNavRect bounds = {-256, -256, 512u, 512u};
    const LaiueNavCell goal = {0, 0};
    (void)LaiueNavGridSetCell(grid, 0, 0, 1u);
    best = 1e30;
    for (uint32_t sample = 0u; sample < SAMPLES; ++sample)
    {
        begin = PlatformMonotonicSeconds();
        if (LaiueNavFlowFieldBuild(field, grid, &bounds, &goal, 1u) != LAIUE_NAV_OK)
            Fail("flow build");
        const double elapsed = PlatformMonotonicSeconds() - begin;
        best = elapsed < best ? elapsed : best;
    }
    Report("flow field 512x512", best, 512u * 512u, "cells");

    LaiueNavFlowFieldDestroy(field);
    LaiueNavGridDestroy(grid);
    for (uint32_t index = 0u; index < threads; ++index)
        LaiueNavQueryDestroy(queries[index]);
    LaiueTaskPoolDestroy(pool);
    LaiueNavGraphDestroy(graph);
    LAIUE_TEST_SUCCESS();
}
