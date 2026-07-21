#include <windows.h>

#include "world/infinite_coord.h"

// InfiniteCoord — знаковое целое произвольной точности, на котором держится
// бесконечный мир. Логика чистая и детерминированная, поэтому infinite_coord.c
// компилируется прямо в тест: символы не экспортируются из laiue_world, а
// расширять ABI DLL ради проверок нельзя (тот же приём, что в protocol_test).
//
// Структура InfiniteCoord публична, поэтому переносы через границу лимба
// (2^64) проверяются прямым сравнением limbs/limbCount/sign, а не косвенно.

static uint32_t coordTestChecks;

// Тест собирается без CRT: вывод — прямая запись в stdout.
static void CoordTestWrite(const char* text)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
    {
        return;
    }
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    DWORD written = 0;
    WriteFile(output, text, length, &written, NULL);
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
    ExitProcess(1);
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

void CoordTestEntryPoint(void)
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

    CoordTestWrite("Проверок пройдено: ");
    CoordTestWriteNumber(coordTestChecks);
    CoordTestWrite("\r\n");
    ExitProcess(0);
}
