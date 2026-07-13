#include "world/infinite_coord.h"

#include <windows.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

static uint64_t Int64Magnitude(int64_t value)
{
    return value < 0 ? 0u - (uint64_t)value : (uint64_t)value;
}

static void InfiniteCoordNormalize(InfiniteCoord* value)
{
    while (value->limbCount > 0 && value->limbs[value->limbCount - 1] == 0)
    {
        value->limbCount--;
    }

    if (value->limbCount == 0)
    {
        if (value->limbs != NULL)
        {
            HeapFree(GetProcessHeap(), 0, value->limbs);
            value->limbs = NULL;
        }
        value->sign = 0;
    }
}

static int32_t InfiniteCoordCompareMagnitudeSmall(const InfiniteCoord* value, uint64_t magnitude)
{
    if (value->limbCount > 1)
    {
        return 1;
    }
    uint64_t current = value->limbCount == 0 ? 0 : value->limbs[0];
    return current < magnitude ? -1 : (current > magnitude ? 1 : 0);
}

static int32_t InfiniteCoordCompareMagnitude(
    const InfiniteCoord* left, const InfiniteCoord* right)
{
    if (left->limbCount != right->limbCount)
    {
        return left->limbCount < right->limbCount ? -1 : 1;
    }

    for (uint32_t index = left->limbCount; index > 0; --index)
    {
        uint64_t leftLimb = left->limbs[index - 1];
        uint64_t rightLimb = right->limbs[index - 1];
        if (leftLimb != rightLimb)
        {
            return leftLimb < rightLimb ? -1 : 1;
        }
    }
    return 0;
}

static bool InfiniteCoordTryCopy(InfiniteCoord* out, const InfiniteCoord* source)
{
    InfiniteCoordInit(out);
    if (source->limbCount == 0)
    {
        return true;
    }

    uint64_t* limbs = HeapAlloc(GetProcessHeap(), 0,
        (size_t)source->limbCount * sizeof(uint64_t));
    if (limbs == NULL)
    {
        return false;
    }

    for (uint32_t i = 0; i < source->limbCount; ++i)
    {
        limbs[i] = source->limbs[i];
    }

    out->limbs = limbs;
    out->limbCount = source->limbCount;
    out->sign = source->sign;
    return true;
}

static bool InfiniteCoordTryAddMagnitudeSmall(InfiniteCoord* value, uint64_t magnitude)
{
    if (magnitude == 0)
    {
        return true;
    }

    if (value->limbCount == 0)
    {
        value->limbs = HeapAlloc(GetProcessHeap(), 0, sizeof(uint64_t));
        if (value->limbs == NULL)
        {
            return false;
        }
        value->limbs[0] = magnitude;
        value->limbCount = 1;
        return true;
    }

    uint64_t old = value->limbs[0];
    value->limbs[0] = old + magnitude;
    uint64_t carry = value->limbs[0] < old ? 1u : 0u;

    for (uint32_t i = 1; i < value->limbCount && carry != 0; ++i)
    {
        old = value->limbs[i];
        value->limbs[i] = old + 1u;
        carry = value->limbs[i] == 0 ? 1u : 0u;
    }

    if (carry != 0)
    {
        uint32_t newCount = value->limbCount + 1;
        uint64_t* expanded = HeapReAlloc(GetProcessHeap(), 0, value->limbs,
            (size_t)newCount * sizeof(uint64_t));
        if (expanded == NULL)
        {
            // Откатываем перенос: исходное значение было ...FFFF + magnitude.
            uint64_t borrow = magnitude;
            for (uint32_t i = 0; i < value->limbCount && borrow != 0; ++i)
            {
                uint64_t current = value->limbs[i];
                value->limbs[i] = current - borrow;
                borrow = current < borrow ? 1u : 0u;
            }
            return false;
        }
        value->limbs = expanded;
        value->limbs[value->limbCount] = 1u;
        value->limbCount = newCount;
    }

    return true;
}

