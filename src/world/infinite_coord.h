#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Знаковое целое произвольной точности. Число лимбов растёт только при
// фактическом удалении от начала мира, поэтому предел задаётся доступной памятью.
typedef struct InfiniteCoord
{
    uint64_t* limbs;       // модуль, little-endian
    uint32_t limbCount;
    int32_t sign;          // -1, 0, +1
} InfiniteCoord;

void InfiniteCoordInit(InfiniteCoord* value);
void InfiniteCoordDestroy(InfiniteCoord* value);

bool InfiniteCoordTryCopyAddInt64(InfiniteCoord* out, const InfiniteCoord* source, int64_t addend);
bool InfiniteCoordTryCopySquareAddInt64(
    InfiniteCoord* out, const InfiniteCoord* source, int64_t addend);
bool InfiniteCoordTryCopyShiftRight(
    InfiniteCoord* out, const InfiniteCoord* source, uint32_t bitCount);
bool InfiniteCoordTrySubtractToInt64(
    const InfiniteCoord* left, const InfiniteCoord* right, int64_t* outDifference);

// Сравнивает left + leftOffset и right + rightOffset без временных bigint.
bool InfiniteCoordEqualsOffsets(const InfiniteCoord* left, int64_t leftOffset,
    const InfiniteCoord* right, int64_t rightOffset);
void InfiniteCoordSwap(InfiniteCoord* a, InfiniteCoord* b);

// floor(value / divisor): возвращаются младшие 64 бита частного и
// неотрицательный остаток [0, divisor).
uint64_t InfiniteCoordDivFloorSmallLow(
    const InfiniteCoord* value, uint64_t divisor, uint64_t* outRemainder);

// Сравнивает value + addend с subtrahend без временного bigint.
int32_t InfiniteCoordCompareAddInt64ToInt64(
    const InfiniteCoord* value, int64_t addend, int64_t subtrahend);

// Хеш и точное сравнение виртуального значения base + offset.
uint64_t InfiniteCoordHashOffset(const InfiniteCoord* base, int64_t offset);
bool InfiniteCoordEqualsOffset(const InfiniteCoord* value, const InfiniteCoord* base, int64_t offset);

// scalar - value с насыщением до int64.
int64_t InfiniteCoordSubtractFromInt64Clamped(int64_t scalar, const InfiniteCoord* value);

// Короткое представление base + offset для UI: точное для небольших чисел,
// научная запись для uint64 и степень двойки для координат любой длины.
void InfiniteCoordFormatShortOffsetW(const InfiniteCoord* base, int64_t offset,
    wchar_t* outText, uint32_t capacity);
