// Регрессия внутреннего child AABB BVH составного тела:
//   RigidCompoundBvhBuild / RigidCompoundBvhQuery
//
// Контракт — в src/physics/compound_bvh.h. Тест намеренно проверяет
// отказ аргументов и границ, отсутствие записи при отказе, выровненный и
// strided вход, детерминизм дерева, строгое исключение касания, вырожденные
// и огромные координаты, минимальную вместимость с канарейками, порядок
// исходных индексов по возрастанию и совпадение со собственным brute force на
// 4096 случайных коробках.
//
// Файл standalone: BVH не экспортируется из `laiue_physics`, поэтому root
// компилирует `src/physics/compound_bvh.c` прямо в тестовый исполняемый файл
// (как `texture_pack_build_test.c`). Собственных экспортов не нужно.
//
// No-CRT сборка не должна растить кадр стека, поэтому все буферы static.

#include "physics/compound_bvh.h"
#include "test_runtime.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BVH_TEST_MAX 4096u

static double compactBounds[BVH_TEST_MAX * 6u];
static RigidCompoundBvhNode nodesA[2u * BVH_TEST_MAX];
static RigidCompoundBvhNode nodesB[2u * BVH_TEST_MAX];
static uint32_t workspaceA[BVH_TEST_MAX];
static uint32_t workspaceB[BVH_TEST_MAX];
static uint32_t queryOutput[BVH_TEST_MAX];
static uint32_t bruteOutput[BVH_TEST_MAX];

// stride = 10 * sizeof(double) = 80: между AABB и следующим есть padding, то
// есть вход заведомо strided, а не плотный.
typedef struct BvhStridedBox
{
    double minimum[3];
    double maximum[3];
    double padding[4];
} BvhStridedBox;

static BvhStridedBox stridedBounds[BVH_TEST_MAX];

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Compound BVH failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static union
{
    double scalar;
    uint64_t bits;
} scalarBits;

static uint64_t DoubleBits(double value)
{
    scalarBits.scalar = value;
    return scalarBits.bits;
}

static double NaNValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff8000000000000);
    return scalarBits.scalar;
}

static double InfValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff0000000000000);
    return scalarBits.scalar;
}

static void StoreBoundEntry(double *destination, const double minimum[3],
                            const double maximum[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        destination[axis] = minimum[axis];
        destination[3 + axis] = maximum[axis];
    }
}

static bool SameNode(const RigidCompoundBvhNode *first, const RigidCompoundBvhNode *second)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (DoubleBits(first->minimum[axis]) != DoubleBits(second->minimum[axis]) ||
            DoubleBits(first->maximum[axis]) != DoubleBits(second->maximum[axis]))
        {
            return false;
        }
    }
    return first->first == second->first && first->second == second->second;
}

static bool SameIndices(const uint32_t *first, const uint32_t *second, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

static bool Ascending(const uint32_t *values, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        if (!(values[index - 1u] < values[index]))
        {
            return false;
        }
    }
    return true;
}

// Строгий тест первого шага BuildBoxManifold: касание гранями не пересечение.
static bool BoxTouches(const double *entry, const double minimum[3], const double maximum[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!(entry[3 + axis] > minimum[axis]) || !(maximum[axis] > entry[axis]))
        {
            return false;
        }
    }
    return true;
}

static uint32_t BruteForce(const double *bounds, uint32_t count, const double minimum[3],
                           const double maximum[3], uint32_t *outIndices)
{
    uint32_t hits = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (BoxTouches(bounds + 6u * index, minimum, maximum))
        {
            outIndices[hits++] = index;
        }
    }
    return hits;
}

static uint32_t randomState;

static uint32_t NextRandom(void)
{
    randomState = randomState * 1664525u + 1013904223u;
    return randomState;
}

static double RandomUnit(void)
{
    return (double)(NextRandom() >> 8) / 16777216.0;
}

static double RandomRange(double low, double high)
{
    return low + (high - low) * RandomUnit();
}

