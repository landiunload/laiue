#pragma once

/* Runtime service contract for the numeric provider. The implementation keeps
 * the existing InfiniteCoord algorithms and exports them through a versioned
 * table so consumers do not import laiue_numeric.dll directly. */

#include "api.h"
#include "mod/module_api.h"
#include "numeric/infinite_coord.h"

#include <stdint.h>

#define LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 1u
#define LAIUE_NUMERIC_SERVICE_NAME "laiue.numeric"

typedef void (*LaiueNumericInitFn)(InfiniteCoord *value);
typedef void (*LaiueNumericDestroyFn)(InfiniteCoord *value);
typedef uint32_t (*LaiueNumericTryCopyAddInt64Fn)(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t addend);
typedef uint32_t (*LaiueNumericTryAddInt64InPlaceFn)(InfiniteCoord *value, int64_t addend);
typedef uint32_t (*LaiueNumericTryCopySquareAddInt64Fn)(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t addend);
typedef uint32_t (*LaiueNumericTryCopyShiftRightFn)(
    InfiniteCoord *out, const InfiniteCoord *source, uint32_t bitCount);
typedef uint32_t (*LaiueNumericTrySubtractToInt64Fn)(
    const InfiniteCoord *left, const InfiniteCoord *right, int64_t *outDifference);
typedef uint32_t (*LaiueNumericEqualsOffsetsFn)(
    const InfiniteCoord *left, int64_t leftOffset,
    const InfiniteCoord *right, int64_t rightOffset);
typedef void (*LaiueNumericSwapFn)(InfiniteCoord *a, InfiniteCoord *b);
typedef uint64_t (*LaiueNumericDivFloorSmallLowFn)(
    const InfiniteCoord *value, uint64_t divisor, uint64_t *outRemainder);
typedef int32_t (*LaiueNumericCompareAddInt64ToInt64Fn)(
    const InfiniteCoord *value, int64_t addend, int64_t subtrahend);
typedef uint64_t (*LaiueNumericHashOffsetFn)(const InfiniteCoord *base, int64_t offset);
typedef uint32_t (*LaiueNumericEqualsOffsetFn)(
    const InfiniteCoord *value, const InfiniteCoord *base, int64_t offset);
typedef int64_t (*LaiueNumericSubtractFromInt64ClampedFn)(
    int64_t scalar, const InfiniteCoord *value);
typedef void (*LaiueNumericFormatShortOffsetWFn)(
    const InfiniteCoord *base, int64_t offset, wchar_t *outText, uint32_t capacity);
typedef int32_t (*LaiueNumericSignFn)(const InfiniteCoord *value);
typedef int32_t (*LaiueNumericCompareFn)(
    const InfiniteCoord *left, const InfiniteCoord *right);
typedef uint32_t (*LaiueNumericTryAddFn)(
    InfiniteCoord *out, const InfiniteCoord *left, const InfiniteCoord *right);
typedef uint32_t (*LaiueNumericTryCopyNegateFn)(
    InfiniteCoord *out, const InfiniteCoord *source);
typedef uint32_t (*LaiueNumericTryCopyMultiplyInt64Fn)(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t factor);
typedef uint32_t (*LaiueNumericTryCopyShiftLeftFn)(
    InfiniteCoord *out, const InfiniteCoord *source, uint32_t bitCount);
typedef double (*LaiueNumericToDoubleSaturatingFn)(const InfiniteCoord *value);
typedef uint32_t (*LaiueNumericTrySetFromDoubleFn)(
    InfiniteCoord *out, double value);

typedef struct LaiueNumericServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueNumericInitFn init;
    LaiueNumericDestroyFn destroy;
    LaiueNumericTryCopyAddInt64Fn tryCopyAddInt64;
    LaiueNumericTryAddInt64InPlaceFn tryAddInt64InPlace;
    LaiueNumericTryCopySquareAddInt64Fn tryCopySquareAddInt64;
    LaiueNumericTryCopyShiftRightFn tryCopyShiftRight;
    LaiueNumericTrySubtractToInt64Fn trySubtractToInt64;
    LaiueNumericEqualsOffsetsFn equalsOffsets;
    LaiueNumericSwapFn swap;
    LaiueNumericDivFloorSmallLowFn divFloorSmallLow;
    LaiueNumericCompareAddInt64ToInt64Fn compareAddInt64ToInt64;
    LaiueNumericHashOffsetFn hashOffset;
    LaiueNumericEqualsOffsetFn equalsOffset;
    LaiueNumericSubtractFromInt64ClampedFn subtractFromInt64Clamped;
    LaiueNumericFormatShortOffsetWFn formatShortOffsetW;
    LaiueNumericSignFn sign;
    LaiueNumericCompareFn compare;
    LaiueNumericTryAddFn tryAdd;
    LaiueNumericTryCopyNegateFn tryCopyNegate;
    LaiueNumericTryCopyMultiplyInt64Fn tryCopyMultiplyInt64;
    LaiueNumericTryCopyShiftLeftFn tryCopyShiftLeft;
    LaiueNumericToDoubleSaturatingFn toDoubleSaturating;
    LaiueNumericTrySetFromDoubleFn trySetFromDouble;
    uintptr_t reserved[4];
} LaiueNumericServiceV1;

/* A static build cannot have several global LaiueModuleGetApiV1 symbols in
 * one executable, so static registries use this module-specific accessor. */
LAIUE_NUMERIC_API const LaiueModuleApiV1 *LaiueNumericGetStaticModuleApiV1(void);
LAIUE_NUMERIC_API const LaiueNumericServiceV1 *LaiueNumericGetStaticServiceV1(void);
