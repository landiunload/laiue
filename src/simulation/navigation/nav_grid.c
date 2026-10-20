#include "navigation/nav_internal.h"

#include <string.h>

#define CHUNK_SHIFT 6u
#define CHUNK_SIDE (1u << CHUNK_SHIFT)
#define CHUNK_MASK (CHUNK_SIDE - 1u)
#define CHUNK_CELLS (CHUNK_SIDE * CHUNK_SIDE)

/* Чанк, целиком заполненный одной стоимостью, не хранит массив клеток:
 * бескрайний луг или море стоят одну запись. */
typedef struct GridChunk
{
    int64_t chunkX;
    int64_t chunkY;
    uint8_t *cells;
    uint8_t uniform;
    uint8_t used;
} GridChunk;

struct LaiueNavGrid
{
    GridChunk *slots;
    uint32_t capacity; /* степень двойки */
    uint32_t count;
    uint8_t defaultCost;
    uint64_t revision;
};

struct LaiueNavFlowField
{
    LaiueNavQuery query;
    uint8_t *directions;
    uint32_t directionCapacity;
    LaiueNavRect bounds;
    bool built;
};

/* Направления по порядку из navigation.h. */
static const int32_t directionX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int32_t directionY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

static int64_t FloorChunk(int64_t value)
{
    return value >= 0 ? value >> CHUNK_SHIFT : ~((~value) >> CHUNK_SHIFT);
}

static uint32_t LocalIndex(int64_t x, int64_t y)
{
    return (uint32_t)(((uint64_t)y & CHUNK_MASK) << CHUNK_SHIFT) |
           (uint32_t)((uint64_t)x & CHUNK_MASK);
}

static uint64_t Mix(int64_t chunkX, int64_t chunkY)
{
    uint64_t value = (uint64_t)chunkX * UINT64_C(0x9E3779B97F4A7C15) ^ (uint64_t)chunkY;
    value ^= value >> 30u;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31u);
}

static const GridChunk *FindChunk(const LaiueNavGrid *grid, int64_t chunkX, int64_t chunkY)
{
    if (grid->capacity == 0u)
        return NULL;
    uint32_t index = (uint32_t)Mix(chunkX, chunkY) & (grid->capacity - 1u);
    for (;;)
    {
        const GridChunk *chunk = &grid->slots[index];
        if (chunk->used == 0u)
            return NULL;
        if (chunk->chunkX == chunkX && chunk->chunkY == chunkY)
            return chunk;
        index = (index + 1u) & (grid->capacity - 1u);
    }
}

static bool Rehash(LaiueNavGrid *grid, uint32_t capacity)
{
    GridChunk *slots = PlatformAllocate((size_t)capacity * sizeof(*slots), true);
    if (slots == NULL)
        return false;
    for (uint32_t index = 0u; index < grid->capacity; ++index)
    {
        const GridChunk *chunk = &grid->slots[index];
        if (chunk->used == 0u)
            continue;
        uint32_t target = (uint32_t)Mix(chunk->chunkX, chunk->chunkY) & (capacity - 1u);
        while (slots[target].used != 0u)
            target = (target + 1u) & (capacity - 1u);
        slots[target] = *chunk;
    }
    PlatformFree(grid->slots);
    grid->slots = slots;
    grid->capacity = capacity;
    return true;
}

static GridChunk *ObtainChunk(LaiueNavGrid *grid, int64_t chunkX, int64_t chunkY)
{
    GridChunk *existing = (GridChunk *)FindChunk(grid, chunkX, chunkY);
    if (existing != NULL)
        return existing;
    /* Заполнение не выше половины держит пробы короткими. */
    if ((grid->count + 1u) * 2u > grid->capacity)
    {
        if (grid->capacity > UINT32_MAX / 4u ||
            !Rehash(grid, grid->capacity == 0u ? 64u : grid->capacity * 2u))
            return NULL;
    }
    uint32_t index = (uint32_t)Mix(chunkX, chunkY) & (grid->capacity - 1u);
    while (grid->slots[index].used != 0u)
        index = (index + 1u) & (grid->capacity - 1u);
    GridChunk *chunk = &grid->slots[index];
    chunk->chunkX = chunkX;
    chunk->chunkY = chunkY;
    chunk->cells = NULL;
    chunk->uniform = grid->defaultCost;
    chunk->used = 1u;
    ++grid->count;
    return chunk;
}

