// Ручной benchmark арифметики произвольной точности на распределении,
// которым её нагружает физика. В ALL не входит и в CTest не регистрируется:
// его запускают осознанно и читают глазами.
//
// Физика держит позиции и скорости в фиксированной точке с масштабом 2^32.
// Обычная координата — тысячи блоков, обычная скорость — сотни блоков в
// секунду; и то и другое укладывается в один лимб с огромным запасом.
// Настоящая многолимбовость появляется только после невероятных разгонов и
// после ухода далеко от начала мира, поэтому малый и широкий случаи меряются
// раздельно: у них разная стоимость и разные узкие места.
//
// Микробенчмарк меряет операции, а не физику. Ускорение шага симуляции по
// нему объявлять нельзя — для этого есть laiue_physics_benchmark и спавнер
// игры.

#include "numeric/infinite_coord.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

// Масштаб фиксированной точки физики: единица — блок либо блок в секунду.
#define NUMERIC_SCALE 4294967296.0
#define NUMERIC_VALUES 256u
#define NUMERIC_ROUNDS 4096u

static volatile uint64_t numericSink;

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

// Наносекунды с тремя знаками: разница между вариантами здесь измеряется
// единицами наносекунд, и миллисекунды такую разницу стирают.
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

static void Report(const char *name, double seconds, uint64_t operations)
{
    WriteText("numeric ");
    WriteText(name);
    WriteText(" ops=");
    WriteUnsigned(operations);
    WriteText(" ns_per_op=");
    WriteNanoseconds(seconds, operations);
    WriteText("\n");
}

static void Fail(const char *name)
{
    WriteText("numeric benchmark failed: ");
    WriteText(name);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state >> 33;
}

// Значение в один лимб: координата в несколько тысяч блоков либо скорость в
// несколько сотен блоков в секунду, обе в масштабе 2^32.
static bool BuildSmall(InfiniteCoord *value, uint64_t *state)
{
    InfiniteCoordInit(value);
    uint64_t blocks = NextRandom(state) % 8192u;
    uint64_t fraction = NextRandom(state);
    int64_t magnitude = (int64_t)(blocks * (uint64_t)NUMERIC_SCALE + (fraction & 0xffffffffu));
    bool negative = (NextRandom(state) & 1u) != 0u;
    return InfiniteCoordTryAddInt64InPlace(value, negative ? -magnitude : magnitude);
}

// Значение в четыре лимба: так выглядит координата после невероятного
// разгона либо после ухода на расстояние, которого int64 уже не хватает.
static bool BuildWide(InfiniteCoord *value, uint64_t *state)
{
    InfiniteCoord base;
    InfiniteCoordInit(&base);
    if (!InfiniteCoordTryAddInt64InPlace(&base, (int64_t)(NextRandom(state) | (1ull << 40))))
    {
        InfiniteCoordDestroy(&base);
        return false;
    }
    // 192 бита сдвига дают ровно четвёртый лимб.
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

typedef struct NumericSet
{
    InfiniteCoord values[NUMERIC_VALUES];
    int64_t addends[NUMERIC_VALUES];
} NumericSet;

static NumericSet smallSet;
static NumericSet wideSet;
static InfiniteCoord scratchValues[NUMERIC_VALUES];

static void ReleaseSet(NumericSet *set)
{
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        InfiniteCoordDestroy(&set->values[index]);
    }
}

// Добавки того же порядка, что в физике: шаг гравитации 24/128 блока в
// секунду и перемещение за шаг, то есть скорость, сдвинутая на семь бит.
static void BuildAddends(NumericSet *set, uint64_t *state)
{
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        uint64_t choice = NextRandom(state) % 3u;
        int64_t addend;
        if (choice == 0u)
        {
            addend = (int64_t)(24u * (uint64_t)NUMERIC_SCALE / 128u);
        }
        else if (choice == 1u)
        {
            addend = (int64_t)((NextRandom(state) % 512u) * (uint64_t)NUMERIC_SCALE / 128u);
        }
        else
        {
            addend = (int64_t)(NextRandom(state) & 0xffffffffu);
        }
        set->addends[index] = (NextRandom(state) & 1u) != 0u ? -addend : addend;
    }
}

static bool CopySet(InfiniteCoord *destination, const NumericSet *source)
{
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
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
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        InfiniteCoordDestroy(&scratchValues[index]);
    }
}

