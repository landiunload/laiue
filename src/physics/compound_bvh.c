#include "physics/compound_bvh.h"

// Шесть подряд идущих double: minimum[0..2], maximum[0..2].
#define COMPOUND_BVH_ENTRY_BYTES (6u * sizeof(double))
// Сбалансированное дерево из Build не глубже log2(UINT32_MAX) == 31, поэтому
// явного стека на 32 кадра хватает с запасом и без рекурсии в Query.
#define COMPOUND_BVH_STACK_FRAMES 32u

static bool CompoundBvhIsFinite(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return ((representation.bits >> 52) & 0x7ffu) != 0x7ffu;
}

static const double *CompoundBvhEntry(const unsigned char *base, size_t stride, uint32_t index)
{
    return (const double *)(const void *)(base + (size_t)index * stride);
}

// Центр интервала без переполнения: при разных знаках берутся половины
// (их сумма не может выйти за double), при одинаковых знаках разность
// конечна, потому что не превышает модуля большего конца.
static double CompoundBvhCenter(double low, double high)
{
    if (low < 0.0 && high > 0.0)
    {
        return low * 0.5 + high * 0.5;
    }
    return low + (high - low) * 0.5;
}

// Полный детерминированный порядок: сначала ключ, при равенстве — исходный
// индекс. Ключи конечны, поэтому сравнения задают строгий порядок.
static bool CompoundBvhBefore(const unsigned char *base, size_t stride, uint32_t left,
                              uint32_t right, int32_t axis)
{
    const double *leftEntry = CompoundBvhEntry(base, stride, left);
    const double *rightEntry = CompoundBvhEntry(base, stride, right);
    double leftCenter = CompoundBvhCenter(leftEntry[axis], leftEntry[3 + axis]);
    double rightCenter = CompoundBvhCenter(rightEntry[axis], rightEntry[3 + axis]);
    if (leftCenter < rightCenter)
    {
        return true;
    }
    if (leftCenter > rightCenter)
    {
        return false;
    }
    return left < right;
}

static void CompoundBvhSwap(uint32_t *left, uint32_t *right)
{
    uint32_t temporary = *left;
    *left = *right;
    *right = temporary;
}

// Heapsort без выделения памяти. Компаратор задаёт полный порядок, поэтому
// перестановки равных элементов не влияют на результат.
static void CompoundBvhSift(const unsigned char *base, size_t stride, uint32_t *values,
                            uint32_t root, uint32_t size, int32_t axis)
{
    for (;;)
    {
        uint32_t child = root * 2u + 1u;
        if (child >= size)
        {
            break;
        }
        if (child + 1u < size &&
            CompoundBvhBefore(base, stride, values[child], values[child + 1u], axis))
        {
            ++child;
        }
        if (!CompoundBvhBefore(base, stride, values[root], values[child], axis))
        {
            break;
        }
        CompoundBvhSwap(&values[root], &values[child]);
        root = child;
    }
}

static void CompoundBvhSortRange(const unsigned char *base, size_t stride, uint32_t *values,
                                 uint32_t begin, uint32_t end, int32_t axis)
{
    uint32_t size = end - begin;
    if (size < 2u)
    {
        return;
    }
    uint32_t *range = values + begin;
    for (uint32_t start = size / 2u; start > 0u; --start)
    {
        CompoundBvhSift(base, stride, range, start - 1u, size, axis);
    }
    for (uint32_t last = size; last > 1u; --last)
    {
        CompoundBvhSwap(&range[0], &range[last - 1u]);
        CompoundBvhSift(base, stride, range, 0u, last - 1u, axis);
    }
}

// Медиана трёх по полному порядку компаратора: возвращает индекс среднего
// значения. Нужна как детерминированная опора разбиения.
static uint32_t CompoundBvhMedianOfThree(const unsigned char *base, size_t stride,
                                         const uint32_t *values, uint32_t first, uint32_t middle,
                                         uint32_t last, int32_t axis)
{
    uint32_t firstValue = values[first];
    uint32_t middleValue = values[middle];
    uint32_t lastValue = values[last];
    if (CompoundBvhBefore(base, stride, firstValue, middleValue, axis))
    {
        if (CompoundBvhBefore(base, stride, middleValue, lastValue, axis))
        {
            return middle;
        }
        return CompoundBvhBefore(base, stride, firstValue, lastValue, axis) ? last : first;
    }
    if (CompoundBvhBefore(base, stride, firstValue, lastValue, axis))
    {
        return first;
    }
    return CompoundBvhBefore(base, stride, middleValue, lastValue, axis) ? last : middle;
}

// Разбиение Хоара (схема Ломуто) вокруг values[pivot]: возвращает индекс, на
// который встал опорный элемент. Влево попадают строго меньшие, вправо — не
// меньшие, поэтому диапазон остаётся корректно разделённым.
static uint32_t CompoundBvhPartition(const unsigned char *base, size_t stride, uint32_t *values,
                                     uint32_t begin, uint32_t end, uint32_t pivot, int32_t axis)
{
    CompoundBvhSwap(&values[pivot], &values[end - 1u]);
    uint32_t pivotValue = values[end - 1u];
    uint32_t store = begin;
    for (uint32_t index = begin; index + 1u < end; ++index)
    {
        if (CompoundBvhBefore(base, stride, values[index], pivotValue, axis))
        {
            CompoundBvhSwap(&values[store], &values[index]);
            ++store;
        }
    }
    CompoundBvhSwap(&values[store], &values[end - 1u]);
    return store;
}

