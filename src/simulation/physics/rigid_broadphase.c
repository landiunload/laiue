#include "physics/rigid_broadphase.h"

#include "physics/rigid_body.h"

#include <float.h>
#include <stddef.h>

// Тест перекрытия AABB — самая горячая операция обхода запроса, и узлы дерева
// умещаются в L2 на целевых сценах, поэтому решает не задержка памяти, а число
// инструкций. Шесть скалярных сравнений double сворачиваются побитовым OR, и
// MSVC их не векторизует. Раскладка узла — minimum[3], затем maximum[3] —
// позволяет проверить по три оси двумя 256-битными сравнениями. Семантика
// IEEE (GT/LT ordered, NaN -> false) и итог совпадают со скалярной побитово,
// поэтому набор кандидатов, порядок обхода и эталонные хеши не меняются.
#if defined(__AVX2__)
#include <immintrin.h>
#define LAIUE_BROADPHASE_OVERLAP_AVX2 1
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) ||           \
    defined(__i386__)
#include <emmintrin.h>
#define LAIUE_BROADPHASE_OVERLAP_SSE2 1
#endif

// The pooled-index/fat-AABB design and the need for active rotations are
// described by Box2D's primary documentation:
// https://box2d.org/documentation/group__tree.html
// https://box2d.org/posts/2014/08/balancing-dynamic-trees/
// This implementation uses a saturating surface-area heuristic, strict AVL
// heights, and a bounded query stack with a parent-link fallback. Queries
// allocate no memory and do not recurse.
#define RIGID_TREE_EMPTY UINT32_MAX
#define RIGID_TREE_MARGIN 0.05
// Запас с упреждением: тело, которое движется равномерно, выходит за
// постоянный запас через пару шагов, и прокси приходится переставлять снова
// и снова — на падающей сцене это почти треть тел за шаг. Толстая коробка
// поэтому вытягивается в ту сторону, куда тело уже сместилось с прошлой
// вставки. Вытягивание ограничено сверху: слишком широкая коробка удорожает
// запросы сильнее, чем экономит на вставках.
//
// Направление берётся по смещению центра относительно прежней толстой
// коробки. Оценка грубая — прежний центр сам смещён прошлым вытягиванием, —
// но ошибаться она может только в сторону большего запаса, а любой больший
// запас остаётся корректным: толстая коробка обязана лишь содержать точную,
// а лишние кандидаты всё равно отсеиваются точной проверкой.
#define RIGID_TREE_PREDICT 2.0
#define RIGID_TREE_PREDICT_CAP (8.0 * RIGID_TREE_MARGIN)
// Порог усадки поднят настолько, чтобы выданное упреждение не считалось
// «коробка стала слишком велика» и не вызывало перевставку на следующем шаге.
#define RIGID_TREE_SHRINK (4.0 * RIGID_TREE_MARGIN + 3.0 * RIGID_TREE_PREDICT_CAP)
// Глубина стека обхода. Высота AVL-дерева предельной ёмкости не превышает
// 45 уровней для uint32-индексов; запас оставлен на случай, если инвариант
// ослабят. 256 байт на кадре стека — сборка без CRT ограничена четырьмя
// килобайтами.
#define RIGID_QUERY_STACK 64u

// Ровно одна кэш-линия на узел. Отдельного поля под номер тела нет: у листа
// нет детей, поэтому его `left` хранит номер, а `right` не используется.
// Лист узнаётся по нулевой высоте, а не по пустому `left` — свободный узел
// пула носит высоту -1 и не спутается ни с тем, ни с другим. Лишнее поле
// стоило бы восьми байт выравнивания на узел и второй кэш-линии на обход.
typedef struct RigidTreeNode
{
    double minimum[3];
    double maximum[3];
    uint32_t parent;
    uint32_t left;
    uint32_t right;
    int32_t height;
} RigidTreeNode;

static bool NodeIsLeaf(const RigidTreeNode *node)
{
    return node->height == 0;
}

static uint32_t NodeCapacity(uint32_t bodyCapacity)
{
    return bodyCapacity * 2u - 1u;
}

uint32_t VoxelRigidBroadphaseBytes(uint32_t bodyCapacity)
{
    if (bodyCapacity == 0u || bodyCapacity > VOXEL_RIGID_MAX_BODIES)
    {
        return 0u;
    }
    uint64_t total = ((uint64_t)bodyCapacity * 2u - 1u) * sizeof(RigidTreeNode) +
                     (uint64_t)bodyCapacity * sizeof(uint32_t) + 63u;
    return total > UINT32_MAX ? 0u : (uint32_t)total;
}

static bool ValidSpan(const void *pointer, size_t bytes)
{
    return pointer != NULL && bytes <= UINTPTR_MAX - (uintptr_t)pointer;
}

static bool SpansOverlap(const void *first, size_t firstBytes,
                          const void *second, size_t secondBytes)
{
    uintptr_t firstAddress = (uintptr_t)first;
    uintptr_t secondAddress = (uintptr_t)second;
    if (firstAddress <= secondAddress)
    {
        return secondAddress - firstAddress < firstBytes;
    }
    return firstAddress - secondAddress < secondBytes;
}

