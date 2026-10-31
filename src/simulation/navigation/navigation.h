#pragma once

/* laiue.navigation — поиск путей без игровых правил.
 *
 * Две модели передвижения, которые приложение сочетает как хочет:
 *
 * 1. Граф сетей (LaiueNavGraph): дороги, полосы, тротуары, рельсы, тропы.
 *    Рёбра направленные, с целой стоимостью и маской видов транспорта.
 *    Стоимость меняется дёшево (пробки), структура — с повторной подготовкой.
 * 2. Сетка свободного движения (LaiueNavGrid): бесконечная плоскость клеток
 *    с целой стоимостью прохода; 0 — непроходимо. Поверх неё — A* между
 *    двумя клетками и поле потока (LaiueNavFlowField) к общей цели для
 *    множества агентов сразу.
 *
 * Вся арифметика целочисленная, порядок обхода и выбор среди равных
 * зафиксированы, поэтому одинаковый вход даёт одинаковый путь на любой
 * платформе и при любом числе потоков.
 *
 * Потоки: граф и сетка меняются одним владельцем. Подготовленный граф и
 * неизменяемая сетка читаются параллельно; у каждого потока свой
 * LaiueNavQuery. Бюджеты (maxExpansions) ограничивают время одного поиска,
 * а пошаговый поиск растягивает длинный маршрут на несколько кадров. */

#include "api.h"
#include "task/task_pool.h"

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t LaiueNavStatus;
#define LAIUE_NAV_OK UINT32_C(0)
#define LAIUE_NAV_INVALID_ARGUMENT UINT32_C(1)
#define LAIUE_NAV_OUT_OF_MEMORY UINT32_C(2)
#define LAIUE_NAV_NOT_FOUND UINT32_C(3)
/* Граф изменён структурно и не подготовлен LaiueNavGraphPrepare. */
#define LAIUE_NAV_NOT_PREPARED UINT32_C(4)
/* Структура графа или сетка изменились во время пошагового поиска. */
#define LAIUE_NAV_STALE UINT32_C(5)
#define LAIUE_NAV_LIMIT_REACHED UINT32_C(6)

/* Итог поиска пути. */
typedef uint32_t LaiueNavPathStatus;
#define LAIUE_NAV_PATH_FOUND UINT32_C(0)
/* Цель недостижима (или дороже maxCost, или вне границ поиска). */
#define LAIUE_NAV_PATH_NO_PATH UINT32_C(1)
/* Исчерпан maxExpansions; пошаговый поиск можно продолжить. */
#define LAIUE_NAV_PATH_IN_PROGRESS UINT32_C(2)
/* Путь найден, но не поместился: count — нужная ёмкость. */
#define LAIUE_NAV_PATH_BUFFER_TOO_SMALL UINT32_C(3)
/* Ошибка аргумента или состояния; подробность в status результата. */
#define LAIUE_NAV_PATH_ERROR UINT32_C(4)

/* ---- Граф сетей ------------------------------------------------------------ */

typedef struct LaiueNavGraph LaiueNavGraph;
typedef struct LaiueNavQuery LaiueNavQuery;

/* Описатели узлов и рёбер: индекс и поколение. Описатель удалённого
 * объекта не совпадает ни с каким живым, 0 — пустой описатель. */
typedef uint64_t LaiueNavNode;
typedef uint64_t LaiueNavEdge;

typedef struct LaiueNavPosition
{
    int64_t x;
    int64_t y;
    int64_t z;
} LaiueNavPosition;

typedef struct LaiueNavGraphDesc
{
    uint32_t structSize;
    uint32_t nodeCapacityHint;
    uint32_t edgeCapacityHint;
} LaiueNavGraphDesc;

typedef struct LaiueNavEdgeDesc
{
    uint32_t structSize;
    /* Стоимость прохода, не меньше 1: время, расстояние — что угодно. */
    uint32_t cost;
    /* Виды передвижения, которым ребро доступно. Ноль — никому. */
    uint32_t modes;
    uint32_t userData;
} LaiueNavEdgeDesc;

typedef struct LaiueNavEdgeInfo
{
    uint32_t structSize;
    LaiueNavNode from;
    LaiueNavNode to;
    uint32_t cost;
    uint32_t modes;
    uint32_t userData;
} LaiueNavEdgeInfo;

typedef struct LaiueNavPathRequest
{
    uint32_t structSize;
    LaiueNavNode start;
    LaiueNavNode goal;
    /* Ребро проходимо, если (edge.modes & modes) != 0. */
    uint32_t modes;
    /* Эвристика A*: евклидово расстояние между позициями узлов, умноженное
     * на heuristicNumerator / heuristicDenominator (каждое до 65535).
     * Должна не превышать настоящую стоимость, иначе путь может оказаться
     * не кратчайшим. Числитель 0 превращает поиск в алгоритм Дейкстры. */
    uint32_t heuristicNumerator;
    uint32_t heuristicDenominator;
    /* Сколько узлов раскрыть за вызов; 0 — без ограничения. */
    uint32_t maxExpansions;
    /* Пути дороже не ищутся; 0 — без ограничения. */
    uint64_t maxCost;
} LaiueNavPathRequest;

