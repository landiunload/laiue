// ROUND2 07-compound: property regression for the optimized coalescer.
//
// Проверяет, что решение VoxelRigidCompoundMergeBoxes (accept/reject) при
// произвольных детерминированных наборах коробок совпадает с независимо
// посчитанным эталоном: полная валидация границ (конечность центра и
// полуребра, строгая положительность, невырожденность center +- halfExtent)
// и затем строгое попарное пересечение по тем же самым c-h/c+h. Это фиксирует
// новую ветку overlap-скана без проверок конечности: она допустима только
// после превалидации и обязана давать те же границы. Сценарий включает
// большие координаты (2^51, 2^53), где c +- h теряет точность.
//
// Крупные значения берутся, чтобы округление c-h/c+h реально влияло на
// результат, а не было тождественным.

#include "physics/compound_shape.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define R2_MAX_BOXES 48u

static VoxelRigidCompoundBox r2Input[R2_MAX_BOXES];
static VoxelRigidCompoundBox r2Output[R2_MAX_BOXES];
static VoxelRigidCompoundBox r2Second[R2_MAX_BOXES];

static uint32_t r2Rng;

static uint32_t NextRandom(void)
{
    r2Rng = r2Rng * 1664525u + 1013904223u;
    return r2Rng;
}

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("r2 compound failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Finite(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return ((representation.bits >> 52) & 0x7ffu) != 0x7ffu;
}

// Эталонная валидация одной коробки (та же арифметика, что и в production).
static bool ExpectedValid(const VoxelRigidCompoundBox *box)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double low = box->center[axis] - box->halfExtent[axis];
        double high = box->center[axis] + box->halfExtent[axis];
        if (!Finite(low) || !Finite(high) || !(low < high))
        {
            return false;
        }
    }
    return true;
}

static void ExpectedBounds(const VoxelRigidCompoundBox *box, double low[3], double high[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        low[axis] = box->center[axis] - box->halfExtent[axis];
        high[axis] = box->center[axis] + box->halfExtent[axis];
    }
}

// Эталонное решение: false, если есть невалидная коробка или строгое
// перекрытие хоть одной пары.
static bool ExpectedDecision(const VoxelRigidCompoundBox *boxes, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!ExpectedValid(&boxes[index]))
        {
            return false;
        }
    }
    for (uint32_t first = 0u; first + 1u < count; ++first)
    {
        double firstLow[3];
        double firstHigh[3];
        ExpectedBounds(&boxes[first], firstLow, firstHigh);
        for (uint32_t second = first + 1u; second < count; ++second)
        {
            double secondLow[3];
            double secondHigh[3];
            ExpectedBounds(&boxes[second], secondLow, secondHigh);
            bool overlap = true;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                if (!(firstLow[axis] < secondHigh[axis]) ||
                    !(secondLow[axis] < firstHigh[axis]))
                {
                    overlap = false;
                    break;
                }
            }
            if (overlap)
            {
                return false;
            }
        }
    }
    return true;
}

static void FillCase(uint32_t count, uint32_t kind)
{
    double scale = 1.0;
    if (kind == 1u)
    {
        scale = 2251799813685248.0; // 2^51, шаг 0.5
    }
    else if (kind == 2u)
    {
        scale = 9007199254740992.0; // 2^53, шаг 2.0
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        double center[3];
        double half[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            // Разнообразные размеры и иногда совпадающие грани.
            center[axis] = (double)(int32_t)(NextRandom() % 64u) - 16.0;
            half[axis] = (double)(1u + (NextRandom() % 4u)) * 0.5;
        }
        if (kind == 1u || kind == 2u)
        {
            // Сдвигаем по X большим шагом, сохраняя малые полуребра: c-h и c+h
            // тогда теряют точность, и overlap-решение зависит от округления.
            center[0] = scale + (double)((NextRandom() % 64u) * 2u);
            half[0] = 1.0;
            half[1] = 1.0;
            half[2] = 1.0;
            center[1] = (double)(NextRandom() % 4u);
            center[2] = (double)(NextRandom() % 4u);
        }
        r2Input[index].center[0] = center[0];
        r2Input[index].center[1] = center[1];
        r2Input[index].center[2] = center[2];
        r2Input[index].halfExtent[0] = half[0];
        r2Input[index].halfExtent[1] = half[1];
        r2Input[index].halfExtent[2] = half[2];
    }
}

static void TestDecisionParity(void)
{
    for (uint32_t kind = 0u; kind < 3u; ++kind)
    {
        for (uint32_t trial = 0u; trial < 400u; ++trial)
        {
            r2Rng = 0x12345678u ^ (kind * 2654435761u) ^ (trial * 2246822519u);
            uint32_t count = 1u + (NextRandom() % R2_MAX_BOXES);
            FillCase(count, kind);
            bool expected = ExpectedDecision(r2Input, count);
            uint32_t resultCount = 0u;
            bool accepted =
                VoxelRigidCompoundMergeBoxes(r2Input, count, r2Output, count, &resultCount);
            Expect(accepted == expected, "decision parity with checked-bounds reference");
            if (accepted)
            {
                Expect(resultCount >= 1u && resultCount <= count, "accepted count is bounded");
                // Детерминизм: повтор даёт побитово тот же результат.
                uint32_t secondCount = 0u;
                Expect(VoxelRigidCompoundMergeBoxes(r2Input, count, r2Second, count,
                                                    &secondCount),
                       "repeat accepted");
                Expect(secondCount == resultCount, "repeat keeps count");
                for (uint32_t index = 0u; index < resultCount * 6u; ++index)
                {
                    const double *first = (const double *)&r2Output[0];
                    const double *second = (const double *)&r2Second[0];
                    Expect(first[index] == second[index] ||
                               (first[index] != first[index] && second[index] != second[index]),
                           "repeat is bit-identical");
                }
            }
        }
    }
}

// Две коробки, которые на 2^53 почти смыкаются: решение обязано совпасть с
// эталоном, а не «округляться» в слияние.
static void TestLargeCoordinateTouch(void)
{
    VoxelRigidCompoundBox pair[2];
    // 2^53 = 9007199254740992, шаг 2.0. halfExtent 1.0 -> интервалы ширины 2.
    pair[0].center[0] = 9007199254740992.0;
    pair[0].center[1] = 0.0;
    pair[0].center[2] = 0.0;
    pair[0].halfExtent[0] = 1.0;
    pair[0].halfExtent[1] = 0.5;
    pair[0].halfExtent[2] = 0.5;
    pair[1] = pair[0];
    pair[1].center[0] = 9007199254740994.0; // ровно следующий представимый
    uint32_t count = 0u;
    bool accepted = VoxelRigidCompoundMergeBoxes(pair, 2u, r2Output, 2u, &count);
    Expect(accepted == ExpectedDecision(pair, 2u), "large touch decision matches reference");
    if (accepted)
    {
        Expect(count == 2u, "unrepresentable large merge is skipped");
    }
}

LAIUE_TEST_ENTRY(R2CompoundOverlapTestEntryPoint)
{
    TestDecisionParity();
    TestLargeCoordinateTouch();
    LAIUE_TEST_SUCCESS();
}
