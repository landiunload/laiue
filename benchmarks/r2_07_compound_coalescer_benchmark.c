// ROUND2 07-compound: dedicated coalescer harness.
//
// Измеряет только VoxelRigidCompoundMergeBoxes (build-time coalescer
// compound-форм) на наборе сценариев: dense-merges (линия/решётка),
// no-merge (разнесённые коробки), mixed и single. Функция берётся из
// laiue_physics (shared), поэтому один и тот же exe можно запускать с
// baseline- и candidate-версией laiue_physics.dll.
//
// Каждая строка вывода машинно-разбираемая:
//   r2c <name> count=<n> result=<r> hash=<hex> ns_per_op=<f> ops=<o>
// hash — FNV-1a 64 по байтам ровно result коробок и по самому resultCount,
// поэтому baseline и candidate обязаны печатать одинаковые hash/result.
//
// Harness без CRT: static buffers, печать через WriteFile, выход ExitProcess.

#include "physics/compound_shape.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define R2C_MAX 4096u
#define R2C_TARGET_SECONDS 0.5

static VoxelRigidCompoundBox r2cIn[R2C_MAX];
static VoxelRigidCompoundBox r2cOut[R2C_MAX];

static volatile uint64_t r2cSink;
static uint32_t r2cBatchReps = 1u;
static uint32_t r2cCount = 1u;

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

static void StoreBox(uint32_t index, double cx, double cy, double cz, double half)
{
    r2cIn[index].center[0] = cx;
    r2cIn[index].center[1] = cy;
    r2cIn[index].center[2] = cz;
    r2cIn[index].halfExtent[0] = half;
    r2cIn[index].halfExtent[1] = half;
    r2cIn[index].halfExtent[2] = half;
}

// kind 0: линия вплотную стоящих кубов — сливается в одну коробку.
// kind 1: разнесённые кубы — слияний нет.
// kind 2: плотная решётка одинаковых кубов — сливается в одну коробку.
// kind 3: dense line + далеко отнесённые separated (mixed).
// kind 4: separated + dense line (mixed, обратный порядок).
static void Fill(uint32_t count, uint32_t kind)
{
    if (kind == 0u)
    {
        for (uint32_t i = 0u; i < count; ++i)
        {
            StoreBox(i, (double)i, 0.0, 0.0, 0.5);
        }
        return;
    }
    if (kind == 1u)
    {
        for (uint32_t i = 0u; i < count; ++i)
        {
            StoreBox(i, (double)i * 3.0, 0.0, 0.0, 0.5);
        }
        return;
    }
    if (kind == 2u)
    {
        uint32_t side = 1u;
        while (side * side * side < count)
        {
            ++side;
        }
        for (uint32_t i = 0u; i < count; ++i)
        {
            double x = (double)(i % side);
            double y = (double)((i / side) % side);
            double z = (double)(i / (side * side));
            StoreBox(i, x, y, z, 0.5);
        }
        return;
    }
    // mixed
    uint32_t dense = count / 2u;
    if (dense == 0u)
    {
        dense = 1u;
    }
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (i < dense)
        {
            if (kind == 3u)
            {
                StoreBox(i, (double)i, 0.0, 0.0, 0.5);
            }
            else
            {
                StoreBox(i, (double)i * 3.0, 100.0, 0.0, 0.5);
            }
        }
        else
        {
            if (kind == 3u)
            {
                StoreBox(i, (double)(i - dense) * 3.0, 100.0, 0.0, 0.5);
            }
            else
            {
                StoreBox(i, (double)(i - dense), 0.0, 0.0, 0.5);
            }
        }
    }
}

// === Хеш результата ===

static uint64_t HashResult(uint32_t resultCount)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *bytes = (const unsigned char *)r2cOut;
    uint64_t byteCount = (uint64_t)resultCount * (uint64_t)sizeof(VoxelRigidCompoundBox);
    for (uint64_t index = 0u; index < byteCount; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        hash ^= (uint64_t)((resultCount >> (8u * index)) & 0xffu);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

// === Измерение ===

static void MergeOnce(void)
{
    uint32_t resultCount = 0u;
    if (!VoxelRigidCompoundMergeBoxes(r2cIn, r2cCount, r2cOut, r2cCount, &resultCount))
    {
        r2cSink ^= UINT64_C(0xC0FFEE);
        return;
    }
    r2cSink += resultCount;
    r2cSink ^= (uint64_t)resultCount;
}

static void MergeBatch(void)
{
    for (uint32_t rep = 0u; rep < r2cBatchReps; ++rep)
    {
        MergeOnce();
    }
}

static void Scenario(const char *name, uint32_t count, uint32_t kind)
{
    r2cCount = count;
    Fill(count, kind);

    // Корректность: один прогон вне таймера даёт resultCount и байтовый хеш.
    uint32_t resultCount = 0u;
    bool accepted = VoxelRigidCompoundMergeBoxes(r2cIn, count, r2cOut, count, &resultCount);
    uint64_t hash = accepted ? HashResult(resultCount) : 0u;

    double nsPerOp = 0.0;
    uint64_t ops = 0u;
    if (count != 0u)
    {
        r2cBatchReps = 1u;
        for (;;)
        {
            double measured = 0.0;
            for (uint32_t warm = 0u; warm < 3u; ++warm)
            {
                double begin = PlatformMonotonicSeconds();
                MergeBatch();
                measured += PlatformMonotonicSeconds() - begin;
            }
            measured /= 3.0;
            if (measured >= 0.0005 || r2cBatchReps >= (1u << 22))
            {
                break;
            }
            r2cBatchReps *= 4u;
        }

        double start = PlatformMonotonicSeconds();
        double elapsed = 0.0;
        do
        {
            MergeBatch();
            ops += (uint64_t)r2cBatchReps;
            elapsed = PlatformMonotonicSeconds() - start;
        } while (elapsed < R2C_TARGET_SECONDS);
        if (ops != 0u && elapsed > 0.0)
        {
            nsPerOp = elapsed * 1000000000.0 / (double)ops;
        }
    }

    WriteText("r2c ");
    WriteText(name);
    WriteText(" count=");
    WriteUnsigned(count);
    WriteText(" result=");
    WriteUnsigned(resultCount);
    WriteText(" accepted=");
    WriteUnsigned(accepted ? 1u : 0u);
    WriteText(" hash=");
    WriteHex(hash);
    WriteText(" ns_per_op=");
    WriteNanoseconds(nsPerOp);
    WriteText(" ops=");
    WriteUnsigned(ops);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(R2CompoundCoalescerEntryPoint)
{
    WriteText("laiue r2 07-compound coalescer benchmark\n");

    Scenario("line.n256", 256u, 0u);
    Scenario("line.n1024", 1024u, 0u);
    Scenario("line.n4096", 4096u, 0u);
    Scenario("grid.n64", 64u, 2u);
    Scenario("grid.n512", 512u, 2u);
    Scenario("separated.n256", 256u, 1u);
    Scenario("separated.n1024", 1024u, 1u);
    Scenario("separated.n4096", 4096u, 1u);
    Scenario("mixed.densefirst.n4096", 4096u, 3u);
    Scenario("mixed.sepfirst.n4096", 4096u, 4u);
    Scenario("single.n1", 1u, 0u);
    Scenario("empty.n0", 0u, 1u);

    WriteText("r2c done sink=");
    WriteUnsigned(r2cSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