static void GenerateRandomBoxes(uint32_t count, double *outBounds, uint32_t seed)
{
    randomState = seed;
    for (uint32_t index = 0u; index < count; ++index)
    {
        double minimum[3];
        double maximum[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            double center = RandomRange(-400.0, 400.0);
            double half = RandomRange(0.0, 30.0);
            minimum[axis] = center - half;
            maximum[axis] = center + half;
        }
        // Каждая шестнадцатая коробка вырождается по случайной оси.
        if ((NextRandom() & 15u) == 0u)
        {
            int32_t axis = (int32_t)(NextRandom() % 3u);
            minimum[axis] = maximum[axis];
        }
        StoreBoundEntry(outBounds + 6u * index, minimum, maximum);
    }
}

static void RandomQuery(double minimum[3], double maximum[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double first = RandomRange(-450.0, 450.0);
        double second = RandomRange(-450.0, 450.0);
        minimum[axis] = first < second ? first : second;
        maximum[axis] = first < second ? second : first;
        if (minimum[axis] == maximum[axis])
        {
            maximum[axis] += 1.0;
        }
    }
}

static void TestSingleBox(void)
{
    double bounds[6];
    double minimum[3] = {0.0, 0.0, 0.0};
    double maximum[3] = {1.0, 1.0, 1.0};
    StoreBoundEntry(bounds, minimum, maximum);

    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(bounds, 6u * sizeof(double), 1u, nodesA, 1u, workspaceA, &root),
           "single box build");
    Expect(root == 0u, "single root is the leaf");
    Expect(nodesA[0].first == 0u && nodesA[0].second == UINT32_MAX, "single leaf layout");
    Expect(DoubleBits(nodesA[0].minimum[1]) == DoubleBits(0.0) &&
               DoubleBits(nodesA[0].maximum[2]) == DoubleBits(1.0),
           "single leaf keeps its bounds");

    double queryMinimum[3] = {0.5, 0.5, 0.5};
    double queryMaximum[3] = {0.6, 0.6, 0.6};
    uint32_t count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 1u, &count),
           "single query");
    Expect(count == 1u && queryOutput[0] == 0u, "single query hits the leaf");

    queryMinimum[0] = 2.0;
    queryMaximum[0] = 3.0;
    count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 1u, &count),
           "single miss query");
    Expect(count == 0u, "single miss has no hits");
}