static bool AllocationValid(const VoxelRigidBroadphase *broadphase)
{
    if (broadphase == NULL)
    {
        return false;
    }
    uint32_t required = VoxelRigidBroadphaseBytes(broadphase->bodyCapacity);
    return required != 0u && broadphase->storageBytes >= required &&
           ValidSpan(broadphase->storage, broadphase->storageBytes) &&
           !SpansOverlap(broadphase, sizeof(*broadphase), broadphase->storage,
                           broadphase->storageBytes);
}

bool RigidBroadphaseValid(const VoxelRigidBroadphase *broadphase, uint32_t bodyCount)
{
    if (!AllocationValid(broadphase) || broadphase->bodyCapacity < bodyCount ||
        broadphase->proxyCount > broadphase->bodyCapacity ||
        broadphase->indexedBodyCount > broadphase->bodyCapacity)
    {
        return false;
    }
    uint32_t capacity = NodeCapacity(broadphase->bodyCapacity);
    return (broadphase->root == RIGID_TREE_EMPTY || broadphase->root < capacity) &&
           (broadphase->freeList == RIGID_TREE_EMPTY || broadphase->freeList < capacity) &&
           (broadphase->root == RIGID_TREE_EMPTY) == (broadphase->proxyCount == 0u);
}

static RigidTreeNode *TreeNodes(const VoxelRigidBroadphase *broadphase)
{
    uintptr_t padding = (0u - (uintptr_t)broadphase->storage) & 63u;
    return (RigidTreeNode *)((uint8_t *)broadphase->storage + padding);
}

static uint32_t *TreeSlots(const VoxelRigidBroadphase *broadphase)
{
    return (uint32_t *)(TreeNodes(broadphase) + NodeCapacity(broadphase->bodyCapacity));
}

void VoxelRigidBroadphaseReset(VoxelRigidBroadphase *broadphase)
{
    if (!AllocationValid(broadphase))
    {
        return;
    }
    broadphase->root = RIGID_TREE_EMPTY;
    broadphase->freeList = 0u;
    broadphase->proxyCount = 0u;
    broadphase->updatedProxyCount = 0u;
    broadphase->visitedNodeCount = 0u;
    broadphase->indexedBodyCount = 0u;
    RigidTreeNode *nodes = TreeNodes(broadphase);
    uint32_t capacity = NodeCapacity(broadphase->bodyCapacity);
    for (uint32_t index = 0u; index < capacity; ++index)
    {
        nodes[index].parent = index + 1u < capacity ? index + 1u : RIGID_TREE_EMPTY;
        nodes[index].left = RIGID_TREE_EMPTY;
        nodes[index].right = RIGID_TREE_EMPTY;
        nodes[index].height = -1;
    }
    uint32_t *slots = TreeSlots(broadphase);
    for (uint32_t index = 0u; index < broadphase->bodyCapacity; ++index)
    {
        slots[index] = RIGID_TREE_EMPTY;
    }
}

bool VoxelRigidBroadphaseInitialize(VoxelRigidBroadphase *broadphase, void *storage,
                                    uint32_t bodyCapacity, uint32_t storageBytes)
{
    uint32_t required = VoxelRigidBroadphaseBytes(bodyCapacity);
    if (broadphase == NULL || required == 0u || storageBytes < required ||
        !ValidSpan(storage, storageBytes) ||
        SpansOverlap(broadphase, sizeof(*broadphase), storage, storageBytes))
    {
        return false;
    }
    broadphase->storage = storage;
    broadphase->storageBytes = storageBytes;
    broadphase->bodyCapacity = bodyCapacity;
    VoxelRigidBroadphaseReset(broadphase);
    return true;
}

static bool BoundsValid(const double minimum[3], const double maximum[3])
{
    if (minimum == NULL || maximum == NULL)
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!(minimum[axis] >= -DBL_MAX && maximum[axis] <= DBL_MAX &&
              minimum[axis] <= maximum[axis]))
        {
            return false;
        }
    }
    return true;
}

static void CountEvent(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
    {
        ++*counter;
    }
}

