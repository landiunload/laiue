#pragma once

#include <stdint.h>

// Беззнаковое целое фиксированной ширины для АБСОЛЮТНОЙ координаты мира.
// 80 лимбов по 64 бита = 5120 бит -> диапазон до ~10^1541 (>= требуемых 10^1488).
// Используется ТОЛЬКО для origin мира (холодный путь): рендер/меш/физика/стриминг
// работают в int64 локально относительно origin, здесь bignum не участвует.
#define BIGCOORD_LIMBS 80

typedef struct BigCoord
{
    uint64_t limb[BIGCOORD_LIMBS];  // little-endian: limb[0] — младший
} BigCoord;

// 64x64->128 умножение и 128/64 деление: переносимо между cl.exe, clang-cl и gcc.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint64_t BigCoordMulHigh(uint64_t a, uint64_t b, uint64_t* low)
{
    uint64_t high;
    *low = _umul128(a, b, &high);
    return high;
}
static inline uint64_t BigCoordDiv128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t* remainder)
{
    return _udiv128(high, low, divisor, remainder);
}
#else
static inline uint64_t BigCoordMulHigh(uint64_t a, uint64_t b, uint64_t* low)
{
    unsigned __int128 product = (unsigned __int128)a * b;
    *low = (uint64_t)product;
    return (uint64_t)(product >> 64);
}
static inline uint64_t BigCoordDiv128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t* remainder)
{
    unsigned __int128 numerator = ((unsigned __int128)high << 64) | low;
    *remainder = (uint64_t)(numerator % divisor);
    return (uint64_t)(numerator / divisor);
}
#endif

static inline BigCoord BigCoordZero(void)
{
    BigCoord value;
    for (int32_t i = 0; i < BIGCOORD_LIMBS; ++i) value.limb[i] = 0;
    return value;
}

// value = value * multiplier + addend (оба помещаются в 64 бита).
// Перенос за старший лимб отбрасывается (выход за диапазон недостижим на практике).
static inline void BigCoordMulAddSmall(BigCoord* value, uint64_t multiplier, uint64_t addend)
{
    uint64_t carry = addend;
    for (int32_t i = 0; i < BIGCOORD_LIMBS; ++i)
    {
        uint64_t low;
        uint64_t high = BigCoordMulHigh(value->limb[i], multiplier, &low);
        uint64_t sum = low + carry;
        uint64_t carryFromSum = (sum < low) ? 1u : 0u;
        value->limb[i] = sum;
        carry = high + carryFromSum;
    }
}

// value := value / divisor (на месте), возвращает остаток (< divisor).
static inline uint64_t BigCoordDivSmall(BigCoord* value, uint64_t divisor)
{
    uint64_t remainder = 0;
    for (int32_t i = BIGCOORD_LIMBS - 1; i >= 0; --i)
    {
        value->limb[i] = BigCoordDiv128(remainder, value->limb[i], divisor, &remainder);
    }
    return remainder;
}

// value := value - subtrahend (subtrahend < 2^64; предполагается value >= subtrahend).
static inline void BigCoordSubSmall(BigCoord* value, uint64_t subtrahend)
{
    uint64_t borrow = subtrahend;
    for (int32_t i = 0; i < BIGCOORD_LIMBS && borrow != 0; ++i)
    {
        uint64_t old = value->limb[i];
        value->limb[i] = old - borrow;
        borrow = (old < borrow) ? 1u : 0u;
    }
}

// Возвращает a - b (предполагается a >= b).
static inline BigCoord BigCoordSub(BigCoord a, BigCoord b)
{
    uint64_t borrow = 0;
    for (int32_t i = 0; i < BIGCOORD_LIMBS; ++i)
    {
        uint64_t afterBorrow = a.limb[i] - borrow;
        uint64_t borrow1 = (a.limb[i] < borrow) ? 1u : 0u;
        a.limb[i] = afterBorrow - b.limb[i];
        uint64_t borrow2 = (afterBorrow < b.limb[i]) ? 1u : 0u;
        borrow = borrow1 + borrow2;  // одновременно 1 быть не может: borrow1=1 => afterBorrow=MAX >= b
    }
    return a;
}

// Помещается ли значение в неотрицательный int64. Если да — кладёт его в *low.
static inline int32_t BigCoordFitsInt64(BigCoord value, uint64_t* low)
{
    for (int32_t i = 1; i < BIGCOORD_LIMBS; ++i)
    {
        if (value.limb[i] != 0) { *low = 0; return 0; }
    }
    *low = value.limb[0];
    return value.limb[0] <= (uint64_t)INT64_MAX;
}

// value := value + addend.
static inline void BigCoordAddSmall(BigCoord* value, uint64_t addend)
{
    uint64_t carry = addend;
    for (int32_t i = 0; i < BIGCOORD_LIMBS && carry != 0; ++i)
    {
        uint64_t sum = value->limb[i] + carry;
        carry = (sum < value->limb[i]) ? 1u : 0u;
        value->limb[i] = sum;
    }
}

// 10^exponent как BigCoord.
static inline BigCoord BigCoordPowTen(uint32_t exponent)
{
    BigCoord value = BigCoordZero();
    value.limb[0] = 1;
    for (uint32_t i = 0; i < exponent; ++i)
    {
        BigCoordMulAddSmall(&value, 10, 0);
    }
    return value;
}
