// ROUND2 06-bvh: A/B стенд внутреннего child AABB BVH составного тела.
//
// Не входит в CTest. Экспортирует ровно две внутренние функции из
// src/physics/compound_bvh.c, поэтому source компилируется прямо в этот
// исполняемый файл (как benchmarks/compound_benchmark.c и
// tests/compound_bvh_test.c). Baseline и candidate — это ДВА разных
// исполняемых файла из одного и того же harness, потому что производственный
// код private/static и не подменяется DLL.
//
// Печатает по строке на сценарий:
//   r2bvh <name> ns_per_op=<ns> ops=<ops> root_or_total=<n> checksum=<hex>
// checksum считается ОДИН раз вне тайминга: по битам всех узлов для build и по
// отсортированным индексам всех query. A/B обязан получить одинаковые
// checksum/hit counts, иначе сравнивается разная работа.
//
// Harness без CRT на Windows: только WriteFile и монотонные секунды.

#include "physics/compound_bvh.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R2BVH_MAX_CHILDREN 4096u
#define R2BVH_MAX_NODES (2u * R2BVH_MAX_CHILDREN)
#define R2BVH_QUERY_COUNT 64u
#define R2BVH_TARGET_SECONDS 0.12

enum
{
    R2BVH_KIND_RANDOM = 0,
    R2BVH_KIND_LINE = 1,
    R2BVH_KIND_LATTICE = 2,
    R2BVH_KIND_EQUAL = 3,
    R2BVH_KIND_DEGENERATE = 4
};

static volatile uint64_t benchSink;

static RigidCompoundBvhNode benchNodes[R2BVH_MAX_NODES];
static RigidCompoundBvhNode fixtureNodes[R2BVH_MAX_NODES];
static uint32_t benchWorkspace[R2BVH_MAX_CHILDREN];
static double benchBounds[R2BVH_MAX_CHILDREN * 6u];
static double benchQueryBounds[R2BVH_QUERY_COUNT * 6u];
static uint32_t benchHits[R2BVH_MAX_CHILDREN];

static uint32_t benchRng = 0x51ed2701u;
static uint32_t batchReps = 1u;
static uint32_t opsPerRep = 1u;
static uint32_t buildCount = 1u;
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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[15u - index] = digits[(value >> (4u * index)) & 0xfu];
    }
    text[16] = '\0';
    WriteText(text);
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

static void FillBounds(uint32_t count, uint32_t kind, uint32_t seed)
{
    benchRng = seed;
    for (uint32_t index = 0u; index < count; ++index)
    {
        double minimum[3];
        double maximum[3];
        if (kind == R2BVH_KIND_RANDOM)
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
        else if (kind == R2BVH_KIND_LINE)
        {
            minimum[0] = (double)index;
            maximum[0] = minimum[0] + 1.0;
            minimum[1] = 0.0;
            maximum[1] = 1.0;
            minimum[2] = 0.0;
            maximum[2] = 1.0;
        }
        else if (kind == R2BVH_KIND_LATTICE)
        {
            uint32_t side = 1u;
            while ((uint64_t)side * side * side < (uint64_t)count)
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
        else if (kind == R2BVH_KIND_EQUAL)
        {
            // Все центры ровно равны: сравнитель уходит в tie-break по индексу,
            // а quickselect работает на вырожденном ключе.
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = -1.0;
                maximum[axis] = 1.0;
            }
        }
        else
        {
            // Вырожденные AABB: нулевая толщина по одной оси, случайный центр.
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                double center = RandomRange(-400.0, 400.0);
                double half = RandomRange(0.5, 30.0);
                minimum[axis] = center - half;
                maximum[axis] = center + half;
            }
            int32_t axis = (int32_t)(NextRandom() % 3u);
            minimum[axis] = maximum[axis];
        }
        StoreBounds(benchBounds + 6u * (size_t)index, minimum, maximum);
    }
}

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

static void FillBroadQuery(uint32_t count, uint32_t seed)
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

// === Контрольные суммы вне тайминга ===

#define R2BVH_FNV_OFFSET UINT64_C(1469598103934665603)
#define R2BVH_FNV_PRIME UINT64_C(1099511628211)

static uint64_t FnvBytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= R2BVH_FNV_PRIME;
    }
    return hash;
}

// === Измерение ===

static void Report(const char *name, double seconds, uint64_t ops, uint64_t result,
                   uint64_t checksum)
{
    WriteText("r2bvh ");
    WriteText(name);
    WriteText(" ns_per_op=");
    WriteNanoseconds(ops == 0u || !(seconds > 0.0) ? 0.0 : seconds * 1000000000.0 / (double)ops);
    WriteText(" ops=");
    WriteUnsigned(ops);
    WriteText(" result=");
    WriteUnsigned(result);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText("\n");
}

