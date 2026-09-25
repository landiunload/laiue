// Ручной benchmark внутреннего child AABB BVH и build-time coalescer-а
// составных форм. Не входит в CTest: его запускают осознанно, чтобы сравнить
// baseline и candidate на одной машине.
//
// BVH не экспортируется из laiue_physics, поэтому root компилирует
// src/physics/compound_bvh.c прямо в этот исполняемый файл — ровно так же,
// как это делает tests/compound_bvh_test.c. Coalescer
// VoxelRigidCompoundMergeBoxes берётся из laiue_physics.
//
// Harness без CRT: печатает строки в консоль и завершается. Каждый сценарий
// калибрует число повторов так, чтобы один timed batch занимал заметное время,
// затем крутит batches до target-секунды. Итог — наносекунды на операцию,
// поэтому разница между baseline и candidate видна в сырых числах.

#include "physics/compound_bvh.h"
#include "physics/compound_shape.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BENCH_MAX_CHILDREN 2048u
#define BENCH_MAX_NODES (2u * BENCH_MAX_CHILDREN)
#define BENCH_MAX_MERGE 4096u
#define BENCH_QUERY_COUNT 64u
#define BENCH_TARGET_SECONDS 0.25

static volatile uint64_t benchSink;

static RigidCompoundBvhNode benchNodes[BENCH_MAX_NODES];
static uint32_t benchWorkspace[BENCH_MAX_CHILDREN];
static double benchBounds[BENCH_MAX_CHILDREN * 6u];
static double benchQueryBounds[BENCH_QUERY_COUNT * 6u];
static uint32_t benchHits[BENCH_MAX_CHILDREN];
static VoxelRigidCompoundBox benchMergeIn[BENCH_MAX_MERGE];
static VoxelRigidCompoundBox benchMergeOut[BENCH_MAX_MERGE];

static uint32_t benchRng = 0x51ed2701u;
static uint32_t batchReps = 1u;
static uint32_t opsPerRep = 1u;
static uint32_t buildCount = 1u;
static uint32_t mergeCount = 1u;
static uint32_t queryCount = 1u;
static uint32_t queryRoot = UINT32_MAX;

// === Печать без CRT ===

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

// Наносекунды с тремя знаками: сравнение идёт по единицам наносекунд.
static void WriteNanoseconds(double value)
{
    if (!(value >= 0.0))
    {
        value = 0.0;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction < 100u)
    {
        WriteText("0");
    }
    if (fraction < 10u)
    {
        WriteText("0");
    }
    WriteUnsigned(fraction > 999u ? 999u : fraction);
}

// === Данные ===

static uint32_t NextRandom(void)
{
    benchRng = benchRng * 1664525u + 1013904223u;
    return benchRng;
}

static double RandomUnit(void)
{
    return (double)(NextRandom() >> 8) / 16777216.0;
}

static double RandomRange(double low, double high)
{
    return low + (high - low) * RandomUnit();
}

static void StoreBounds(double *entry, const double minimum[3], const double maximum[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        entry[axis] = minimum[axis];
        entry[3 + axis] = maximum[axis];
    }
}

// kind 0: случайные коробки (разные размеры, частые ничьи маловероятны).
// kind 1: линия одинаковых вплотную стоящих кубов (ничьи по центрам часты).
// kind 2: решётка одинаковых кубов.
static void FillBounds(uint32_t count, uint32_t kind, uint32_t seed)
{
    benchRng = seed;
    for (uint32_t index = 0u; index < count; ++index)
    {
        double minimum[3];
        double maximum[3];
        if (kind == 0u)
        {
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                double center = RandomRange(-400.0, 400.0);
                double half = RandomRange(0.0, 30.0);
                minimum[axis] = center - half;
                maximum[axis] = center + half;
            }
            if ((NextRandom() & 15u) == 0u)
            {
                int32_t axis = (int32_t)(NextRandom() % 3u);
                minimum[axis] = maximum[axis];
            }
        }
        else if (kind == 1u)
        {
            minimum[0] = (double)index;
            maximum[0] = minimum[0] + 1.0;
            minimum[1] = 0.0;
            maximum[1] = 1.0;
            minimum[2] = 0.0;
            maximum[2] = 1.0;
        }
        else
        {
            uint32_t side = 1u;
            while (side * side * side < count)
            {
                ++side;
            }
            double x = (double)(index % side);
            double y = (double)((index / side) % side);
            double z = (double)(index / (side * side));
            minimum[0] = x;
            maximum[0] = x + 1.0;
            minimum[1] = y;
            maximum[1] = y + 1.0;
            minimum[2] = z;
            maximum[2] = z + 1.0;
        }
        StoreBounds(benchBounds + 6u * (size_t)index, minimum, maximum);
    }
}