static void InfiniteCoordSubtractMagnitudeSmall(InfiniteCoord* value, uint64_t magnitude)
{
    uint64_t borrow = magnitude;
    for (uint32_t i = 0; i < value->limbCount && borrow != 0; ++i)
    {
        uint64_t current = value->limbs[i];
        value->limbs[i] = current - borrow;
        borrow = current < borrow ? 1u : 0u;
    }
    InfiniteCoordNormalize(value);
}

static bool InfiniteCoordTryAddInt64InPlace(InfiniteCoord* value, int64_t addend)
{
    if (addend == 0)
    {
        return true;
    }

    int32_t addSign = addend < 0 ? -1 : 1;
    uint64_t magnitude = Int64Magnitude(addend);

    if (value->sign == 0)
    {
        if (!InfiniteCoordTryAddMagnitudeSmall(value, magnitude))
        {
            return false;
        }
        value->sign = addSign;
        return true;
    }

    if (value->sign == addSign)
    {
        return InfiniteCoordTryAddMagnitudeSmall(value, magnitude);
    }

    int32_t comparison = InfiniteCoordCompareMagnitudeSmall(value, magnitude);
    if (comparison == 0)
    {
        InfiniteCoordDestroy(value);
        return true;
    }
    if (comparison > 0)
    {
        InfiniteCoordSubtractMagnitudeSmall(value, magnitude);
        return true;
    }

    uint64_t current = value->limbCount == 0 ? 0 : value->limbs[0];
    value->limbs[0] = magnitude - current;
    value->limbCount = 1;
    value->sign = addSign;
    return true;
}

static void SignedDifference(int64_t left, int64_t right,
    int32_t* outSign, uint64_t* outMagnitude)
{
    if ((left >= 0) == (right >= 0))
    {
        if (left == right)
        {
            *outSign = 0;
            *outMagnitude = 0;
        }
        else if (left > right)
        {
            *outSign = 1;
            *outMagnitude = (uint64_t)left - (uint64_t)right;
        }
        else
        {
            *outSign = -1;
            *outMagnitude = (uint64_t)right - (uint64_t)left;
        }
        return;
    }

    if (left >= 0)
    {
        *outSign = 1;
        *outMagnitude = (uint64_t)left + Int64Magnitude(right);
    }
    else
    {
        *outSign = -1;
        *outMagnitude = Int64Magnitude(left) + (uint64_t)right;
    }
}

static void CopyMagnitudeInfo(
    const InfiniteCoord* value, uint64_t* outLow, bool* outWide)
{
    *outLow = value->limbCount == 0 ? 0 : value->limbs[0];
    *outWide = false;
    for (uint32_t index = 1; index < value->limbCount; ++index)
    {
        if (value->limbs[index] != 0)
        {
            *outWide = true;
            break;
        }
    }
}

static void AddMagnitudeInfo(
    const InfiniteCoord* left, const InfiniteCoord* right,
    uint64_t* outLow, bool* outWide)
{
    uint64_t carry = 0;
    uint32_t count = left->limbCount > right->limbCount
        ? left->limbCount : right->limbCount;
    *outLow = 0;
    *outWide = false;

    for (uint32_t index = 0; index < count; ++index)
    {
        uint64_t a = index < left->limbCount ? left->limbs[index] : 0;
        uint64_t b = index < right->limbCount ? right->limbs[index] : 0;
        uint64_t sum = a + b;
        uint64_t firstCarry = sum < a ? 1u : 0u;
        uint64_t result = sum + carry;
        uint64_t secondCarry = result < sum ? 1u : 0u;
        carry = firstCarry | secondCarry;

        if (index == 0) *outLow = result;
        else if (result != 0) *outWide = true;
    }
    if (carry != 0) *outWide = true;
}

static void SubtractMagnitudeInfo(
    const InfiniteCoord* larger, const InfiniteCoord* smaller,
    uint64_t* outLow, bool* outWide)
{
    uint64_t borrow = 0;
    *outLow = 0;
    *outWide = false;

    for (uint32_t index = 0; index < larger->limbCount; ++index)
    {
        uint64_t a = larger->limbs[index];
        uint64_t b = index < smaller->limbCount ? smaller->limbs[index] : 0;
        uint64_t difference = a - b;
        uint64_t firstBorrow = a < b ? 1u : 0u;
        uint64_t result = difference - borrow;
        uint64_t secondBorrow = difference < borrow ? 1u : 0u;
        borrow = firstBorrow | secondBorrow;

        if (index == 0) *outLow = result;
        else if (result != 0) *outWide = true;
    }
}