typedef struct LaiueNavPathResult
{
    uint32_t structSize;
    LaiueNavPathStatus pathStatus;
    LaiueNavStatus status;
    /* Число рёбер (графа) или клеток (сетки) пути. */
    uint32_t count;
    uint32_t expansions;
    uint64_t cost;
} LaiueNavPathResult;

typedef struct LaiueNavBatchItem
{
    LaiueNavPathRequest request;
    LaiueNavEdge *edges;
    uint32_t capacity;
    LaiueNavPathResult result;
} LaiueNavBatchItem;

LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphCreate(const LaiueNavGraphDesc *desc,
                                                        LaiueNavGraph **outGraph);
LAIUE_NAVIGATION_API void LaiueNavGraphDestroy(LaiueNavGraph *graph);

LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphAddNode(LaiueNavGraph *graph,
                                                         const LaiueNavPosition *position,
                                                         uint32_t userData, LaiueNavNode *outNode);
/* Удаляет узел вместе со всеми входящими и исходящими рёбрами. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphRemoveNode(LaiueNavGraph *graph,
                                                            LaiueNavNode node);
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphGetNode(const LaiueNavGraph *graph,
                                                         LaiueNavNode node,
                                                         LaiueNavPosition *outPosition,
                                                         uint32_t *outUserData);
/* Позиция влияет только на эвристику, поэтому не требует подготовки. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphSetNodePosition(LaiueNavGraph *graph,
                                                                 LaiueNavNode node,
                                                                 const LaiueNavPosition *position);

LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphAddEdge(LaiueNavGraph *graph, LaiueNavNode from,
                                                         LaiueNavNode to,
                                                         const LaiueNavEdgeDesc *desc,
                                                         LaiueNavEdge *outEdge);
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphRemoveEdge(LaiueNavGraph *graph,
                                                            LaiueNavEdge edge);
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphGetEdge(const LaiueNavGraph *graph,
                                                         LaiueNavEdge edge,
                                                         LaiueNavEdgeInfo *outInfo);
/* Дешёвые изменения без подготовки: подходят для пробок и перекрытий.
 * Нельзя вызывать, пока граф параллельно читают поиски. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphSetEdgeCost(LaiueNavGraph *graph,
                                                             LaiueNavEdge edge, uint32_t cost);
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphSetEdgeModes(LaiueNavGraph *graph,
                                                              LaiueNavEdge edge, uint32_t modes);

/* Собирает компактное представление для поиска после структурных изменений.
 * Без изменений ничего не делает. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGraphPrepare(LaiueNavGraph *graph);
LAIUE_NAVIGATION_API bool LaiueNavGraphIsPrepared(const LaiueNavGraph *graph);
LAIUE_NAVIGATION_API uint32_t LaiueNavGraphNodeCount(const LaiueNavGraph *graph);
LAIUE_NAVIGATION_API uint32_t LaiueNavGraphEdgeCount(const LaiueNavGraph *graph);
/* Растёт при любом изменении; приложение по нему узнаёт устаревшие пути. */
LAIUE_NAVIGATION_API uint64_t LaiueNavGraphRevision(const LaiueNavGraph *graph);

/* Рабочая память поиска; одна на поток, переиспользуется между поисками. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavQueryCreate(LaiueNavQuery **outQuery);
LAIUE_NAVIGATION_API void LaiueNavQueryDestroy(LaiueNavQuery *query);

/* Полный поиск за один вызов. edges получает путь от start к goal. */
LAIUE_NAVIGATION_API void LaiueNavFindPath(const LaiueNavGraph *graph, LaiueNavQuery *query,
                                           const LaiueNavPathRequest *request, LaiueNavEdge *edges,
                                           uint32_t capacity, LaiueNavPathResult *outResult);

/* Пошаговый поиск: Begin запоминает запрос, каждый Continue раскрывает не
 * больше maxExpansions узлов (0 — до конца). Пока результат IN_PROGRESS,
 * граф можно менять только по стоимости рёбер; структурное изменение даёт
 * STALE. Путь забирается LaiueNavQueryGetPath после FOUND. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavQueryBegin(LaiueNavQuery *query,
                                                       const LaiueNavGraph *graph,
                                                       const LaiueNavPathRequest *request);
LAIUE_NAVIGATION_API void LaiueNavQueryContinue(LaiueNavQuery *query, uint32_t maxExpansions,
                                                LaiueNavPathResult *outResult);
LAIUE_NAVIGATION_API void LaiueNavQueryGetPath(LaiueNavQuery *query, LaiueNavEdge *edges,
                                               uint32_t capacity, LaiueNavPathResult *outResult);

/* Пакет независимых поисков. Запросы делятся между queries непрерывными
 * отрезками; исполнитель (необязательный) запускает отрезки параллельно.
 * Результат каждого элемента не зависит от числа потоков. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavFindPathsBatch(
    const LaiueNavGraph *graph, const LaiueTaskExecutor *executor, LaiueNavQuery *const *queries,
    uint32_t queryCount, LaiueNavBatchItem *items, uint32_t itemCount);

/* ---- Сетка свободного движения -------------------------------------------- */