static void FillQuery(uint32_t count, uint32_t seed)
{
    benchRng = seed;
    for (uint32_t index = 0u; index < count; ++index)
    {
        double minimum[3];
        double maximum[3];
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
        StoreBounds(benchQueryBounds + 6u * (size_t)index, minimum, maximum);
    }
}

// Запрос на маленький AABB: попаданий обычно одно-два.
static void FillNarrowQuery(uint32_t count, uint32_t seed)
{
    benchRng = seed;
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t target = NextRandom() % buildCount;
        const double *entry = benchBounds + 6u * (size_t)target;
        double center[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            center[axis] = entry[axis] + (entry[3 + axis] - entry[axis]) * 0.5;
        }
        double minimum[3];
        double maximum[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = center[axis] - 0.05;
            maximum[axis] = center[axis] + 0.05;
        }
        StoreBounds(benchQueryBounds + 6u * (size_t)index, minimum, maximum);
    }
}

// === Измерение ===

static void Report(const char *name, double seconds, uint64_t ops)
{
    WriteText("compound ");
    WriteText(name);
    WriteText(" ns_per_op=");
    WriteNanoseconds(ops == 0u || !(seconds > 0.0) ? 0.0 : seconds * 1000000000.0 / (double)ops);
    WriteText(" ops=");
    WriteUnsigned(ops);
    WriteText("\n");
}

static void Measure(const char *name, void (*batch)(void), double targetSeconds)
{
    // Калибровка: один batch обязан занимать заметное время, чтобы вызов
    // таймера на итерацию не искажал измерение.
    batchReps = 1u;
    for (;;)
    {
        double measured = 0.0;
        for (uint32_t warm = 0u; warm < 3u; ++warm)
        {
            double begin = PlatformMonotonicSeconds();
            batch();
            measured += PlatformMonotonicSeconds() - begin;
        }
        measured /= 3.0;
        if (measured >= 0.0005 || batchReps >= (1u << 24))
        {
            break;
        }
        batchReps *= 4u;
    }

    uint64_t ops = 0u;
    double start = PlatformMonotonicSeconds();
    double elapsed = 0.0;
    do
    {
        batch();
        ops += (uint64_t)batchReps * (uint64_t)opsPerRep;
        elapsed = PlatformMonotonicSeconds() - start;
    } while (elapsed < targetSeconds);
    Report(name, elapsed, ops);
}

// === Сценарии BVH Build ===

static void BuildOnce(void)
{
    uint32_t root = UINT32_MAX;
    if (!RigidCompoundBvhBuild(benchBounds, 6u * sizeof(double), buildCount, benchNodes,
                               2u * buildCount - 1u, benchWorkspace, &root))
    {
        benchSink ^= UINT64_C(0xDEADBEEF);
        return;
    }
    benchSink ^= root;
    benchSink ^= benchNodes[root].first;
    benchSink ^= benchNodes[0].second;
}

static void BuildBatch(void)
{
    for (uint32_t rep = 0u; rep < batchReps; ++rep)
    {
        BuildOnce();
    }
}

static void ScenarioBuild(const char *name, uint32_t count, uint32_t kind)
{
    buildCount = count;
    FillBounds(count, kind, 0x51ed2701u ^ (count * 2654435761u));
    opsPerRep = 1u;
    Measure(name, BuildBatch, BENCH_TARGET_SECONDS);
}

// === Сценарии BVH Query ===

static void QueryOnce(uint32_t slot)
{
    uint32_t count = 0u;
    if (!RigidCompoundBvhQuery(benchNodes, queryRoot, benchQueryBounds + 6u * (size_t)slot,
                               benchQueryBounds + 6u * (size_t)slot + 3u, benchHits, buildCount,
                               &count))
    {
        benchSink ^= UINT64_C(0xBADF00D);
        return;
    }
    benchSink += count;
    if (count > 0u)
    {
        benchSink ^= benchHits[count - 1u];
    }
}

static void QueryBatch(void)
{
    for (uint32_t rep = 0u; rep < batchReps; ++rep)
    {
        for (uint32_t op = 0u; op < queryCount; ++op)
        {
            QueryOnce(op);
        }
    }
}

