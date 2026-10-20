#include "navigation/nav_internal.h"

#include <string.h>

#define NAV_HEURISTIC_LIMIT 65535u
/* Разности координат больше этой дают нулевую эвристику: квадрат суммы
 * трёх осей обязан уместиться в 64 бита. */
#define NAV_HEURISTIC_AXIS_LIMIT (INT64_C(1) << 31)

static LaiueNavNode MakeHandle(uint32_t index, uint32_t generation)
{
    return ((uint64_t)generation << 32u) | index;
}

static uint32_t HandleIndex(uint64_t handle)
{
    return (uint32_t)handle;
}

static uint32_t HandleGeneration(uint64_t handle)
{
    return (uint32_t)(handle >> 32u);
}

static uint32_t NextGeneration(uint32_t generation)
{
    return generation == UINT32_MAX ? 1u : generation + 1u;
}

static const NavNodeRecord *LiveNode(const LaiueNavGraph *graph, LaiueNavNode node)
{
    const uint32_t index = HandleIndex(node);
    if (graph == NULL || index >= graph->nodeSlots)
        return NULL;
    const NavNodeRecord *record = &graph->nodes[index];
    return record->alive != 0u && record->generation == HandleGeneration(node) ? record : NULL;
}

static const NavEdgeRecord *LiveEdge(const LaiueNavGraph *graph, LaiueNavEdge edge)
{
    const uint32_t index = HandleIndex(edge);
    if (graph == NULL || index >= graph->edgeSlots)
        return NULL;
    const NavEdgeRecord *record = &graph->edges[index];
    return record->alive != 0u && record->generation == HandleGeneration(edge) ? record : NULL;
}

static bool Grow(void **array, uint32_t *capacity, uint32_t needed, size_t elementSize,
                 uint32_t **freeList)
{
    if (needed <= *capacity)
        return true;
    uint32_t grown = *capacity == 0u ? 64u : *capacity;
    while (grown < needed)
    {
        if (grown > UINT32_MAX / 2u)
            return false;
        grown *= 2u;
    }
    void *records = PlatformReallocate(*array, (size_t)grown * elementSize, false);
    if (records == NULL)
        return false;
    *array = records;
    uint32_t *list = PlatformReallocate(*freeList, (size_t)grown * sizeof(uint32_t), false);
    if (list == NULL)
        return false;
    *freeList = list;
    *capacity = grown;
    return true;
}

static void MarkStructureChanged(LaiueNavGraph *graph)
{
    ++graph->revision;
    ++graph->structureRevision;
    graph->prepared = false;
}

LaiueNavStatus LaiueNavGraphCreate(const LaiueNavGraphDesc *desc, LaiueNavGraph **outGraph)
{
    if (outGraph != NULL)
        *outGraph = NULL;
    if (outGraph == NULL || (desc != NULL && desc->structSize < sizeof(*desc)))
        return LAIUE_NAV_INVALID_ARGUMENT;
    LaiueNavGraph *graph = PlatformAllocate(sizeof(*graph), true);
    if (graph == NULL)
        return LAIUE_NAV_OUT_OF_MEMORY;
    if (desc != NULL && (!Grow((void **)&graph->nodes, &graph->nodeCapacity, desc->nodeCapacityHint,
                               sizeof(NavNodeRecord), &graph->freeNodes) ||
                         !Grow((void **)&graph->edges, &graph->edgeCapacity, desc->edgeCapacityHint,
                               sizeof(NavEdgeRecord), &graph->freeEdges)))
    {
        LaiueNavGraphDestroy(graph);
        return LAIUE_NAV_OUT_OF_MEMORY;
    }
    /* Пустой граф сразу подготовлен: поиск по нему просто ничего не найдёт. */
    const LaiueNavStatus status = LaiueNavGraphPrepare(graph);
    if (status != LAIUE_NAV_OK)
    {
        LaiueNavGraphDestroy(graph);
        return status;
    }
    *outGraph = graph;
    return LAIUE_NAV_OK;
}

void LaiueNavGraphDestroy(LaiueNavGraph *graph)
{
    if (graph == NULL)
        return;
    PlatformFree(graph->nodes);
    PlatformFree(graph->freeNodes);
    PlatformFree(graph->edges);
    PlatformFree(graph->freeEdges);
    PlatformFree(graph->csr);
    PlatformFree(graph->csrStart);
    PlatformFree(graph->edgeSlot);
    PlatformFree(graph);
}