static void TestRejectBuild(void)
{
    double bounds[24];
    double minimum[3] = {0.0, 0.0, 0.0};
    double maximum[3] = {1.0, 1.0, 1.0};
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        minimum[0] = (double)index * 3.0;
        maximum[0] = minimum[0] + 1.0;
        StoreBoundEntry(bounds + 6u * index, minimum, maximum);
    }
    size_t stride = 6u * sizeof(double);
    uint32_t root = 0xDEADBEEFu;

    Expect(!RigidCompoundBvhBuild(NULL, stride, 4u, nodesA, 7u, workspaceA, &root),
           "null bounds rejected");
    Expect(root == 0xDEADBEEFu, "null bounds leaves root unchanged");
    Expect(!RigidCompoundBvhBuild(bounds, stride, 4u, NULL, 7u, workspaceA, &root),
           "null nodes rejected");
    Expect(!RigidCompoundBvhBuild(bounds, stride, 4u, nodesA, 7u, NULL, &root),
           "null workspace rejected");
    Expect(!RigidCompoundBvhBuild(bounds, stride, 4u, nodesA, 7u, workspaceA, NULL),
           "null outRoot rejected");
    Expect(!RigidCompoundBvhBuild(bounds, stride, 0u, nodesA, 7u, workspaceA, &root),
           "zero count rejected");
    Expect(root == 0xDEADBEEFu, "zero count leaves root unchanged");
    Expect(!RigidCompoundBvhBuild(bounds, 47u, 4u, nodesA, 7u, workspaceA, &root),
           "short stride rejected");
    Expect(!RigidCompoundBvhBuild(bounds, 52u, 4u, nodesA, 7u, workspaceA, &root),
           "unaligned stride rejected");
    Expect(!RigidCompoundBvhBuild(bounds, stride, 4u, nodesA, 6u, workspaceA, &root),
           "capacity below 2*count-1 rejected");
    Expect(root == 0xDEADBEEFu, "capacity rejection leaves root unchanged");
    Expect(!RigidCompoundBvhBuild(bounds, stride, UINT32_MAX, nodesA, UINT32_MAX, workspaceA,
                                  &root),
           "overflowing count rejected");

    // Точная минимальная вместимость принимается.
    Expect(RigidCompoundBvhBuild(bounds, stride, 4u, nodesA, 7u, workspaceA, &root),
           "exact minimal capacity accepted");
    Expect(root < 7u, "exact capacity root is a valid node");

    double broken[24];
    for (uint32_t index = 0u; index < 24u; ++index)
    {
        broken[index] = bounds[index];
    }
    broken[0] = NaNValue();
    root = 0xDEADBEEFu;
    Expect(!RigidCompoundBvhBuild(broken, stride, 4u, nodesA, 7u, workspaceA, &root),
           "NaN minimum rejected");
    Expect(root == 0xDEADBEEFu, "NaN rejection leaves root unchanged");
    broken[0] = bounds[0];
    broken[5] = InfValue();
    Expect(!RigidCompoundBvhBuild(broken, stride, 4u, nodesA, 7u, workspaceA, &root),
           "infinite maximum rejected");
    broken[5] = bounds[5];
    broken[1] = -InfValue();
    Expect(!RigidCompoundBvhBuild(broken, stride, 4u, nodesA, 7u, workspaceA, &root),
           "negative infinite minimum rejected");
    broken[1] = bounds[1];
    broken[0] = 2.0;
    broken[3] = 1.0;
    Expect(!RigidCompoundBvhBuild(broken, stride, 4u, nodesA, 7u, workspaceA, &root),
           "inverted minimum/maximum rejected");

    // Отказ ничего не пишет: канарейки в nodes и workspace остаются целы.
    nodesA[0].first = 0xAAAAAAAAu;
    nodesA[0].second = 0xBBBBBBBBu;
    workspaceA[0] = 0xCCCCCCCCu;
    broken[0] = NaNValue();
    Expect(!RigidCompoundBvhBuild(broken, stride, 4u, nodesA, 7u, workspaceA, &root),
           "rejected build writes nothing");
    Expect(nodesA[0].first == 0xAAAAAAAAu && nodesA[0].second == 0xBBBBBBBBu &&
               workspaceA[0] == 0xCCCCCCCCu,
           "rejection keeps node and workspace canaries");
}

static void TestRejectQuery(void)
{
    double bounds[18];
    double minimum[3] = {0.0, 0.0, 0.0};
    double maximum[3] = {1.0, 1.0, 1.0};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        minimum[0] = (double)index * 3.0;
        maximum[0] = minimum[0] + 1.0;
        StoreBoundEntry(bounds + 6u * index, minimum, maximum);
    }
    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(bounds, 6u * sizeof(double), 3u, nodesA, 5u, workspaceA, &root),
           "query rejection fixture built");

    double queryMinimum[3] = {-1.0, -1.0, -1.0};
    double queryMaximum[3] = {7.0, 1.0, 1.0};
    uint32_t count = 0x12345678u;

    Expect(!RigidCompoundBvhQuery(NULL, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "null nodes rejected");
    Expect(!RigidCompoundBvhQuery(nodesA, root, NULL, queryMaximum, queryOutput, 3u, &count),
           "null minimum rejected");
    Expect(!RigidCompoundBvhQuery(nodesA, root, queryMinimum, NULL, queryOutput, 3u, &count),
           "null maximum rejected");
    Expect(!RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, NULL, 3u, &count),
           "null output rejected");
    Expect(!RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, NULL),
           "null outCount rejected");
    Expect(!RigidCompoundBvhQuery(nodesA, UINT32_MAX, queryMinimum, queryMaximum, queryOutput, 3u,
                                  &count),
           "missing root rejected");
    Expect(count == 0x12345678u, "query rejection leaves outCount unchanged");

    double badMinimum[3] = {NaNValue(), -1.0, -1.0};
    Expect(!RigidCompoundBvhQuery(nodesA, root, badMinimum, queryMaximum, queryOutput, 3u, &count),
           "NaN query rejected");
    double inverted[3] = {8.0, -1.0, -1.0};
    Expect(!RigidCompoundBvhQuery(nodesA, root, inverted, queryMaximum, queryOutput, 3u, &count),
           "inverted query rejected");

    Expect(!RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 2u, &count),
           "insufficient query capacity rejected");
    Expect(count == 0x12345678u, "capacity failure leaves outCount unchanged");
    count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "exact query capacity accepted");
    Expect(count == 3u && Ascending(queryOutput, 3u), "all three leaves returned ascending");
}

