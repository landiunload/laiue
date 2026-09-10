#include "numeric/infinite_coord.h"
#include "test_runtime.h"

#include <float.h>
#include <string.h>

// InfiniteCoord — публичное знаковое целое произвольной точности, на котором
// держится бесконечный мир. Тест проходит через экспортированный ABI world.
//
// Структура InfiniteCoord публична, поэтому переносы через границу лимба
// (2^64) проверяются прямым сравнением limbs/limbCount/sign, а не косвенно.

static uint32_t coordTestChecks;

static void CoordTestWrite(const char* text)
{
    LaiueTestRuntimeWrite(text);
}

static void CoordTestWriteNumber(uint32_t value)
{
    char text[11];
    uint32_t position = sizeof(text) - 1;
    text[position] = '\0';
    do
    {
        text[--position] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (value != 0 && position != 0);
    CoordTestWrite(&text[position]);
}

// Имя проверки вместо номера: список растёт, перенумеровывать его при каждой
// вставке — верный способ разойтись с отчётом CTest.
static void CoordTestExpect(bool condition, const char* name)
{
    ++coordTestChecks;
    if (condition)
    {
        return;
    }
    CoordTestWrite("Проверка не пройдена: ");
    CoordTestWrite(name);
    CoordTestWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool WideEquals(const wchar_t* left, const wchar_t* right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

// --- Помощники построения значений ---------------------------------------

static void CoordSetInt64(InfiniteCoord* out, int64_t value)
{
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    CoordTestExpect(InfiniteCoordTryCopyAddInt64(out, &zero, value),
        "построение из int64");
    InfiniteCoordDestroy(&zero);
}

static void CoordAddInt64(InfiniteCoord* value, int64_t addend)
{
    InfiniteCoord result;
    CoordTestExpect(InfiniteCoordTryCopyAddInt64(&result, value, addend),
        "добавление int64");
    InfiniteCoordDestroy(value);
    *value = result;
}

// Строит ровно 2^64 сложениями, укладывающимися в int64: перенос из нулевого
// лимба в первый — самый частый путь во всей арифметике мира.
static void CoordSetTwoPow64(InfiniteCoord* out)
{
    CoordSetInt64(out, (int64_t)0x7fffffffffffffffLL); // 2^63 - 1
    CoordAddInt64(out, (int64_t)0x7fffffffffffffffLL); // 2^64 - 2
    CoordAddInt64(out, 2);                              // 2^64
}

// --- Проверки -------------------------------------------------------------

static void TestInitAndInt64RoundTrip(void)
{
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    CoordTestExpect(zero.sign == 0 && zero.limbCount == 0 && zero.limbs == NULL,
        "Init не дал нейтральный ноль");

    // Destroy на нуле безопасен и идемпотентен.
    InfiniteCoordDestroy(&zero);
    InfiniteCoordDestroy(&zero);
    CoordTestExpect(zero.sign == 0 && zero.limbs == NULL,
        "Destroy испортил нулевое значение");

    static const int64_t samples[6] = {
        0, 1, -1, INT64_MAX, INT64_MIN, -1234567890123456789LL,
    };
    for (uint32_t i = 0; i < 6u; ++i)
    {
        InfiniteCoord value;
        CoordSetInt64(&value, samples[i]);
        InfiniteCoord base;
        InfiniteCoordInit(&base);
        int64_t restored = 0;
        CoordTestExpect(
            InfiniteCoordTrySubtractToInt64(&value, &base, &restored),
            "SubtractToInt64 отверг значение, помещающееся в int64");
        CoordTestExpect(restored == samples[i],
            "int64 -> coord -> int64 изменил значение");
        InfiniteCoordDestroy(&value);
        InfiniteCoordDestroy(&base);
    }

    // Плюс и минус одной величины возвращают ровно ноль, а не -0.
    InfiniteCoord cancel;
    CoordSetInt64(&cancel, 5000000000LL);
    CoordAddInt64(&cancel, -5000000000LL);
    CoordTestExpect(cancel.sign == 0 && cancel.limbCount == 0,
        "сложение противоположных не дало чистый ноль");
    InfiniteCoordDestroy(&cancel);
}

static void TestCarryAcrossLimb(void)
{
    InfiniteCoord value;
    CoordSetTwoPow64(&value);
    CoordTestExpect(value.sign == 1 && value.limbCount == 2
        && value.limbs[0] == 0 && value.limbs[1] == 1,
        "перенос через границу лимба построил не 2^64");

    // Заём в обратную сторону: 2^64 - 1 должно схлопнуться в один лимб 0xFFFF...
    CoordAddInt64(&value, -1);
    CoordTestExpect(value.sign == 1 && value.limbCount == 1
        && value.limbs[0] == 0xffffffffffffffffULL,
        "заём через границу лимба построил не 2^64 - 1");
    InfiniteCoordDestroy(&value);

    // Пересечение нуля из отрицательной области в положительную.
    InfiniteCoord crossing;
    CoordSetInt64(&crossing, -3);
    CoordAddInt64(&crossing, 10);
    int64_t restored = 0;
    InfiniteCoord base;
    InfiniteCoordInit(&base);
    CoordTestExpect(InfiniteCoordTrySubtractToInt64(&crossing, &base, &restored)
        && restored == 7,
        "переход через ноль дал неверный результат");
    InfiniteCoordDestroy(&crossing);
    InfiniteCoordDestroy(&base);
}

static void TestDivFloor(void)
{
    // Положительное: обычное деление с остатком.
    InfiniteCoord seven;
    CoordSetInt64(&seven, 7);
    uint64_t remainder = 0;
    uint64_t quotient = InfiniteCoordDivFloorSmallLow(&seven, 3, &remainder);
    CoordTestExpect(quotient == 2 && remainder == 1,
        "DivFloor(7,3) неверен");
    InfiniteCoordDestroy(&seven);

    // Отрицательное неделимое: округление ВНИЗ и неотрицательный остаток.
    // floor(-7/3) = -3, остаток 2 (а не -2 с частным -2).
    InfiniteCoord negativeSeven;
    CoordSetInt64(&negativeSeven, -7);
    quotient = InfiniteCoordDivFloorSmallLow(&negativeSeven, 3, &remainder);
    CoordTestExpect(quotient == (0ull - 3ull) && remainder == 2,
        "DivFloor(-7,3) не округлил вниз");
    InfiniteCoordDestroy(&negativeSeven);

    // Отрицательное, делится нацело: остаток 0, частное точное.
    InfiniteCoord negativeSix;
    CoordSetInt64(&negativeSix, -6);
    quotient = InfiniteCoordDivFloorSmallLow(&negativeSix, 3, &remainder);
    CoordTestExpect(quotient == (0ull - 2ull) && remainder == 0,
        "DivFloor(-6,3) неверен при делении нацело");
    InfiniteCoordDestroy(&negativeSix);

    // Деление через границу лимба: floor(2^64 / 10).
    InfiniteCoord twoPow64;
    CoordSetTwoPow64(&twoPow64);
    quotient = InfiniteCoordDivFloorSmallLow(&twoPow64, 10, &remainder);
    CoordTestExpect(quotient == 1844674407370955161ULL && remainder == 6,
        "DivFloor(2^64,10) через границу лимба неверен");
    InfiniteCoordDestroy(&twoPow64);
}

static void TestShiftRight(void)
{
    // Сдвиг протаскивает младший бит верхнего лимба в старший бит нижнего.
    // (2^64 + 5) >> 1 = 2^63 + 2.
    InfiniteCoord value;
    CoordSetTwoPow64(&value);
    CoordAddInt64(&value, 5);
    InfiniteCoord shifted;
    CoordTestExpect(InfiniteCoordTryCopyShiftRight(&shifted, &value, 1),
        "ShiftRight вернул ошибку");
    CoordTestExpect(shifted.sign == 1 && shifted.limbCount == 1
        && shifted.limbs[0] == 0x8000000000000002ULL,
        "ShiftRight на 1 через границу лимба неверен");
    InfiniteCoordDestroy(&value);
    InfiniteCoordDestroy(&shifted);

    // Сдвиг на 0 — точная копия; сдвиг на 64 и больше не поддержан.
    InfiniteCoord source;
    CoordSetInt64(&source, 42);
    InfiniteCoord copy;
    CoordTestExpect(InfiniteCoordTryCopyShiftRight(&copy, &source, 0)
        && copy.limbCount == 1 && copy.limbs[0] == 42,
        "ShiftRight на 0 не дал копию");
    InfiniteCoord tooWide;
    CoordTestExpect(!InfiniteCoordTryCopyShiftRight(&tooWide, &source, 64),
        "ShiftRight принял сдвиг на 64 бита");
    InfiniteCoordDestroy(&source);
    InfiniteCoordDestroy(&copy);
}

static void TestSquare(void)
{
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    InfiniteCoord base;
    InfiniteCoordInit(&base);

    // (0 + 5)^2 = 25.
    InfiniteCoord small;
    CoordTestExpect(InfiniteCoordTryCopySquareAddInt64(&small, &zero, 5),
        "SquareAdd вернул ошибку");
    int64_t restored = 0;
    CoordTestExpect(InfiniteCoordTrySubtractToInt64(&small, &base, &restored)
        && restored == 25,
        "(0+5)^2 != 25");
    InfiniteCoordDestroy(&small);

    // Квадрат всегда неотрицателен: (0 + (-5))^2 = 25.
    InfiniteCoord negative;
    CoordTestExpect(InfiniteCoordTryCopySquareAddInt64(&negative, &zero, -5),
        "SquareAdd(-5) вернул ошибку");
    CoordTestExpect(negative.sign == 1
        && InfiniteCoordTrySubtractToInt64(&negative, &base, &restored)
        && restored == 25,
        "(0-5)^2 не дал +25");
    InfiniteCoordDestroy(&negative);

    // Квадрат, пересекающий границу лимба: (2^32)^2 = 2^64.
    InfiniteCoord wide;
    CoordTestExpect(
        InfiniteCoordTryCopySquareAddInt64(&wide, &zero, 0x100000000LL),
        "SquareAdd(2^32) вернул ошибку");
    CoordTestExpect(wide.sign == 1 && wide.limbCount == 2
        && wide.limbs[0] == 0 && wide.limbs[1] == 1,
        "(2^32)^2 != 2^64");
    InfiniteCoordDestroy(&wide);

    // Крупный квадрат, ещё помещающийся в int64: 3e9^2 = 9e18.
    InfiniteCoord big;
    CoordTestExpect(
        InfiniteCoordTryCopySquareAddInt64(&big, &zero, 3000000000LL),
        "SquareAdd(3e9) вернул ошибку");
    CoordTestExpect(InfiniteCoordTrySubtractToInt64(&big, &base, &restored)
        && restored == 9000000000000000000LL,
        "(3e9)^2 != 9e18");
    InfiniteCoordDestroy(&big);

    InfiniteCoordDestroy(&zero);
    InfiniteCoordDestroy(&base);
}

static void TestSubtractToInt64Boundaries(void)
{
    InfiniteCoord base;
    InfiniteCoordInit(&base);

    // Ровно INT64_MIN восстанавливается, а не переполняется.
    InfiniteCoord minimum;
    CoordSetInt64(&minimum, INT64_MIN);
    int64_t restored = 0;
    CoordTestExpect(InfiniteCoordTrySubtractToInt64(&minimum, &base, &restored)
        && restored == INT64_MIN,
        "SubtractToInt64 потерял INT64_MIN");
    InfiniteCoordDestroy(&minimum);

    // 2^64 в int64 не помещается — функция обязана отказать, а не усечь.
    InfiniteCoord tooBig;
    CoordSetTwoPow64(&tooBig);
    CoordTestExpect(!InfiniteCoordTrySubtractToInt64(&tooBig, &base, &restored),
        "SubtractToInt64 принял значение шире int64");
    InfiniteCoordDestroy(&tooBig);
    InfiniteCoordDestroy(&base);
}

static void TestCompareAddInt64(void)
{
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    CoordTestExpect(
        InfiniteCoordCompareAddInt64ToInt64(&zero, 5, 3) > 0,
        "Compare(0+5, 3) не положителен");
    CoordTestExpect(
        InfiniteCoordCompareAddInt64ToInt64(&zero, 3, 3) == 0,
        "Compare(0+3, 3) не ноль");
    CoordTestExpect(
        InfiniteCoordCompareAddInt64ToInt64(&zero, 2, 5) < 0,
        "Compare(0+2, 5) не отрицателен");
    InfiniteCoordDestroy(&zero);

    // Значение шире int64 перевешивает любой int64-порог.
    InfiniteCoord huge;
    CoordSetTwoPow64(&huge);
    CoordTestExpect(
        InfiniteCoordCompareAddInt64ToInt64(&huge, 0, INT64_MAX) > 0,
        "Compare(2^64, INT64_MAX) не положителен");
    InfiniteCoordDestroy(&huge);
}

static void TestEqualsAndOffsets(void)
{
    InfiniteCoord base;
    CoordSetTwoPow64(&base);

    InfiniteCoord shifted;
    CoordTestExpect(InfiniteCoordTryCopyAddInt64(&shifted, &base, 1000),
        "построение base+1000 не удалось");

    CoordTestExpect(InfiniteCoordEqualsOffset(&shifted, &base, 1000),
        "EqualsOffset не признал base+1000");
    CoordTestExpect(!InfiniteCoordEqualsOffset(&shifted, &base, 999),
        "EqualsOffset принял неверное смещение");

    // Виртуальные значения base+1000 и base+1000 равны при совпадающих
    // смещениях с обеих сторон.
    CoordTestExpect(
        InfiniteCoordEqualsOffsets(&shifted, 0, &base, 1000),
        "EqualsOffsets не сопоставил равные значения со смещениями");
    CoordTestExpect(
        !InfiniteCoordEqualsOffsets(&shifted, 1, &base, 1000),
        "EqualsOffsets признал равными несовпадающие значения");

    InfiniteCoordDestroy(&base);
    InfiniteCoordDestroy(&shifted);
}

static void TestHashOffset(void)
{
    // Хеш зависит от виртуального значения base + offset, а не от способа
    // записи: hash(base, offset) == hash(base+offset, 0).
    static const int64_t offsets[3] = { 5, -3, 1000000 };
    for (uint32_t i = 0; i < 3u; ++i)
    {
        InfiniteCoord base;
        CoordSetTwoPow64(&base);
        InfiniteCoord materialized;
        CoordTestExpect(
            InfiniteCoordTryCopyAddInt64(&materialized, &base, offsets[i]),
            "материализация base+offset не удалась");

        uint64_t viaOffset = InfiniteCoordHashOffset(&base, offsets[i]);
        uint64_t viaValue = InfiniteCoordHashOffset(&materialized, 0);
        CoordTestExpect(viaOffset == viaValue,
            "hash(base,offset) != hash(base+offset,0)");
        // Детерминизм.
        CoordTestExpect(viaOffset == InfiniteCoordHashOffset(&base, offsets[i]),
            "HashOffset недетерминирован");

        InfiniteCoordDestroy(&base);
        InfiniteCoordDestroy(&materialized);
    }

    // Разные значения обязаны различаться хешем (иначе соседние чанки
    // столкнулись бы в таблице).
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    CoordTestExpect(
        InfiniteCoordHashOffset(&zero, 0) != InfiniteCoordHashOffset(&zero, 1),
        "HashOffset совпал для 0 и 1");
    InfiniteCoordDestroy(&zero);
}

static void TestSwap(void)
{
    InfiniteCoord left;
    CoordSetInt64(&left, 111);
    InfiniteCoord right;
    CoordSetInt64(&right, -222);
    InfiniteCoordSwap(&left, &right);
    CoordTestExpect(left.sign == -1 && left.limbs[0] == 222
        && right.sign == 1 && right.limbs[0] == 111,
        "Swap не обменял содержимое");
    InfiniteCoordDestroy(&left);
    InfiniteCoordDestroy(&right);
}

static void TestFormatShort(void)
{
    wchar_t text[32];
    InfiniteCoord base;
    InfiniteCoordInit(&base);

    InfiniteCoordFormatShortOffsetW(&base, 0, text, 32);
    CoordTestExpect(WideEquals(text, L"0"), "Format(0) != \"0\"");

    InfiniteCoordFormatShortOffsetW(&base, 12345, text, 32);
    CoordTestExpect(WideEquals(text, L"12345"), "Format(12345) неверен");

    InfiniteCoordFormatShortOffsetW(&base, -42, text, 32);
    CoordTestExpect(WideEquals(text, L"-42"), "Format(-42) неверен");

    // Восемь значащих цифр переходят в научную запись X.YZe<порядок>.
    InfiniteCoordFormatShortOffsetW(&base, 12345678, text, 32);
    CoordTestExpect(WideEquals(text, L"1.23e7"),
        "Format(12345678) не дал научную запись");

    InfiniteCoordDestroy(&base);

    // Значение шире одного лимба показывается как степень двойки.
    InfiniteCoord twoPow64;
    CoordSetTwoPow64(&twoPow64);
    InfiniteCoordFormatShortOffsetW(&twoPow64, 0, text, 32);
    CoordTestExpect(WideEquals(text, L"~2^64"),
        "Format(2^64) не дал \"~2^64\"");
    InfiniteCoordDestroy(&twoPow64);
}


// === Общая арифметика ===
//
// Скорость твёрдого тела накапливается именно этими операциями, поэтому
// проверяются переносы через границу лимба, знаки и насыщение к double.

static void CoordSet(InfiniteCoord* value, int64_t initial)
{
    InfiniteCoordInit(value);
    InfiniteCoord result;
    if (InfiniteCoordTryCopyAddInt64(&result, value, initial))
    {
        InfiniteCoordDestroy(value);
        *value = result;
    }
}

static bool CoordEqualsInt64(const InfiniteCoord* value, int64_t expected)
{
    InfiniteCoord reference;
    CoordSet(&reference, expected);
    bool equal = InfiniteCoordCompare(value, &reference) == 0;
    InfiniteCoordDestroy(&reference);
    return equal;
}

static void TestAddAndNegate(void)
{
    InfiniteCoord inPlace;
    CoordSet(&inPlace, 7);
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&inPlace, 5),
                    "добавление int64 на месте");
    CoordTestExpect(CoordEqualsInt64(&inPlace, 12), "7 + 5 на месте = 12");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&inPlace, -20),
                    "отрицательное добавление int64 на месте");
    CoordTestExpect(CoordEqualsInt64(&inPlace, -8), "12 - 20 на месте = -8");
    CoordTestExpect(!InfiniteCoordTryAddInt64InPlace(NULL, 1),
                    "нулевой указатель отклонён");
    InfiniteCoordDestroy(&inPlace);

    InfiniteCoord left;
    InfiniteCoord right;
    InfiniteCoord sum;

    CoordSet(&left, 7);
    CoordSet(&right, -19);
    CoordTestExpect(InfiniteCoordTryAdd(&sum, &left, &right), "сложение разных знаков");
    CoordTestExpect(CoordEqualsInt64(&sum, -12), "7 + (-19) = -12");
    InfiniteCoordDestroy(&sum);
    InfiniteCoordDestroy(&right);

    // Ровно противоположные слагаемые обязаны дать канонический ноль.
    CoordSet(&right, -7);
    CoordTestExpect(InfiniteCoordTryAdd(&sum, &left, &right), "сложение до нуля");
    CoordTestExpect(InfiniteCoordSign(&sum) == 0 && sum.limbCount == 0 && sum.limbs == NULL,
                    "ноль хранится без лимбов");
    InfiniteCoordDestroy(&sum);
    InfiniteCoordDestroy(&right);
    InfiniteCoordDestroy(&left);

    // Перенос через границу лимба: 2^64-1 плюс 1.
    CoordSet(&left, 0);
    InfiniteCoord grown;
    CoordTestExpect(InfiniteCoordTryCopyAddInt64(&grown, &left, INT64_MAX), "подготовка");
    InfiniteCoordDestroy(&left);
    CoordTestExpect(InfiniteCoordTryAdd(&sum, &grown, &grown), "удвоение через лимб");
    CoordTestExpect(sum.limbCount == 1 && sum.limbs[0] == 0xFFFFFFFFFFFFFFFEull,
                    "2 * (2^63-1) укладывается в один лимб");
    InfiniteCoordDestroy(&sum);

    InfiniteCoord negated;
    CoordTestExpect(InfiniteCoordTryCopyNegate(&negated, &grown), "смена знака");
    CoordTestExpect(InfiniteCoordSign(&negated) == -1, "знак стал отрицательным");
    CoordTestExpect(InfiniteCoordTryAdd(&sum, &grown, &negated), "число плюс его противоположное");
    CoordTestExpect(InfiniteCoordSign(&sum) == 0, "сумма противоположных — ноль");
    InfiniteCoordDestroy(&sum);
    InfiniteCoordDestroy(&negated);
    InfiniteCoordDestroy(&grown);
}