typedef struct LaiueNavGrid LaiueNavGrid;
typedef struct LaiueNavFlowField LaiueNavFlowField;

typedef struct LaiueNavCell
{
    int64_t x;
    int64_t y;
} LaiueNavCell;

/* Прямоугольник клеток [x, x + width) × [y, y + height). */
typedef struct LaiueNavRect
{
    int64_t x;
    int64_t y;
    uint32_t width;
    uint32_t height;
} LaiueNavRect;

#define LAIUE_NAV_CELL_BLOCKED 0u
/* Стоимость шага: прямой 1000, диагональный 1414, умноженные на стоимость
 * клетки, в которую шагают. Диагональ запрещена, если закрыт любой из двух
 * прямых соседей: агент не срезает угол стены. */
#define LAIUE_NAV_GRID_STRAIGHT_STEP 1000u
#define LAIUE_NAV_GRID_DIAGONAL_STEP 1414u
/* Самая большая область одного поиска или поля потока. */
#define LAIUE_NAV_GRID_MAXIMUM_AREA (UINT32_C(1) << 24)

typedef struct LaiueNavGridDesc
{
    uint32_t structSize;
    /* Стоимость клеток, которые не задавались: 0 — стена, 1–255 — проход. */
    uint8_t defaultCost;
} LaiueNavGridDesc;

typedef struct LaiueNavGridPathRequest
{
    uint32_t structSize;
    LaiueNavCell start;
    LaiueNavCell goal;
    /* Поиск не выходит за прямоугольник. Нулевой — рамка вокруг start и
     * goal с полями margin клеток. */
    LaiueNavRect bounds;
    uint32_t margin;
    uint32_t maxExpansions;
    uint64_t maxCost;
} LaiueNavGridPathRequest;

LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGridCreate(const LaiueNavGridDesc *desc,
                                                       LaiueNavGrid **outGrid);
LAIUE_NAVIGATION_API void LaiueNavGridDestroy(LaiueNavGrid *grid);
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGridSetCell(LaiueNavGrid *grid, int64_t x, int64_t y,
                                                        uint8_t cost);
LAIUE_NAVIGATION_API uint8_t LaiueNavGridGetCell(const LaiueNavGrid *grid, int64_t x, int64_t y);
/* При LAIUE_NAV_OUT_OF_MEMORY сетка и её ревизия остаются прежними. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavGridFillRect(LaiueNavGrid *grid,
                                                         const LaiueNavRect *rect, uint8_t cost);
LAIUE_NAVIGATION_API uint64_t LaiueNavGridRevision(const LaiueNavGrid *grid);

/* A* по сетке. cells получает клетки от start до goal включительно. */
LAIUE_NAVIGATION_API void LaiueNavGridFindPath(const LaiueNavGrid *grid, LaiueNavQuery *query,
                                               const LaiueNavGridPathRequest *request,
                                               LaiueNavCell *cells, uint32_t capacity,
                                               LaiueNavPathResult *outResult);
/* Убирает промежуточные клетки там, где прямая видимость проходит только
 * по проходимым клеткам не дороже пропускаемых. Первая и последняя клетка
 * остаются. Возвращает новое число клеток. */
LAIUE_NAVIGATION_API uint32_t LaiueNavGridSmoothPath(const LaiueNavGrid *grid, LaiueNavCell *cells,
                                                     uint32_t count);

/* Направления поля потока: 0 — +x, далее против часовой стрелки шагом 45°
 * (1 — +x+y, 2 — +y, … 7 — +x−y). Цель и недостижимые клетки дают NONE. */
#define LAIUE_NAV_FLOW_NONE UINT8_C(255)
#define LAIUE_NAV_FLOW_UNREACHABLE UINT32_MAX

LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavFlowFieldCreate(LaiueNavFlowField **outField);
LAIUE_NAVIGATION_API void LaiueNavFlowFieldDestroy(LaiueNavFlowField *field);
/* Стоимость пути до ближайшей цели для каждой клетки bounds и направление
 * первого шага. Цели вне bounds и в стенах игнорируются. */
LAIUE_NAVIGATION_API LaiueNavStatus LaiueNavFlowFieldBuild(LaiueNavFlowField *field,
                                                           const LaiueNavGrid *grid,
                                                           const LaiueNavRect *bounds,
                                                           const LaiueNavCell *goals,
                                                           uint32_t goalCount);
LAIUE_NAVIGATION_API uint8_t LaiueNavFlowFieldDirection(const LaiueNavFlowField *field, int64_t x,
                                                        int64_t y);
LAIUE_NAVIGATION_API uint32_t LaiueNavFlowFieldCost(const LaiueNavFlowField *field, int64_t x,
                                                    int64_t y);