static bool Materialize(GridChunk *chunk)
{
    if (chunk->cells != NULL)
        return true;
    chunk->cells = PlatformAllocate(CHUNK_CELLS, false);
    if (chunk->cells == NULL)
        return false;
    memset(chunk->cells, chunk->uniform, CHUNK_CELLS);
    return true;
}

LaiueNavStatus LaiueNavGridCreate(const LaiueNavGridDesc *desc, LaiueNavGrid **outGrid)
{
    if (outGrid != NULL)
        *outGrid = NULL;
    if (desc == NULL || outGrid == NULL || desc->structSize < sizeof(*desc))
        return LAIUE_NAV_INVALID_ARGUMENT;
    LaiueNavGrid *grid = PlatformAllocate(sizeof(*grid), true);
    if (grid == NULL)
        return LAIUE_NAV_OUT_OF_MEMORY;
    grid->defaultCost = desc->defaultCost;
    *outGrid = grid;
    return LAIUE_NAV_OK;
}

void LaiueNavGridDestroy(LaiueNavGrid *grid)
{
    if (grid == NULL)
        return;
    for (uint32_t index = 0u; index < grid->capacity; ++index)
        PlatformFree(grid->slots[index].cells);
    PlatformFree(grid->slots);
    PlatformFree(grid);
}

uint8_t LaiueNavGridGetCell(const LaiueNavGrid *grid, int64_t x, int64_t y)
{
    if (grid == NULL)
        return LAIUE_NAV_CELL_BLOCKED;
    const GridChunk *chunk = FindChunk(grid, FloorChunk(x), FloorChunk(y));
    if (chunk == NULL)
        return grid->defaultCost;
    return chunk->cells == NULL ? chunk->uniform : chunk->cells[LocalIndex(x, y)];
}