static void TestMultiplyAndShiftLeft(void)
{
    InfiniteCoord value;
    InfiniteCoord product;
    CoordSet(&value, 3);
    CoordTestExpect(InfiniteCoordTryCopyMultiplyInt64(&product, &value, -5), "умножение на int64");
    CoordTestExpect(CoordEqualsInt64(&product, -15), "3 * (-5) = -15");
    InfiniteCoordDestroy(&product);

    CoordTestExpect(InfiniteCoordTryCopyMultiplyInt64(&product, &value, 0), "умножение на ноль");
    CoordTestExpect(InfiniteCoordSign(&product) == 0, "ноль поглощает");
    InfiniteCoordDestroy(&product);
    InfiniteCoordDestroy(&value);

    // Сдвиг влево обязан пересекать границу лимба.
    CoordSet(&value, 1);
    InfiniteCoord shifted;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &value, 64u), "сдвиг на целый лимб");
    CoordTestExpect(shifted.limbCount == 2 && shifted.limbs[0] == 0 && shifted.limbs[1] == 1,
                    "1 << 64 — это второй лимб");
    InfiniteCoordDestroy(&shifted);

    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &value, 70u), "сдвиг с остатком");
    CoordTestExpect(shifted.limbCount == 2 && shifted.limbs[0] == 0 &&
                        shifted.limbs[1] == (1ull << 6),
                    "1 << 70 — шестой бит второго лимба");
    InfiniteCoordDestroy(&shifted);
    InfiniteCoordDestroy(&value);

    // Сдвиг влево и обратно вправо обязан вернуть исходное число.
    CoordSet(&value, 1234567);
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &value, 40u), "сдвиг туда");
    InfiniteCoord restored;
    CoordTestExpect(InfiniteCoordTryCopyShiftRight(&restored, &shifted, 40u), "сдвиг обратно");
    CoordTestExpect(CoordEqualsInt64(&restored, 1234567), "сдвиг обратим");
    InfiniteCoordDestroy(&restored);
    InfiniteCoordDestroy(&shifted);
    InfiniteCoordDestroy(&value);
}

