// R2 A/B-стенд арифметики бесконечных координат (агент 02-numeric).
//
// Отличие от numeric_physics_benchmark.c: тот же набор «типовых» нагрузок плюс
// неблагоприятные случаи (перенос из лимба, точное погашение, сдвиг в ноль),
// и печать КАЖДОГО прогона отдельной строкой, чтобы A/B-скрипт считал медиану,
// min/max, IQR и парные проценты, а не доверял одной цифре.
//
// Стенд линкуется с laiue_numeric (DLL), поэтому baseline и candidate
// сравниваются ОДНИМ И ТЕМ ЖЕ exe при подмене только laiue_numeric.dll.
//
// Каждая нагрузка: прогрев вне статистики (он же считает checksum), затем
// reps измеренных проходов. Checksum обязан совпасть у baseline и candidate —
// иначе сравнение недействительно.
//
// В ALL и CTest не входит: запускается руками через A/B-скрипт.

#include "numeric/infinite_coord.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define R2_NUMERIC_SCALE 4294967296.0
#define R2_NUMERIC_VALUES 256u
#define R2_NUMERIC_ROUNDS 8192u
#define R2_NUMERIC_OPS ((uint64_t)R2_NUMERIC_VALUES * (uint64_t)R2_NUMERIC_ROUNDS)

static volatile uint64_t numericSink;

// --- Вывод без CRT -------------------------------------------------------

static void WriteText(const char* text)
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
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        uint32_t shift = (15u - index) * 4u;
        uint32_t digit = (uint32_t)((value >> shift) & 0xFu);
        text[2u + index] = (char)(digit < 10u ? ('0' + digit) : ('a' + digit - 10u));
    }
    text[18] = '\0';
    WriteText(text);
}

// Наносекунды на операцию с тремя знаками после запятой.
static void WriteNanoseconds(double seconds, uint64_t operations)
{
    if (operations == 0u || !(seconds > 0.0))
    {
        WriteText("0.000");
        return;
    }
    double value = seconds * 1000000000.0 / (double)operations;
    if (value > 1000000.0)
    {
        value = 1000000.0;
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

static void Fail(const char* name)
{
    WriteText("r2-numeric failed: ");
    WriteText(name);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

// --- Значения ------------------------------------------------------------

static uint64_t NextRandom(uint64_t* state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state >> 33;
}

typedef struct NumericSet
{
    InfiniteCoord values[R2_NUMERIC_VALUES];
    int64_t addends[R2_NUMERIC_VALUES];
} NumericSet;

static NumericSet smallSet;
static NumericSet wideSet;
static NumericSet carrySet;      // однолимбовые у самой границы: добавка +1 даёт перенос
static InfiniteCoord scratchValues[R2_NUMERIC_VALUES];

static bool BuildSmall(InfiniteCoord* value, uint64_t* state)
{
    InfiniteCoordInit(value);
    uint64_t blocks = NextRandom(state) % 8192u;
    uint64_t fraction = NextRandom(state);
    int64_t magnitude = (int64_t)(blocks * (uint64_t)R2_NUMERIC_SCALE + (fraction & 0xffffffffu));
    bool negative = (NextRandom(state) & 1u) != 0u;
    return InfiniteCoordTryAddInt64InPlace(value, negative ? -magnitude : magnitude);
}

static bool BuildWide(InfiniteCoord* value, uint64_t* state)
{
    InfiniteCoord base;
    InfiniteCoordInit(&base);
    if (!InfiniteCoordTryAddInt64InPlace(&base, (int64_t)(NextRandom(state) | (1ull << 40))))
    {
        InfiniteCoordDestroy(&base);
        return false;
    }
    InfiniteCoord shifted;
    bool ok = InfiniteCoordTryCopyShiftLeft(&shifted, &base, 63u);
    InfiniteCoordDestroy(&base);
    if (!ok)
    {
        return false;
    }
    InfiniteCoord wider;
    ok = InfiniteCoordTryCopyShiftLeft(&wider, &shifted, 63u);
    InfiniteCoordDestroy(&shifted);
    if (!ok)
    {
        return false;
    }
    ok = InfiniteCoordTryCopyShiftLeft(value, &wider, 63u);
    InfiniteCoordDestroy(&wider);
    if (!ok)
    {
        return false;
    }
    if ((NextRandom(state) & 1u) != 0u)
    {
        value->sign = -value->sign;
    }
    return true;
}

// Однолимбовое значение ровно на границе: 0xFFFFFFFFFFFFFFFF (или его знак).
static bool BuildCarry(InfiniteCoord* value, uint64_t* state)
{
    InfiniteCoordInit(value);
    if (!InfiniteCoordTryAddInt64InPlace(value, (int64_t)0x7fffffffffffffffLL))
    {
        return false;
    }
    if (!InfiniteCoordTryAddInt64InPlace(value, (int64_t)0x7fffffffffffffffLL))
    {
        return false;
    }
    if (!InfiniteCoordTryAddInt64InPlace(value, 1))
    {
        return false;
    }
    if (value->sign == 0 || value->limbCount != 1u || value->limbs[0] != 0xffffffffffffffffull)
    {
        return false;
    }
    (void)state;
    return true;
}

static void BuildAddends(NumericSet* set, uint64_t* state)
{
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        uint64_t choice = NextRandom(state) % 3u;
        int64_t addend;
        if (choice == 0u)
        {
            addend = (int64_t)(24u * (uint64_t)R2_NUMERIC_SCALE / 128u);
        }
        else if (choice == 1u)
        {
            addend = (int64_t)((NextRandom(state) % 512u) * (uint64_t)R2_NUMERIC_SCALE / 128u);
        }
        else
        {
            addend = (int64_t)(NextRandom(state) & 0xffffffffu);
        }
        set->addends[index] = (NextRandom(state) & 1u) != 0u ? -addend : addend;
    }
}

static void ReleaseSet(NumericSet* set)
{
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        InfiniteCoordDestroy(&set->values[index]);
    }
}

static bool CopySet(InfiniteCoord* destination, const NumericSet* source)
{
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        if (!InfiniteCoordTryCopyAddInt64(&destination[index], &source->values[index], 0))
        {
            return false;
        }
    }
    return true;
}