// Без ранних выходов. Это самый горячий тест обхода, и предсказать его
// нельзя: примерно половина узлов отсеивается, причём вперемежку. Шесть
// сравнений без переходов дешевле одного неверно предсказанного.
static bool BoundsOverlap(const RigidTreeNode *node,
                           const double minimum[3], const double maximum[3])
{
#if defined(LAIUE_BROADPHASE_OVERLAP_AVX2)
    // Дорожки 0..2 — оси X,Y,Z. Дорожка 3 первой загрузки — maximum[0], второй —
    // поля узла; обе отбрасываются маской & 7, поэтому на результат не влияют.
    // Вторая загрузка читает байты 24..55 — в пределах той же 64-байтной
    // строки узла.
    __m256d nodeMinimum = _mm256_loadu_pd(&node->minimum[0]);
    __m256d nodeMaximum = _mm256_loadu_pd(&node->maximum[0]);
    __m256d queryMaximum = _mm256_setr_pd(maximum[0], maximum[1], maximum[2], 0.0);
    __m256d queryMinimum = _mm256_setr_pd(minimum[0], minimum[1], minimum[2], 0.0);
    __m256d separated = _mm256_or_pd(_mm256_cmp_pd(nodeMinimum, queryMaximum, _CMP_GT_OQ),
                                     _mm256_cmp_pd(nodeMaximum, queryMinimum, _CMP_LT_OQ));
    return (_mm256_movemask_pd(separated) & 7) == 0;
#elif defined(LAIUE_BROADPHASE_OVERLAP_SSE2)
    // Оси 0..1 сравниваются одной парой, ось 2 — парной загрузкой со сдвигом,
    // у которой значима только младшая дорожка.
    __m128d nodeMinimum01 = _mm_loadu_pd(&node->minimum[0]);
    __m128d nodeMaximum01 = _mm_loadu_pd(&node->maximum[0]);
    __m128d nodeMinimum2 = _mm_loadu_pd(&node->minimum[2]);
    __m128d nodeMaximum2 = _mm_loadu_pd(&node->maximum[2]);
    __m128d queryMaximum01 = _mm_loadu_pd(&maximum[0]);
    __m128d queryMinimum01 = _mm_loadu_pd(&minimum[0]);
    __m128d queryMaximum2 = _mm_set1_pd(maximum[2]);
    __m128d queryMinimum2 = _mm_set1_pd(minimum[2]);
    __m128d separated01 = _mm_or_pd(_mm_cmplt_pd(nodeMaximum01, queryMinimum01),
                                    _mm_cmpgt_pd(nodeMinimum01, queryMaximum01));
    __m128d separated2 = _mm_or_pd(_mm_cmplt_pd(nodeMaximum2, queryMinimum2),
                                   _mm_cmpgt_pd(nodeMinimum2, queryMaximum2));
    int separatedBits = _mm_movemask_pd(separated01) | (_mm_movemask_pd(separated2) & 1);
    return separatedBits == 0;
#else
    int separated = (node->minimum[0] > maximum[0]) | (node->maximum[0] < minimum[0]) |
                    (node->minimum[1] > maximum[1]) | (node->maximum[1] < minimum[1]) |
                    (node->minimum[2] > maximum[2]) | (node->maximum[2] < minimum[2]);
    return separated == 0;
#endif
}

// Метрика узла — половина площади поверхности, а не сумма рёбер. Сумма
// рёбер растёт с расстоянием линейно и потому слишком дёшево оценивает
// объединение двух далёких коробок: в разреженной сцене от этого раздуваются
// внутренние узлы, и запрос спускается в заведомо пустые ветви. Площадь
// растёт квадратично и такие объединения отвергает. Ею же оценивают деревья
// Box2D и Bullet. NaN и переполнение дают DBL_MAX: сравнение обязано
// оставаться определённым на предельных координатах.
static double BoundsMetric(const RigidTreeNode *first, const RigidTreeNode *second)
{
    double extent[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double minimum = first->minimum[axis];
        double maximum = first->maximum[axis];
        if (second != NULL)
        {
            if (second->minimum[axis] < minimum) minimum = second->minimum[axis];
            if (second->maximum[axis] > maximum) maximum = second->maximum[axis];
        }
        extent[axis] = maximum - minimum;
    }
    double total = extent[0] * extent[1] + extent[1] * extent[2] + extent[2] * extent[0];
    return total <= DBL_MAX ? total : DBL_MAX;
}

// Площадь ребёнка и площадь его объединения с листом за один проход. Спуск
// считает обе величины для обоих детей на каждом уровне, а раздельные вызовы
// читали бы одни и те же границы дважды. Арифметика та же, что в
// BoundsMetric, поэтому значения совпадают побитово.
static void DescentMetrics(const RigidTreeNode *child, const RigidTreeNode *leaf, double *outOwn,
                            double *outUnion)
{
    double own[3];
    double merged[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double minimum = child->minimum[axis];
        double maximum = child->maximum[axis];
        own[axis] = maximum - minimum;
        if (leaf->minimum[axis] < minimum) minimum = leaf->minimum[axis];
        if (leaf->maximum[axis] > maximum) maximum = leaf->maximum[axis];
        merged[axis] = maximum - minimum;
    }
    double ownTotal = own[0] * own[1] + own[1] * own[2] + own[2] * own[0];
    double mergedTotal = merged[0] * merged[1] + merged[1] * merged[2] + merged[2] * merged[0];
    *outOwn = ownTotal <= DBL_MAX ? ownTotal : DBL_MAX;
    *outUnion = mergedTotal <= DBL_MAX ? mergedTotal : DBL_MAX;
}

static void RefreshNode(RigidTreeNode *nodes, uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    const RigidTreeNode *left = &nodes[node->left];
    const RigidTreeNode *right = &nodes[node->right];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        node->minimum[axis] = left->minimum[axis] < right->minimum[axis] ?
                              left->minimum[axis] : right->minimum[axis];
        node->maximum[axis] = left->maximum[axis] > right->maximum[axis] ?
                              left->maximum[axis] : right->maximum[axis];
    }
    node->height = 1 + (left->height > right->height ? left->height : right->height);
}