static void TestDoubleConversion(void)
{
    InfiniteCoord value;
    CoordSet(&value, 0);
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&value) == 0.0, "ноль в double");
    InfiniteCoordDestroy(&value);

    CoordSet(&value, -1234567890123);
    double converted = InfiniteCoordToDoubleSaturating(&value);
    CoordTestExpect(converted == -1234567890123.0, "точное значение до 2^53");
    InfiniteCoordDestroy(&value);

    // Число за пределами double обязано насыщаться, а не превращаться в
    // бесконечность: бесконечность отравила бы дальнейшую арифметику.
    CoordSet(&value, 1);
    InfiniteCoord huge;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&huge, &value, 4000u), "очень большое число");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&huge) == DBL_MAX, "насыщение вверх");
    InfiniteCoord negativeHuge;
    CoordTestExpect(InfiniteCoordTryCopyNegate(&negativeHuge, &huge), "и вниз");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&negativeHuge) == -DBL_MAX, "насыщение вниз");
    InfiniteCoordDestroy(&negativeHuge);
    InfiniteCoordDestroy(&huge);
    InfiniteCoordDestroy(&value);

    // Обратное преобразование усекает к нулю и отвергает не-числа.
    InfiniteCoord fromDouble;
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&fromDouble, 7.9), "double в число");
    CoordTestExpect(CoordEqualsInt64(&fromDouble, 7), "усечение к нулю вверх");
    InfiniteCoordDestroy(&fromDouble);
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&fromDouble, -7.9), "отрицательный double");
    CoordTestExpect(CoordEqualsInt64(&fromDouble, -7), "усечение к нулю вниз");
    InfiniteCoordDestroy(&fromDouble);
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&fromDouble, 0.5), "дробь меньше единицы");
    CoordTestExpect(InfiniteCoordSign(&fromDouble) == 0, "дробь усекается в ноль");
    InfiniteCoordDestroy(&fromDouble);

    // Большое, но представимое: 2^70 обязано пройти круг без потерь.
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&fromDouble, 1180591620717411303424.0),
                    "2^70 из double");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&fromDouble) == 1180591620717411303424.0,
                    "2^70 обратно в double");
    InfiniteCoordDestroy(&fromDouble);

    double notANumber = 0.0;
    uint64_t nanBits = 0x7FF8000000000000ull;
    // Copy one complete IEEE 754 representation; both objects are eight bytes.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memcpy(&notANumber, &nanBits, sizeof(notANumber));
    CoordTestExpect(!InfiniteCoordTrySetFromDouble(&fromDouble, notANumber), "NaN отвергается");
}