static void ReleaseScratch(void)
{
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        InfiniteCoordDestroy(&scratchValues[index]);
    }
}

// --- Checksum ------------------------------------------------------------
//
// Считается только на прогреве (вне статистики), чтобы не утяжелять
// измеряемый цикл. Значение обязано совпасть у baseline и candidate.

static uint64_t checksumState = UINT64_C(0x243f6a8885a308d3);

static uint64_t Mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static void ChecksumCoord(const InfiniteCoord* value)
{
    uint64_t word = (uint64_t)value->limbCount
        ^ ((uint64_t)(uint32_t)value->sign << 32);
    if (value->limbCount > 0u && value->limbs != NULL)
    {
        word ^= value->limbs[0] + 0x9e3779b97f4a7c15ULL;
        if (value->limbCount > 1u)
        {
            word ^= value->limbs[value->limbCount - 1u];
        }
    }
    checksumState = Mix64(checksumState ^ word);
}

static void ChecksumValue(uint64_t value)
{
    checksumState = Mix64(checksumState ^ value);
}

// --- Измерение -----------------------------------------------------------
//
// pass(checksum) выполняет ровно один полный проход нагрузки (подготовка,
// операции, освобождение). Подготовка/освобождение — микросекунды против
// десятков-сотен миллисекунд прохода и одинаковы для обеих версий.

typedef void (*WorkloadPass)(bool checksum);

static void Measure(const char* name, WorkloadPass pass, uint32_t repetitions)
{
    pass(true); // Прогрев вне статистики; он же считает checksum.

    for (uint32_t rep = 0u; rep < repetitions; ++rep)
    {
        double begin = PlatformMonotonicSeconds();
        pass(false);
        double elapsed = PlatformMonotonicSeconds() - begin;

        WriteText("r2num ");
        WriteText(name);
        WriteText(" rep=");
        WriteUnsigned(rep);
        WriteText(" ops=");
        WriteUnsigned(R2_NUMERIC_OPS);
        WriteText(" ns_per_op=");
        WriteNanoseconds(elapsed, R2_NUMERIC_OPS);
        WriteText("\n");
    }
}