LaiueNavStatus LaiueNavGridSetCell(LaiueNavGrid *grid, int64_t x, int64_t y, uint8_t cost)
{
    if (grid == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    const int64_t chunkX = FloorChunk(x);
    const int64_t chunkY = FloorChunk(y);
    const GridChunk *found = FindChunk(grid, chunkX, chunkY);
    const uint8_t current =
        found == NULL ? grid->defaultCost
                      : (found->cells == NULL ? found->uniform : found->cells[LocalIndex(x, y)]);
    if (current == cost)
        return LAIUE_NAV_OK;
    GridChunk *chunk = ObtainChunk(grid, chunkX, chunkY);
    if (chunk == NULL || !Materialize(chunk))
        return LAIUE_NAV_OUT_OF_MEMORY;
    chunk->cells[LocalIndex(x, y)] = cost;
    ++grid->revision;
    return LAIUE_NAV_OK;
}

LaiueNavStatus LaiueNavGridFillRect(LaiueNavGrid *grid, const LaiueNavRect *rect, uint8_t cost)
{
    if (grid == NULL || rect == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    if (rect->width == 0u || rect->height == 0u)
        return LAIUE_NAV_OK;
    if (rect->x > INT64_MAX - (int64_t)rect->width || rect->y > INT64_MAX - (int64_t)rect->height)
        return LAIUE_NAV_INVALID_ARGUMENT;
    const int64_t endX = rect->x + (int64_t)rect->width;
    const int64_t endY = rect->y + (int64_t)rect->height;
    for (int64_t chunkY = FloorChunk(rect->y); chunkY <= FloorChunk(endY - 1); ++chunkY)
        for (int64_t chunkX = FloorChunk(rect->x); chunkX <= FloorChunk(endX - 1); ++chunkX)
        {
            const int64_t originX = chunkX * (int64_t)CHUNK_SIDE;
            const int64_t originY = chunkY * (int64_t)CHUNK_SIDE;
            const int64_t fromX = rect->x > originX ? rect->x : originX;
            const int64_t fromY = rect->y > originY ? rect->y : originY;
            const int64_t toX =
                endX < originX + (int64_t)CHUNK_SIDE ? endX : originX + (int64_t)CHUNK_SIDE;
            const int64_t toY =
                endY < originY + (int64_t)CHUNK_SIDE ? endY : originY + (int64_t)CHUNK_SIDE;
            const bool whole = fromX == originX && fromY == originY &&
                               toX == originX + (int64_t)CHUNK_SIDE &&
                               toY == originY + (int64_t)CHUNK_SIDE;
            if (whole && cost == grid->defaultCost && FindChunk(grid, chunkX, chunkY) == NULL)
                continue;
            GridChunk *chunk = ObtainChunk(grid, chunkX, chunkY);
            if (chunk == NULL)
                return LAIUE_NAV_OUT_OF_MEMORY;
            if (whole)
            {
                PlatformFree(chunk->cells);
                chunk->cells = NULL;
                chunk->uniform = cost;
                continue;
            }
            if (!Materialize(chunk))
                return LAIUE_NAV_OUT_OF_MEMORY;
            for (int64_t y = fromY; y < toY; ++y)
                memset(chunk->cells + LocalIndex(fromX, y), cost, (size_t)(toX - fromX));
        }
    ++grid->revision;
    return LAIUE_NAV_OK;
}

uint64_t LaiueNavGridRevision(const LaiueNavGrid *grid)
{
    return grid == NULL ? 0u : grid->revision;
}

/* ---- Чтение клеток в окне поиска ---------------------------------------------
 * Последний найденный чанк запоминается: соседние клетки почти всегда лежат
 * в нём же. */

typedef struct CellReader
{
    const LaiueNavGrid *grid;
    const GridChunk *chunk;
    int64_t chunkX;
    int64_t chunkY;
    bool cached;
} CellReader;

static uint8_t ReadCell(CellReader *reader, int64_t x, int64_t y)
{
    const int64_t chunkX = FloorChunk(x);
    const int64_t chunkY = FloorChunk(y);
    if (!reader->cached || chunkX != reader->chunkX || chunkY != reader->chunkY)
    {
        reader->chunk = FindChunk(reader->grid, chunkX, chunkY);
        reader->chunkX = chunkX;
        reader->chunkY = chunkY;
        reader->cached = true;
    }
    const GridChunk *chunk = reader->chunk;
    if (chunk == NULL)
        return reader->grid->defaultCost;
    return chunk->cells == NULL ? chunk->uniform : chunk->cells[LocalIndex(x, y)];
}

static bool Inside(const LaiueNavRect *rect, int64_t x, int64_t y)
{
    return x >= rect->x && y >= rect->y && (uint64_t)(x - rect->x) < rect->width &&
           (uint64_t)(y - rect->y) < rect->height;
}

static uint32_t AreaIndex(const LaiueNavRect *rect, int64_t x, int64_t y)
{
    return (uint32_t)(y - rect->y) * rect->width + (uint32_t)(x - rect->x);
}

/* Шаг из (x, y) в направлении direction разрешён: клетка проходима, а у
 * диагонали проходимы оба прямых соседа. Возвращает стоимость шага или 0. */
static uint64_t StepCost(CellReader *reader, const LaiueNavRect *bounds, int64_t x, int64_t y,
                         uint32_t direction, uint8_t enteringCost)
{
    const int64_t nextX = x + directionX[direction];
    const int64_t nextY = y + directionY[direction];
    if (!Inside(bounds, nextX, nextY) || enteringCost == LAIUE_NAV_CELL_BLOCKED)
        return 0u;
    if ((direction & 1u) != 0u)
    {
        if (ReadCell(reader, nextX, y) == LAIUE_NAV_CELL_BLOCKED ||
            ReadCell(reader, x, nextY) == LAIUE_NAV_CELL_BLOCKED)
            return 0u;
        return (uint64_t)LAIUE_NAV_GRID_DIAGONAL_STEP * enteringCost;
    }
    return (uint64_t)LAIUE_NAV_GRID_STRAIGHT_STEP * enteringCost;
}

static uint64_t Octile(int64_t fromX, int64_t fromY, int64_t toX, int64_t toY)
{
    const uint64_t dx = fromX >= toX ? (uint64_t)(fromX - toX) : (uint64_t)(toX - fromX);
    const uint64_t dy = fromY >= toY ? (uint64_t)(fromY - toY) : (uint64_t)(toY - fromY);
    const uint64_t larger = dx > dy ? dx : dy;
    const uint64_t smaller = dx > dy ? dy : dx;
    return LAIUE_NAV_GRID_STRAIGHT_STEP * larger +
           (LAIUE_NAV_GRID_DIAGONAL_STEP - LAIUE_NAV_GRID_STRAIGHT_STEP) * smaller;
}

static void SetGridResult(LaiueNavPathResult *result, LaiueNavPathStatus pathStatus,
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

static bool ResolveBounds(const LaiueNavGridPathRequest *request, LaiueNavRect *bounds)
{
    if (request->bounds.width != 0u && request->bounds.height != 0u)
    {
        *bounds = request->bounds;
    }
    else
    {
        const int64_t margin = (int64_t)request->margin;
        const int64_t minimumX =
            request->start.x < request->goal.x ? request->start.x : request->goal.x;
        const int64_t minimumY =
            request->start.y < request->goal.y ? request->start.y : request->goal.y;
        const int64_t maximumX =
            request->start.x > request->goal.x ? request->start.x : request->goal.x;
        const int64_t maximumY =
            request->start.y > request->goal.y ? request->start.y : request->goal.y;
        if (minimumX < INT64_MIN + margin || minimumY < INT64_MIN + margin ||
            maximumX > INT64_MAX - margin - 1 || maximumY > INT64_MAX - margin - 1)
            return false;
        const uint64_t width = (uint64_t)(maximumX - minimumX) + 1u + 2u * (uint64_t)margin;
        const uint64_t height = (uint64_t)(maximumY - minimumY) + 1u + 2u * (uint64_t)margin;
        if (width > UINT32_MAX || height > UINT32_MAX)
            return false;
        bounds->x = minimumX - margin;
        bounds->y = minimumY - margin;
        bounds->width = (uint32_t)width;
        bounds->height = (uint32_t)height;
    }
    return (uint64_t)bounds->width * bounds->height <= LAIUE_NAV_GRID_MAXIMUM_AREA &&
           bounds->x <= INT64_MAX - (int64_t)bounds->width &&
           bounds->y <= INT64_MAX - (int64_t)bounds->height;
}

void LaiueNavGridFindPath(const LaiueNavGrid *grid, LaiueNavQuery *query,
                          const LaiueNavGridPathRequest *request, LaiueNavCell *cells,
                          uint32_t capacity, LaiueNavPathResult *outResult)
{
    LaiueNavRect bounds;
    if (grid == NULL || query == NULL || request == NULL ||
        request->structSize < sizeof(*request) || (cells == NULL && capacity != 0u) ||
        !ResolveBounds(request, &bounds))
    {
        SetGridResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_INVALID_ARGUMENT, 0u, 0u, 0u);
        return;
    }
    query->kind = NAV_SEARCH_GRID;
    query->graph = NULL;
    query->state = LAIUE_NAV_PATH_NO_PATH;
    CellReader reader = {grid, NULL, 0, 0, false};
    if (!Inside(&bounds, request->start.x, request->start.y) ||
        !Inside(&bounds, request->goal.x, request->goal.y) ||
        ReadCell(&reader, request->start.x, request->start.y) == LAIUE_NAV_CELL_BLOCKED ||
        ReadCell(&reader, request->goal.x, request->goal.y) == LAIUE_NAV_CELL_BLOCKED)
    {
        SetGridResult(outResult, LAIUE_NAV_PATH_NO_PATH, LAIUE_NAV_OK, 0u, 0u, 0u);
        return;
    }
    if (!NavQueryReset(query, bounds.width * bounds.height))
    {
        SetGridResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_OUT_OF_MEMORY, 0u, 0u, 0u);
        return;
    }
    const uint32_t start = AreaIndex(&bounds, request->start.x, request->start.y);
    const uint32_t goal = AreaIndex(&bounds, request->goal.x, request->goal.y);
    NavQuerySet(query, start, 0u, NAV_NONE);
    LaiueNavPathStatus state = LAIUE_NAV_PATH_NO_PATH;
    if (!NavHeapPush(query,
                     Octile(request->start.x, request->start.y, request->goal.x, request->goal.y),
                     0u, start))
    {
        SetGridResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_OUT_OF_MEMORY, 0u, 0u, 0u);
        return;
    }
    while (query->heapCount != 0u)
    {
        if (request->maxExpansions != 0u && query->expansions >= request->maxExpansions)
        {
            state = LAIUE_NAV_PATH_IN_PROGRESS;
            break;
        }
        const NavHeapEntry top = NavHeapPop(query);
        if (NavQueryClosed(query, top.node) || top.g != query->nodes[top.node].g)
            continue;
        if (request->maxCost != 0u && top.f > request->maxCost)
            break;
        query->nodes[top.node].closed = query->currentStamp;
        ++query->expansions;
        if (top.node == goal)
        {
            state = LAIUE_NAV_PATH_FOUND;
            query->foundCost = top.g;
            break;
        }
        const int64_t x = bounds.x + (int64_t)(top.node % bounds.width);
        const int64_t y = bounds.y + (int64_t)(top.node / bounds.width);
        for (uint32_t direction = 0u; direction < 8u; ++direction)
        {
            const int64_t nextX = x + directionX[direction];
            const int64_t nextY = y + directionY[direction];
            if (!Inside(&bounds, nextX, nextY))
                continue;
            const uint32_t next = AreaIndex(&bounds, nextX, nextY);
            if (NavQueryClosed(query, next))
                continue;
            const uint64_t step =
                StepCost(&reader, &bounds, x, y, direction, ReadCell(&reader, nextX, nextY));
            if (step == 0u)
                continue;
            const uint64_t g = top.g + step;
            if (NavQueryVisited(query, next) && query->nodes[next].g <= g)
                continue;
            NavQuerySet(query, next, g, top.node);
            if (!NavHeapPush(query, g + Octile(nextX, nextY, request->goal.x, request->goal.y), g,
                             next))
            {
                SetGridResult(outResult, LAIUE_NAV_PATH_ERROR, LAIUE_NAV_OUT_OF_MEMORY, 0u,
                              query->expansions, 0u);
                return;
            }
        }
    }
    query->state = state;
    if (state != LAIUE_NAV_PATH_FOUND)
    {
        SetGridResult(outResult, state, LAIUE_NAV_OK, 0u, query->expansions, 0u);
        return;
    }
    uint32_t count = 1u;
    for (uint32_t node = goal; node != start; node = query->nodes[node].parent)
        ++count;
    if (count > capacity)
    {
        SetGridResult(outResult, LAIUE_NAV_PATH_BUFFER_TOO_SMALL, LAIUE_NAV_OK, count,
                      query->expansions, query->foundCost);
        return;
    }
    uint32_t position = count;
    for (uint32_t node = goal;; node = query->nodes[node].parent)
    {
        cells[--position].x = bounds.x + (int64_t)(node % bounds.width);
        cells[position].y = bounds.y + (int64_t)(node / bounds.width);
        if (node == start)
            break;
    }
    SetGridResult(outResult, LAIUE_NAV_PATH_FOUND, LAIUE_NAV_OK, count, query->expansions,
                  query->foundCost);
}