static void TestCompareOrder(void)
{
    InfiniteCoord left;
    InfiniteCoord right;
    CoordSet(&left, -5);
    CoordSet(&right, 3);
    CoordTestExpect(InfiniteCoordCompare(&left, &right) < 0, "отрицательное меньше положительного");
    CoordTestExpect(InfiniteCoordCompare(&right, &left) > 0, "и наоборот");
    CoordTestExpect(InfiniteCoordCompare(&left, &left) == 0, "число равно себе");
    InfiniteCoordDestroy(&right);

    // У отрицательных больший модуль означает меньшее число.
    CoordSet(&right, -9);
    CoordTestExpect(InfiniteCoordCompare(&right, &left) < 0, "-9 меньше -5");
    InfiniteCoordDestroy(&right);
    InfiniteCoordDestroy(&left);
}

// Пачкает кучу единицами: выделяет блок ровно нужного размера, заполняет его
// одними единицами и освобождает. Следующее выделение того же размера с
// большой вероятностью получит именно его, и пропущенная запись лимба
// перестанет маскироваться нулями свежей страницы. Приём эвристический и
// санитайзер не заменяет, но без него проверка раскладки проходит по
// случайности: без него подмена «не заполнять младшие лимбы нулями» тестами
// не ловилась вовсе.
static void CoordDirtyHeap(uint32_t limbCount)
{
    InfiniteCoord one;
    CoordSet(&one, 1);
    InfiniteCoord wide;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&wide, &one, limbCount * 64u),
        "заготовка мусора");
    InfiniteCoordDestroy(&one);
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&wide, -1), "заготовка мусора");
    CoordTestExpect(wide.limbCount == limbCount, "заготовка мусора нужной длины");
    // Плотная копия выделяет ровно limbCount лимбов, а не с запасом.
    InfiniteCoord tight;
    CoordTestExpect(InfiniteCoordTryCopyAddInt64(&tight, &wide, 0), "плотная копия мусора");
    InfiniteCoordDestroy(&wide);
    InfiniteCoordDestroy(&tight);
}