static void InfiniteCoordDifferenceInfo(
    const InfiniteCoord* left, const InfiniteCoord* right,
    int32_t* outSign, uint64_t* outMagnitude, bool* outWide)
{
    if (left->sign == 0)
    {
        *outSign = -right->sign;
        CopyMagnitudeInfo(right, outMagnitude, outWide);
        return;
    }
    if (right->sign == 0)
    {
        *outSign = left->sign;
        CopyMagnitudeInfo(left, outMagnitude, outWide);
        return;
    }
    if (left->sign != right->sign)
    {
        *outSign = left->sign;
        AddMagnitudeInfo(left, right, outMagnitude, outWide);
        return;
    }

    int32_t comparison = InfiniteCoordCompareMagnitude(left, right);
    if (comparison == 0)
    {
        *outSign = 0;
        *outMagnitude = 0;
        *outWide = false;
        return;
    }

    if (comparison > 0)
    {
        *outSign = left->sign;
        SubtractMagnitudeInfo(left, right, outMagnitude, outWide);
    }
    else
    {
        *outSign = -left->sign;
        SubtractMagnitudeInfo(right, left, outMagnitude, outWide);
    }
}

static int32_t InfiniteCoordSignAfterSmall(
    const InfiniteCoord* value, int32_t addSign, uint64_t addMagnitude)
{
    if (addSign == 0 || addMagnitude == 0)
    {
        return value->sign;
    }
    if (value->sign == 0 || value->sign == addSign)
    {
        return addSign;
    }

    int32_t comparison = InfiniteCoordCompareMagnitudeSmall(value, addMagnitude);
    if (comparison == 0)
    {
        return 0;
    }
    return comparison > 0 ? value->sign : addSign;
}

static void InfiniteCoordTwosComplementLow128(
    const InfiniteCoord* value, uint64_t* outLow, uint64_t* outHigh)
{
    uint64_t lowMagnitude = value->limbCount > 0 ? value->limbs[0] : 0;
    uint64_t highMagnitude = value->limbCount > 1 ? value->limbs[1] : 0;

    if (value->sign >= 0)
    {
        *outLow = lowMagnitude;
        *outHigh = highMagnitude;
        return;
    }

    uint64_t low = ~lowMagnitude + 1u;
    uint64_t carry = low == 0 ? 1u : 0u;
    *outLow = low;
    *outHigh = ~highMagnitude + carry;
}

static uint64_t Mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void InfiniteCoordInit(InfiniteCoord* value)
{
    value->limbs = NULL;
    value->limbCount = 0;
    value->sign = 0;
}

void InfiniteCoordDestroy(InfiniteCoord* value)
{
    if (value->limbs != NULL)
    {
        HeapFree(GetProcessHeap(), 0, value->limbs);
    }
    InfiniteCoordInit(value);
}

bool InfiniteCoordTryCopyAddInt64(InfiniteCoord* out, const InfiniteCoord* source, int64_t addend)
{
    InfiniteCoord temporary;
    if (!InfiniteCoordTryCopy(&temporary, source))
    {
        return false;
    }
    if (!InfiniteCoordTryAddInt64InPlace(&temporary, addend))
    {
        InfiniteCoordDestroy(&temporary);
        return false;
    }
    *out = temporary;
    return true;
}

static uint64_t Multiply64(uint64_t left, uint64_t right, uint64_t* outHigh)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return _umul128(left, right, outHigh);
#else
    unsigned __int128 product = (unsigned __int128)left * right;
    *outHigh = (uint64_t)(product >> 64);
    return (uint64_t)product;
#endif
}

static uint64_t AddWithCarry64(
    uint64_t left, uint64_t right, uint64_t carry, uint64_t* outValue)
{
    uint64_t sum = left + right;
    uint64_t firstCarry = sum < left ? 1u : 0u;
    uint64_t result = sum + carry;
    uint64_t secondCarry = result < sum ? 1u : 0u;
    *outValue = result;
    return firstCarry | secondCarry;
}