/* Прямая из from в to проходит только по клеткам со стоимостью от 1 до
 * limit. Проход точно через угол проверяет обе клетки у угла. */
static bool LineOfSight(CellReader *reader, LaiueNavCell from, LaiueNavCell to, uint8_t limit)
{
    const int64_t stepX = to.x >= from.x ? 1 : -1;
    const int64_t stepY = to.y >= from.y ? 1 : -1;
    const uint64_t spanX = to.x >= from.x ? (uint64_t)(to.x - from.x) : (uint64_t)(from.x - to.x);
    const uint64_t spanY = to.y >= from.y ? (uint64_t)(to.y - from.y) : (uint64_t)(from.y - to.y);
    if (spanX > (UINT64_C(1) << 30) || spanY > (UINT64_C(1) << 30))
        return false;
    int64_t x = from.x;
    int64_t y = from.y;
    uint64_t passedX = 0u;
    uint64_t passedY = 0u;
    while (passedX < spanX || passedY < spanY)
    {
        /* Сравниваются (passedX + 0.5) / spanX и (passedY + 0.5) / spanY. */
        const int64_t decision =
            (int64_t)((1u + 2u * passedX) * spanY) - (int64_t)((1u + 2u * passedY) * spanX);
        if (decision == 0)
        {
            const uint8_t sideA = ReadCell(reader, x + stepX, y);
            const uint8_t sideB = ReadCell(reader, x, y + stepY);
            if (sideA == LAIUE_NAV_CELL_BLOCKED || sideA > limit ||
                sideB == LAIUE_NAV_CELL_BLOCKED || sideB > limit)
                return false;
            x += stepX;
            y += stepY;
            ++passedX;
            ++passedY;
        }
        else if (decision < 0)
        {
            x += stepX;
            ++passedX;
        }
        else
        {
            y += stepY;
            ++passedY;
        }
        const uint8_t cost = ReadCell(reader, x, y);
        if (cost == LAIUE_NAV_CELL_BLOCKED || cost > limit)
            return false;
    }
    return true;
}