// Старший бит теперь ищется инструкцией процессора, а не побитовым циклом.
// Ошибка в индексе смещает результат ровно на степень двойки, поэтому
// проверяются точные значения на границах лимба и слова.
static void TestBitLengthConversion(void)
{
    static const int64_t exact[7] = {
        1, 2, 3, 255, 4294967296LL, 4611686018427387904LL, 9223372036854775807LL,
    };
    static const double expected[7] = {
        1.0, 2.0, 3.0, 255.0, 4294967296.0, 4611686018427387904.0, 9223372036854775807.0,
    };
    for (uint32_t index = 0; index < 7u; ++index)
    {
        InfiniteCoord value;
        CoordSet(&value, exact[index]);
        double converted = InfiniteCoordToDoubleSaturating(&value);
        // 2^63-1 в double округляется до 2^63; остальные представимы точно.
        double reference = index == 6u ? 9223372036854775808.0 : expected[index];
        CoordTestExpect(converted == reference, "перевод в double сместил степень двойки");
        InfiniteCoord negative;
        CoordSet(&negative, exact[index]);
        negative.sign = -1;
        CoordTestExpect(InfiniteCoordToDoubleSaturating(&negative) == -reference,
            "знак при переводе в double потерян");
        InfiniteCoordDestroy(&negative);
        InfiniteCoordDestroy(&value);
    }

    // Ровно 2^64: старший бит лежит в первом лимбе, а младший лимб нулевой.
    InfiniteCoord wide;
    CoordSetTwoPow64(&wide);
    CoordTestExpect(wide.limbCount == 2u && wide.limbs[0] == 0u && wide.limbs[1] == 1u,
        "2^64 собран неверно");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&wide) == 18446744073709551616.0,
        "2^64 в double");

    // 2^1023 ещё представимо, 2^1024 уже нет: там начинается насыщение.
    InfiniteCoord huge;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&huge, &wide, 959u), "сдвиг до 2^1023");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&huge) > 0.0
            && InfiniteCoordToDoubleSaturating(&huge) < DBL_MAX,
        "2^1023 обязано остаться конечным и не насыщенным");
    InfiniteCoord overflowing;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&overflowing, &huge, 2u), "сдвиг за 2^1024");
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&overflowing) == DBL_MAX,
        "за пределом double обязано быть насыщение, а не бесконечность");
    overflowing.sign = -1;
    CoordTestExpect(InfiniteCoordToDoubleSaturating(&overflowing) == -DBL_MAX,
        "насыщение вниз");
    InfiniteCoordDestroy(&overflowing);
    InfiniteCoordDestroy(&huge);
    InfiniteCoordDestroy(&wide);
}