LaiueNavStatus LaiueNavGraphAddNode(LaiueNavGraph *graph, const LaiueNavPosition *position,
                                    uint32_t userData, LaiueNavNode *outNode)
{
    if (outNode != NULL)
        *outNode = 0u;
    if (graph == NULL || position == NULL || outNode == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    uint32_t index;
    if (graph->freeNodeCount != 0u)
        index = graph->freeNodes[--graph->freeNodeCount];
    else
    {
        if (graph->nodeSlots == NAV_NONE - 1u)
            return LAIUE_NAV_LIMIT_REACHED;
        if (!Grow((void **)&graph->nodes, &graph->nodeCapacity, graph->nodeSlots + 1u,
                  sizeof(NavNodeRecord), &graph->freeNodes))
            return LAIUE_NAV_OUT_OF_MEMORY;
        index = graph->nodeSlots++;
        graph->nodes[index].generation = 0u;
    }
    NavNodeRecord *record = &graph->nodes[index];
    record->generation = NextGeneration(record->generation);
    record->position = *position;
    record->userData = userData;
    record->firstOut = NAV_NONE;
    record->firstIn = NAV_NONE;
    record->alive = 1u;
    ++graph->aliveNodes;
    MarkStructureChanged(graph);
    *outNode = MakeHandle(index, record->generation);
    return LAIUE_NAV_OK;
}

static void UnlinkOut(LaiueNavGraph *graph, uint32_t edgeIndex)
{
    NavEdgeRecord *edge = &graph->edges[edgeIndex];
    uint32_t *link = &graph->nodes[edge->from].firstOut;
    while (*link != edgeIndex)
        link = &graph->edges[*link].nextOut;
    *link = edge->nextOut;
}

static void UnlinkIn(LaiueNavGraph *graph, uint32_t edgeIndex)
{
    NavEdgeRecord *edge = &graph->edges[edgeIndex];
    uint32_t *link = &graph->nodes[edge->to].firstIn;
    while (*link != edgeIndex)
        link = &graph->edges[*link].nextIn;
    *link = edge->nextIn;
}

static void FreeEdge(LaiueNavGraph *graph, uint32_t edgeIndex)
{
    NavEdgeRecord *edge = &graph->edges[edgeIndex];
    edge->alive = 0u;
    edge->generation = NextGeneration(edge->generation);
    graph->freeEdges[graph->freeEdgeCount++] = edgeIndex;
    --graph->aliveEdges;
}

LaiueNavStatus LaiueNavGraphRemoveNode(LaiueNavGraph *graph, LaiueNavNode node)
{
    if (LiveNode(graph, node) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    const uint32_t index = HandleIndex(node);
    NavNodeRecord *record = &graph->nodes[index];
    while (record->firstOut != NAV_NONE)
    {
        const uint32_t edgeIndex = record->firstOut;
        record->firstOut = graph->edges[edgeIndex].nextOut;
        UnlinkIn(graph, edgeIndex);
        FreeEdge(graph, edgeIndex);
    }
    while (record->firstIn != NAV_NONE)
    {
        const uint32_t edgeIndex = record->firstIn;
        record->firstIn = graph->edges[edgeIndex].nextIn;
        UnlinkOut(graph, edgeIndex);
        FreeEdge(graph, edgeIndex);
    }
    record->alive = 0u;
    record->generation = NextGeneration(record->generation);
    graph->freeNodes[graph->freeNodeCount++] = index;
    --graph->aliveNodes;
    MarkStructureChanged(graph);
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphGetNode(const LaiueNavGraph *graph, LaiueNavNode node,
                                    LaiueNavPosition *outPosition, uint32_t *outUserData)
{
    const NavNodeRecord *record = LiveNode(graph, node);
    if (record == NULL)
        return LAIUE_NAV_NOT_FOUND;
    if (outPosition != NULL)
        *outPosition = record->position;
    if (outUserData != NULL)
        *outUserData = record->userData;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphSetNodePosition(LaiueNavGraph *graph, LaiueNavNode node,
                                            const LaiueNavPosition *position)
{
    if (position == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (LiveNode(graph, node) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    graph->nodes[HandleIndex(node)].position = *position;
    ++graph->revision;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphAddEdge(LaiueNavGraph *graph, LaiueNavNode from, LaiueNavNode to,
                                    const LaiueNavEdgeDesc *desc, LaiueNavEdge *outEdge)
{
    if (outEdge != NULL)
        *outEdge = 0u;
    if (graph == NULL || desc == NULL || outEdge == NULL || desc->structSize < sizeof(*desc) ||
        desc->cost == 0u)
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (LiveNode(graph, from) == NULL || LiveNode(graph, to) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    uint32_t index;
    if (graph->freeEdgeCount != 0u)
        index = graph->freeEdges[--graph->freeEdgeCount];
    else
    {
        if (graph->edgeSlots == NAV_NONE - 1u)
            return LAIUE_NAV_LIMIT_REACHED;
        if (!Grow((void **)&graph->edges, &graph->edgeCapacity, graph->edgeSlots + 1u,
                  sizeof(NavEdgeRecord), &graph->freeEdges))
            return LAIUE_NAV_OUT_OF_MEMORY;
        index = graph->edgeSlots++;
        graph->edges[index].generation = 0u;
    }
    NavEdgeRecord *edge = &graph->edges[index];
    edge->generation = NextGeneration(edge->generation);
    edge->from = HandleIndex(from);
    edge->to = HandleIndex(to);
    edge->cost = desc->cost;
    edge->modes = desc->modes;
    edge->userData = desc->userData;
    edge->alive = 1u;
    edge->nextOut = graph->nodes[edge->from].firstOut;
    graph->nodes[edge->from].firstOut = index;
    edge->nextIn = graph->nodes[edge->to].firstIn;
    graph->nodes[edge->to].firstIn = index;
    ++graph->aliveEdges;
    MarkStructureChanged(graph);
    *outEdge = MakeHandle(index, edge->generation);
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphRemoveEdge(LaiueNavGraph *graph, LaiueNavEdge edge)
{
    if (LiveEdge(graph, edge) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    const uint32_t index = HandleIndex(edge);
    UnlinkOut(graph, index);
    UnlinkIn(graph, index);
    FreeEdge(graph, index);
    MarkStructureChanged(graph);
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphGetEdge(const LaiueNavGraph *graph, LaiueNavEdge edge,
                                    LaiueNavEdgeInfo *outInfo)
{
    const NavEdgeRecord *record = LiveEdge(graph, edge);
    if (outInfo == NULL || outInfo->structSize < sizeof(*outInfo))
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (record == NULL)
        return LAIUE_NAV_NOT_FOUND;
    outInfo->from = MakeHandle(record->from, graph->nodes[record->from].generation);
    outInfo->to = MakeHandle(record->to, graph->nodes[record->to].generation);
    outInfo->cost = record->cost;
    outInfo->modes = record->modes;
    outInfo->userData = record->userData;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphSetEdgeCost(LaiueNavGraph *graph, LaiueNavEdge edge, uint32_t cost)
{
    if (cost == 0u)
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (LiveEdge(graph, edge) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    const uint32_t index = HandleIndex(edge);
    graph->edges[index].cost = cost;
    if (graph->prepared)
        graph->csr[graph->edgeSlot[index]].cost = cost;
    ++graph->revision;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphSetEdgeModes(LaiueNavGraph *graph, LaiueNavEdge edge, uint32_t modes)
{
    if (LiveEdge(graph, edge) == NULL)
        return LAIUE_NAV_NOT_FOUND;
    const uint32_t index = HandleIndex(edge);
    graph->edges[index].modes = modes;
    if (graph->prepared)
        graph->csr[graph->edgeSlot[index]].modes = modes;
    ++graph->revision;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGraphPrepare(LaiueNavGraph *graph)
{
    if (graph == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (graph->prepared)
        return LAIUE_NAV_OK;
    const uint32_t nodeSlots = graph->nodeSlots;
    uint32_t *start = PlatformAllocate(((size_t)nodeSlots + 1u) * sizeof(*start), true);
    NavCsrEdge *csr = PlatformAllocate(
        (size_t)(graph->aliveEdges == 0u ? 1u : graph->aliveEdges) * sizeof(*csr), false);
    uint32_t *slot = PlatformAllocate(
        (size_t)(graph->edgeSlots == 0u ? 1u : graph->edgeSlots) * sizeof(*slot), false);
    if (start == NULL || csr == NULL || slot == NULL)
    {
        PlatformFree(start);
        PlatformFree(csr);
        PlatformFree(slot);
        return LAIUE_NAV_OUT_OF_MEMORY;
    }
    /* Рёбра узла идут по возрастанию индекса ребра: порядок не зависит от
     * истории вставок и удалений, только от индексов. */
    for (uint32_t index = 0u; index < graph->edgeSlots; ++index)
        if (graph->edges[index].alive != 0u)
            ++start[graph->edges[index].from + 1u];
    for (uint32_t index = 0u; index < nodeSlots; ++index)
        start[index + 1u] += start[index];
    for (uint32_t index = 0u; index < graph->edgeSlots; ++index)
    {
        const NavEdgeRecord *edge = &graph->edges[index];
        slot[index] = NAV_NONE;
        if (edge->alive == 0u)
            continue;
        /* start[from] временно служит курсором заполнения. */
        const uint32_t position = start[edge->from]++;
        csr[position].to = edge->to;
        csr[position].cost = edge->cost;
        csr[position].modes = edge->modes;
        csr[position].edge = index;
        slot[index] = position;
    }
    for (uint32_t index = nodeSlots; index != 0u; --index)
        start[index] = start[index - 1u];
    start[0] = 0u;
    PlatformFree(graph->csr);
    PlatformFree(graph->csrStart);
    PlatformFree(graph->edgeSlot);
    graph->csr = csr;
    graph->csrStart = start;
    graph->edgeSlot = slot;
    graph->csrNodeSlots = nodeSlots;
    graph->prepared = true;
    return LAIUE_NAV_OK;
}

bool LaiueNavGraphIsPrepared(const LaiueNavGraph *graph)
{
    return graph != NULL && graph->prepared;
}

uint32_t LaiueNavGraphNodeCount(const LaiueNavGraph *graph)
{
    return graph == NULL ? 0u : graph->aliveNodes;
}

uint32_t LaiueNavGraphEdgeCount(const LaiueNavGraph *graph)
{
    return graph == NULL ? 0u : graph->aliveEdges;
}

uint64_t LaiueNavGraphRevision(const LaiueNavGraph *graph)
{
    return graph == NULL ? 0u : graph->revision;
}

/* ---- Поиск ----------------------------------------------------------------- */

static uint64_t AbsoluteDifference(int64_t first, int64_t second)
{
    return first >= second ? (uint64_t)first - (uint64_t)second
                           : (uint64_t)second - (uint64_t)first;
}

static uint64_t Heuristic(const LaiueNavQuery *query, uint32_t node)
{
    const LaiueNavPathRequest *request = &query->request;
    if (request->heuristicNumerator == 0u)
        return 0u;
    const LaiueNavPosition *from = &query->graph->nodes[node].position;
    const LaiueNavPosition *to = &query->graph->nodes[query->goalIndex].position;
    const uint64_t dx = AbsoluteDifference(from->x, to->x);
    const uint64_t dy = AbsoluteDifference(from->y, to->y);
    const uint64_t dz = AbsoluteDifference(from->z, to->z);
    if (dx >= (uint64_t)NAV_HEURISTIC_AXIS_LIMIT || dy >= (uint64_t)NAV_HEURISTIC_AXIS_LIMIT ||
        dz >= (uint64_t)NAV_HEURISTIC_AXIS_LIMIT)
        return 0u;
    /* Корень снизу и деление снизу оставляют оценку не выше точной. */
    const uint64_t distance = NavIntegerSqrt(dx * dx + dy * dy + dz * dz);
    return distance * request->heuristicNumerator / request->heuristicDenominator;
}

static void SetResult(LaiueNavPathResult *result, LaiueNavPathStatus pathStatus,
                      LaiueNavStatus status, uint32_t count, uint32_t expansions, uint64_t cost)
{
    if (result == NULL || result->structSize < sizeof(*result))
        return;
    result->pathStatus = pathStatus;
    result->status = status;
    result->count = count;
    result->expansions = expansions;
    result->cost = cost;
}

static void Fail(LaiueNavQuery *query, LaiueNavStatus status)
{
    query->state = LAIUE_NAV_PATH_ERROR;
    query->failure = status;
}

LaiueNavStatus LaiueNavQueryBegin(LaiueNavQuery *query, const LaiueNavGraph *graph,
                                  const LaiueNavPathRequest *request)
{
    if (query == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    query->kind = NAV_SEARCH_GRAPH;
    query->graph = graph;
    if (graph == NULL || request == NULL || request->structSize < sizeof(*request) ||
        request->heuristicNumerator > NAV_HEURISTIC_LIMIT ||
        request->heuristicDenominator > NAV_HEURISTIC_LIMIT ||
        (request->heuristicNumerator != 0u && request->heuristicDenominator == 0u))
    {
        Fail(query, LAIUE_NAV_INVALID_ARGUMENT);
        return LAIUE_NAV_INVALID_ARGUMENT;
    }
    if (!graph->prepared)
    {
        Fail(query, LAIUE_NAV_NOT_PREPARED);
        return LAIUE_NAV_NOT_PREPARED;
    }
    if (LiveNode(graph, request->start) == NULL || LiveNode(graph, request->goal) == NULL)
    {
        Fail(query, LAIUE_NAV_NOT_FOUND);
        return LAIUE_NAV_NOT_FOUND;
    }
    if (!NavQueryReset(query, graph->nodeSlots))
    {
        Fail(query, LAIUE_NAV_OUT_OF_MEMORY);
        return LAIUE_NAV_OUT_OF_MEMORY;
    }
    query->request = *request;
    query->request.structSize = sizeof(query->request);
    query->structureRevision = graph->structureRevision;
    query->startIndex = HandleIndex(request->start);
    query->goalIndex = HandleIndex(request->goal);
    query->state = LAIUE_NAV_PATH_IN_PROGRESS;
    const uint32_t start = query->startIndex;
    NavQuerySet(query, start, 0u, NAV_NONE);
    if (!NavHeapPush(query, Heuristic(query, start), 0u, start))
    {
        Fail(query, LAIUE_NAV_OUT_OF_MEMORY);
        return LAIUE_NAV_OUT_OF_MEMORY;
    }
    return LAIUE_NAV_OK;
}

static uint32_t PathLength(const LaiueNavQuery *query)
{
    uint32_t count = 0u;
    for (uint32_t node = query->goalIndex; node != query->startIndex;)
    {
        const NavCsrEdge *edge = &query->graph->csr[query->nodes[node].parent];
        node = query->graph->edges[edge->edge].from;
        ++count;
    }
    return count;
}

void LaiueNavQueryContinue(LaiueNavQuery *query, uint32_t maxExpansions,
                           LaiueNavPathResult *outResult)
{
    if (query == NULL || query->kind != NAV_SEARCH_GRAPH)
    {
        SetResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_INVALID_ARGUMENT, 0u, 0u, 0u);
        return;
    }
    const LaiueNavGraph *graph = query->graph;
    if (query->state == LAIUE_NAV_PATH_IN_PROGRESS &&
        (!graph->prepared || graph->structureRevision != query->structureRevision))
        Fail(query, LAIUE_NAV_STALE);
    const LaiueNavPathRequest *request = &query->request;
    uint32_t budget = maxExpansions;
    while (query->state == LAIUE_NAV_PATH_IN_PROGRESS)
    {
        if (query->heapCount == 0u)
        {
            query->state = LAIUE_NAV_PATH_NO_PATH;
            break;
        }
        if (maxExpansions != 0u && budget == 0u)
            break;
        const NavHeapEntry top = NavHeapPop(query);
        const uint32_t node = top.node;
        if (NavQueryClosed(query, node) || top.g != query->nodes[node].g)
            continue;
        if (request->maxCost != 0u && top.f > request->maxCost)
        {
            query->state = LAIUE_NAV_PATH_NO_PATH;
            break;
        }
        query->nodes[node].closed = query->currentStamp;
        ++query->expansions;
        if (budget != 0u)
            --budget;
        if (node == query->goalIndex)
        {
            query->state = LAIUE_NAV_PATH_FOUND;
            query->foundCost = top.g;
            break;
        }
        const uint32_t end = graph->csrStart[node + 1u];
        for (uint32_t slot = graph->csrStart[node]; slot < end; ++slot)
        {
            const NavCsrEdge *edge = &graph->csr[slot];
            if ((edge->modes & request->modes) == 0u)
                continue;
            const uint32_t next = edge->to;
            if (NavQueryClosed(query, next))
                continue;
            const uint64_t g = top.g + edge->cost;
            if (NavQueryVisited(query, next) && query->nodes[next].g <= g)
                continue;
            NavQuerySet(query, next, g, slot);
            const uint64_t h = Heuristic(query, next);
            const uint64_t f = h > UINT64_MAX - g ? UINT64_MAX : g + h;
            if (!NavHeapPush(query, f, g, next))
            {
                Fail(query, LAIUE_NAV_OUT_OF_MEMORY);
                break;
            }
        }
    }
    const uint32_t count = query->state == LAIUE_NAV_PATH_FOUND ? PathLength(query) : 0u;
    SetResult(outResult, query->state, query->failure, count, query->expansions, query->foundCost);
}

void LaiueNavQueryGetPath(LaiueNavQuery *query, LaiueNavEdge *edges, uint32_t capacity,
                          LaiueNavPathResult *outResult)
{
    if (query == NULL || query->kind != NAV_SEARCH_GRAPH || query->state != LAIUE_NAV_PATH_FOUND ||
        (edges == NULL && capacity != 0u))
    {
        SetResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_INVALID_ARGUMENT, 0u, 0u, 0u);
        return;
    }
    const LaiueNavGraph *graph = query->graph;
    if (!graph->prepared || graph->structureRevision != query->structureRevision)
    {
        SetResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_STALE, 0u, query->expansions, 0u);
        return;
    }
    const uint32_t count = PathLength(query);
    if (count > capacity)
    {
        SetResult(outResult, LAIUE_NAV_PATH_BUFFER_TOO_SMALL, LAIUE_NAV_OK, count,
                  query->expansions, query->foundCost);
        return;
    }
    uint32_t position = count;
    for (uint32_t node = query->goalIndex; node != query->startIndex;)
    {
        const NavCsrEdge *edge = &graph->csr[query->nodes[node].parent];
        const NavEdgeRecord *record = &graph->edges[edge->edge];
        edges[--position] = MakeHandle(edge->edge, record->generation);
        node = record->from;
    }
    SetResult(outResult, LAIUE_NAV_PATH_FOUND, LAIUE_NAV_OK, count, query->expansions,
              query->foundCost);
}

void LaiueNavFindPath(const LaiueNavGraph *graph, LaiueNavQuery *query,
                      const LaiueNavPathRequest *request, LaiueNavEdge *edges, uint32_t capacity,
                      LaiueNavPathResult *outResult)
{
    const LaiueNavStatus status = LaiueNavQueryBegin(query, graph, request);
    if (status != LAIUE_NAV_OK)
    {
        SetResult(outResult, LAIUE_NAV_PATH_ERROR, status, 0u, 0u, 0u);
        return;
    }
    LaiueNavQueryContinue(query, request->maxExpansions, outResult);
    if (query->state == LAIUE_NAV_PATH_FOUND)
        LaiueNavQueryGetPath(query, edges, capacity, outResult);
}

typedef struct BatchContext
{
    const LaiueNavGraph *graph;
    LaiueNavQuery *const *queries;
    uint32_t queryCount;
    LaiueNavBatchItem *items;
    uint32_t itemCount;
} BatchContext;

static void BatchRange(void *context, uint32_t begin, uint32_t end)
{
    const BatchContext *batch = context;
    for (uint32_t group = begin; group < end; ++group)
    {
        /* Отрезок элементов группы зависит только от числа групп. */
        const uint64_t first = (uint64_t)batch->itemCount * group / batch->queryCount;
        const uint64_t last = (uint64_t)batch->itemCount * (group + 1u) / batch->queryCount;
        for (uint64_t index = first; index < last; ++index)
        {
            LaiueNavBatchItem *item = &batch->items[index];
            item->result.structSize = sizeof(item->result);
            LaiueNavFindPath(batch->graph, batch->queries[group], &item->request, item->edges,
                             item->capacity, &item->result);
        }
    }
}

LaiueNavStatus LaiueNavFindPathsBatch(const LaiueNavGraph *graph, const LaiueTaskExecutor *executor,
                                      LaiueNavQuery *const *queries, uint32_t queryCount,
                                      LaiueNavBatchItem *items, uint32_t itemCount)
{
    if (graph == NULL || queries == NULL || queryCount == 0u ||
        (items == NULL && itemCount != 0u) ||
        (executor != NULL && executor->structSize < sizeof(*executor)))
        return LAIUE_NAV_INVALID_ARGUMENT;
    for (uint32_t index = 0u; index < queryCount; ++index)
        if (queries[index] == NULL)
            return LAIUE_NAV_INVALID_ARGUMENT;
    if (!graph->prepared)
        return LAIUE_NAV_NOT_PREPARED;
    BatchContext context = {graph, queries, queryCount, items, itemCount};
    if (executor != NULL && executor->run != NULL && queryCount > 1u)
        executor->run(executor->context, queryCount, 1u, BatchRange, &context);
    else
        BatchRange(&context, 0u, queryCount);
    return LAIUE_NAV_OK;
}