static void Measure(const char *name, void (*batch)(void), double targetSeconds, uint64_t result,
                    uint64_t checksum)
{
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
    Report(name, elapsed, ops, result, checksum);
}

// === Build сценарии ===

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
    uint32_t root = UINT32_MAX;
    if (!RigidCompoundBvhBuild(benchBounds, 6u * sizeof(double), count, benchNodes, 2u * count - 1u,
                               benchWorkspace, &root))
    {
        WriteText("r2bvh build fixture failed\n");
        LaiueTestRuntimeExit(1);
    }
    uint64_t checksum = FnvBytes(R2BVH_FNV_OFFSET, benchNodes,
                                 (size_t)(2u * count - 1u) * sizeof(RigidCompoundBvhNode));
    Measure(name, BuildBatch, R2BVH_TARGET_SECONDS, (uint64_t)root, checksum);
}

// === Query сценарии ===

static void QueryOnce(uint32_t slot)
{
    uint32_t count = 0u;
    if (!RigidCompoundBvhQuery(fixtureNodes, queryRoot, benchQueryBounds + 6u * (size_t)slot,
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
    if (!RigidCompoundBvhBuild(benchBounds, 6u * sizeof(double), count, fixtureNodes,
                               2u * count - 1u, benchWorkspace, &queryRoot))
    {
        WriteText("r2bvh query fixture failed\n");
        LaiueTestRuntimeExit(1);
    }
    queryCount = R2BVH_QUERY_COUNT;
    if (narrow)
    {
        FillNarrowQuery(queryCount, 0x000c0ffeeu);
    }
    else
    {
        FillBroadQuery(queryCount, 0x000c0ffeeu);
    }

    uint64_t checksum = R2BVH_FNV_OFFSET;
    uint64_t total = 0u;
    for (uint32_t slot = 0u; slot < queryCount; ++slot)
    {
        uint32_t count2 = 0u;
        if (!RigidCompoundBvhQuery(fixtureNodes, queryRoot, benchQueryBounds + 6u * (size_t)slot,
                                   benchQueryBounds + 6u * (size_t)slot + 3u, benchHits, buildCount,
                                   &count2))
        {
            WriteText("r2bvh query validation failed\n");
            LaiueTestRuntimeExit(1);
        }
        total += count2;
        if (count2 > 0u)
        {
            checksum = FnvBytes(checksum, benchHits, (size_t)count2 * sizeof(uint32_t));
        }
    }

    opsPerRep = queryCount;
    Measure(name, QueryBatch, R2BVH_TARGET_SECONDS, total, checksum);
}

LAIUE_TEST_ENTRY(R2_06BvhBenchmarkEntryPoint)
{
    WriteText("r2bvh benchmark start\n");

    ScenarioBuild("build.random.n1", 1u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n2", 2u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n3", 3u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n8", 8u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n64", 64u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n512", 512u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n2048", 2048u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.random.n4096", 4096u, R2BVH_KIND_RANDOM);
    ScenarioBuild("build.line.n2048", 2048u, R2BVH_KIND_LINE);
    ScenarioBuild("build.lattice.n2048", 2048u, R2BVH_KIND_LATTICE);
    ScenarioBuild("build.equal.n2048", 2048u, R2BVH_KIND_EQUAL);
    ScenarioBuild("build.degenerate.n2048", 2048u, R2BVH_KIND_DEGENERATE);

    ScenarioQuery("query.narrow.n64", 64u, R2BVH_KIND_RANDOM, true);
    ScenarioQuery("query.narrow.n2048", 2048u, R2BVH_KIND_RANDOM, true);
    ScenarioQuery("query.broad.n2048", 2048u, R2BVH_KIND_RANDOM, false);
    ScenarioQuery("query.broad.line.n2048", 2048u, R2BVH_KIND_LINE, false);
    ScenarioQuery("query.broad.lattice.n2048", 2048u, R2BVH_KIND_LATTICE, false);
    ScenarioQuery("query.broad.equal.n2048", 2048u, R2BVH_KIND_EQUAL, false);
    ScenarioQuery("query.narrow.degenerate.n2048", 2048u, R2BVH_KIND_DEGENERATE, true);
    ScenarioQuery("query.tiny.n3", 3u, R2BVH_KIND_RANDOM, true);

    WriteText("r2bvh benchmark done sink=");
    WriteUnsigned(benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