static void ReplaceChild(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                          uint32_t parent, uint32_t oldChild, uint32_t newChild)
{
    if (parent == RIGID_TREE_EMPTY)
    {
        broadphase->root = newChild;
    }
    else if (nodes[parent].left == oldChild)
    {
        nodes[parent].left = newChild;
    }
    else
    {
        nodes[parent].right = newChild;
    }
    nodes[newChild].parent = parent;
}

static bool KeepFirstGrandchild(const RigidTreeNode *nodes, uint32_t first,
                                uint32_t second, uint32_t fixed)
{
    if (nodes[first].height != nodes[second].height)
    {
        return nodes[first].height > nodes[second].height;
    }
    // Either choice preserves AVL heights. Minimize the new lower node's box;
    // the upper node contains the same subtree, so its bounds cannot change.
    double firstCost = BoundsMetric(&nodes[fixed], &nodes[first]);
    double secondCost = BoundsMetric(&nodes[fixed], &nodes[second]);
    return secondCost < firstCost || (secondCost == firstCost && first < second);
}

static uint32_t BalanceNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                             uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    if (node->height < 2)
    {
        return index;
    }
    uint32_t left = node->left;
    uint32_t right = node->right;
    int32_t difference = nodes[right].height - nodes[left].height;
    if (difference > 1)
    {
        uint32_t first = nodes[right].left;
        uint32_t second = nodes[right].right;
        ReplaceChild(broadphase, nodes, node->parent, index, right);
        nodes[right].left = index;
        node->parent = right;
        bool keepFirst = KeepFirstGrandchild(nodes, first, second, left);
        uint32_t kept = keepFirst ? first : second;
        uint32_t moved = keepFirst ? second : first;
        nodes[right].right = kept;
        node->right = moved;
        nodes[kept].parent = right;
        nodes[moved].parent = index;
        RefreshNode(nodes, index);
        RefreshNode(nodes, right);
        return right;
    }
    if (difference < -1)
    {
        uint32_t first = nodes[left].left;
        uint32_t second = nodes[left].right;
        ReplaceChild(broadphase, nodes, node->parent, index, left);
        nodes[left].right = index;
        node->parent = left;
        bool keepFirst = KeepFirstGrandchild(nodes, first, second, right);
        uint32_t kept = keepFirst ? first : second;
        uint32_t moved = keepFirst ? second : first;
        nodes[left].left = kept;
        node->left = moved;
        nodes[kept].parent = left;
        nodes[moved].parent = index;
        RefreshNode(nodes, index);
        RefreshNode(nodes, left);
        return left;
    }
    return index;
}

static void ImproveNodeQuality(RigidTreeNode *nodes, uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    if (node->height < 2) return;
    uint32_t branch = nodes[node->left].height > nodes[node->right].height
                          ? node->left : node->right;
    uint32_t outside = branch == node->left ? node->right : node->left;
    // A single child/grandchild swap can preserve AVL balance only when the
    // branch is one level taller than the outer child. Equal-height children
    // would leave the new inner branch two levels taller than the moved child.
    if (nodes[branch].height != nodes[outside].height + 1) return;
    double bestCost = BoundsMetric(&nodes[branch], NULL);
    uint32_t bestMoved = RIGID_TREE_EMPTY;
    // Height balance alone cannot repair spatially overlapping branches after
    // objects move. Try swapping one outer child with an inner grandchild.
    // Both affected nodes must remain AVL-balanced. For an already balanced
    // parent this also preserves its height, so no extra ancestor pass is needed.
    // Only the inner branch changes bounds: decreasing its saturating perimeter
    // decreases the tree's total metric. The outer and moved nodes retain their
    // own bounds, while the union at this parent is unchanged.
    for (uint32_t child = 0u; child < 2u; ++child)
    {
        uint32_t moved = child == 0u ? nodes[branch].left : nodes[branch].right;
        uint32_t retained = child == 0u ? nodes[branch].right : nodes[branch].left;
        int32_t outsideHeight = nodes[outside].height;
        int32_t retainedHeight = nodes[retained].height;
        int32_t difference = outsideHeight - retainedHeight;
        if (difference < -1 || difference > 1) continue;
        int32_t branchHeight = 1 + (outsideHeight > retainedHeight
                                       ? outsideHeight : retainedHeight);
        difference = branchHeight - nodes[moved].height;
        if (difference < -1 || difference > 1) continue;
        double cost = BoundsMetric(&nodes[outside], &nodes[retained]);
        if (cost < bestCost)
        {
            bestCost = cost;
            bestMoved = moved;
        }
    }
    if (bestMoved == RIGID_TREE_EMPTY) return;
    if (node->left == branch) node->right = bestMoved;
    else node->left = bestMoved;
    if (nodes[branch].left == bestMoved) nodes[branch].left = outside;
    else nodes[branch].right = outside;
    nodes[bestMoved].parent = index;
    nodes[outside].parent = branch;
    RefreshNode(nodes, branch);
    RefreshNode(nodes, index);
}

