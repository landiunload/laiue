#include "navigation/navigation.h"
#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

#define MODE_WALK 1u
#define MODE_CAR 2u
#define SIDE 40u
#define SPACING 10

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Navigation failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static uint32_t randomState = 0x12345678u;

static uint32_t NextRandom(void)
{
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 17u;
    randomState ^= randomState << 5u;
    return randomState;
}

/* ---- Город-решётка ------------------------------------------------------------ */

typedef struct City
{
    LaiueNavGraph *graph;
    LaiueNavNode nodes[SIDE * SIDE];
    LaiueNavEdge edges[SIDE * SIDE * 4u];
    uint32_t edgeFrom[SIDE * SIDE * 4u];
    uint32_t edgeTo[SIDE * SIDE * 4u];
    uint32_t edgeCost[SIDE * SIDE * 4u];
    uint32_t edgeModes[SIDE * SIDE * 4u];
    uint32_t edgeCount;
} City;

static City city;

static void AddRoad(uint32_t from, uint32_t to, uint32_t cost, uint32_t modes)
{
    LaiueNavEdgeDesc desc = {sizeof(desc), cost, modes, city.edgeCount};
    LaiueNavEdge edge = 0u;
    Expect(LaiueNavGraphAddEdge(city.graph, city.nodes[from], city.nodes[to], &desc, &edge) ==
               LAIUE_NAV_OK,
           "add edge");
    city.edges[city.edgeCount] = edge;
    city.edgeFrom[city.edgeCount] = from;
    city.edgeTo[city.edgeCount] = to;
    city.edgeCost[city.edgeCount] = cost;
    city.edgeModes[city.edgeCount] = modes;
    ++city.edgeCount;
}

static void BuildCity(void)
{
    LaiueNavGraphDesc desc = {sizeof(desc), SIDE * SIDE, SIDE * SIDE * 4u};
    Expect(LaiueNavGraphCreate(&desc, &city.graph) == LAIUE_NAV_OK, "graph create");
    Expect(LaiueNavGraphIsPrepared(city.graph), "empty graph is prepared");
    for (uint32_t y = 0u; y < SIDE; ++y)
        for (uint32_t x = 0u; x < SIDE; ++x)
        {
            const LaiueNavPosition position = {(int64_t)x * SPACING, (int64_t)y * SPACING, 0};
            Expect(LaiueNavGraphAddNode(city.graph, &position, y * SIDE + x,
                                        &city.nodes[y * SIDE + x]) == LAIUE_NAV_OK,
                   "add node");
        }
    for (uint32_t y = 0u; y < SIDE; ++y)
        for (uint32_t x = 0u; x < SIDE; ++x)
        {
            const uint32_t here = y * SIDE + x;
            /* Стоимость не меньше длины ребра: эвристика 1/1 допустима. */
            if (x + 1u < SIDE)
            {
                const uint32_t modes = (y % 4u == 0u) ? (MODE_WALK | MODE_CAR) : MODE_WALK;
                AddRoad(here, here + 1u, SPACING + NextRandom() % 30u, modes);
                AddRoad(here + 1u, here, SPACING + NextRandom() % 30u, modes);
            }
            if (y + 1u < SIDE)
            {
                const uint32_t modes = (x % 4u == 0u) ? (MODE_WALK | MODE_CAR) : MODE_WALK;
                AddRoad(here, here + SIDE, SPACING + NextRandom() % 30u, modes);
                AddRoad(here + SIDE, here, SPACING + NextRandom() % 30u, modes);
            }
        }
    Expect(!LaiueNavGraphIsPrepared(city.graph), "structure change needs preparation");
    Expect(LaiueNavGraphPrepare(city.graph) == LAIUE_NAV_OK, "prepare");
    Expect(LaiueNavGraphNodeCount(city.graph) == SIDE * SIDE &&
               LaiueNavGraphEdgeCount(city.graph) == city.edgeCount,
           "counts");
}