uint32_t LaiueNavGridSmoothPath(const LaiueNavGrid *grid, LaiueNavCell *cells, uint32_t count)
{
    if (grid == NULL || cells == NULL || count <= 2u)
        return cells == NULL ? 0u : count;
    CellReader reader = {grid, NULL, 0, 0, false};
    uint32_t output = 1u;
    uint32_t anchor = 0u;
    while (anchor < count - 1u)
    {
        uint32_t reach = anchor + 1u;
        uint8_t limit = ReadCell(&reader, cells[reach].x, cells[reach].y);
        for (uint32_t candidate = anchor + 2u; candidate < count; ++candidate)
        {
            const uint8_t cost =
                ReadCell(&reader, cells[candidate - 1u].x, cells[candidate - 1u].y);
            const uint8_t candidateLimit = cost > limit ? cost : limit;
            const uint8_t end = ReadCell(&reader, cells[candidate].x, cells[candidate].y);
            const uint8_t combined = end > candidateLimit ? end : candidateLimit;
            if (!LineOfSight(&reader, cells[anchor], cells[candidate], combined))
                break;
            reach = candidate;
            limit = combined;
        }
        cells[output++] = cells[reach];
        anchor = reach;
    }
    return output;
}

/* ---- Поле потока ------------------------------------------------------------- */