// improve отключает починку качества. Гасит её только перемещение прокси:
// там за удалением сразу идёт вставка, и подъём после удаления шёл бы по
// дереву, которое через мгновение изменится снова.
static void RefreshAncestors(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                              uint32_t index, bool improve)
{
    while (index != RIGID_TREE_EMPTY)
    {
        RefreshNode(nodes, index);
        uint32_t balanced = BalanceNode(broadphase, nodes, index);
        // At most one spatial-quality swap per refitted node; never recurse or
        // iterate until convergence during a simulation tick.
        if (improve)
        {
            ImproveNodeQuality(nodes, balanced);
        }
        index = nodes[balanced].parent;
    }
}

static uint32_t TakeNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes)
{
    uint32_t result = broadphase->freeList;
    if (result != RIGID_TREE_EMPTY)
    {
        broadphase->freeList = nodes[result].parent;
        nodes[result].parent = RIGID_TREE_EMPTY;
        nodes[result].left = RIGID_TREE_EMPTY;
        nodes[result].right = RIGID_TREE_EMPTY;
        nodes[result].height = 0;
    }
    return result;
}

static void ReturnNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t index)
{
    nodes[index].parent = broadphase->freeList;
    nodes[index].height = -1;
    broadphase->freeList = index;
}

static bool InsertLeaf(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t leaf)
{
    if (broadphase->root == RIGID_TREE_EMPTY)
    {
        broadphase->root = leaf;
        nodes[leaf].parent = RIGID_TREE_EMPTY;
        return true;
    }
    uint32_t sibling = broadphase->root;
    // Descending to a leaf keeps each insertion's height change <= 1,
    // allowing strict AVL rebalancing on the single ancestor path.
    while (!NodeIsLeaf(&nodes[sibling]))
    {
        uint32_t left = nodes[sibling].left;
        uint32_t right = nodes[sibling].right;
        double leftSize = 0.0;
        double leftUnion = 0.0;
        double rightSize = 0.0;
        double rightUnion = 0.0;
        DescentMetrics(&nodes[left], &nodes[leaf], &leftSize, &leftUnion);
        DescentMetrics(&nodes[right], &nodes[leaf], &rightSize, &rightUnion);
        double leftCost = leftUnion - leftSize;
        double rightCost = rightUnion - rightSize;
        bool chooseLeft = leftCost < rightCost ||
            (leftCost == rightCost && (leftSize < rightSize ||
                                       (leftSize == rightSize && left < right)));
        sibling = chooseLeft ? left : right;
    }
    uint32_t parent = TakeNode(broadphase, nodes);
    if (parent == RIGID_TREE_EMPTY)
    {
        return false;
    }
    uint32_t previousParent = nodes[sibling].parent;
    nodes[parent].left = sibling;
    nodes[parent].right = leaf;
    nodes[leaf].parent = parent;
    ReplaceChild(broadphase, nodes, previousParent, sibling, parent);
    nodes[sibling].parent = parent;
    RefreshAncestors(broadphase, nodes, parent, true);
    return true;
}

// improve передаётся дальше в подъём по предкам. Перемещение прокси гасит
// починку: сразу за удалением идёт вставка, и она пройдёт по дереву снова.
// Настоящее удаление тела — другое дело, за ним не следует ничего.
static void RemoveLeaf(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t leaf,
                        bool improve)
{
    if (broadphase->root == leaf)
    {
        broadphase->root = RIGID_TREE_EMPTY;
        nodes[leaf].parent = RIGID_TREE_EMPTY;
        return;
    }
    uint32_t parent = nodes[leaf].parent;
    uint32_t sibling = nodes[parent].left == leaf ? nodes[parent].right : nodes[parent].left;
    uint32_t grandparent = nodes[parent].parent;
    ReplaceChild(broadphase, nodes, grandparent, parent, sibling);
    ReturnNode(broadphase, nodes, parent);
    nodes[leaf].parent = RIGID_TREE_EMPTY;
    RefreshAncestors(broadphase, nodes, grandparent, improve);
}

// Перестройка меняет только топологию: листья, их толстые коробки и карта
// слотов остаются прежними, поэтому набор кандидатов у запроса не меняется.
// Сборка идёт сверху вниз по середине габарита вдоль самой длинной оси —
// это приближение медианы, не требующее ни сортировки, ни временного массива:
// узлы дерева связываются в список через неиспользуемое поле `right` листа.
// Минимальная доля меньшей половины не даёт выродиться в цепочку.

// Возвращает внутренние узлы поддерева в пул и отцепляет листья. Глубина
// рекурсии равна высоте дерева, а она логарифмическая.
static void ReleaseSubtree(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                           uint32_t index)
{
    if (NodeIsLeaf(&nodes[index]))
    {
        nodes[index].parent = RIGID_TREE_EMPTY;
        return;
    }
    uint32_t left = nodes[index].left;
    uint32_t right = nodes[index].right;
    ReleaseSubtree(broadphase, nodes, left);
    ReleaseSubtree(broadphase, nodes, right);
    ReturnNode(broadphase, nodes, index);
}