static void AddWideProduct(uint64_t* limbs, uint32_t index,
    uint64_t low, uint64_t high, uint64_t extra)
{
    uint64_t carry = AddWithCarry64(
        limbs[index], low, 0, &limbs[index]);
    ++index;
    carry = AddWithCarry64(
        limbs[index], high, carry, &limbs[index]);
    ++index;
    carry = AddWithCarry64(
        limbs[index], extra, carry, &limbs[index]);
    ++index;

    while (carry != 0)
    {
        carry = AddWithCarry64(
            limbs[index], 0, carry, &limbs[index]);
        ++index;
    }
}

bool InfiniteCoordTryCopySquareAddInt64(
    InfiniteCoord* out, const InfiniteCoord* source, int64_t addend)
{
    InfiniteCoord value;
    if (!InfiniteCoordTryCopyAddInt64(&value, source, addend))
    {
        return false;
    }

    if (value.limbCount == 0)
    {
        *out = value;
        return true;
    }

    if (value.limbCount > (UINT32_MAX - 1u) / 2u)
    {
        InfiniteCoordDestroy(&value);
        return false;
    }

    uint32_t capacity = value.limbCount * 2u + 1u;
    uint64_t* limbs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)capacity * sizeof(uint64_t));
    if (limbs == NULL)
    {
        InfiniteCoordDestroy(&value);
        return false;
    }

    // Специализированное возведение в квадрат: диагональ считается один раз,
    // попарные произведения — один раз и удваиваются. Почти вдвое меньше
    // 64x64 умножений, чем у общего школьного умножения.
    for (uint32_t leftIndex = 0;
         leftIndex < value.limbCount; ++leftIndex)
    {
        uint64_t diagonalHigh;
        uint64_t diagonalLow = Multiply64(
            value.limbs[leftIndex], value.limbs[leftIndex], &diagonalHigh);
        AddWideProduct(limbs, leftIndex * 2u,
            diagonalLow, diagonalHigh, 0);

        for (uint32_t rightIndex = leftIndex + 1;
             rightIndex < value.limbCount; ++rightIndex)
        {
            uint64_t productHigh;
            uint64_t productLow = Multiply64(
                value.limbs[leftIndex], value.limbs[rightIndex], &productHigh);
            uint64_t doubledLow = productLow << 1;
            uint64_t doubledHigh = (productHigh << 1) | (productLow >> 63);
            uint64_t extra = productHigh >> 63;
            AddWideProduct(limbs, leftIndex + rightIndex,
                doubledLow, doubledHigh, extra);
        }
    }

    InfiniteCoordDestroy(&value);

    out->limbs = limbs;
    out->limbCount = capacity;
    out->sign = 1;
    InfiniteCoordNormalize(out);
    return true;
}

bool InfiniteCoordTryCopyShiftRight(
    InfiniteCoord* out, const InfiniteCoord* source, uint32_t bitCount)
{
    if (bitCount == 0)
    {
        return InfiniteCoordTryCopy(out, source);
    }
    if (bitCount >= 64)
    {
        return false;
    }

    InfiniteCoordInit(out);
    if (source->limbCount == 0)
    {
        return true;
    }

    out->limbs = HeapAlloc(GetProcessHeap(), 0,
        (size_t)source->limbCount * sizeof(uint64_t));
    if (out->limbs == NULL)
    {
        return false;
    }

    for (uint32_t index = 0; index < source->limbCount; ++index)
    {
        uint64_t shifted = source->limbs[index] >> bitCount;
        if (index + 1 < source->limbCount)
        {
            shifted |= source->limbs[index + 1] << (64u - bitCount);
        }
        out->limbs[index] = shifted;
    }

    out->limbCount = source->limbCount;
    out->sign = source->sign;
    InfiniteCoordNormalize(out);
    return true;
}