// Накопление на месте: горячий путь физики. Половина добавок отрицательна,
// поэтому заём через границу лимба встречается на каждом наборе.
static void BenchmarkAddInPlace(const char *name, NumericSet *set)
{
    if (!CopySet(scratchValues, set))
    {
        Fail(name);
    }
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            if (!InfiniteCoordTryAddInt64InPlace(&scratchValues[index], set->addends[index]))
            {
                Fail(name);
            }
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        numericSink += scratchValues[index].limbCount;
    }
    ReleaseScratch();
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Копия со сложением: так физика читает скорость, не трогая исходную.
static void BenchmarkCopyAdd(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            InfiniteCoord copy;
            if (!InfiniteCoordTryCopyAddInt64(&copy, &set->values[index], set->addends[index]))
            {
                Fail(name);
            }
            numericSink += copy.limbCount;
            InfiniteCoordDestroy(&copy);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Сдвиг вправо на семь бит: перемещение за шаг из скорости.
static void BenchmarkShiftRight(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            InfiniteCoord shifted;
            if (!InfiniteCoordTryCopyShiftRight(&shifted, &set->values[index], 7u))
            {
                Fail(name);
            }
            numericSink += shifted.limbCount;
            InfiniteCoordDestroy(&shifted);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Сдвиг влево на 32 бита: перевод целой части double в фиксированную точку.
static void BenchmarkShiftLeft(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            InfiniteCoord shifted;
            if (!InfiniteCoordTryCopyShiftLeft(&shifted, &set->values[index], 32u))
            {
                Fail(name);
            }
            numericSink += shifted.limbCount;
            InfiniteCoordDestroy(&shifted);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Сумма двух произвольных чисел: rebasing и широкий путь накопления.
static void BenchmarkAddPair(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            InfiniteCoord sum;
            uint32_t other = (index + 97u) % NUMERIC_VALUES;
            if (!InfiniteCoordTryAdd(&sum, &set->values[index], &set->values[other]))
            {
                Fail(name);
            }
            numericSink += sum.limbCount;
            InfiniteCoordDestroy(&sum);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Мост в double: физика читает так каждую скорость на каждом шаге.
static void BenchmarkToDouble(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    double total = 0.0;
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            total += InfiniteCoordToDoubleSaturating(&set->values[index]);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    numericSink += (uint64_t)(total != 0.0 ? 1u : 0u);
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Обратный мост: целая часть double в число произвольной точности.
static void BenchmarkFromDouble(const char *name)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            InfiniteCoord value;
            double source = (double)(index + 1u) * 1024.0 + (double)round;
            if (!InfiniteCoordTrySetFromDouble(&value, source))
            {
                Fail(name);
            }
            numericSink += value.limbCount;
            InfiniteCoordDestroy(&value);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Деление на 2*pi в масштабе 2^32: приведение угла поворота.
static void BenchmarkDivFloor(const char *name, NumericSet *set)
{
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            uint64_t remainder = 0u;
            numericSink +=
                InfiniteCoordDivFloorSmallLow(&set->values[index], 26986075409ull, &remainder);
            numericSink += remainder;
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

// Переход через ноль: значение то освобождает лимбы, то заводит их заново.
// Отдельный случай именно потому, что здесь работает не арифметика, а
// распределитель памяти.
static void BenchmarkZeroCrossing(const char *name)
{
    InfiniteCoord value;
    InfiniteCoordInit(&value);
    const int64_t step = (int64_t)(3u * (uint64_t)NUMERIC_SCALE);
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < NUMERIC_ROUNDS; ++round)
    {
        for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
        {
            if (!InfiniteCoordTryAddInt64InPlace(&value, (index & 1u) != 0u ? -step : step))
            {
                Fail(name);
            }
            numericSink += value.limbCount;
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    InfiniteCoordDestroy(&value);
    Report(name, elapsed, (uint64_t)NUMERIC_ROUNDS * NUMERIC_VALUES);
}

LAIUE_TEST_ENTRY(NumericPhysicsBenchmarkEntryPoint)
{
    WriteText("laiue numeric benchmark\n");

    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        if (!BuildSmall(&smallSet.values[index], &state))
        {
            Fail("построение малых значений");
        }
    }
    BuildAddends(&smallSet, &state);
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        if (!BuildWide(&wideSet.values[index], &state))
        {
            Fail("построение широких значений");
        }
    }
    BuildAddends(&wideSet, &state);

    // Набор действительно такой, каким задуман: иначе «широкий» случай мерил
    // бы то же самое, что и малый.
    for (uint32_t index = 0u; index < NUMERIC_VALUES; ++index)
    {
        if (smallSet.values[index].limbCount > 1u)
        {
            Fail("малое значение не помещается в лимб");
        }
        if (wideSet.values[index].limbCount != 4u)
        {
            Fail("широкое значение не в четыре лимба");
        }
    }

    BenchmarkAddInPlace("add_in_place.small", &smallSet);
    BenchmarkAddInPlace("add_in_place.wide", &wideSet);
    BenchmarkCopyAdd("copy_add.small", &smallSet);
    BenchmarkCopyAdd("copy_add.wide", &wideSet);
    BenchmarkShiftRight("shift_right.small", &smallSet);
    BenchmarkShiftRight("shift_right.wide", &wideSet);
    BenchmarkShiftLeft("shift_left.small", &smallSet);
    BenchmarkShiftLeft("shift_left.wide", &wideSet);
    BenchmarkAddPair("add_pair.small", &smallSet);
    BenchmarkAddPair("add_pair.wide", &wideSet);
    BenchmarkToDouble("to_double.small", &smallSet);
    BenchmarkToDouble("to_double.wide", &wideSet);
    BenchmarkFromDouble("from_double");
    BenchmarkDivFloor("div_floor.small", &smallSet);
    BenchmarkDivFloor("div_floor.wide", &wideSet);
    BenchmarkZeroCrossing("zero_crossing");

    ReleaseSet(&smallSet);
    ReleaseSet(&wideSet);
    WriteText("numeric benchmark done sink=");
    WriteUnsigned(numericSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