static void TestTouchExclusion(void)
{
    double bounds[18];
    double minimum[3] = {0.0, 0.0, 0.0};
    double maximum[3] = {1.0, 1.0, 1.0};
    StoreBoundEntry(bounds, minimum, maximum);
    minimum[0] = 1.0;
    maximum[0] = 2.0;
    StoreBoundEntry(bounds + 6u, minimum, maximum);
    // Нулевая толщина по x: плоскость в x = 5.
    minimum[0] = 5.0;
    maximum[0] = 5.0;
    StoreBoundEntry(bounds + 12u, minimum, maximum);

    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(bounds, 6u * sizeof(double), 3u, nodesA, 5u, workspaceA, &root),
           "touch fixture built");

    double queryMinimum[3] = {0.5, 0.5, 0.5};
    double queryMaximum[3] = {1.0, 1.0, 1.0};
    uint32_t count = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "face touch query");
    Expect(count == 1u && queryOutput[0] == 0u, "face touch excludes the neighbour");

    queryMaximum[0] = 1.5;
    count = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "overlap query");
    Expect(count == 2u && queryOutput[0] == 0u && queryOutput[1] == 1u, "overlap hits both boxes");

    // Плоскость x = 5 не задевается ни слева, ни справа при касании.
    queryMinimum[0] = 4.9;
    queryMaximum[0] = 5.0;
    count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "degenerate left touch query");
    Expect(count == 0u, "degenerate slab left touch is excluded");
    queryMinimum[0] = 5.0;
    queryMaximum[0] = 5.5;
    count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "degenerate right touch query");
    Expect(count == 0u, "degenerate slab right touch is excluded");
    queryMinimum[0] = 4.9;
    queryMaximum[0] = 5.1;
    count = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "degenerate crossing query");
    Expect(count == 1u && queryOutput[0] == 2u, "degenerate slab is hit when crossed");

    queryMinimum[0] = 10.0;
    queryMaximum[0] = 11.0;
    count = 99u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 3u, &count),
           "separated query");
    Expect(count == 0u, "separated box is not hit");
}

static void TestHugeAndNonfinite(void)
{
    double bounds[12];
    double minimum[3];
    double maximum[3];
    minimum[0] = -DBL_MAX;
    minimum[1] = -DBL_MAX;
    minimum[2] = -DBL_MAX;
    maximum[0] = DBL_MAX;
    maximum[1] = DBL_MAX;
    maximum[2] = DBL_MAX;
    StoreBoundEntry(bounds, minimum, maximum);
    minimum[0] = DBL_MAX * 0.5;
    minimum[1] = 0.0;
    minimum[2] = 0.0;
    maximum[0] = DBL_MAX;
    maximum[1] = 0.0;
    maximum[2] = 0.0;
    StoreBoundEntry(bounds + 6u, minimum, maximum);

    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(bounds, 6u * sizeof(double), 2u, nodesA, 3u, workspaceA, &root),
           "huge finite build");
    double queryMinimum[3] = {-1.0, -1.0, -1.0};
    double queryMaximum[3] = {1.0, 1.0, 1.0};
    uint32_t count = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, queryMinimum, queryMaximum, queryOutput, 2u, &count),
           "huge finite query");
    Expect(count == 1u && queryOutput[0] == 0u, "huge query hits only the full-range box");

    double broken[12];
    for (uint32_t index = 0u; index < 12u; ++index)
    {
        broken[index] = bounds[index];
    }
    broken[0] = NaNValue();
    Expect(!RigidCompoundBvhBuild(broken, 6u * sizeof(double), 2u, nodesA, 3u, workspaceA, &root),
           "NaN bound rejected");
    broken[0] = -InfValue();
    Expect(!RigidCompoundBvhBuild(broken, 6u * sizeof(double), 2u, nodesA, 3u, workspaceA, &root),
           "infinite bound rejected");
}

