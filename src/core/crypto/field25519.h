#pragma once

/* Поле 2^255 - 19, общее для X25519 и Ed25519. Внутренний заголовок
 * криптографии движка.
 *
 * Элемент поля — пять разрядов по 51 биту в uint64_t: разряд i весит
 * 2^(51i). Произведение двух разрядов занимает до 108 бит, и суммы таких
 * произведений копятся в 128-битном Wide. Разряды всегда неотрицательны:
 * вычитание прибавляет 2p, поэтому знаковых сдвигов нет.
 *
 * «Почти приведённый» элемент — выход умножения и возведения в квадрат: все
 * разряды меньше 2^51, кроме второго, который меньше 2^51 + 2^13. Сумма двух
 * таких элементов и разность почти приведённых дают разряды меньше 2^53, а
 * умножение принимает разряды до 2^54: в сумме для разряда пять
 * произведений, и у четырёх из них множитель 19, поэтому она меньше 2^115, а
 * перенос из неё помещается в 64 бита.
 *
 * Время работы не зависит от значений: нет ни ветвлений, ни обращений к
 * памяти по ним. */

#include "crypto/wide.h"
#include "crypto/wipe.h"

#include <stdbool.h>
#include <stdint.h>

#define FIELD_MASK ((UINT64_C(1) << 51u) - 1u)
#define FIELD_BYTES 32u

typedef uint64_t Field[5];

static inline uint64_t FieldLoad64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 8u; index > 0u; --index)
        value = (value << 8u) | bytes[index - 1u];
    return value;
}

static inline void FieldStore64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (8u * index));
}

/* Суммы произведений для пяти разрядов становятся почти приведённым
 * элементом. Каждая сумма меньше 2^115. */
static inline void FieldReduce(Field output, Wide t0, Wide t1, Wide t2, Wide t3, Wide t4)
{
    t1 = WideAddSmall(t1, WideShift(t0, 51u));
    t2 = WideAddSmall(t2, WideShift(t1, 51u));
    t3 = WideAddSmall(t3, WideShift(t2, 51u));
    t4 = WideAddSmall(t4, WideShift(t3, 51u));
    /* 2^255 = 19 по модулю p. Сумма старшего разряда — не больше пяти
     * произведений без множителя 19, перенос из неё меньше 5 * 2^57 + 2^13,
     * и младший разряд с ним остаётся меньше 2^64. */
    const uint64_t low = (WideLow(t0) & FIELD_MASK) + 19u * WideShift(t4, 51u);
    output[0] = low & FIELD_MASK;
    output[1] = (WideLow(t1) & FIELD_MASK) + (low >> 51u);
    output[2] = WideLow(t2) & FIELD_MASK;
    output[3] = WideLow(t3) & FIELD_MASK;
    output[4] = WideLow(t4) & FIELD_MASK;
}

static inline void FieldAdd(Field output, const Field left, const Field right)
{
    for (uint32_t index = 0u; index < 5u; ++index)
        output[index] = left[index] + right[index];
}

/* left + 2p - right. right обязан быть почти приведённым: тогда каждый его
 * разряд не больше разряда числа 2p. */
static inline void FieldSubtract(Field output, const Field left, const Field right)
{
    output[0] = left[0] + ((UINT64_C(1) << 52u) - 38u) - right[0];
    for (uint32_t index = 1u; index < 5u; ++index)
        output[index] = left[index] + ((UINT64_C(1) << 52u) - 2u) - right[index];
}

