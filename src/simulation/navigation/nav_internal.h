#pragma once

#include "navigation/navigation.h"

#include "platform/system.h"

#define NAV_NONE UINT32_MAX

typedef struct NavNodeRecord
{
    LaiueNavPosition position;
    uint32_t generation;
    uint32_t userData;
    uint32_t firstOut;
    uint32_t firstIn;
    uint32_t alive;
} NavNodeRecord;

typedef struct NavEdgeRecord
{
    uint32_t from;
    uint32_t to;
    uint32_t cost;
    uint32_t modes;
    uint32_t userData;
    uint32_t generation;
    uint32_t nextOut;
    uint32_t nextIn;
    uint32_t alive;
} NavEdgeRecord;

/* Компактное ребро для поиска: исходящие рёбра узла лежат подряд. */
typedef struct NavCsrEdge
{
    uint32_t to;
    uint32_t cost;
    uint32_t modes;
    uint32_t edge;
} NavCsrEdge;

struct LaiueNavGraph
{
    NavNodeRecord *nodes;
    uint32_t nodeSlots;
    uint32_t nodeCapacity;
    uint32_t aliveNodes;
    uint32_t *freeNodes;
    uint32_t freeNodeCount;

    NavEdgeRecord *edges;
    uint32_t edgeSlots;
    uint32_t edgeCapacity;
    uint32_t aliveEdges;
    uint32_t *freeEdges;
    uint32_t freeEdgeCount;

    NavCsrEdge *csr;
    uint32_t *csrStart; /* nodeSlots + 1 */
    uint32_t *edgeSlot; /* edgeSlots: позиция ребра в csr */
    uint32_t csrNodeSlots;
    bool prepared;

    uint64_t revision;
    uint64_t structureRevision;
};

/* Элемент открытого списка: f по возрастанию, при равенстве — дальше
 * продвинувшийся (больший g), затем меньший индекс узла. */
typedef struct NavHeapEntry
{
    uint64_t f;
    uint64_t g;
    uint32_t node;
} NavHeapEntry;

typedef enum NavSearchKind
{
    NAV_SEARCH_NONE,
    NAV_SEARCH_GRAPH,
    NAV_SEARCH_GRID,
} NavSearchKind;

/* Всё состояние узла в одной записи: раскрытие соседа трогает одну линию
 * кеша, а не четыре массива. */
typedef struct NavNodeState
{
    uint64_t g;
    uint32_t parent;
    uint32_t stamp;  /* == currentStamp — g и parent действительны */
    uint32_t closed; /* == currentStamp — узел раскрыт */
    uint32_t reserved;
} NavNodeState;

struct LaiueNavQuery
{
    NavNodeState *nodes;
    uint32_t capacity;
    uint32_t currentStamp;

    NavHeapEntry *heap;
    uint32_t heapCount;
    uint32_t heapCapacity;

    NavSearchKind kind;
    LaiueNavPathStatus state;
    LaiueNavStatus failure;
    uint32_t expansions;
    uint64_t foundCost;

    const LaiueNavGraph *graph;
    uint64_t structureRevision;
    LaiueNavPathRequest request;
    uint32_t startIndex;
    uint32_t goalIndex;
};

/* Готовит память под nodeCount узлов и начинает новый поиск. */
bool NavQueryReset(LaiueNavQuery *query, uint32_t nodeCount);
bool NavHeapPush(LaiueNavQuery *query, uint64_t f, uint64_t g, uint32_t node);
NavHeapEntry NavHeapPop(LaiueNavQuery *query);

static inline bool NavQueryVisited(const LaiueNavQuery *query, uint32_t node)
{
    return query->nodes[node].stamp == query->currentStamp;
}

static inline bool NavQueryClosed(const LaiueNavQuery *query, uint32_t node)
{
    return query->nodes[node].closed == query->currentStamp;
}

static inline void NavQuerySet(LaiueNavQuery *query, uint32_t node, uint64_t g, uint32_t parent)
{
    NavNodeState *state = &query->nodes[node];
    state->g = g;
    state->parent = parent;
    state->stamp = query->currentStamp;
}

/* Целый квадратный корень с округлением вниз. */
uint64_t NavIntegerSqrt(uint64_t value);