static void TestStridedInput(void)
{
    uint32_t count = 37u;
    GenerateRandomBoxes(count, compactBounds, 0x51ED2701u);
    for (uint32_t index = 0u; index < count; ++index)
    {
        for (int32_t component = 0; component < 6; ++component)
        {
            double *destination = &stridedBounds[index].minimum[0];
            destination[component] = compactBounds[6u * index + (uint32_t)component];
        }
    }

    uint32_t compactRoot = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesA,
                                 2u * count - 1u, workspaceA, &compactRoot),
           "compact build");
    uint32_t stridedRoot = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(stridedBounds, sizeof(BvhStridedBox), count, nodesB,
                                 2u * count - 1u, workspaceB, &stridedRoot),
           "strided build");
    Expect(compactRoot == stridedRoot, "strided build shares the deterministic root");
    for (uint32_t index = 0u; index < 2u * count - 1u; ++index)
    {
        Expect(SameNode(&nodesA[index], &nodesB[index]), "strided build repeats every node");
    }

    randomState = 0x000055AAu;
    for (uint32_t trial = 0u; trial < 20u; ++trial)
    {
        double minimum[3];
        double maximum[3];
        RandomQuery(minimum, maximum);
        uint32_t compactCount = 0u;
        uint32_t stridedCount = 0u;
        Expect(RigidCompoundBvhQuery(nodesA, compactRoot, minimum, maximum, queryOutput, count,
                                     &compactCount),
               "compact strided query");
        Expect(RigidCompoundBvhQuery(nodesB, stridedRoot, minimum, maximum, bruteOutput, count,
                                     &stridedCount),
               "strided query");
        Expect(compactCount == stridedCount, "strided query keeps the hit count");
        Expect(SameIndices(queryOutput, bruteOutput, compactCount),
               "strided query keeps the hit indices");
    }
}

static void TestTreeDeterminism(void)
{
    uint32_t count = 512u;
    GenerateRandomBoxes(count, compactBounds, 0x0BADF00Du);
    uint32_t firstRoot = UINT32_MAX;
    uint32_t secondRoot = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesA,
                                 2u * count - 1u, workspaceA, &firstRoot),
           "first deterministic build");
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesB,
                                 2u * count - 1u, workspaceB, &secondRoot),
           "second deterministic build");
    Expect(firstRoot == secondRoot, "deterministic build keeps the root");
    for (uint32_t index = 0u; index < 2u * count - 1u; ++index)
    {
        Expect(SameNode(&nodesA[index], &nodesB[index]), "deterministic build repeats every node");
    }
}

static void TestRandomAgainstBruteForce(void)
{
    uint32_t count = BVH_TEST_MAX;
    GenerateRandomBoxes(count, compactBounds, 0x51ED2701u);
    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesA,
                                 2u * count - 1u, workspaceA, &root),
           "random 4096 build");

    randomState = 0x000C0FFEu;
    for (uint32_t trial = 0u; trial < 200u; ++trial)
    {
        double minimum[3];
        double maximum[3];
        RandomQuery(minimum, maximum);
        uint32_t expectedCount = BruteForce(compactBounds, count, minimum, maximum, bruteOutput);
        uint32_t actualCount = 0u;
        Expect(RigidCompoundBvhQuery(nodesA, root, minimum, maximum, queryOutput, count,
                                     &actualCount),
               "random query");
        Expect(actualCount == expectedCount, "random query count matches brute force");
        Expect(SameIndices(queryOutput, bruteOutput, actualCount),
               "random query indices match brute force");
        Expect(Ascending(queryOutput, actualCount), "random query returns ascending indices");
    }

    double allMinimum[3] = {-1000.0, -1000.0, -1000.0};
    double allMaximum[3] = {1000.0, 1000.0, 1000.0};
    uint32_t allCount = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, allMinimum, allMaximum, queryOutput, count,
                                 &allCount),
           "full coverage query");
    Expect(allCount == count, "full coverage returns every child");
    for (uint32_t index = 0u; index < count; ++index)
    {
        Expect(queryOutput[index] == index, "full coverage restores original ascending order");
    }
}