/* Эталон: Дейкстра за O(V²) по собственному списку рёбер теста. */
static uint64_t ReferenceCost(uint32_t start, uint32_t goal, uint32_t modes)
{
    static uint64_t distance[SIDE * SIDE];
    static uint8_t done[SIDE * SIDE];
    for (uint32_t index = 0u; index < SIDE * SIDE; ++index)
    {
        distance[index] = UINT64_MAX;
        done[index] = 0u;
    }
    distance[start] = 0u;
    for (;;)
    {
        uint32_t best = UINT32_MAX;
        for (uint32_t index = 0u; index < SIDE * SIDE; ++index)
            if (done[index] == 0u && distance[index] != UINT64_MAX &&
                (best == UINT32_MAX || distance[index] < distance[best]))
                best = index;
        if (best == UINT32_MAX || best == goal)
            break;
        done[best] = 1u;
        for (uint32_t edge = 0u; edge < city.edgeCount; ++edge)
            if (city.edgeFrom[edge] == best && (city.edgeModes[edge] & modes) != 0u &&
                distance[best] + city.edgeCost[edge] < distance[city.edgeTo[edge]])
                distance[city.edgeTo[edge]] = distance[best] + city.edgeCost[edge];
    }
    return distance[goal];
}

static LaiueNavPathRequest Request(uint32_t start, uint32_t goal, uint32_t modes)
{
    LaiueNavPathRequest request;
    memset(&request, 0, sizeof(request));
    request.structSize = sizeof(request);
    request.start = city.nodes[start];
    request.goal = city.nodes[goal];
    request.modes = modes;
    request.heuristicNumerator = 1u;
    request.heuristicDenominator = 1u;
    return request;
}

static LaiueNavPathResult EmptyResult(void)
{
    LaiueNavPathResult result;
    memset(&result, 0, sizeof(result));
    result.structSize = sizeof(result);
    return result;
}

/* Путь непрерывен, начинается в start, кончается в goal, а сумма стоимостей
 * его рёбер равна заявленной. */
static void CheckPath(const LaiueNavEdge *edges, uint32_t count, uint32_t start, uint32_t goal,
                      uint32_t modes, uint64_t cost)
{
    LaiueNavNode at = city.nodes[start];
    uint64_t total = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        LaiueNavEdgeInfo info;
        memset(&info, 0, sizeof(info));
        info.structSize = sizeof(info);
        Expect(LaiueNavGraphGetEdge(city.graph, edges[index], &info) == LAIUE_NAV_OK, "path edge");
        Expect(info.from == at, "path is continuous");
        Expect((info.modes & modes) != 0u, "path respects modes");
        total += info.cost;
        at = info.to;
    }
    Expect(at == city.nodes[goal], "path ends at goal");
    Expect(total == cost, "path cost matches");
}

static LaiueNavEdge pathEdges[SIDE * SIDE];

static void TestOptimality(LaiueNavQuery *query)
{
    for (uint32_t trial = 0u; trial < 60u; ++trial)
    {
        const uint32_t start = NextRandom() % (SIDE * SIDE);
        const uint32_t goal = NextRandom() % (SIDE * SIDE);
        const uint32_t modes = trial % 3u == 0u ? MODE_CAR : MODE_WALK;
        const uint64_t expected = ReferenceCost(start, goal, modes);
        LaiueNavPathRequest request = Request(start, goal, modes);
        LaiueNavPathResult result = EmptyResult();
        LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &result);
        if (expected == UINT64_MAX)
        {
            Expect(result.pathStatus == LAIUE_NAV_PATH_NO_PATH, "unreachable goal reported");
            continue;
        }
        Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND, "path found");
        Expect(result.cost == expected, "A* cost equals Dijkstra cost");
        CheckPath(pathEdges, result.count, start, goal, modes, result.cost);

        /* Дейкстра без эвристики раскрывает не меньше узлов, а стоимость та же. */
        request.heuristicNumerator = 0u;
        LaiueNavPathResult plain = EmptyResult();
        LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &plain);
        Expect(plain.cost == expected && plain.expansions >= result.expansions,
               "heuristic only saves work");
    }
}