static inline void FieldMultiply(Field output, const Field left, const Field right)
{
    const uint64_t f0 = left[0];
    const uint64_t f1 = left[1];
    const uint64_t f2 = left[2];
    const uint64_t f3 = left[3];
    const uint64_t f4 = left[4];
    const uint64_t g0 = right[0];
    const uint64_t g1 = right[1];
    const uint64_t g2 = right[2];
    const uint64_t g3 = right[3];
    const uint64_t g4 = right[4];
    /* Произведения с весом 2^255 и выше сворачиваются множителем 19. */
    const uint64_t g1x19 = 19u * g1;
    const uint64_t g2x19 = 19u * g2;
    const uint64_t g3x19 = 19u * g3;
    const uint64_t g4x19 = 19u * g4;
    Wide t0 = WideMultiply(f0, g0);
    t0 = WideMultiplyAdd(t0, f1, g4x19);
    t0 = WideMultiplyAdd(t0, f2, g3x19);
    t0 = WideMultiplyAdd(t0, f3, g2x19);
    t0 = WideMultiplyAdd(t0, f4, g1x19);
    Wide t1 = WideMultiply(f0, g1);
    t1 = WideMultiplyAdd(t1, f1, g0);
    t1 = WideMultiplyAdd(t1, f2, g4x19);
    t1 = WideMultiplyAdd(t1, f3, g3x19);
    t1 = WideMultiplyAdd(t1, f4, g2x19);
    Wide t2 = WideMultiply(f0, g2);
    t2 = WideMultiplyAdd(t2, f1, g1);
    t2 = WideMultiplyAdd(t2, f2, g0);
    t2 = WideMultiplyAdd(t2, f3, g4x19);
    t2 = WideMultiplyAdd(t2, f4, g3x19);
    Wide t3 = WideMultiply(f0, g3);
    t3 = WideMultiplyAdd(t3, f1, g2);
    t3 = WideMultiplyAdd(t3, f2, g1);
    t3 = WideMultiplyAdd(t3, f3, g0);
    t3 = WideMultiplyAdd(t3, f4, g4x19);
    Wide t4 = WideMultiply(f0, g4);
    t4 = WideMultiplyAdd(t4, f1, g3);
    t4 = WideMultiplyAdd(t4, f2, g2);
    t4 = WideMultiplyAdd(t4, f3, g1);
    t4 = WideMultiplyAdd(t4, f4, g0);
    FieldReduce(output, t0, t1, t2, t3, t4);
}

/* Квадрат: пятнадцать произведений вместо двадцати пяти. */
static inline void FieldSquare(Field output, const Field value)
{
    const uint64_t f0 = value[0];
    const uint64_t f1 = value[1];
    const uint64_t f2 = value[2];
    const uint64_t f3 = value[3];
    const uint64_t f4 = value[4];
    const uint64_t f0x2 = 2u * f0;
    const uint64_t f1x2 = 2u * f1;
    const uint64_t f2x2 = 2u * f2;
    const uint64_t f3x2 = 2u * f3;
    const uint64_t f3x19 = 19u * f3;
    const uint64_t f4x19 = 19u * f4;
    Wide t0 = WideMultiply(f0, f0);
    t0 = WideMultiplyAdd(t0, f1x2, f4x19);
    t0 = WideMultiplyAdd(t0, f2x2, f3x19);
    Wide t1 = WideMultiply(f0x2, f1);
    t1 = WideMultiplyAdd(t1, f2x2, f4x19);
    t1 = WideMultiplyAdd(t1, f3, f3x19);
    Wide t2 = WideMultiply(f0x2, f2);
    t2 = WideMultiplyAdd(t2, f1, f1);
    t2 = WideMultiplyAdd(t2, f3x2, f4x19);
    Wide t3 = WideMultiply(f0x2, f3);
    t3 = WideMultiplyAdd(t3, f1x2, f2);
    t3 = WideMultiplyAdd(t3, f4, f4x19);
    Wide t4 = WideMultiply(f0x2, f4);
    t4 = WideMultiplyAdd(t4, f1x2, f3);
    t4 = WideMultiplyAdd(t4, f2, f2);
    FieldReduce(output, t0, t1, t2, t3, t4);
}

/* Слабое приведение без умножений: разряды меньше 2^63 становятся почти
 * приведённым элементом. Нужно сумме, которую вычитают или упаковывают. */