static uint32_t RebuildSubtree(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                               uint32_t head, uint32_t count)
{
    if (count == 1u)
    {
        nodes[head].parent = RIGID_TREE_EMPTY;
        nodes[head].right = RIGID_TREE_EMPTY;
        return head;
    }
    double minimum[3];
    double maximum[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        minimum[axis] = nodes[head].minimum[axis];
        maximum[axis] = nodes[head].maximum[axis];
    }
    for (uint32_t item = nodes[head].right; item != RIGID_TREE_EMPTY; item = nodes[item].right)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (nodes[item].minimum[axis] < minimum[axis]) minimum[axis] = nodes[item].minimum[axis];
            if (nodes[item].maximum[axis] > maximum[axis]) maximum[axis] = nodes[item].maximum[axis];
        }
    }
    uint32_t axis = 0u;
    for (uint32_t candidate = 1u; candidate < 3u; ++candidate)
    {
        if (maximum[candidate] - minimum[candidate] > maximum[axis] - minimum[axis])
        {
            axis = candidate;
        }
    }
    double pivot = (minimum[axis] + maximum[axis]) * 0.5;
    uint32_t spatialCount = 0u;
    for (uint32_t item = head; item != RIGID_TREE_EMPTY; item = nodes[item].right)
    {
        if ((nodes[item].minimum[axis] + nodes[item].maximum[axis]) * 0.5 <= pivot)
        {
            ++spatialCount;
        }
    }
    // Вырожденный разрез по середине габарита заменяется делением по числу:
    // обе половины не меньше четверти, поэтому высота остаётся логарифмической.
    bool spatial = spatialCount >= 1u && spatialCount < count &&
                   spatialCount >= count / 4u && spatialCount <= count - count / 4u;
    uint32_t half = count / 2u;
    uint32_t leftHead = RIGID_TREE_EMPTY;
    uint32_t leftTail = RIGID_TREE_EMPTY;
    uint32_t rightHead = RIGID_TREE_EMPTY;
    uint32_t rightTail = RIGID_TREE_EMPTY;
    uint32_t leftCount = 0u;
    uint32_t item = head;
    while (item != RIGID_TREE_EMPTY)
    {
        uint32_t next = nodes[item].right;
        nodes[item].right = RIGID_TREE_EMPTY;
        bool toLeft = spatial
                          ? (nodes[item].minimum[axis] + nodes[item].maximum[axis]) * 0.5 <= pivot
                          : leftCount < half;
        if (toLeft)
        {
            if (leftTail == RIGID_TREE_EMPTY) leftHead = item; else nodes[leftTail].right = item;
            leftTail = item;
            ++leftCount;
        }
        else
        {
            if (rightTail == RIGID_TREE_EMPTY) rightHead = item; else nodes[rightTail].right = item;
            rightTail = item;
        }
        item = next;
    }
    uint32_t node = TakeNode(broadphase, nodes);
    nodes[node].parent = RIGID_TREE_EMPTY;
    nodes[node].left = RebuildSubtree(broadphase, nodes, leftHead, leftCount);
    nodes[node].right = RebuildSubtree(broadphase, nodes, rightHead, count - leftCount);
    nodes[nodes[node].left].parent = node;
    nodes[nodes[node].right].parent = node;
    RefreshNode(nodes, node);
    return node;
}

static void RebuildTree(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t *slots)
{
    if (broadphase->root == RIGID_TREE_EMPTY)
    {
        return;
    }
    ReleaseSubtree(broadphase, nodes, broadphase->root);
    broadphase->root = RIGID_TREE_EMPTY;
    uint32_t head = RIGID_TREE_EMPTY;
    uint32_t tail = RIGID_TREE_EMPTY;
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < broadphase->bodyCapacity; ++slot)
    {
        uint32_t leaf = slots[slot];
        if (leaf == RIGID_TREE_EMPTY)
        {
            continue;
        }
        nodes[leaf].right = RIGID_TREE_EMPTY;
        if (tail == RIGID_TREE_EMPTY) head = leaf; else nodes[tail].right = leaf;
        tail = leaf;
        ++count;
    }
    broadphase->root = RebuildSubtree(broadphase, nodes, head, count);
}

// Первичная инкрементальная сборка даёт заметно худшую форму, чем сборка
// сверху вниз: листья вставляются по одному, и топология получается
// неровной. Когда последний свободный слот впервые получает лист, форма
// собирается заново, а дальше поддерживается обычными вставками и поворотами.
static void RebuildWhenFull(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                            uint32_t *slots, bool becameFull)
{
    if (becameFull)
    {
        RebuildTree(broadphase, nodes, slots);
    }
}