bool InfiniteCoordTrySubtractToInt64(
    const InfiniteCoord* left, const InfiniteCoord* right, int64_t* outDifference)
{
    if (outDifference == NULL) return false;

    int32_t sign;
    uint64_t magnitude;
    bool wide;
    InfiniteCoordDifferenceInfo(left, right, &sign, &magnitude, &wide);
    if (wide) return false;

    if (sign > 0)
    {
        if (magnitude > (uint64_t)INT64_MAX) return false;
        *outDifference = (int64_t)magnitude;
    }
    else if (sign < 0)
    {
        if (magnitude > (1ull << 63)) return false;
        *outDifference = magnitude == (1ull << 63)
            ? INT64_MIN : -(int64_t)magnitude;
    }
    else
    {
        *outDifference = 0;
    }
    return true;
}

bool InfiniteCoordEqualsOffsets(const InfiniteCoord* left, int64_t leftOffset,
    const InfiniteCoord* right, int64_t rightOffset)
{
    int32_t baseSign;
    uint64_t baseMagnitude;
    bool baseWide;
    InfiniteCoordDifferenceInfo(
        left, right, &baseSign, &baseMagnitude, &baseWide);
    if (baseWide) return false;

    int32_t offsetSign;
    uint64_t offsetMagnitude;
    SignedDifference(rightOffset, leftOffset, &offsetSign, &offsetMagnitude);
    return baseSign == offsetSign && baseMagnitude == offsetMagnitude;
}

void InfiniteCoordSwap(InfiniteCoord* a, InfiniteCoord* b)
{
    InfiniteCoord temporary = *a;
    *a = *b;
    *b = temporary;
}

static uint64_t Divide128By64(uint64_t high, uint64_t low,
    uint64_t divisor, uint64_t* outRemainder)
{
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    return _udiv128(high, low, divisor, outRemainder);
#elif defined(__clang__) && defined(_M_X64)
    // У clang-cl нет интринсика _udiv128, а деление unsigned __int128
    // требует функций compiler-rt, которых нет в сборке с /NODEFAULTLIB.
    // На каждом шаге high < divisor, поэтому частное помещается в uint64_t.
    __asm__("divq %2"
        : "+a"(low), "+d"(high)
        : "r"(divisor)
        : "cc");
    *outRemainder = high;
    return low;
#else
    // Переносимый резервный путь без деления расширенного целого.
    uint64_t quotient = 0;
    for (uint32_t bit = 64; bit > 0; --bit)
    {
        uint32_t shift = bit - 1;
        uint64_t carry = high >> 63;
        high = (high << 1) | ((low >> shift) & 1u);
        if (carry != 0 || high >= divisor)
        {
            high -= divisor;
            quotient |= 1ull << shift;
        }
    }
    *outRemainder = high;
    return quotient;
#endif
}

uint64_t InfiniteCoordDivFloorSmallLow(
    const InfiniteCoord* value, uint64_t divisor, uint64_t* outRemainder)
{
    uint64_t remainder = 0;
    uint64_t quotientLow = 0;

    for (uint32_t index = value->limbCount; index > 0; --index)
    {
        quotientLow = Divide128By64(
            remainder, value->limbs[index - 1], divisor, &remainder);
    }

    if (value->sign >= 0)
    {
        *outRemainder = remainder;
        return quotientLow;
    }

    if (remainder == 0)
    {
        *outRemainder = 0;
        return 0u - quotientLow;
    }

    *outRemainder = divisor - remainder;
    return 0u - quotientLow - 1u;
}

int32_t InfiniteCoordCompareAddInt64ToInt64(
    const InfiniteCoord* value, int64_t addend, int64_t subtrahend)
{
    int32_t differenceSign;
    uint64_t differenceMagnitude;
    SignedDifference(addend, subtrahend, &differenceSign, &differenceMagnitude);
    return InfiniteCoordSignAfterSmall(value, differenceSign, differenceMagnitude);
}

uint64_t InfiniteCoordHashOffset(const InfiniteCoord* base, int64_t offset)
{
    uint64_t low;
    uint64_t high;
    InfiniteCoordTwosComplementLow128(base, &low, &high);

    uint64_t previous = low;
    low += (uint64_t)offset;
    uint64_t carry = low < previous ? 1u : 0u;
    high += (offset < 0 ? UINT64_MAX : 0u) + carry;

    return Mix64(low) ^ Mix64(high + 0x9e3779b97f4a7c15ULL);
}