// --- Нагрузки ------------------------------------------------------------

// Типовая: накопление на месте (горячий путь физики, кучи нет).
static void PassAddInPlace(bool checksum, const NumericSet* set)
{
    if (!CopySet(scratchValues, set))
    {
        Fail("add_in_place");
    }
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            if (!InfiniteCoordTryAddInt64InPlace(&scratchValues[index], set->addends[index]))
            {
                Fail("add_in_place");
            }
            if (checksum)
            {
                ChecksumCoord(&scratchValues[index]);
            }
            numericSink += scratchValues[index].limbCount;
        }
    }
    ReleaseScratch();
}

static void PassAddInPlaceSmall(bool checksum) { PassAddInPlace(checksum, &smallSet); }
static void PassAddInPlaceWide(bool checksum) { PassAddInPlace(checksum, &wideSet); }

// Типовая: копия со сложением (1 alloc + 1 free на операцию).
static void PassCopyAdd(bool checksum, const NumericSet* set)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord copy;
            if (!InfiniteCoordTryCopyAddInt64(&copy, &set->values[index], set->addends[index]))
            {
                Fail("copy_add");
            }
            if (checksum)
            {
                ChecksumCoord(&copy);
                ChecksumValue((uint64_t)set->addends[index]);
            }
            numericSink += copy.limbCount;
            InfiniteCoordDestroy(&copy);
        }
    }
}

static void PassCopyAddSmall(bool checksum) { PassCopyAdd(checksum, &smallSet); }
static void PassCopyAddWide(bool checksum) { PassCopyAdd(checksum, &wideSet); }

// Неблагоприятная: источник 0xFFFF...FF, добавка +1 — перенос из старшего
// лимба, то есть второй поход в кучу там, где baseline копирует и расширяет.
static void PassCopyAddCarry(bool checksum)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord copy;
            int64_t addend = (index & 1u) != 0u
                ? (int64_t)(R2_NUMERIC_VALUES - index)
                : (int64_t)(1 + index);
            if (!InfiniteCoordTryCopyAddInt64(&copy, &carrySet.values[index], addend))
            {
                Fail("copy_add_carry");
            }
            if (checksum)
            {
                ChecksumCoord(&copy);
            }
            numericSink += copy.limbCount;
            InfiniteCoordDestroy(&copy);
        }
    }
}

// Неблагоприятная: точное погашение через копию — результат обязан быть
// каноническим нулём без лимбов и без выделения.
static void PassCopyAddCancel(bool checksum)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            const InfiniteCoord* source = &smallSet.values[index];
            int64_t addend = source->sign > 0
                ? -(int64_t)source->limbs[0]
                : (int64_t)source->limbs[0];
            InfiniteCoord copy;
            if (!InfiniteCoordTryCopyAddInt64(&copy, source, addend))
            {
                Fail("copy_add_cancel");
            }
            if (checksum)
            {
                ChecksumCoord(&copy);
            }
            numericSink += copy.limbCount;
            InfiniteCoordDestroy(&copy);
        }
    }
}

// Типовая: сдвиг вправо на 7 бит (1 alloc + 1 free).
static void PassShiftRight(bool checksum, const NumericSet* set)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord shifted;
            if (!InfiniteCoordTryCopyShiftRight(&shifted, &set->values[index], 7u))
            {
                Fail("shift_right");
            }
            if (checksum)
            {
                ChecksumCoord(&shifted);
            }
            numericSink += shifted.limbCount;
            InfiniteCoordDestroy(&shifted);
        }
    }
}

static void PassShiftRightSmall(bool checksum) { PassShiftRight(checksum, &smallSet); }
static void PassShiftRightWide(bool checksum) { PassShiftRight(checksum, &wideSet); }

// Неблагоприятная: сдвиг вправо, полностью съедающий модуль, — результат ноль.
static void PassShiftRightZero(bool checksum)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord shifted;
            if (!InfiniteCoordTryCopyShiftRight(&shifted, &carrySet.values[index], 63u))
            {
                Fail("shift_right_zero");
            }
            if (checksum)
            {
                ChecksumCoord(&shifted);
            }
            numericSink += shifted.limbCount;
            InfiniteCoordDestroy(&shifted);
        }
    }
}