static void TestTimeSlicing(LaiueNavQuery *query)
{
    const uint32_t start = 0u;
    const uint32_t goal = SIDE * SIDE - 1u;
    LaiueNavPathRequest request = Request(start, goal, MODE_WALK);
    LaiueNavPathResult full = EmptyResult();
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &full);
    Expect(full.pathStatus == LAIUE_NAV_PATH_FOUND, "full search");
    static LaiueNavEdge fullEdges[SIDE * SIDE];
    memcpy(fullEdges, pathEdges, (size_t)full.count * sizeof(*fullEdges));

    Expect(LaiueNavQueryBegin(query, city.graph, &request) == LAIUE_NAV_OK, "begin");
    LaiueNavPathResult step = EmptyResult();
    uint32_t slices = 0u;
    do
    {
        LaiueNavQueryContinue(query, 25u, &step);
        ++slices;
        Expect(slices < 10000u, "slicing terminates");
    } while (step.pathStatus == LAIUE_NAV_PATH_IN_PROGRESS);
    Expect(slices > 2u && step.pathStatus == LAIUE_NAV_PATH_FOUND && step.cost == full.cost &&
               step.expansions == full.expansions,
           "sliced search equals full search");
    LaiueNavPathResult path = EmptyResult();
    LaiueNavQueryGetPath(query, pathEdges, SIDE * SIDE, &path);
    Expect(path.count == full.count &&
               memcmp(pathEdges, fullEdges, path.count * sizeof(*pathEdges)) == 0,
           "sliced path equals full path");
    LaiueNavQueryGetPath(query, pathEdges, 1u, &path);
    Expect(path.pathStatus == LAIUE_NAV_PATH_BUFFER_TOO_SMALL && path.count == full.count,
           "short buffer reports needed size");

    /* Бюджет одного вызова. */
    request.maxExpansions = 5u;
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &path);
    Expect(path.pathStatus == LAIUE_NAV_PATH_IN_PROGRESS && path.expansions == 5u,
           "expansion budget stops the search");
    request.maxExpansions = 0u;
    request.maxCost = full.cost - 1u;
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &path);
    Expect(path.pathStatus == LAIUE_NAV_PATH_NO_PATH, "cost limit");
    request.maxCost = full.cost;
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &path);
    Expect(path.pathStatus == LAIUE_NAV_PATH_FOUND, "cost limit is inclusive");

    /* Стоимость можно менять посреди пошагового поиска, структуру — нет. */
    request.maxCost = 0u;
    Expect(LaiueNavQueryBegin(query, city.graph, &request) == LAIUE_NAV_OK, "begin again");
    LaiueNavQueryContinue(query, 10u, &step);
    Expect(LaiueNavGraphSetEdgeCost(city.graph, city.edges[0], city.edgeCost[0]) == LAIUE_NAV_OK,
           "cost change");
    LaiueNavQueryContinue(query, 10u, &step);
    Expect(step.pathStatus == LAIUE_NAV_PATH_IN_PROGRESS, "cost change keeps search alive");
    const LaiueNavPosition distant = {-1000, -1000, 0};
    LaiueNavNode extra = 0u;
    Expect(LaiueNavGraphAddNode(city.graph, &distant, 0u, &extra) == LAIUE_NAV_OK, "extra node");
    LaiueNavQueryContinue(query, 10u, &step);
    Expect(step.pathStatus == LAIUE_NAV_PATH_ERROR && step.status == LAIUE_NAV_STALE,
           "structural change makes search stale");
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &step);
    Expect(step.pathStatus == LAIUE_NAV_PATH_ERROR && step.status == LAIUE_NAV_NOT_PREPARED,
           "unprepared graph is refused");
    Expect(LaiueNavGraphRemoveNode(city.graph, extra) == LAIUE_NAV_OK, "remove extra");
    Expect(LaiueNavGraphPrepare(city.graph) == LAIUE_NAV_OK, "prepare again");
}