static void TestCapacityAndCanaries(void)
{
    uint32_t count = 5u;
    GenerateRandomBoxes(count, compactBounds, 0x00007E57u);
    double allMinimum[3] = {-1000.0, -1000.0, -1000.0};
    double allMaximum[3] = {1000.0, 1000.0, 1000.0};
    uint32_t hits = BruteForce(compactBounds, count, allMinimum, allMaximum, bruteOutput);
    Expect(hits == count, "canary fixture covers every child");

    uint32_t usable = 2u * count - 1u;
    nodesA[usable].first = 0xAAAAAAAAu;
    nodesA[usable].second = 0xBBBBBBBBu;
    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesA, usable,
                                 workspaceA, &root),
           "tight node capacity build");
    Expect(nodesA[usable].first == 0xAAAAAAAAu && nodesA[usable].second == 0xBBBBBBBBu,
           "build keeps the node canary past the last used node");

    workspaceA[count] = 0xCCCCCCCCu;
    uint32_t secondRoot = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(compactBounds, 6u * sizeof(double), count, nodesB, usable,
                                 workspaceA, &secondRoot),
           "workspace canary build");
    Expect(workspaceA[count] == 0xCCCCCCCCu, "build keeps the workspace canary past count");
    Expect(secondRoot == root, "canary rebuild keeps the deterministic root");

    uint32_t reported = 0x12345678u;
    Expect(!RigidCompoundBvhQuery(nodesA, root, allMinimum, allMaximum, queryOutput, hits - 1u,
                                  &reported),
           "one short query capacity rejected");
    Expect(reported == 0x12345678u, "short capacity leaves outCount unchanged");
    Expect(RigidCompoundBvhQuery(nodesA, root, allMinimum, allMaximum, queryOutput, hits,
                                 &reported),
           "exact query capacity accepted");
    Expect(reported == hits, "exact capacity reports every hit");
    Expect(SameIndices(queryOutput, bruteOutput, hits), "exact capacity indices match brute force");
}

// Явная проверка исходного порядка: коробки лежат по убыванию x, поэтому
// обход дерева не совпадает с возрастанием индексов, а Query обязан его
// восстановить.
static void TestOriginalIndexOrdering(void)
{
    double bounds[6 * 8u];
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        double minimum[3] = {(double)(7u - index) * 2.0, 0.0, 0.0};
        double maximum[3] = {minimum[0] + 1.0, 1.0, 1.0};
        StoreBoundEntry(bounds + 6u * index, minimum, maximum);
    }
    uint32_t root = UINT32_MAX;
    Expect(RigidCompoundBvhBuild(bounds, 6u * sizeof(double), 8u, nodesA, 15u, workspaceA, &root),
           "ordering fixture built");
    double minimum[3] = {-1.0, -1.0, -1.0};
    double maximum[3] = {100.0, 2.0, 2.0};
    uint32_t count = 0u;
    Expect(RigidCompoundBvhQuery(nodesA, root, minimum, maximum, queryOutput, 8u, &count),
           "ordering query");
    Expect(count == 8u, "ordering query returns every box");
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        Expect(queryOutput[index] == index, "ordering query restores original indices");
    }
}

LAIUE_TEST_ENTRY(CompoundBvhTestEntryPoint)
{
    TestSingleBox();
    TestRejectBuild();
    TestRejectQuery();
    TestTouchExclusion();
    TestHugeAndNonfinite();
    TestStridedInput();
    TestTreeDeterminism();
    TestRandomAgainstBruteForce();
    TestCapacityAndCanaries();
    TestOriginalIndexOrdering();
    LaiueTestRuntimeWrite("compound-bvh: ok\n");
    LAIUE_TEST_SUCCESS();
}
