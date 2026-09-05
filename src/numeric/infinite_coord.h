#pragma once

#include "api.h"

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

LAIUE_NUMERIC_API void InfiniteCoordInit(InfiniteCoord* value);
LAIUE_NUMERIC_API void InfiniteCoordDestroy(InfiniteCoord* value);

LAIUE_NUMERIC_API bool InfiniteCoordTryCopyAddInt64(InfiniteCoord* out, const InfiniteCoord* source, int64_t addend);
// Добавляет небольшое целое непосредственно к значению. В отличие от
// CopyAdd не создаёт промежуточную копию; это горячий путь для fixed-point
// физики. При неудаче value остаётся без изменений.
LAIUE_NUMERIC_API bool InfiniteCoordTryAddInt64InPlace(InfiniteCoord* value, int64_t addend);
LAIUE_NUMERIC_API bool InfiniteCoordTryCopySquareAddInt64(
    InfiniteCoord* out, const InfiniteCoord* source, int64_t addend);
LAIUE_NUMERIC_API bool InfiniteCoordTryCopyShiftRight(
    InfiniteCoord* out, const InfiniteCoord* source, uint32_t bitCount);
LAIUE_NUMERIC_API bool InfiniteCoordTrySubtractToInt64(
    const InfiniteCoord* left, const InfiniteCoord* right, int64_t* outDifference);

// Сравнивает left + leftOffset и right + rightOffset без временных bigint.
LAIUE_NUMERIC_API bool InfiniteCoordEqualsOffsets(const InfiniteCoord* left, int64_t leftOffset,
    const InfiniteCoord* right, int64_t rightOffset);
LAIUE_NUMERIC_API void InfiniteCoordSwap(InfiniteCoord* a, InfiniteCoord* b);

// floor(value / divisor): возвращаются младшие 64 бита частного и
// неотрицательный остаток [0, divisor).
LAIUE_NUMERIC_API uint64_t InfiniteCoordDivFloorSmallLow(
    const InfiniteCoord* value, uint64_t divisor, uint64_t* outRemainder);

// Сравнивает value + addend с subtrahend без временного bigint.
LAIUE_NUMERIC_API int32_t InfiniteCoordCompareAddInt64ToInt64(
    const InfiniteCoord* value, int64_t addend, int64_t subtrahend);

// Хеш и точное сравнение виртуального значения base + offset.
LAIUE_NUMERIC_API uint64_t InfiniteCoordHashOffset(const InfiniteCoord* base, int64_t offset);
LAIUE_NUMERIC_API bool InfiniteCoordEqualsOffset(const InfiniteCoord* value, const InfiniteCoord* base, int64_t offset);

// scalar - value с насыщением до int64.
LAIUE_NUMERIC_API int64_t InfiniteCoordSubtractFromInt64Clamped(int64_t scalar, const InfiniteCoord* value);

// Короткое представление base + offset для UI: точное для небольших чисел,
// научная запись для uint64 и степень двойки для координат любой длины.
LAIUE_NUMERIC_API void InfiniteCoordFormatShortOffsetW(const InfiniteCoord* base, int64_t offset,
    wchar_t* outText, uint32_t capacity);

// === Общая арифметика ===
//
// Мир пользуется числами произвольной точности для координат, physics —
// для скоростей в фиксированной точке: скорость, которую некуда ограничить,
// нельзя держать ни в int64, ни в double. Операции ниже — минимум, которым
// такая величина накапливается и превращается обратно в локальные числа.

LAIUE_NUMERIC_API int32_t InfiniteCoordSign(const InfiniteCoord* value);
LAIUE_NUMERIC_API int32_t InfiniteCoordCompare(
    const InfiniteCoord* left, const InfiniteCoord* right);

// out = left + right. out обязан отличаться от обоих слагаемых.
LAIUE_NUMERIC_API bool InfiniteCoordTryAdd(
    InfiniteCoord* out, const InfiniteCoord* left, const InfiniteCoord* right);
LAIUE_NUMERIC_API bool InfiniteCoordTryCopyNegate(
    InfiniteCoord* out, const InfiniteCoord* source);
LAIUE_NUMERIC_API bool InfiniteCoordTryCopyMultiplyInt64(
    InfiniteCoord* out, const InfiniteCoord* source, int64_t factor);
LAIUE_NUMERIC_API bool InfiniteCoordTryCopyShiftLeft(
    InfiniteCoord* out, const InfiniteCoord* source, uint32_t bitCount);

// Приближение double. За пределами диапазона возвращается +-DBL_MAX, а не
// бесконечность: бесконечность отравила бы всю дальнейшую арифметику, а
// насыщение оставляет число пригодным для сравнения и отрисовки.
LAIUE_NUMERIC_API double InfiniteCoordToDoubleSaturating(const InfiniteCoord* value);

// Целая часть value с усечением к нулю. false для NaN и бесконечности.
LAIUE_NUMERIC_API bool InfiniteCoordTrySetFromDouble(InfiniteCoord* out, double value);
