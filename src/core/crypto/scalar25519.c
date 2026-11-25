#include "crypto/scalar25519.h"

#include "crypto/wide.h"
#include "crypto/wipe.h"

#include <stddef.h>

/* Скаляр считается разрядами по 64 бита. Приведение — алгоритм Барретта
 * (HAC 14.42) с основанием b = 2^64 и k = 4: без делений и ветвлений. */

static const uint64_t scalarOrder[4] = {
    UINT64_C(0x5812631a5cf5d3ed),
    UINT64_C(0x14def9dea2f79cd6),
    UINT64_C(0x0000000000000000),
    UINT64_C(0x1000000000000000),
};

/* mu = floor(2^512 / L): 260 бит. */
static const uint64_t scalarBarrett[5] = {
    UINT64_C(0xed9ce5a30a2c131b), UINT64_C(0x2106215d086329a7), UINT64_C(0xffffffffffffffeb),
    UINT64_C(0xffffffffffffffff), UINT64_C(0x000000000000000f),
};

static uint64_t Load64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 8u; index > 0u; --index)
        value = (value << 8u) | bytes[index - 1u];
    return value;
}

static void Store64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (8u * index));
}

/* left - right - borrow; borrow становится 1, если вычитание заняло. Заём —
 * старший бит выражения, а не сравнение. */
static uint64_t SubtractBorrow(uint64_t left, uint64_t right, uint64_t *borrow)
{
    const uint64_t difference = left - right - *borrow;
    *borrow = ((~left & right) | (~(left ^ right) & difference)) >> 63u;
    return difference;
}

/* Вычитает L, если value не меньше L. value — пять разрядов. */
static void SubtractOrder(uint64_t value[5])
{
    uint64_t difference[5];
    uint64_t borrow = 0u;
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        const uint64_t order = index < 4u ? scalarOrder[index] : 0u;
        difference[index] = SubtractBorrow(value[index], order, &borrow);
    }
    /* Заём — value меньше L и остаётся прежним. */
    const uint64_t keep = 0u - borrow;
    for (uint32_t index = 0u; index < 5u; ++index)
        value[index] = (value[index] & keep) | (difference[index] & ~keep);
    CryptoWipe(difference, sizeof(difference));
}

/* value mod L для value < 2^512, записанного восемью разрядами. */
static void Reduce(uint8_t output[CRYPTO_SCALAR_SIZE], const uint64_t value[8])
{
    /* q1 = value / b^3 — разряды 3..7; q3 = q1 * mu / b^5. */
    uint64_t product[10] = {0u};
    for (uint32_t row = 0u; row < 5u; ++row)
    {
        uint64_t carry = 0u;
        for (uint32_t column = 0u; column < 5u; ++column)
        {
            Wide sum = WideMultiply(value[3u + row], scalarBarrett[column]);
            sum = WideAddSmall(sum, product[row + column]);
            sum = WideAddSmall(sum, carry);
            product[row + column] = WideLow(sum);
            carry = WideHigh(sum);
        }
        product[row + 5u] = carry;
    }
    const uint64_t *quotient = product + 5u;
    /* r2 = q3 * L mod b^5: только пять младших разрядов произведения. */
    uint64_t multiple[5] = {0u};
    for (uint32_t row = 0u; row < 5u; ++row)
    {
        uint64_t carry = 0u;
        for (uint32_t column = 0u; column < 4u && row + column < 5u; ++column)
        {
            Wide sum = WideMultiply(quotient[row], scalarOrder[column]);
            sum = WideAddSmall(sum, multiple[row + column]);
            sum = WideAddSmall(sum, carry);
            multiple[row + column] = WideLow(sum);
            carry = WideHigh(sum);
        }
        if (row == 0u)
            multiple[4] = carry;
    }
    /* r = r1 - r2 mod b^5, где r1 = value mod b^5. HAC 14.43 обещает r < 3L,
     * но для этого L оценка частного ошибается не больше чем на единицу:
     * отброшенные разряды value и дробная часть 2^512 / L отнимают от
     * value / L меньше 0,23. Поэтому r < 2L, и хватает одного вычитания. */
    uint64_t remainder[5];
    uint64_t borrow = 0u;
    for (uint32_t index = 0u; index < 5u; ++index)
        remainder[index] = SubtractBorrow(value[index], multiple[index], &borrow);
    SubtractOrder(remainder);
    for (size_t index = 0u; index < 4u; ++index)
        Store64(output + 8u * index, remainder[index]);
    CryptoWipe(product, sizeof(product));
    CryptoWipe(multiple, sizeof(multiple));
    CryptoWipe(remainder, sizeof(remainder));
}

void CryptoScalarReduce(uint8_t output[CRYPTO_SCALAR_SIZE],
                        const uint8_t value[CRYPTO_SCALAR_WIDE_SIZE])
{
    uint64_t limbs[8];
    for (size_t index = 0u; index < 8u; ++index)
        limbs[index] = Load64(value + 8u * index);
    Reduce(output, limbs);
    CryptoWipe(limbs, sizeof(limbs));
}

void CryptoScalarMultiplyAdd(uint8_t output[CRYPTO_SCALAR_SIZE],
                             const uint8_t left[CRYPTO_SCALAR_SIZE],
                             const uint8_t right[CRYPTO_SCALAR_SIZE],
                             const uint8_t addend[CRYPTO_SCALAR_SIZE])
{
    uint64_t a[4];
    uint64_t b[4];
    uint64_t value[8] = {0u};
    for (size_t index = 0u; index < 4u; ++index)
    {
        a[index] = Load64(left + 8u * index);
        b[index] = Load64(right + 8u * index);
        value[index] = Load64(addend + 8u * index);
    }
    /* Слагаемое уже лежит в младших разрядах, и строки произведения
     * прибавляются к нему с переносом. Разряд row + 4 до строки row никто не
     * трогал, поэтому её последний перенос просто записывается туда. Сумма
     * меньше (2^256 - 1)^2 + 2^256 < 2^512. */
    for (uint32_t row = 0u; row < 4u; ++row)
    {
        uint64_t carry = 0u;
        for (uint32_t column = 0u; column < 4u; ++column)
        {
            Wide sum = WideMultiply(a[row], b[column]);
            sum = WideAddSmall(sum, value[row + column]);
            sum = WideAddSmall(sum, carry);
            value[row + column] = WideLow(sum);
            carry = WideHigh(sum);
        }
        value[row + 4u] = carry;
    }
    Reduce(output, value);
    CryptoWipe(a, sizeof(a));
    CryptoWipe(b, sizeof(b));
    CryptoWipe(value, sizeof(value));
}

bool CryptoScalarIsCanonical(const uint8_t scalar[CRYPTO_SCALAR_SIZE])
{
    uint64_t borrow = 0u;
    for (size_t index = 0u; index < 4u; ++index)
        (void)SubtractBorrow(Load64(scalar + 8u * index), scalarOrder[index], &borrow);
    return borrow != 0u;
}