// Quickselect с медианой трёх: ставит элемент ранга target на место target,
// оставляя слева меньшие, справа не меньшие. Это дешевле полной сортировки
// диапазона на каждом уровне рекурсии Build. Глубина ограничена, иначе
// враждебный порядок центров дал бы квадратичный худший случай; после лимита
// диапазон честно сортируется heapsort-ом, то есть асимптотика не хуже
// прежней, а типичная — линейная на уровень.
static void CompoundBvhSelectRange(const unsigned char *base, size_t stride, uint32_t *values,
                                   uint32_t begin, uint32_t end, uint32_t target, int32_t axis)
{
    uint32_t depthLimit = 0u;
    for (uint32_t size = end - begin; size > 1u; size >>= 1u)
    {
        ++depthLimit;
    }
    depthLimit *= 2u;

    uint32_t depth = 0u;
    while (end - begin > 1u)
    {
        if (depth >= depthLimit)
        {
            CompoundBvhSortRange(base, stride, values, begin, end, axis);
            return;
        }
        ++depth;
        uint32_t medianIndex = begin + (end - begin) / 2u;
        uint32_t pivot =
            CompoundBvhMedianOfThree(base, stride, values, begin, medianIndex, end - 1u, axis);
        uint32_t position = CompoundBvhPartition(base, stride, values, begin, end, pivot, axis);
        if (position == target)
        {
            return;
        }
        if (target < position)
        {
            end = position;
        }
        else
        {
            begin = position + 1u;
        }
    }
}

// Пост-обход: сначала полностью строится левое поддерево, затем правое,
// затем их родитель. Поэтому first < second у каждого внутреннего узла, а
// индекс родителя больше индексов обоих детей. Глубина не больше log2(count).
static uint32_t CompoundBvhBuildRange(const unsigned char *base, size_t stride,
                                      uint32_t *workspace, RigidCompoundBvhNode *nodes,
                                      uint32_t begin, uint32_t end, uint32_t *next)
{
    uint32_t size = end - begin;
    if (size == 1u)
    {
        return workspace[begin];
    }

    const double *firstEntry = CompoundBvhEntry(base, stride, workspace[begin]);
    double low[3];
    double high[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        low[axis] = firstEntry[axis];
        high[axis] = firstEntry[3 + axis];
    }
    for (uint32_t position = begin + 1u; position < end; ++position)
    {
        const double *entry = CompoundBvhEntry(base, stride, workspace[position]);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (entry[axis] < low[axis])
            {
                low[axis] = entry[axis];
            }
            if (entry[3 + axis] > high[axis])
            {
                high[axis] = entry[3 + axis];
            }
        }
    }

    int32_t axis = 0;
    double extent = high[0] - low[0];
    for (int32_t candidate = 1; candidate < 3; ++candidate)
    {
        double candidateExtent = high[candidate] - low[candidate];
        if (candidateExtent > extent)
        {
            extent = candidateExtent;
            axis = candidate;
        }
    }

    uint32_t middle = begin + size / 2u;
    CompoundBvhSelectRange(base, stride, workspace, begin, end, middle, axis);

    uint32_t left = CompoundBvhBuildRange(base, stride, workspace, nodes, begin, middle, next);
    uint32_t right = CompoundBvhBuildRange(base, stride, workspace, nodes, middle, end, next);
    uint32_t index = *next;
    *next = index + 1u;
    for (int32_t component = 0; component < 3; ++component)
    {
        double leftMinimum = nodes[left].minimum[component];
        double rightMinimum = nodes[right].minimum[component];
        nodes[index].minimum[component] = leftMinimum < rightMinimum ? leftMinimum : rightMinimum;
        double leftMaximum = nodes[left].maximum[component];
        double rightMaximum = nodes[right].maximum[component];
        nodes[index].maximum[component] = leftMaximum > rightMaximum ? leftMaximum : rightMaximum;
    }
    nodes[index].first = left;
    nodes[index].second = right;
    return index;
}