static void TestEdits(LaiueNavQuery *query)
{
    const uint32_t start = 5u * SIDE + 5u;
    const uint32_t goal = 5u * SIDE + 6u;
    LaiueNavPathRequest request = Request(start, goal, MODE_WALK);
    LaiueNavPathResult result = EmptyResult();
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND, "neighbour path");

    /* Пробка на прямом ребре уводит путь в объезд без подготовки графа. */
    uint32_t direct = UINT32_MAX;
    for (uint32_t edge = 0u; edge < city.edgeCount; ++edge)
        if (city.edgeFrom[edge] == start && city.edgeTo[edge] == goal)
            direct = edge;
    Expect(direct != UINT32_MAX, "direct edge exists");
    Expect(LaiueNavGraphSetEdgeCost(city.graph, city.edges[direct], 100000u) == LAIUE_NAV_OK,
           "jam the road");
    city.edgeCost[direct] = 100000u;
    Expect(LaiueNavGraphIsPrepared(city.graph), "cost change keeps preparation");
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND && result.count > 1u &&
               result.cost == ReferenceCost(start, goal, MODE_WALK),
           "jammed road is avoided");

    /* Удалённый узел: старый описатель мёртв и после повторного использования слота. */
    const LaiueNavNode removed = city.nodes[start];
    Expect(LaiueNavGraphRemoveNode(city.graph, removed) == LAIUE_NAV_OK, "remove node");
    Expect(LaiueNavGraphGetNode(city.graph, removed, NULL, NULL) == LAIUE_NAV_NOT_FOUND,
           "removed handle is dead");
    Expect(LaiueNavGraphRemoveNode(city.graph, removed) == LAIUE_NAV_NOT_FOUND, "double remove");
    Expect(LaiueNavGraphRemoveEdge(city.graph, city.edges[direct]) == LAIUE_NAV_NOT_FOUND,
           "incident edges were removed");
    const LaiueNavPosition position = {5 * SPACING, 5 * SPACING, 0};
    LaiueNavNode reborn = 0u;
    Expect(LaiueNavGraphAddNode(city.graph, &position, 77u, &reborn) == LAIUE_NAV_OK, "re-add");
    Expect(reborn != removed && (uint32_t)reborn == (uint32_t)removed,
           "slot reused with a new generation");
    Expect(LaiueNavGraphGetNode(city.graph, removed, NULL, NULL) == LAIUE_NAV_NOT_FOUND,
           "old handle stays dead");
    Expect(LaiueNavGraphPrepare(city.graph) == LAIUE_NAV_OK, "prepare after edits");
    request.start = reborn;
    LaiueNavFindPath(city.graph, query, &request, pathEdges, SIDE * SIDE, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_NO_PATH, "isolated node has no path");
}

static void TestBatch(LaiueNavQuery *const *queries, const LaiueTaskExecutor *executor)
{
    enum
    {
        ITEMS = 64
    };
    static LaiueNavBatchItem serial[ITEMS];
    static LaiueNavBatchItem parallel[ITEMS];
    static LaiueNavEdge serialEdges[ITEMS][SIDE * 4u];
    static LaiueNavEdge parallelEdges[ITEMS][SIDE * 4u];
    for (uint32_t index = 0u; index < ITEMS; ++index)
    {
        const uint32_t start = NextRandom() % (SIDE * SIDE);
        const uint32_t goal = NextRandom() % (SIDE * SIDE);
        LaiueNavPathRequest request = Request(start, goal, MODE_WALK);
        /* Узел 5,5 пересоздан: берём живые описатели. */
        if (start == 5u * SIDE + 5u || goal == 5u * SIDE + 5u)
            request = Request(0u, 1u, MODE_WALK);
        serial[index].request = request;
        serial[index].edges = serialEdges[index];
        serial[index].capacity = SIDE * 4u;
        parallel[index] = serial[index];
        parallel[index].edges = parallelEdges[index];
    }
    Expect(LaiueNavFindPathsBatch(city.graph, NULL, queries, 1u, serial, ITEMS) == LAIUE_NAV_OK,
           "serial batch");
    Expect(LaiueNavFindPathsBatch(city.graph, executor, queries, 4u, parallel, ITEMS) ==
               LAIUE_NAV_OK,
           "parallel batch");
    for (uint32_t index = 0u; index < ITEMS; ++index)
    {
        const LaiueNavPathResult *first = &serial[index].result;
        const LaiueNavPathResult *second = &parallel[index].result;
        Expect(first->pathStatus == second->pathStatus && first->cost == second->cost &&
                   first->count == second->count && first->expansions == second->expansions,
               "batch result does not depend on threads");
        if (first->pathStatus == LAIUE_NAV_PATH_FOUND)
            Expect(memcmp(serialEdges[index], parallelEdges[index],
                          first->count * sizeof(LaiueNavEdge)) == 0,
                   "batch path does not depend on threads");
    }
}