static inline void FieldCarry(Field value)
{
    value[1] += value[0] >> 51u;
    value[0] &= FIELD_MASK;
    value[2] += value[1] >> 51u;
    value[1] &= FIELD_MASK;
    value[3] += value[2] >> 51u;
    value[2] &= FIELD_MASK;
    value[4] += value[3] >> 51u;
    value[3] &= FIELD_MASK;
    /* Перенос из старшего разряда меньше 2^13, с множителем 19 — меньше
     * 2^18, поэтому младший разряд отдаёт второму не больше единицы. */
    value[0] += 19u * (value[4] >> 51u);
    value[4] &= FIELD_MASK;
    value[1] += value[0] >> 51u;
    value[0] &= FIELD_MASK;
}

/* -value для почти приведённого value; результат тоже почти приведён. */
static inline void FieldNegate(Field output, const Field value)
{
    const Field zero = {0u};
    FieldSubtract(output, zero, value);
    FieldCarry(output);
}

/* value^(2^count), count не меньше единицы. */
static inline void FieldSquareTimes(Field output, const Field value, uint32_t count)
{
    FieldSquare(output, value);
    for (uint32_t index = 1u; index < count; ++index)
        FieldSquare(output, output);
}

/* Обмен без ветвления: swap равен 0 или 1. */
static inline void FieldSwap(Field left, Field right, uint64_t swap)
{
    const uint64_t mask = 0u - swap;
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        const uint64_t difference = mask & (left[index] ^ right[index]);
        left[index] ^= difference;
        right[index] ^= difference;
    }
}

/* Общее начало обращения и квадратного корня: ones250 = value^(2^250 - 1) и
 * power11 = value^11. onesN — value^(2^N - 1), показатель из N единиц. */
static inline void FieldPowerOnes250(Field ones250, Field power11, const Field value)
{
    Field square;
    Field power9;
    Field ones5;
    Field ones10;
    Field ones20;
    Field ones50;
    Field ones100;
    Field power;
    FieldSquare(square, value);
    FieldSquareTimes(power, square, 2u);
    FieldMultiply(power9, power, value);
    FieldMultiply(power11, power9, square);
    FieldSquare(power, power11);
    FieldMultiply(ones5, power, power9);
    FieldSquareTimes(power, ones5, 5u);
    FieldMultiply(ones10, power, ones5);
    FieldSquareTimes(power, ones10, 10u);
    FieldMultiply(ones20, power, ones10);
    FieldSquareTimes(power, ones20, 20u);
    FieldMultiply(power, power, ones20);
    FieldSquareTimes(power, power, 10u);
    FieldMultiply(ones50, power, ones10);
    FieldSquareTimes(power, ones50, 50u);
    FieldMultiply(ones100, power, ones50);
    FieldSquareTimes(power, ones100, 100u);
    FieldMultiply(power, power, ones100);
    FieldSquareTimes(power, power, 50u);
    FieldMultiply(ones250, power, ones50);
    CryptoWipe(square, sizeof(Field));
    CryptoWipe(power9, sizeof(Field));
    CryptoWipe(ones5, sizeof(Field));
    CryptoWipe(ones10, sizeof(Field));
    CryptoWipe(ones20, sizeof(Field));
    CryptoWipe(ones50, sizeof(Field));
    CryptoWipe(ones100, sizeof(Field));
    CryptoWipe(power, sizeof(Field));
}

/* value^(p - 2) = value^(2^255 - 21): 254 возведения в квадрат и 11
 * умножений. */
static inline void FieldInvert(Field output, const Field value)
{
    Field ones250;
    Field power11;
    FieldPowerOnes250(ones250, power11, value);
    /* 2^255 - 32, затем + 11. */
    FieldSquareTimes(ones250, ones250, 5u);
    FieldMultiply(output, ones250, power11);
    CryptoWipe(ones250, sizeof(Field));
    CryptoWipe(power11, sizeof(Field));
}