bool RigidCompoundBvhBuild(const void *bounds, size_t stride, uint32_t count,
                           RigidCompoundBvhNode *nodes, uint32_t nodeCapacity,
                           uint32_t *workspace, uint32_t *outRoot)
{
    if (bounds == NULL || nodes == NULL || workspace == NULL || outRoot == NULL)
    {
        return false;
    }
    if (count == 0u || stride < COMPOUND_BVH_ENTRY_BYTES)
    {
        return false;
    }
    if ((stride % sizeof(double)) != 0u || ((uintptr_t)bounds % _Alignof(double)) != 0u ||
        ((uintptr_t)nodes % _Alignof(RigidCompoundBvhNode)) != 0u)
    {
        return false;
    }
    uint64_t required = 2ull * (uint64_t)count - 1ull;
    if (required > (uint64_t)nodeCapacity)
    {
        return false;
    }
    // Последний AABB занимает (count - 1) * stride + 48 байт. Проверяем это
    // до любого обращения к памяти, чтобы не строить адрес за пределами
    // адресуемого пространства.
    uint64_t steps = (uint64_t)(count - 1u);
    if (steps > (UINT64_MAX - (uint64_t)COMPOUND_BVH_ENTRY_BYTES) / (uint64_t)stride)
    {
        return false;
    }
    uint64_t span = steps * (uint64_t)stride + (uint64_t)COMPOUND_BVH_ENTRY_BYTES;
    if (span > (uint64_t)UINTPTR_MAX - (uint64_t)(uintptr_t)bounds)
    {
        return false;
    }

    const unsigned char *base = (const unsigned char *)bounds;
    // Сначала полная проверка, потом запись: отказ не оставляет следа.
    for (uint32_t index = 0u; index < count; ++index)
    {
        const double *entry = CompoundBvhEntry(base, stride, index);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            double low = entry[axis];
            double high = entry[3 + axis];
            if (!CompoundBvhIsFinite(low) || !CompoundBvhIsFinite(high) || !(low <= high))
            {
                return false;
            }
        }
    }

    for (uint32_t index = 0u; index < count; ++index)
    {
        const double *entry = CompoundBvhEntry(base, stride, index);
        workspace[index] = index;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            nodes[index].minimum[axis] = entry[axis];
            nodes[index].maximum[axis] = entry[3 + axis];
        }
        nodes[index].first = index;
        nodes[index].second = UINT32_MAX;
    }

    uint32_t next = count;
    uint32_t root = CompoundBvhBuildRange(base, stride, workspace, nodes, 0u, count, &next);
    *outRoot = root;
    return true;
}

// Тот же строгий тест, что первая строка BuildBoxManifold: касание гранями
// перекрытием не считается. Сравнение через > отбрасывает NaN, если он всё
// же дойдёт сюда.
static bool CompoundBvhTouches(const RigidCompoundBvhNode *node, const double minimum[3],
                               const double maximum[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!(node->maximum[axis] > minimum[axis]) || !(maximum[axis] > node->minimum[axis]))
        {
            return false;
        }
    }
    return true;
}

static void CompoundBvhSiftValues(uint32_t *values, uint32_t root, uint32_t size)
{
    for (;;)
    {
        uint32_t child = root * 2u + 1u;
        if (child >= size)
        {
            break;
        }
        if (child + 1u < size && values[child] < values[child + 1u])
        {
            ++child;
        }
        if (!(values[root] < values[child]))
        {
            break;
        }
        CompoundBvhSwap(&values[root], &values[child]);
        root = child;
    }
}

// Исходные индексы листьев уникальны, поэтому обычный heapsort даёт строго
// возрастающий порядок.
static void CompoundBvhSortIndices(uint32_t *values, uint32_t size)
{
    for (uint32_t start = size / 2u; start > 0u; --start)
    {
        CompoundBvhSiftValues(values, start - 1u, size);
    }
    for (uint32_t last = size; last > 1u; --last)
    {
        CompoundBvhSwap(&values[0], &values[last - 1u]);
        CompoundBvhSiftValues(values, 0u, last - 1u);
    }
}

bool RigidCompoundBvhQuery(const RigidCompoundBvhNode *nodes, uint32_t root,
                           const double minimum[3], const double maximum[3],
                           uint32_t *outIndices, uint32_t capacity, uint32_t *outCount)
{
    if (nodes == NULL || minimum == NULL || maximum == NULL || outIndices == NULL ||
        outCount == NULL)
    {
        return false;
    }
    if (root == UINT32_MAX)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!CompoundBvhIsFinite(minimum[axis]) || !CompoundBvhIsFinite(maximum[axis]) ||
            !(minimum[axis] <= maximum[axis]))
        {
            return false;
        }
    }

    uint32_t stack[COMPOUND_BVH_STACK_FRAMES];
    uint32_t stackSize = 0u;
    stack[stackSize++] = root;
    uint32_t hits = 0u;
    while (stackSize > 0u)
    {
        uint32_t index = stack[--stackSize];
        const RigidCompoundBvhNode *node = &nodes[index];
        if (!CompoundBvhTouches(node, minimum, maximum))
        {
            continue;
        }
        if (node->second == UINT32_MAX)
        {
            if (hits >= capacity)
            {
                return false;
            }
            outIndices[hits++] = node->first;
            continue;
        }
        // Перед двумя записями проверяем, что кадры помещаются целиком.
        if (stackSize > COMPOUND_BVH_STACK_FRAMES - 2u)
        {
            return false;
        }
        stack[stackSize++] = node->first;
        stack[stackSize++] = node->second;
    }

    CompoundBvhSortIndices(outIndices, hits);
    *outCount = hits;
    return true;
}