static void ScenarioQuery(const char *name, uint32_t count, uint32_t kind, bool narrow)
{
    buildCount = count;
    FillBounds(count, kind, 0x0badf00du ^ (count * 2654435761u));
    queryRoot = UINT32_MAX;
    if (!RigidCompoundBvhBuild(benchBounds, 6u * sizeof(double), count, benchNodes, 2u * count - 1u,
                               benchWorkspace, &queryRoot))
    {
        WriteText("compound query fixture build failed\n");
        LaiueTestRuntimeExit(1);
    }
    queryCount = BENCH_QUERY_COUNT;
    if (narrow)
    {
        FillNarrowQuery(queryCount, 0x000c0ffeeu);
    }
    else
    {
        FillQuery(queryCount, 0x000c0ffeeu);
    }
    opsPerRep = queryCount;
    Measure(name, QueryBatch, BENCH_TARGET_SECONDS);
}

// === Сценарии coalescer-а ===

static void FillMerge(uint32_t count, uint32_t kind)
{
    if (kind == 0u)
    {
        // Линия вплотную стоящих кубов: каждое слияние сдвигает хвост.
        for (uint32_t index = 0u; index < count; ++index)
        {
            benchMergeIn[index].center[0] = (double)index;
            benchMergeIn[index].center[1] = 0.0;
            benchMergeIn[index].center[2] = 0.0;
            benchMergeIn[index].halfExtent[0] = 0.5;
            benchMergeIn[index].halfExtent[1] = 0.5;
            benchMergeIn[index].halfExtent[2] = 0.5;
        }
    }
    else
    {
        // Разнесённые кубы: слияний нет, доминирует превалидация.
        for (uint32_t index = 0u; index < count; ++index)
        {
            benchMergeIn[index].center[0] = (double)index * 3.0;
            benchMergeIn[index].center[1] = 0.0;
            benchMergeIn[index].center[2] = 0.0;
            benchMergeIn[index].halfExtent[0] = 0.5;
            benchMergeIn[index].halfExtent[1] = 0.5;
            benchMergeIn[index].halfExtent[2] = 0.5;
        }
    }
}

static void MergeOnce(void)
{
    uint32_t resultCount = 0u;
    if (!VoxelRigidCompoundMergeBoxes(benchMergeIn, mergeCount, benchMergeOut, mergeCount,
                                      &resultCount))
    {
        benchSink ^= UINT64_C(0xC0FFEE);
        return;
    }
    benchSink += resultCount;
    benchSink ^= (uint64_t)resultCount;
    if (resultCount > 0u)
    {
        union
        {
            double scalar;
            uint64_t bits;
        } representation = {benchMergeOut[0].center[0]};
        benchSink ^= representation.bits;
    }
}

static void MergeBatch(void)
{
    for (uint32_t rep = 0u; rep < batchReps; ++rep)
    {
        MergeOnce();
    }
}

static void ScenarioMerge(const char *name, uint32_t count, uint32_t kind)
{
    mergeCount = count;
    FillMerge(count, kind);
    opsPerRep = 1u;
    Measure(name, MergeBatch, BENCH_TARGET_SECONDS);
}

LAIUE_TEST_ENTRY(CompoundBenchmarkEntryPoint)
{
    WriteText("laiue compound bvh/shape benchmark\n");

    ScenarioBuild("build.random.n8", 8u, 0u);
    ScenarioBuild("build.random.n64", 64u, 0u);
    ScenarioBuild("build.random.n512", 512u, 0u);
    ScenarioBuild("build.random.n2048", 2048u, 0u);
    ScenarioBuild("build.line.n2048", 2048u, 1u);

    ScenarioQuery("query.narrow.n64", 64u, 0u, true);
    ScenarioQuery("query.narrow.n2048", 2048u, 0u, true);
    ScenarioQuery("query.broad.n2048", 2048u, 0u, false);
    ScenarioQuery("query.broad.line.n2048", 2048u, 1u, false);

    ScenarioMerge("merge.line.n256", 256u, 0u);
    ScenarioMerge("merge.line.n1024", 1024u, 0u);
    ScenarioMerge("merge.line.n4096", 4096u, 0u);
    ScenarioMerge("merge.separated.n1024", 1024u, 1u);

    WriteText("compound benchmark done sink=");
    WriteUnsigned(benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