bool RigidBroadphaseSetProxy(VoxelRigidBroadphase *broadphase, uint32_t slot,
                              const double minimum[3], const double maximum[3])
{
    if (!RigidBroadphaseValid(broadphase, 0u) || slot >= broadphase->bodyCapacity ||
        !BoundsValid(minimum, maximum))
    {
        return false;
    }
    RigidTreeNode *nodes = TreeNodes(broadphase);
    uint32_t *slots = TreeSlots(broadphase);
    uint32_t leaf = slots[slot];
    bool existing = leaf != RIGID_TREE_EMPTY;
    if (existing)
    {
        // Оба условия проверяются одним проходом по той же кэш-линии: у сцены
        // с множеством неподвижных тел этот путь исполняется на каждом теле
        // каждый шаг и почти всегда заканчивается здесь.
        const RigidTreeNode *node = &nodes[leaf];
        bool keep = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            // Containment alone retains giant proxies after a shape shrinks or
            // a slot is reused. A wider hysteresis envelope avoids per-tick churn.
            if (minimum[axis] < node->minimum[axis] || maximum[axis] > node->maximum[axis] ||
                node->minimum[axis] < minimum[axis] - RIGID_TREE_SHRINK ||
                node->maximum[axis] > maximum[axis] + RIGID_TREE_SHRINK)
            {
                keep = false;
            }
        }
        if (keep)
        {
            return true;
        }
    }
    double previousMinimum[3] = {0.0, 0.0, 0.0};
    double previousMaximum[3] = {0.0, 0.0, 0.0};
    if (existing)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            previousMinimum[axis] = nodes[leaf].minimum[axis];
            previousMaximum[axis] = nodes[leaf].maximum[axis];
        }
        RemoveLeaf(broadphase, nodes, leaf, false);
    }
    else
    {
        leaf = TakeNode(broadphase, nodes);
        if (leaf == RIGID_TREE_EMPTY)
        {
            return false;
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double ahead = 0.0;
        double behind = 0.0;
        if (existing)
        {
            double previous = (previousMinimum[axis] + previousMaximum[axis]) * 0.5;
            double current = (minimum[axis] + maximum[axis]) * 0.5;
            double shift = (current - previous) * RIGID_TREE_PREDICT;
            if (shift > RIGID_TREE_PREDICT_CAP)
            {
                shift = RIGID_TREE_PREDICT_CAP;
            }
            if (shift < -RIGID_TREE_PREDICT_CAP)
            {
                shift = -RIGID_TREE_PREDICT_CAP;
            }
            ahead = shift > 0.0 ? shift : 0.0;
            behind = shift < 0.0 ? -shift : 0.0;
        }
        nodes[leaf].minimum[axis] = minimum[axis] - RIGID_TREE_MARGIN - behind;
        nodes[leaf].maximum[axis] = maximum[axis] + RIGID_TREE_MARGIN + ahead;
    }
    nodes[leaf].left = slot;
    if (!InsertLeaf(broadphase, nodes, leaf))
    {
        if (!existing) ReturnNode(broadphase, nodes, leaf);
        return false;
    }
    slots[slot] = leaf;
    if (!existing) ++broadphase->proxyCount;
    CountEvent(&broadphase->updatedProxyCount);
    bool becameFull = !existing && broadphase->proxyCount == broadphase->bodyCapacity;
    RebuildWhenFull(broadphase, nodes, slots, becameFull);
    return true;
}

void RigidBroadphaseRemoveProxy(VoxelRigidBroadphase *broadphase, uint32_t slot)
{
    if (!RigidBroadphaseValid(broadphase, 0u) || slot >= broadphase->bodyCapacity)
    {
        return;
    }
    uint32_t *slots = TreeSlots(broadphase);
    uint32_t leaf = slots[slot];
    if (leaf == RIGID_TREE_EMPTY)
    {
        return;
    }
    RigidTreeNode *nodes = TreeNodes(broadphase);
    RemoveLeaf(broadphase, nodes, leaf, true);
    ReturnNode(broadphase, nodes, leaf);
    slots[slot] = RIGID_TREE_EMPTY;
    --broadphase->proxyCount;
    CountEvent(&broadphase->updatedProxyCount);
}