LaiueNavStatus LaiueNavFlowFieldCreate(LaiueNavFlowField **outField)
{
    if (outField == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    *outField = PlatformAllocate(sizeof(LaiueNavFlowField), true);
    return *outField == NULL ? LAIUE_NAV_OUT_OF_MEMORY : LAIUE_NAV_OK;
}

void LaiueNavFlowFieldDestroy(LaiueNavFlowField *field)
{
    if (field == NULL)
        return;
    PlatformFree(field->query.nodes);
    PlatformFree(field->query.heap);
    PlatformFree(field->directions);
    PlatformFree(field);
}

LaiueNavStatus LaiueNavFlowFieldBuild(LaiueNavFlowField *field, const LaiueNavGrid *grid,
                                      const LaiueNavRect *bounds, const LaiueNavCell *goals,
                                      uint32_t goalCount)
{
    if (field == NULL || grid == NULL || bounds == NULL || (goals == NULL && goalCount != 0u) ||
        bounds->width == 0u || bounds->height == 0u ||
        (uint64_t)bounds->width * bounds->height > LAIUE_NAV_GRID_MAXIMUM_AREA ||
        bounds->x > INT64_MAX - (int64_t)bounds->width ||
        bounds->y > INT64_MAX - (int64_t)bounds->height)
        return LAIUE_NAV_INVALID_ARGUMENT;
    field->built = false;
    const uint32_t area = bounds->width * bounds->height;
    LaiueNavQuery *query = &field->query;
    if (!NavQueryReset(query, area))
        return LAIUE_NAV_OUT_OF_MEMORY;
    if (area > field->directionCapacity)
    {
        uint8_t *directions = PlatformAllocate(area, false);
        if (directions == NULL)
            return LAIUE_NAV_OUT_OF_MEMORY;
        PlatformFree(field->directions);
        field->directions = directions;
        field->directionCapacity = area;
    }
    memset(field->directions, LAIUE_NAV_FLOW_NONE, area);
    field->bounds = *bounds;
    CellReader reader = {grid, NULL, 0, 0, false};
    for (uint32_t index = 0u; index < goalCount; ++index)
    {
        const LaiueNavCell goal = goals[index];
        if (!Inside(bounds, goal.x, goal.y) ||
            ReadCell(&reader, goal.x, goal.y) == LAIUE_NAV_CELL_BLOCKED)
            continue;
        const uint32_t node = AreaIndex(bounds, goal.x, goal.y);
        if (NavQueryVisited(query, node))
            continue;
        NavQuerySet(query, node, 0u, NAV_NONE);
        if (!NavHeapPush(query, 0u, 0u, node))
            return LAIUE_NAV_OUT_OF_MEMORY;
    }
    /* Дейкстра от целей назад: шаг v → u стоит как вход агента в u,
     * поэтому стоимость клетки совпадает с прямым поиском из неё. */
    while (query->heapCount != 0u)
    {
        const NavHeapEntry top = NavHeapPop(query);
        if (NavQueryClosed(query, top.node) || top.g != query->nodes[top.node].g)
            continue;
        query->nodes[top.node].closed = query->currentStamp;
        const int64_t x = bounds->x + (int64_t)(top.node % bounds->width);
        const int64_t y = bounds->y + (int64_t)(top.node / bounds->width);
        const uint8_t enteringCost = ReadCell(&reader, x, y);
        for (uint32_t direction = 0u; direction < 8u; ++direction)
        {
            const int64_t fromX = x + directionX[direction];
            const int64_t fromY = y + directionY[direction];
            if (!Inside(bounds, fromX, fromY) ||
                ReadCell(&reader, fromX, fromY) == LAIUE_NAV_CELL_BLOCKED)
                continue;
            const uint32_t from = AreaIndex(bounds, fromX, fromY);
            if (NavQueryClosed(query, from))
                continue;
            /* Шаг из соседа обратно в текущую клетку — противоположное направление. */
            const uint64_t step =
                StepCost(&reader, bounds, fromX, fromY, (direction + 4u) & 7u, enteringCost);
            if (step == 0u)
                continue;
            const uint64_t g = top.g + step;
            if (NavQueryVisited(query, from) && query->nodes[from].g <= g)
                continue;
            NavQuerySet(query, from, g, top.node);
            field->directions[from] = (uint8_t)((direction + 4u) & 7u);
            if (!NavHeapPush(query, g, g, from))
                return LAIUE_NAV_OUT_OF_MEMORY;
        }
    }
    field->built = true;
    return LAIUE_NAV_OK;
}

uint8_t LaiueNavFlowFieldDirection(const LaiueNavFlowField *field, int64_t x, int64_t y)
{
    if (field == NULL || !field->built || !Inside(&field->bounds, x, y))
        return LAIUE_NAV_FLOW_NONE;
    return field->directions[AreaIndex(&field->bounds, x, y)];
}

uint32_t LaiueNavFlowFieldCost(const LaiueNavFlowField *field, int64_t x, int64_t y)
{
    if (field == NULL || !field->built || !Inside(&field->bounds, x, y))
        return LAIUE_NAV_FLOW_UNREACHABLE;
    const uint32_t index = AreaIndex(&field->bounds, x, y);
    if (!NavQueryVisited(&field->query, index))
        return LAIUE_NAV_FLOW_UNREACHABLE;
    const uint64_t cost = field->query.nodes[index].g;
    return cost >= LAIUE_NAV_FLOW_UNREACHABLE ? LAIUE_NAV_FLOW_UNREACHABLE - 1u : (uint32_t)cost;
}