bool InfiniteCoordEqualsOffset(const InfiniteCoord* value, const InfiniteCoord* base, int64_t offset)
{
    if (offset == 0)
    {
        if (value->sign != base->sign || value->limbCount != base->limbCount)
        {
            return false;
        }
        for (uint32_t i = 0; i < value->limbCount; ++i)
        {
            if (value->limbs[i] != base->limbs[i]) return false;
        }
        return true;
    }

    int32_t offsetSign = offset < 0 ? -1 : 1;
    uint64_t magnitude = Int64Magnitude(offset);

    if (base->sign == 0)
    {
        return value->sign == offsetSign && value->limbCount == 1
            && value->limbs[0] == magnitude;
    }

    if (base->sign == offsetSign)
    {
        if (value->sign != base->sign)
        {
            return false;
        }

        uint64_t carry = magnitude;
        uint32_t expectedCount = base->limbCount;
        for (uint32_t i = 0; i < base->limbCount; ++i)
        {
            uint64_t limb = base->limbs[i] + carry;
            carry = limb < base->limbs[i] ? 1u : 0u;
            if (i >= value->limbCount || value->limbs[i] != limb)
            {
                return false;
            }
        }
        if (carry != 0)
        {
            expectedCount++;
            if (value->limbCount < expectedCount || value->limbs[expectedCount - 1] != 1u)
            {
                return false;
            }
        }
        return value->limbCount == expectedCount;
    }

    int32_t comparison = InfiniteCoordCompareMagnitudeSmall(base, magnitude);
    if (comparison == 0)
    {
        return value->sign == 0;
    }
    if (comparison < 0)
    {
        uint64_t baseMagnitude = base->limbCount == 0 ? 0 : base->limbs[0];
        return value->sign == offsetSign && value->limbCount == 1
            && value->limbs[0] == magnitude - baseMagnitude;
    }

    if (value->sign != base->sign)
    {
        return false;
    }

    uint64_t borrow = magnitude;
    uint32_t expectedCount = base->limbCount;
    for (uint32_t i = 0; i < base->limbCount; ++i)
    {
        uint64_t limb = base->limbs[i] - borrow;
        borrow = base->limbs[i] < borrow ? 1u : 0u;
        if (i < value->limbCount)
        {
            if (value->limbs[i] != limb) return false;
        }
        else if (limb != 0)
        {
            return false;
        }
        if (i == base->limbCount - 1 && limb == 0)
        {
            expectedCount--;
        }
    }
    return value->limbCount == expectedCount;
}

uint32_t InfiniteCoordLow32Offset(const InfiniteCoord* base, int64_t offset)
{
    uint64_t low;
    uint64_t high;
    InfiniteCoordTwosComplementLow128(base, &low, &high);
    (void)high;
    return (uint32_t)(low + (uint64_t)offset);
}

int64_t InfiniteCoordSubtractFromInt64Clamped(int64_t scalar, const InfiniteCoord* value)
{
    if (value->sign == 0)
    {
        return scalar;
    }
    if (value->limbCount > 1)
    {
        return value->sign > 0 ? INT64_MIN : INT64_MAX;
    }

    uint64_t magnitude = value->limbs[0];
    if (value->sign > 0)
    {
        if (scalar < 0)
        {
            uint64_t room = (uint64_t)(scalar - INT64_MIN);
            return magnitude > room ? INT64_MIN : scalar - (int64_t)magnitude;
        }
        if (magnitude > (uint64_t)scalar + (uint64_t)INT64_MAX + 1u)
        {
            return INT64_MIN;
        }
        uint64_t resultMagnitude = magnitude - (uint64_t)scalar;
        if (magnitude > (uint64_t)scalar)
        {
            return resultMagnitude == (1ULL << 63) ? INT64_MIN : -(int64_t)resultMagnitude;
        }
        return scalar - (int64_t)magnitude;
    }

    if (scalar >= 0)
    {
        return magnitude > (uint64_t)INT64_MAX - (uint64_t)scalar
            ? INT64_MAX : scalar + (int64_t)magnitude;
    }
    uint64_t negativeMagnitude = Int64Magnitude(scalar);
    if (magnitude >= negativeMagnitude)
    {
        uint64_t positive = magnitude - negativeMagnitude;
        return positive > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)positive;
    }
    return -(int64_t)(negativeMagnitude - magnitude);
}