/* value^((p - 5) / 8) = value^(2^252 - 3): основа квадратного корня. */
static inline void FieldPower22523(Field output, const Field value)
{
    Field ones250;
    Field power11;
    FieldPowerOnes250(ones250, power11, value);
    /* 2^252 - 4, затем + 1. */
    FieldSquareTimes(ones250, ones250, 2u);
    FieldMultiply(output, ones250, value);
    CryptoWipe(ones250, sizeof(Field));
    CryptoWipe(power11, sizeof(Field));
}

/* Старший бит 255 отбрасывается маской старшего разряда: в X25519 он не
 * используется, а в точке Ed25519 хранит знак x. */
static inline void FieldUnpack(Field output, const uint8_t bytes[FIELD_BYTES])
{
    /* Разряд i начинается с бита 51i. */
    output[0] = FieldLoad64(bytes) & FIELD_MASK;
    output[1] = (FieldLoad64(bytes + 6u) >> 3u) & FIELD_MASK;
    output[2] = (FieldLoad64(bytes + 12u) >> 6u) & FIELD_MASK;
    output[3] = (FieldLoad64(bytes + 19u) >> 1u) & FIELD_MASK;
    output[4] = (FieldLoad64(bytes + 24u) >> 12u) & FIELD_MASK;
}

/* Канонический вид: значение строго меньше p. value почти приведён, то есть
 * меньше 2^255 + 2^64 и тем более меньше 2p. */
static inline void FieldPack(uint8_t bytes[FIELD_BYTES], const Field value)
{
    uint64_t f0 = value[0];
    uint64_t f1 = value[1];
    uint64_t f2 = value[2];
    uint64_t f3 = value[3];
    uint64_t f4 = value[4];
    /* Цепочка переносов числа value + 19 даёт его бит 2^255: единица ровно
     * тогда, когда value не меньше p, и тогда p вычитается один раз —
     * прибавлением 19 и отбрасыванием этого бита. */
    uint64_t carry = (f0 + 19u) >> 51u;
    carry = (f1 + carry) >> 51u;
    carry = (f2 + carry) >> 51u;
    carry = (f3 + carry) >> 51u;
    carry = (f4 + carry) >> 51u;
    f0 += 19u * carry;
    f1 += f0 >> 51u;
    f0 &= FIELD_MASK;
    f2 += f1 >> 51u;
    f1 &= FIELD_MASK;
    f3 += f2 >> 51u;
    f2 &= FIELD_MASK;
    f4 += f3 >> 51u;
    f3 &= FIELD_MASK;
    f4 &= FIELD_MASK;
    FieldStore64(bytes, f0 | (f1 << 51u));
    FieldStore64(bytes + 8u, (f1 >> 13u) | (f2 << 38u));
    FieldStore64(bytes + 16u, (f2 >> 26u) | (f3 << 25u));
    FieldStore64(bytes + 24u, (f3 >> 39u) | (f4 << 12u));
}

/* Сравнения ниже упаковывают значение, поэтому тоже требуют почти
 * приведённых аргументов. */

static inline bool FieldIsZero(const Field value)
{
    uint8_t bytes[FIELD_BYTES];
    FieldPack(bytes, value);
    uint8_t any = 0u;
    for (uint32_t index = 0u; index < FIELD_BYTES; ++index)
        any |= bytes[index];
    return any == 0u;
}

static inline bool FieldEqual(const Field left, const Field right)
{
    uint8_t leftBytes[FIELD_BYTES];
    uint8_t rightBytes[FIELD_BYTES];
    FieldPack(leftBytes, left);
    FieldPack(rightBytes, right);
    uint8_t difference = 0u;
    for (uint32_t index = 0u; index < FIELD_BYTES; ++index)
        difference |= (uint8_t)(leftBytes[index] ^ rightBytes[index]);
    return difference == 0u;
}

/* Знак по RFC 8032: младший бит канонического вида, 0 или 1. */
static inline uint8_t FieldIsNegative(const Field value)
{
    uint8_t bytes[FIELD_BYTES];
    FieldPack(bytes, value);
    return (uint8_t)(bytes[0] & 1u);
}