// Целая часть double собирается сразу в нужной раскладке лимбов. Проверяется
// именно раскладка: ошибка в числе лимбов или в границе сдвига даёт число,
// отличающееся ровно на степень 2^64, и сравнение с int64 её не увидит.
static void TestSetFromDoubleLayout(void)
{
    InfiniteCoord value;

    // Меньше единицы — канонический ноль, знак не сохраняется.
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, 0.5), "0.5 принято");
    CoordTestExpect(value.sign == 0 && value.limbCount == 0u, "0.5 обязано дать ноль");
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, -0.75), "-0.75 принято");
    CoordTestExpect(value.sign == 0 && value.limbCount == 0u, "-0.75 обязано дать ноль");

    // Усечение к нулю, а не округление.
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, 1.9), "1.9 принято");
    CoordTestExpect(CoordEqualsInt64(&value, 1), "1.9 усекается до 1");
    InfiniteCoordDestroy(&value);
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, -1.9), "-1.9 принято");
    CoordTestExpect(CoordEqualsInt64(&value, -1), "-1.9 усекается до -1");
    InfiniteCoordDestroy(&value);

    // 2^64: сдвиг не кратен лимбу, младший лимб обнуляется переносом.
    CoordDirtyHeap(2u);
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, 18446744073709551616.0), "2^64");
    CoordTestExpect(value.sign == 1 && value.limbCount == 2u && value.limbs[0] == 0u
            && value.limbs[1] == 1u,
        "2^64 из double собран неверно");
    InfiniteCoordDestroy(&value);

    // 2^116: сдвиг кратен лимбу, младший лимб целиком нулевой.
    CoordDirtyHeap(2u);
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, 83076749736557242056487941267521536.0),
        "2^116");
    CoordTestExpect(value.sign == 1 && value.limbCount == 2u && value.limbs[0] == 0u
            && value.limbs[1] == 4503599627370496ull,
        "2^116 из double собран неверно");
    InfiniteCoordDestroy(&value);

    // 2^128: два пустых лимба снизу, старший бит в третьем.
    CoordDirtyHeap(3u);
    CoordTestExpect(
        InfiniteCoordTrySetFromDouble(&value, 340282366920938463463374607431768211456.0),
        "2^128");
    CoordTestExpect(value.sign == 1 && value.limbCount == 3u && value.limbs[0] == 0u
            && value.limbs[1] == 0u && value.limbs[2] == 1u,
        "2^128 из double собран неверно");
    InfiniteCoordDestroy(&value);

    // Отрицательное значение той же величины отличается только знаком.
    CoordTestExpect(InfiniteCoordTrySetFromDouble(&value, -18446744073709551616.0), "-2^64");
    CoordTestExpect(value.sign == -1 && value.limbCount == 2u && value.limbs[0] == 0u
            && value.limbs[1] == 1u,
        "-2^64 из double собран неверно");
    InfiniteCoordDestroy(&value);

    // Отказ обязан оставить приёмник нетронутым: вызывающий вправе держать
    // там своё значение и не терять его из-за бесконечности на входе.
    InfiniteCoord keeper;
    CoordSet(&keeper, 12345);
    double infinity = DBL_MAX * 2.0;
    CoordTestExpect(!InfiniteCoordTrySetFromDouble(&keeper, infinity), "бесконечность отвергается");
    CoordTestExpect(CoordEqualsInt64(&keeper, 12345), "отказ испортил приёмник");
    CoordTestExpect(!InfiniteCoordTrySetFromDouble(&keeper, -infinity),
        "минус бесконечность отвергается");
    CoordTestExpect(CoordEqualsInt64(&keeper, 12345), "отказ испортил приёмник");
    InfiniteCoordDestroy(&keeper);
}