typedef struct ShortCoordinateWriter
{
    wchar_t* text;
    uint32_t capacity;
    uint32_t length;
} ShortCoordinateWriter;

static void ShortCoordinateWriteCharacter(ShortCoordinateWriter* writer, wchar_t character)
{
    if (writer->capacity == 0)
    {
        return;
    }
    if (writer->length + 1 < writer->capacity)
    {
        writer->text[writer->length++] = character;
        writer->text[writer->length] = L'\0';
    }
}

static void ShortCoordinateWriteUnsigned(ShortCoordinateWriter* writer, uint64_t value)
{
    wchar_t reversed[24];
    uint32_t count = 0;
    do
    {
        reversed[count++] = (wchar_t)(L'0' + value % 10u);
        value /= 10u;
    }
    while (value != 0);

    while (count > 0)
    {
        ShortCoordinateWriteCharacter(writer, reversed[--count]);
    }
}

static uint32_t UnsignedDecimalDigits(uint64_t value)
{
    uint32_t digits = 1;
    while (value >= 10u)
    {
        value /= 10u;
        ++digits;
    }
    return digits;
}

static uint32_t HighestSetBitIndex(uint64_t value)
{
    uint32_t index = 0;
    while (value > 1u)
    {
        value >>= 1;
        ++index;
    }
    return index;
}

void InfiniteCoordFormatShortOffsetW(const InfiniteCoord* base, int64_t offset,
    wchar_t* outText, uint32_t capacity)
{
    if (capacity == 0)
    {
        return;
    }
    outText[0] = L'\0';

    InfiniteCoord value;
    if (!InfiniteCoordTryCopyAddInt64(&value, base, offset))
    {
        if (capacity > 1)
        {
            outText[0] = L'?';
            outText[1] = L'\0';
        }
        return;
    }

    ShortCoordinateWriter writer = { outText, capacity, 0 };
    if (value.sign < 0)
    {
        ShortCoordinateWriteCharacter(&writer, L'-');
    }

    if (value.limbCount == 0)
    {
        ShortCoordinateWriteCharacter(&writer, L'0');
    }
    else if (value.limbCount == 1)
    {
        uint64_t magnitude = value.limbs[0];
        uint32_t digits = UnsignedDecimalDigits(magnitude);
        if (digits <= 7)
        {
            ShortCoordinateWriteUnsigned(&writer, magnitude);
        }
        else
        {
            uint64_t divisor = 1;
            for (uint32_t index = 3; index < digits; ++index)
            {
                divisor *= 10u;
            }
            uint64_t leading = magnitude / divisor;
            ShortCoordinateWriteCharacter(&writer, (wchar_t)(L'0' + leading / 100u));
            ShortCoordinateWriteCharacter(&writer, L'.');
            ShortCoordinateWriteCharacter(&writer, (wchar_t)(L'0' + (leading / 10u) % 10u));
            ShortCoordinateWriteCharacter(&writer, (wchar_t)(L'0' + leading % 10u));
            ShortCoordinateWriteCharacter(&writer, L'e');
            ShortCoordinateWriteUnsigned(&writer, digits - 1u);
        }
    }
    else
    {
        uint64_t top = value.limbs[value.limbCount - 1];
        uint64_t highestBit = (uint64_t)(value.limbCount - 1) * 64u
            + HighestSetBitIndex(top);
        ShortCoordinateWriteCharacter(&writer, L'~');
        ShortCoordinateWriteCharacter(&writer, L'2');
        ShortCoordinateWriteCharacter(&writer, L'^');
        ShortCoordinateWriteUnsigned(&writer, highestBit);
    }

    InfiniteCoordDestroy(&value);
}