// Типовая: сдвиг влево на 32 бита (перевод целой части double).
static void PassShiftLeft(bool checksum, const NumericSet* set)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord shifted;
            if (!InfiniteCoordTryCopyShiftLeft(&shifted, &set->values[index], 32u))
            {
                Fail("shift_left");
            }
            if (checksum)
            {
                ChecksumCoord(&shifted);
            }
            numericSink += shifted.limbCount;
            InfiniteCoordDestroy(&shifted);
        }
    }
}

static void PassShiftLeftSmall(bool checksum) { PassShiftLeft(checksum, &smallSet); }
static void PassShiftLeftWide(bool checksum) { PassShiftLeft(checksum, &wideSet); }

// Типовая: сумма двух произвольных чисел (rebase/широкий путь).
static void PassAddPair(bool checksum, const NumericSet* set)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord sum;
            uint32_t other = (index + 97u) % R2_NUMERIC_VALUES;
            if (!InfiniteCoordTryAdd(&sum, &set->values[index], &set->values[other]))
            {
                Fail("add_pair");
            }
            if (checksum)
            {
                ChecksumCoord(&sum);
            }
            numericSink += sum.limbCount;
            InfiniteCoordDestroy(&sum);
        }
    }
}

static void PassAddPairSmall(bool checksum) { PassAddPair(checksum, &smallSet); }
static void PassAddPairWide(bool checksum) { PassAddPair(checksum, &wideSet); }

// Неблагоприятная: сумма однолимбовых операндов с переносом в следующий лимб.
static void PassAddPairCarry(bool checksum)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord sum;
            uint32_t other = (index + 1u) % R2_NUMERIC_VALUES;
            if (!InfiniteCoordTryAdd(&sum, &carrySet.values[index], &carrySet.values[other]))
            {
                Fail("add_pair_carry");
            }
            if (checksum)
            {
                ChecksumCoord(&sum);
            }
            numericSink += sum.limbCount;
            InfiniteCoordDestroy(&sum);
        }
    }
}

// Типовая: мост в double (кучи нет).
static void PassToDouble(bool checksum, const NumericSet* set)
{
    double total = 0.0;
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            total += InfiniteCoordToDoubleSaturating(&set->values[index]);
        }
    }
    if (checksum)
    {
        ChecksumValue((uint64_t)(total != 0.0 ? 1u : 0u));
        ChecksumValue((uint64_t)set->values[0].limbCount);
    }
    numericSink += (uint64_t)(total != 0.0 ? 1u : 0u);
}

static void PassToDoubleSmall(bool checksum) { PassToDouble(checksum, &smallSet); }
static void PassToDoubleWide(bool checksum) { PassToDouble(checksum, &wideSet); }

// Типовая: обратный мост из double (1 alloc + 1 free).
static void PassFromDouble(bool checksum)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            InfiniteCoord value;
            double source = (double)(index + 1u) * 1024.0 + (double)round;
            if (!InfiniteCoordTrySetFromDouble(&value, source))
            {
                Fail("from_double");
            }
            if (checksum)
            {
                ChecksumCoord(&value);
            }
            numericSink += value.limbCount;
            InfiniteCoordDestroy(&value);
        }
    }
}

// Типовая: деление на 2*pi (кучи нет).
static void PassDivFloor(bool checksum, const NumericSet* set)
{
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            uint64_t remainder = 0u;
            uint64_t quotient =
                InfiniteCoordDivFloorSmallLow(&set->values[index], 26986075409ull, &remainder);
            if (checksum)
            {
                ChecksumValue(quotient ^ remainder);
            }
            numericSink += quotient;
            numericSink += remainder;
        }
    }
}

static void PassDivFloorSmall(bool checksum) { PassDivFloor(checksum, &smallSet); }
static void PassDivFloorWide(bool checksum) { PassDivFloor(checksum, &wideSet); }