/* ---- Сетка -------------------------------------------------------------------- */

static LaiueNavGridPathRequest GridRequest(int64_t startX, int64_t startY, int64_t goalX,
                                           int64_t goalY, uint32_t margin)
{
    LaiueNavGridPathRequest request;
    memset(&request, 0, sizeof(request));
    request.structSize = sizeof(request);
    request.start.x = startX;
    request.start.y = startY;
    request.goal.x = goalX;
    request.goal.y = goalY;
    request.margin = margin;
    return request;
}

static LaiueNavCell cells[4096];

static void CheckCellPath(const LaiueNavGrid *grid, const LaiueNavCell *path, uint32_t count,
                          LaiueNavGridPathRequest request, uint64_t cost)
{
    Expect(count != 0u && path[0].x == request.start.x && path[0].y == request.start.y &&
               path[count - 1u].x == request.goal.x && path[count - 1u].y == request.goal.y,
           "cell path endpoints");
    uint64_t total = 0u;
    for (uint32_t index = 1u; index < count; ++index)
    {
        const int64_t dx = path[index].x - path[index - 1u].x;
        const int64_t dy = path[index].y - path[index - 1u].y;
        Expect(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && (dx != 0 || dy != 0), "unit steps");
        const uint8_t entering = LaiueNavGridGetCell(grid, path[index].x, path[index].y);
        Expect(entering != LAIUE_NAV_CELL_BLOCKED, "path avoids walls");
        if (dx != 0 && dy != 0)
        {
            Expect(LaiueNavGridGetCell(grid, path[index - 1u].x + dx, path[index - 1u].y) != 0u &&
                       LaiueNavGridGetCell(grid, path[index - 1u].x, path[index - 1u].y + dy) != 0u,
                   "no corner cutting");
            total += (uint64_t)LAIUE_NAV_GRID_DIAGONAL_STEP * entering;
        }
        else
            total += (uint64_t)LAIUE_NAV_GRID_STRAIGHT_STEP * entering;
    }
    Expect(total == cost, "cell path cost matches");
}