bool RigidBroadphaseQuery(VoxelRigidBroadphase *broadphase, const double minimum[3],
                           const double maximum[3], uint32_t *outSlots,
                           uint32_t capacity, uint32_t *outCount)
{
    if (!RigidBroadphaseValid(broadphase, 0u) || !BoundsValid(minimum, maximum) ||
        outCount == NULL || (capacity != 0u && outSlots == NULL))
    {
        return false;
    }
    *outCount = 0u;
    const RigidTreeNode *nodes = TreeNodes(broadphase);
    // Запрос на каждом попадании пишет в outSlots, а компилятор не может
    // доказать, что этот массив не пересекается с minimum/maximum. Без
    // локальных копий он обязан перечитывать границы после каждой записи.
    // Копии лежат в стеке, их адрес не уходит наружу, и шесть чисел живут в
    // регистрах весь обход. Значения те же, поведение не меняется.
    double queryMinimum[3];
    double queryMaximum[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        queryMinimum[axis] = minimum[axis];
        queryMaximum[axis] = maximum[axis];
    }
    // Явный стек вместо хождения по родительским ссылкам. Обход по ссылкам
    // возвращался в узел ещё раз после каждого поддерева, и на каждый
    // проверенный узел приходилось ровно две итерации цикла: половина работы
    // уходила на возврат вверх. Со стеком итерация ровно одна.
    //
    // Глубина ограничена высотой дерева, а она у AVL не превышает
    // 1.44*log2(n) — для предельной ёмкости это 32 уровня. Инвариант
    // машиной не проверяется, поэтому слишком глубокое дерево не считается
    // невозможным: обход тогда идёт прежним путём по родительским ссылкам,
    // и полнота сохраняется в любом случае.
    //
    // На стеке лежит пара номеров детей — не сам узел. Номера детей лежат в
    // той же строке кэша, что и границы, по которым узел только что проверили,
    // и потому достаются даром; а снятие со стека перестаёт быть чтением узла
    // вовсе. Запись — два слова на уровень, поэтому и место под неё вдвое
    // больше, а глубина в уровнях осталась прежней.
    uint32_t stack[RIGID_QUERY_STACK * 2u];
    uint64_t visited = 0u;
    uint32_t written = 0u;
    bool overflow = false;
    uint32_t depth = 0u;
    // Глубина обхода не превышает высоту дерева: на каждом спуске снимается
    // одна пара и кладётся не больше двух. Высота хранится в корне, поэтому
    // запас проверяется один раз до цикла, а не на каждом узле.
    bool fits = broadphase->root != RIGID_TREE_EMPTY &&
                nodes[broadphase->root].height + 2 <= (int32_t)RIGID_QUERY_STACK;
    if (fits)
    {
        const RigidTreeNode *root = &nodes[broadphase->root];
        ++visited;
        if (BoundsOverlap(root, queryMinimum, queryMaximum))
        {
            if (!NodeIsLeaf(root))
            {
                stack[depth++] = root->left;
                stack[depth++] = root->right;
            }
            else if (capacity != 0u)
            {
                outSlots[written++] = root->left;
            }
            else
            {
                overflow = true;
            }
        }
    }
    // На стеке лежат только дети тех узлов, про которые уже известно, что они
    // пересекают запрос и не являются листьями. Оба ребёнка читаются и
    // проверяются подряд: их загрузки не зависят ни друг от друга, ни от
    // исхода проверки, поэтому уходят в память параллельно, а не цепочкой.
    // Число проверенных узлов при этом то же самое.
    //
    // Пока ветвь одна — а на запрос по одному телу так почти всегда, — спуск
    // идёт прямо, без записи на стек и снятия с него: пара номеров уже в
    // регистрах. На стек кладётся только отложенная вторая ветвь.
    while (!overflow && depth != 0u)
    {
        uint32_t rightIndex = stack[--depth];
        uint32_t leftIndex = stack[--depth];
        for (;;)
        {
            const RigidTreeNode *left = &nodes[leftIndex];
            const RigidTreeNode *right = &nodes[rightIndex];
            bool leftHit = BoundsOverlap(left, queryMinimum, queryMaximum);
            bool rightHit = BoundsOverlap(right, queryMinimum, queryMaximum);
            bool leftLeaf = NodeIsLeaf(left);
            bool rightLeaf = NodeIsLeaf(right);
            visited += 2u;

            if (leftHit && leftLeaf)
            {
                if (written == capacity)
                {
                    overflow = true;
                    break;
                }
                outSlots[written++] = left->left;
            }
            if (rightHit && rightLeaf)
            {
                if (written == capacity)
                {
                    overflow = true;
                    break;
                }
                outSlots[written++] = right->left;
            }

            // Правая ветвь откладывается: разбирается она второй, и поддеревья
            // идут слева направо. Лист правой ветви при этом выдаётся раньше
            // левого поддерева — порядок выдачи заголовок не фиксирует, а
            // вызывающий приводит его к каноническому по stableId.
            bool leftBranch = leftHit && !leftLeaf;
            bool rightBranch = rightHit && !rightLeaf;
            if (leftBranch)
            {
                if (rightBranch)
                {
                    stack[depth++] = right->left;
                    stack[depth++] = right->right;
                }
                leftIndex = left->left;
                rightIndex = left->right;
                continue;
            }
            if (rightBranch)
            {
                leftIndex = right->left;
                rightIndex = right->right;
                continue;
            }
            break;
        }
    }

    if (fits || broadphase->root == RIGID_TREE_EMPTY)
    {
        *outCount = written;
        uint64_t total = (uint64_t)broadphase->visitedNodeCount + visited;
        broadphase->visitedNodeCount = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
        return !overflow;
    }

    // Запасной путь: дерево оказалось глубже, чем допускает инвариант AVL.
    // Он ничего не хранит и потому работает на дереве любой глубины.
    visited = 0u;
    written = 0u;
    overflow = false;
    uint32_t current = broadphase->root;
    uint32_t previous = RIGID_TREE_EMPTY;
    while (current != RIGID_TREE_EMPTY)
    {
        const RigidTreeNode *node = &nodes[current];
        uint32_t next = node->parent;
        if (previous == node->parent)
        {
            ++visited;
            if (BoundsOverlap(node, queryMinimum, queryMaximum))
            {
                if (NodeIsLeaf(node))
                {
                    if (written == capacity)
                    {
                        overflow = true;
                        break;
                    }
                    outSlots[written++] = node->left;
                }
                else
                {
                    next = node->left;
                }
            }
        }
        else if (previous == node->left)
        {
            next = node->right;
        }
        previous = current;
        current = next;
    }
    *outCount = written;
    uint64_t total = (uint64_t)broadphase->visitedNodeCount + visited;
    broadphase->visitedNodeCount = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
    return !overflow;
}