// Типовая: переход через ноль (alloc/free по половине операций).
static void PassZeroCrossing(bool checksum)
{
    InfiniteCoord value;
    InfiniteCoordInit(&value);
    const int64_t step = (int64_t)(3u * (uint64_t)R2_NUMERIC_SCALE);
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            if (!InfiniteCoordTryAddInt64InPlace(&value, (index & 1u) != 0u ? -step : step))
            {
                Fail("zero_crossing");
            }
            if (checksum)
            {
                ChecksumCoord(&value);
            }
            numericSink += value.limbCount;
        }
    }
    InfiniteCoordDestroy(&value);
}

// Типовая: накопление на месте у границы лимба (перенос в старший лимб и
// обратный заём каждые две операции).
static void PassCarryInPlace(bool checksum)
{
    InfiniteCoord value;
    InfiniteCoordInit(&value);
    uint64_t carryState = 1u;
    if (!BuildCarry(&value, &carryState))
    {
        Fail("carry_in_place");
    }
    for (uint32_t round = 0u; round < R2_NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
        {
            int64_t addend = (index & 1u) != 0u ? -1 : 1;
            if (!InfiniteCoordTryAddInt64InPlace(&value, addend))
            {
                Fail("carry_in_place");
            }
            if (checksum)
            {
                ChecksumCoord(&value);
            }
            numericSink += value.limbCount;
        }
    }
    InfiniteCoordDestroy(&value);
}

LAIUE_TEST_ENTRY(R2NumericAbEntryPoint)
{
    WriteText("laiue r2 numeric A/B harness\n");

    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        if (!BuildSmall(&smallSet.values[index], &state))
        {
            Fail("построение малых значений");
        }
        if (!BuildCarry(&carrySet.values[index], &state))
        {
            Fail("построение граничных значений");
        }
    }
    BuildAddends(&smallSet, &state);
    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        if (!BuildWide(&wideSet.values[index], &state))
        {
            Fail("построение широких значений");
        }
    }
    BuildAddends(&wideSet, &state);

    for (uint32_t index = 0u; index < R2_NUMERIC_VALUES; ++index)
    {
        if (smallSet.values[index].limbCount > 1u)
        {
            Fail("малое значение не помещается в лимб");
        }
        if (wideSet.values[index].limbCount != 4u)
        {
            Fail("широкое значение не в четыре лимба");
        }
        if (carrySet.values[index].limbCount != 1u)
        {
            Fail("граничное значение не в один лимб");
        }
    }

    const uint32_t repetitions = 5u;
    Measure("add_in_place.small", PassAddInPlaceSmall, repetitions);
    Measure("add_in_place.wide", PassAddInPlaceWide, repetitions);
    Measure("copy_add.small", PassCopyAddSmall, repetitions);
    Measure("copy_add.wide", PassCopyAddWide, repetitions);
    Measure("copy_add_carry", PassCopyAddCarry, repetitions);
    Measure("copy_add_cancel", PassCopyAddCancel, repetitions);
    Measure("shift_right.small", PassShiftRightSmall, repetitions);
    Measure("shift_right.wide", PassShiftRightWide, repetitions);
    Measure("shift_right_zero", PassShiftRightZero, repetitions);
    Measure("shift_left.small", PassShiftLeftSmall, repetitions);
    Measure("shift_left.wide", PassShiftLeftWide, repetitions);
    Measure("add_pair.small", PassAddPairSmall, repetitions);
    Measure("add_pair.wide", PassAddPairWide, repetitions);
    Measure("add_pair_carry", PassAddPairCarry, repetitions);
    Measure("to_double.small", PassToDoubleSmall, repetitions);
    Measure("to_double.wide", PassToDoubleWide, repetitions);
    Measure("from_double", PassFromDouble, repetitions);
    Measure("div_floor.small", PassDivFloorSmall, repetitions);
    Measure("div_floor.wide", PassDivFloorWide, repetitions);
    Measure("zero_crossing", PassZeroCrossing, repetitions);
    Measure("carry_in_place", PassCarryInPlace, repetitions);

    ReleaseSet(&smallSet);
    ReleaseSet(&wideSet);
    ReleaseSet(&carrySet);

    WriteText("r2num checksum ");
    WriteHex(checksumState);
    WriteText(" sink=");
    WriteUnsigned(numericSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}