// Сдвиг влево пишет каждый лимб ровно один раз, поэтому проверяется именно
// раскладка: потерянный перенос между лимбами иначе останется незамеченным.
static void TestShiftLeftLayout(void)
{
    InfiniteCoord one;
    CoordSet(&one, 1);

    InfiniteCoord shifted;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &one, 0u), "сдвиг на ноль");
    CoordTestExpect(shifted.limbCount == 1u && shifted.limbs[0] == 1u, "сдвиг на ноль изменил");
    InfiniteCoordDestroy(&shifted);

    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &one, 64u), "сдвиг на лимб");
    CoordTestExpect(shifted.limbCount == 2u && shifted.limbs[0] == 0u && shifted.limbs[1] == 1u,
        "сдвиг ровно на лимб собран неверно");
    InfiniteCoordDestroy(&shifted);

    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &one, 63u), "сдвиг на 63");
    CoordTestExpect(shifted.limbCount == 1u && shifted.limbs[0] == (1ull << 63),
        "сдвиг на 63 не должен заводить второй лимб");
    InfiniteCoordDestroy(&shifted);
    InfiniteCoordDestroy(&one);

    // Перенос старших бит в следующий лимб при сдвиге, не кратном лимбу.
    InfiniteCoord full;
    CoordSet(&full, (int64_t)0x7fffffffffffffffLL);
    CoordAddInt64(&full, (int64_t)0x7fffffffffffffffLL);
    CoordAddInt64(&full, 1);  // 2^64 - 1, один лимб из одних единиц
    CoordTestExpect(full.limbCount == 1u && full.limbs[0] == 0xffffffffffffffffull,
        "2^64-1 собран неверно");
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &full, 4u), "сдвиг единиц на 4");
    CoordTestExpect(shifted.limbCount == 2u && shifted.limbs[0] == 0xfffffffffffffff0ull
            && shifted.limbs[1] == 0xfull,
        "перенос при сдвиге влево потерян");
    InfiniteCoordDestroy(&shifted);

    // Сдвиг многолимбового значения сразу на лимб и ещё немного.
    CoordDirtyHeap(3u);
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &full, 68u), "сдвиг на 68");
    CoordTestExpect(shifted.limbCount == 3u && shifted.limbs[0] == 0u
            && shifted.limbs[1] == 0xfffffffffffffff0ull && shifted.limbs[2] == 0xfull,
        "сдвиг через границу лимба собран неверно");
    InfiniteCoordDestroy(&shifted);
    InfiniteCoordDestroy(&full);

    // Ноль остаётся нулём при любом сдвиге.
    InfiniteCoord zero;
    InfiniteCoordInit(&zero);
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&shifted, &zero, 130u), "сдвиг нуля");
    CoordTestExpect(shifted.sign == 0 && shifted.limbCount == 0u && shifted.limbs == NULL,
        "сдвиг нуля обязан дать канонический ноль");
    InfiniteCoordDestroy(&shifted);
    InfiniteCoordDestroy(&zero);
}

