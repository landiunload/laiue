#pragma once

#include <stdint.h>

// Беззнаковое целое фиксированной ширины для АБСОЛЮТНОЙ координаты мира.
// 520 лимбов по 64 бита = 33280 бит -> диапазон до ~10^10018.
// Используется ТОЛЬКО для origin мира (холодный путь): рендер/меш/физика/стриминг
// работают в int64 локально относительно origin, здесь bignum не участвует.
#define BIGCOORD_LIMBS 520

typedef struct BigCoord
{
    uint64_t limb[BIGCOORD_LIMBS];  // little-endian: limb[0] — младший
} BigCoord;

// 128/64 деление: переносимо между cl.exe, clang-cl и gcc.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint64_t BigCoordDiv128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t* remainder)
{
    return _udiv128(high, low, divisor, remainder);
}
#else
static inline uint64_t BigCoordDiv128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t* remainder)
{
    unsigned __int128 numerator = ((unsigned __int128)high << 64) | low;
    *remainder = (uint64_t)(numerator % divisor);
    return (uint64_t)(numerator / divisor);
}
#endif

static inline void BigCoordSetMaximum(BigCoord* value)
{
    for (int32_t i = 0; i < BIGCOORD_LIMBS; ++i)
    {
        value->limb[i] = UINT64_MAX;
    }
}

// Возвращает младшие 64 бита частного value / divisor и полный остаток.
// В отличие от копирования BigCoord и деления на месте, не создаёт
// 4160-байтовые временные значения на стеке.
static inline uint64_t BigCoordDivSmallLow(
    const BigCoord* value, uint64_t divisor, uint64_t* outRemainder)
{
    uint64_t remainder = 0;
    uint64_t quotientLow = 0;

    for (int32_t i = BIGCOORD_LIMBS - 1; i >= 0; --i)
    {
        uint64_t quotient = BigCoordDiv128(remainder, value->limb[i], divisor, &remainder);
        if (i == 0)
        {
            quotientLow = quotient;
        }
    }
    *outRemainder = remainder;
    return quotientLow;
}