static void TestGrid(LaiueNavQuery *query)
{
    LaiueNavGridDesc desc = {sizeof(desc), 1u};
    LaiueNavGrid *grid = NULL;
    Expect(LaiueNavGridCreate(&desc, &grid) == LAIUE_NAV_OK, "grid create");

    LaiueNavGridPathRequest request = GridRequest(0, 0, 10, 5, 4u);
    LaiueNavPathResult result = EmptyResult();
    LaiueNavGridFindPath(grid, query, &request, cells, 4096u, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND && result.cost == 10u * 1000u + 5u * 414u,
           "open field cost is octile");
    CheckCellPath(grid, cells, result.count, request, result.cost);
    Expect(LaiueNavGridSmoothPath(grid, cells, result.count) == 2u, "open field smooths to a line");

    /* Стена через границы чанков в отрицательных координатах с проходом. */
    const LaiueNavRect wall = {-3, -80, 1u, 160u};
    Expect(LaiueNavGridFillRect(grid, &wall, LAIUE_NAV_CELL_BLOCKED) == LAIUE_NAV_OK, "wall");
    Expect(LaiueNavGridSetCell(grid, -3, 70, 1u) == LAIUE_NAV_OK, "gap");
    Expect(LaiueNavGridGetCell(grid, -3, -65) == 0u && LaiueNavGridGetCell(grid, -3, 70) == 1u &&
               LaiueNavGridGetCell(grid, -4, -65) == 1u,
           "cells read back across chunks");
    request = GridRequest(-20, 0, 15, 0, 75u);
    LaiueNavGridFindPath(grid, query, &request, cells, 4096u, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND, "path through the gap");
    CheckCellPath(grid, cells, result.count, request, result.cost);
    bool throughGap = false;
    for (uint32_t index = 0u; index < result.count; ++index)
        throughGap = throughGap || (cells[index].x == -3 && cells[index].y == 70);
    Expect(throughGap, "path uses the gap");
    const uint32_t smoothed = LaiueNavGridSmoothPath(grid, cells, result.count);
    Expect(smoothed >= 3u && smoothed < result.count, "smoothing keeps corners around the wall");
    for (uint32_t index = 1u; index < smoothed; ++index)
        Expect(LaiueNavGridGetCell(grid, cells[index].x, cells[index].y) != 0u,
               "smoothed points open");

    /* Без прохода и с узкой рамкой цель недостижима. */
    request.margin = 10u;
    LaiueNavGridFindPath(grid, query, &request, cells, 4096u, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_NO_PATH, "bounds limit the search");

    /* Диагональ между двумя стенами запрещена. */
    LaiueNavGrid *corner = NULL;
    Expect(LaiueNavGridCreate(&desc, &corner) == LAIUE_NAV_OK, "corner grid");
    Expect(LaiueNavGridSetCell(corner, 1, 0, 0u) == LAIUE_NAV_OK &&
               LaiueNavGridSetCell(corner, 0, 1, 0u) == LAIUE_NAV_OK,
           "corner walls");
    request = GridRequest(0, 0, 1, 1, 0u);
    LaiueNavGridFindPath(corner, query, &request, cells, 4096u, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_NO_PATH, "no squeezing through a corner");

    /* Дорогая клетка обходится, если объезд дешевле. */
    Expect(LaiueNavGridSetCell(corner, 1, 0, 1u) == LAIUE_NAV_OK, "open corner");
    request = GridRequest(0, 5, 4, 5, 2u);
    const LaiueNavRect swamp = {2, 4, 1u, 3u};
    Expect(LaiueNavGridFillRect(corner, &swamp, 50u) == LAIUE_NAV_OK, "swamp");
    LaiueNavGridFindPath(corner, query, &request, cells, 4096u, &result);
    Expect(result.pathStatus == LAIUE_NAV_PATH_FOUND, "swamp path");
    CheckCellPath(corner, cells, result.count, request, result.cost);
    for (uint32_t index = 0u; index < result.count; ++index)
        Expect(!(cells[index].x == 2 && cells[index].y >= 4 && cells[index].y <= 6),
               "expensive cells are avoided");

    /* Однородный чанк и его частичное изменение. */
    const LaiueNavRect field = {-128, -128, 256u, 256u};
    Expect(LaiueNavGridFillRect(corner, &field, 3u) == LAIUE_NAV_OK, "uniform fill");
    Expect(LaiueNavGridGetCell(corner, -128, -128) == 3u &&
               LaiueNavGridGetCell(corner, 127, 127) == 3u &&
               LaiueNavGridGetCell(corner, 128, 0) == 1u,
           "uniform chunks read back");
    Expect(LaiueNavGridSetCell(corner, 10, 10, 9u) == LAIUE_NAV_OK &&
               LaiueNavGridGetCell(corner, 10, 10) == 9u &&
               LaiueNavGridGetCell(corner, 11, 10) == 3u,
           "uniform chunk materializes on edit");

    /* Поле потока: стоимость каждой клетки равна стоимости A* из неё, а
     * направления ведут к цели. */
    LaiueNavFlowField *flow = NULL;
    Expect(LaiueNavFlowFieldCreate(&flow) == LAIUE_NAV_OK, "flow create");
    const LaiueNavRect bounds = {-30, -30, 60u, 60u};
    const LaiueNavCell goal = {12, 0};
    Expect(LaiueNavFlowFieldBuild(flow, grid, &bounds, &goal, 1u) == LAIUE_NAV_OK, "flow build");
    Expect(LaiueNavFlowFieldCost(flow, 12, 0) == 0u &&
               LaiueNavFlowFieldDirection(flow, 12, 0) == LAIUE_NAV_FLOW_NONE,
           "goal cell");
    Expect(LaiueNavFlowFieldCost(flow, -3, 0) == LAIUE_NAV_FLOW_UNREACHABLE, "wall unreachable");
    static const int32_t stepX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int32_t stepY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (uint32_t trial = 0u; trial < 25u; ++trial)
    {
        const int64_t x = -30 + (int64_t)(NextRandom() % 60u);
        const int64_t y = -30 + (int64_t)(NextRandom() % 60u);
        const uint32_t cost = LaiueNavFlowFieldCost(flow, x, y);
        if (LaiueNavGridGetCell(grid, x, y) == 0u)
        {
            Expect(cost == LAIUE_NAV_FLOW_UNREACHABLE, "blocked start unreachable");
            continue;
        }
        LaiueNavGridPathRequest check = GridRequest(x, y, goal.x, goal.y, 0u);
        check.bounds = bounds;
        LaiueNavGridFindPath(grid, query, &check, cells, 4096u, &result);
        if (result.pathStatus != LAIUE_NAV_PATH_FOUND)
        {
            Expect(cost == LAIUE_NAV_FLOW_UNREACHABLE, "unreachable agrees");
            continue;
        }
        Expect(cost == result.cost, "flow cost equals A* cost");
        int64_t walkX = x;
        int64_t walkY = y;
        for (uint32_t steps = 0u; walkX != goal.x || walkY != goal.y; ++steps)
        {
            const uint8_t direction = LaiueNavFlowFieldDirection(flow, walkX, walkY);
            Expect(direction < 8u && steps < 3600u, "flow leads to the goal");
            const uint32_t before = LaiueNavFlowFieldCost(flow, walkX, walkY);
            walkX += stepX[direction];
            walkY += stepY[direction];
            Expect(LaiueNavFlowFieldCost(flow, walkX, walkY) < before, "flow strictly descends");
        }
    }
    LaiueNavFlowFieldDestroy(flow);
    LaiueNavGridDestroy(corner);
    LaiueNavGridDestroy(grid);
}

LAIUE_TEST_ENTRY(NavigationTestEntryPoint)
{
    BuildCity();
    LaiueNavQuery *queries[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t index = 0u; index < 4u; ++index)
        Expect(LaiueNavQueryCreate(&queries[index]) == LAIUE_NAV_OK, "query create");
    LaiueTaskPool *pool = LaiueTaskPoolCreate(4u);
    Expect(pool != NULL, "pool");
    LaiueTaskExecutor executor;
    memset(&executor, 0, sizeof(executor));
    executor.structSize = sizeof(executor);
    Expect(LaiueTaskPoolGetExecutor(pool, &executor), "executor");

    TestOptimality(queries[0]);
    TestTimeSlicing(queries[0]);
    TestBatch(queries, &executor);
    TestEdits(queries[0]);
    TestGrid(queries[1]);

    LaiueTaskPoolDestroy(pool);
    for (uint32_t index = 0u; index < 4u; ++index)
        LaiueNavQueryDestroy(queries[index]);
    LaiueNavGraphDestroy(city.graph);
    LaiueTestRuntimeWrite("navigation: passed\n");
    LAIUE_TEST_SUCCESS();
}