// Переносы и заёмы через границу лимба вместе с крайними значениями int64.
static void TestCarryBorrowAndInt64Minimum(void)
{
    // Заём из старшего лимба: 2^64 - 1 обязано снова стать одним лимбом.
    InfiniteCoord value;
    CoordSetTwoPow64(&value);
    CoordAddInt64(&value, -1);
    CoordTestExpect(value.limbCount == 1u && value.limbs[0] == 0xffffffffffffffffull
            && value.sign == 1,
        "заём через границу лимба потерян");
    InfiniteCoordDestroy(&value);

    // INT64_MIN как добавка: модуль 2^63 не помещается в положительный int64,
    // и знак обязан обрабатываться отдельно от величины.
    InfiniteCoordInit(&value);
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, INT64_MIN), "0 + INT64_MIN");
    CoordTestExpect(value.sign == -1 && value.limbCount == 1u
            && value.limbs[0] == 9223372036854775808ull,
        "INT64_MIN добавлен неверно");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, INT64_MAX), "прибавление INT64_MAX");
    CoordTestExpect(CoordEqualsInt64(&value, -1), "INT64_MIN + INT64_MAX = -1");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, 1), "прибавление единицы");
    CoordTestExpect(value.sign == 0 && value.limbCount == 0u,
        "погашение до нуля обязано дать канонический ноль");
    InfiniteCoordDestroy(&value);

    // Смена знака через ноль: модуль добавки больше модуля значения.
    CoordSet(&value, 5);
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, -12), "смена знака");
    CoordTestExpect(CoordEqualsInt64(&value, -7), "5 + (-12) = -7");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, INT64_MIN), "-7 + INT64_MIN");
    // -(2^63 + 7): модуль перевалил за половину лимба, но лимб всё ещё один.
    CoordTestExpect(value.sign == -1 && value.limbCount == 1u
            && value.limbs[0] == 9223372036854775815ull,
        "отрицательная добавка за половину лимба посчитана неверно");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, INT64_MAX), "обратно");
    CoordTestExpect(CoordEqualsInt64(&value, -8), "-7 + INT64_MIN + INT64_MAX = -8");
    InfiniteCoordDestroy(&value);

    // Заём через два лимба: 2^128 - 1 обязано дать два лимба из единиц.
    InfiniteCoord one;
    CoordSet(&one, 1);
    InfiniteCoord big;
    CoordTestExpect(InfiniteCoordTryCopyShiftLeft(&big, &one, 128u), "2^128");
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&big, -1), "2^128 - 1");
    CoordTestExpect(big.limbCount == 2u && big.limbs[0] == 0xffffffffffffffffull
            && big.limbs[1] == 0xffffffffffffffffull,
        "заём через два лимба потерян");
    InfiniteCoordDestroy(&big);
    InfiniteCoordDestroy(&one);
}

// Совпадение указателей там, где заголовок его допускает.
static void TestAliasing(void)
{
    // Сложение числа с самим собой: заголовок запрещает совпадение только с
    // приёмником, слагаемые совпадать вправе.
    InfiniteCoord value;
    CoordSetTwoPow64(&value);
    InfiniteCoord doubled;
    CoordTestExpect(InfiniteCoordTryAdd(&doubled, &value, &value), "x + x");
    CoordTestExpect(doubled.limbCount == 2u && doubled.limbs[0] == 0u && doubled.limbs[1] == 2u,
        "сложение значения с собой дало не удвоение");
    InfiniteCoordDestroy(&doubled);

    // Сравнение и равенство со смещением на самом себе.
    CoordTestExpect(InfiniteCoordCompare(&value, &value) == 0, "значение равно себе");
    CoordTestExpect(InfiniteCoordEqualsOffset(&value, &value, 0), "значение равно себе со смещением");
    CoordTestExpect(!InfiniteCoordEqualsOffset(&value, &value, 1), "смещение обязано различать");

    // Обмен значения с самим собой обязан оставить его нетронутым.
    uint64_t* limbs = value.limbs;
    uint32_t limbCount = value.limbCount;
    InfiniteCoordSwap(&value, &value);
    CoordTestExpect(value.limbs == limbs && value.limbCount == limbCount && value.sign == 1,
        "обмен с самим собой испортил значение");

    // Накопление на месте — единственный путь, где приёмник и есть источник.
    CoordTestExpect(InfiniteCoordTryAddInt64InPlace(&value, -1), "накопление на месте");
    CoordTestExpect(value.limbCount == 1u && value.limbs[0] == 0xffffffffffffffffull,
        "накопление на месте испортило значение");
    InfiniteCoordDestroy(&value);
}

LAIUE_TEST_ENTRY(CoordTestEntryPoint)
{
    TestInitAndInt64RoundTrip();
    TestCarryAcrossLimb();
    TestDivFloor();
    TestShiftRight();
    TestSquare();
    TestSubtractToInt64Boundaries();
    TestCompareAddInt64();
    TestEqualsAndOffsets();
    TestHashOffset();
    TestSwap();
    TestFormatShort();
    TestAddAndNegate();
    TestMultiplyAndShiftLeft();
    TestDoubleConversion();
    TestCompareOrder();
    TestBitLengthConversion();
    TestSetFromDoubleLayout();
    TestShiftLeftLayout();
    TestCarryBorrowAndInt64Minimum();
    TestAliasing();

    CoordTestWrite("Проверок пройдено: ");
    CoordTestWriteNumber(coordTestChecks);
    CoordTestWrite("\r\n");
    LAIUE_TEST_SUCCESS();
}
