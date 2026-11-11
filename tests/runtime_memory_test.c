/* memset, memcpy, memmove и memcmp сборки Windows без CRT — функции, которые
 * компилятор вызывает сам. Все длины вокруг 16-байтных порций и переходов
 * между способами копирования, все выравнивания, перекрытия в обе стороны,
 * знак сравнения. */

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SPAN 1100u
#define AREA (2u * SPAN + 128u)

/* Вызовы через volatile-указатели: иначе компилятор развернул бы их сам и
 * проверялся бы он, а не runtime. */
static void *(*volatile setMemory)(void *, int, size_t) = memset;
static void *(*volatile copyMemory)(void *, const void *, size_t) = memcpy;
static void *(*volatile moveMemory)(void *, const void *, size_t) = memmove;
static int (*volatile compareMemory)(const void *, const void *, size_t) = memcmp;

static uint8_t source[AREA];
static uint8_t target[AREA];
static uint8_t expected[AREA];
static uint8_t saved[AREA];
static uint32_t randomState = UINT32_C(0x9E3779B9);

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static uint8_t NextByte(void)
{
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 17u;
    randomState ^= randomState << 5u;
    return (uint8_t)(randomState >> 24u);
}

static void FillRandom(uint8_t *bytes, size_t size)
{
    for (size_t index = 0u; index < size; ++index)
        bytes[index] = NextByte();
}

/* Эталонные циклы пишут через volatile: компилятор не заменит их вызовом
 * той функции, которую проверяет тест. */
static void ReferenceCopy(volatile uint8_t *output, const uint8_t *input, size_t size)
{
    for (size_t index = 0u; index < size; ++index)
        output[index] = input[index];
}

static void ReferenceFill(volatile uint8_t *output, uint8_t value, size_t size)
{
    for (size_t index = 0u; index < size; ++index)
        output[index] = value;
}

static bool Same(const uint8_t *first, const uint8_t *second, size_t size)
{
    for (size_t index = 0u; index < size; ++index)
    {
        if (first[index] != second[index])
            return false;
    }
    return true;
}

/* Все длины до 70 байт, затем реже: каждая граница 16-байтных порций, хвосты
 * любой длины и переходы между короткими длинами, циклом и rep-инструкциями
 * около 32 и 512 байт. */
static size_t NextSize(size_t size)
{
    if (size < 70u)
        return size + 1u;
    return size < 600u ? size + 13u : size + 97u;
}

static void TestSet(void)
{
    for (size_t offset = 0u; offset < 16u; ++offset)
    {
        for (size_t size = 0u; size <= SPAN; size = NextSize(size))
        {
            const size_t region = offset + size + 16u;
            /* Значение шире байта: пишется только младший байт. */
            const int value = (int)(0x100u + size * 37u + offset);
            FillRandom(target, region);
            ReferenceCopy(expected, target, region);
            ReferenceFill(expected + offset, (uint8_t)value, size);
            Expect(setMemory(target + offset, value, size) == target + offset,
                   "memset returns its destination");
            Expect(Same(target, expected, region), "memset fills exactly its range");
        }
    }
}

static void TestCopy(void)
{
    for (size_t to = 0u; to < 16u; ++to)
    {
        for (size_t from = 0u; from < 16u; ++from)
        {
            for (size_t size = 0u; size <= SPAN; size = NextSize(size))
            {
                const size_t region = size + 32u;
                FillRandom(source, region);
                FillRandom(target, region);
                ReferenceCopy(expected, target, region);
                ReferenceCopy(expected + to, source + from, size);
                Expect(copyMemory(target + to, source + from, size) == target + to,
                       "memcpy returns its destination");
                Expect(Same(target, expected, region), "memcpy copies exactly its range");
            }
        }
    }
}

/* Сдвиги в обе стороны внутри одного буфера: перекрытие на любую долю
 * длины, касание и непересекающиеся области. */
static void TestMove(void)
{
    const size_t from = 64u;
    for (size_t shift = 0u; shift <= 96u; ++shift)
    {
        const size_t to = from - 48u + shift;
        for (size_t size = 0u; size <= SPAN; size = NextSize(size))
        {
            const size_t region = from + 64u + size;
            FillRandom(target, region);
            ReferenceCopy(expected, target, region);
            ReferenceCopy(saved, target + from, size);
            ReferenceCopy(expected + to, saved, size);
            Expect(moveMemory(target + to, target + from, size) == target + to,
                   "memmove returns its destination");
            Expect(Same(target, expected, region), "memmove copies overlapping ranges");
        }
    }
    for (size_t size = 0u; size <= SPAN; size = NextSize(size))
    {
        const size_t region = size + 16u;
        FillRandom(source, region);
        FillRandom(target, region);
        ReferenceCopy(expected, target, region);
        ReferenceCopy(expected + 3u, source + 5u, size);
        Expect(moveMemory(target + 3u, source + 5u, size) == target + 3u,
               "memmove returns its destination");
        Expect(Same(target, expected, region), "memmove copies separate ranges");
    }
}

static int Sign(int value)
{
    return (value > 0) - (value < 0);
}

/* Решает первый различающийся байт, сравнённый без знака; последующие
 * различия с обратным знаком, в том же слове или дальше, не мешают. Пары
 * различаются в разных младших битах: порядок задаёт байт, а не бит. */
static void ExpectOrder(uint8_t *left, uint8_t *right, size_t size, size_t first)
{
    static const uint8_t pairs[6][2] = {{0x80u, 0x7Fu}, {0x80u, 0x00u}, {0x04u, 0x00u},
                                        {0x30u, 0x10u}, {0xC0u, 0x40u}, {0x09u, 0x01u}};
    FillRandom(left, size);
    ReferenceCopy(right, left, size);
    for (size_t later = first + 1u; later < size; later += 3u)
    {
        left[later] = 0x00u;
        right[later] = 0xFFu;
    }
    left[first] = pairs[first % 6u][0];
    right[first] = pairs[first % 6u][1];
    Expect(Sign(compareMemory(left, right, size)) == 1, "memcmp compares bytes without sign");
    Expect(Sign(compareMemory(right, left, size)) == -1, "memcmp is antisymmetric");
    Expect(compareMemory(left, right, first) == 0, "memcmp stops at its length");
}

static void TestCompare(void)
{
    for (size_t offset = 0u; offset < 16u; ++offset)
    {
        uint8_t *left = source + offset;
        uint8_t *right = target + 15u - offset;
        for (size_t size = 0u; size <= SPAN; size = NextSize(size))
        {
            FillRandom(left, size);
            ReferenceCopy(right, left, size);
            Expect(compareMemory(left, right, size) == 0, "memcmp finds equal ranges equal");
            if (size == 0u)
                continue;
            /* Короткие длины — различие в каждой позиции: на границах слов и
             * порций и в хвостах. */
            if (size <= 40u)
            {
                for (size_t first = 0u; first < size; ++first)
                    ExpectOrder(left, right, size, first);
                continue;
            }
            ExpectOrder(left, right, size, 0u);
            ExpectOrder(left, right, size, size - 1u);
            const size_t high = NextByte();
            ExpectOrder(left, right, size, (high << 8u | NextByte()) % size);
        }
    }
}

LAIUE_TEST_ENTRY(RuntimeMemoryTestEntryPoint)
{
    TestSet();
    TestCopy();
    TestMove();
    TestCompare();
    LaiueTestRuntimeWrite("runtime memory: passed\n");
    LAIUE_TEST_SUCCESS();
}
