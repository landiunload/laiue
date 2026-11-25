#pragma once

/* 128-битные произведения для криптографии движка.
 *
 * Арифметика поля и MAC копит суммы произведений 64 на 64 бита в Wide.
 * LAIUE_CRYPTO_PORTABLE выбирает переносимый путь и там, где компилятор умеет
 * быстрый: тесты собирают оба. */

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#if defined(__SIZEOF_INT128__) && !defined(LAIUE_CRYPTO_PORTABLE)

/* GCC и Clang, включая clang-cl, держат 128-битное целое на 64-битных целях:
 * произведение становится одной инструкцией, сумма — парой. */
#if defined(__GNUC__)
__extension__ typedef unsigned __int128 Wide;
#else
typedef unsigned __int128 Wide;
#endif

static inline Wide WideMultiply(uint64_t left, uint64_t right)
{
    return (Wide)left * right;
}

static inline Wide WideAdd(Wide sum, Wide value)
{
    return sum + value;
}

static inline Wide WideAddSmall(Wide sum, uint64_t value)
{
    return sum + value;
}

static inline uint64_t WideLow(Wide value)
{
    return (uint64_t)value;
}

static inline uint64_t WideHigh(Wide value)
{
    return (uint64_t)(value >> 64u);
}

/* value >> count при 0 < count < 64; результат обязан помещаться в 64 бита. */
static inline uint64_t WideShift(Wide value, uint32_t count)
{
    return (uint64_t)(value >> count);
}

#else

typedef struct Wide
{
    uint64_t low;
    uint64_t high;
} Wide;

#if defined(_MSC_VER) && !defined(__clang__) && !defined(LAIUE_CRYPTO_PORTABLE) &&                 \
    (defined(_M_X64) || defined(_M_ARM64))
#define LAIUE_CRYPTO_MSVC_WIDE 1
#endif

static inline Wide WideMultiply(uint64_t left, uint64_t right)
{
    Wide product;
#if defined(LAIUE_CRYPTO_MSVC_WIDE) && defined(_M_X64)
    product.low = _umul128(left, right, &product.high);
#elif defined(LAIUE_CRYPTO_MSVC_WIDE)
    product.low = left * right;
    product.high = __umulh(left, right);
#else
    /* Четыре произведения 32 на 32 бита. */
    const uint64_t leftLow = (uint32_t)left;
    const uint64_t leftHigh = left >> 32u;
    const uint64_t rightLow = (uint32_t)right;
    const uint64_t rightHigh = right >> 32u;
    const uint64_t lowProduct = leftLow * rightLow;
    const uint64_t crossLeft = leftHigh * rightLow;
    const uint64_t crossRight = leftLow * rightHigh;
    const uint64_t middle = (lowProduct >> 32u) + (uint32_t)crossLeft + (uint32_t)crossRight;
    product.low = (middle << 32u) | (uint32_t)lowProduct;
    product.high =
        leftHigh * rightHigh + (crossLeft >> 32u) + (crossRight >> 32u) + (middle >> 32u);
#endif
    return product;
}

static inline Wide WideAdd(Wide sum, Wide value)
{
#if defined(LAIUE_CRYPTO_MSVC_WIDE) && defined(_M_X64)
    const unsigned char carry = _addcarry_u64(0u, sum.low, value.low, &sum.low);
    (void)_addcarry_u64(carry, sum.high, value.high, &sum.high);
#else
    /* Перенос — старший бит выражения, а не сравнение: время не зависит от
     * значений и там, где компилятор не знает инструкции сложения с
     * переносом. */
    const uint64_t low = sum.low + value.low;
    const uint64_t carry = ((sum.low & value.low) | ((sum.low | value.low) & ~low)) >> 63u;
    sum.low = low;
    sum.high += value.high + carry;
#endif
    return sum;
}

static inline Wide WideAddSmall(Wide sum, uint64_t value)
{
    const Wide wide = {value, 0u};
    return WideAdd(sum, wide);
}

static inline uint64_t WideLow(Wide value)
{
    return value.low;
}

static inline uint64_t WideHigh(Wide value)
{
    return value.high;
}

/* value >> count при 0 < count < 64; результат обязан помещаться в 64 бита. */
static inline uint64_t WideShift(Wide value, uint32_t count)
{
    return (value.high << (64u - count)) | (value.low >> count);
}

#endif

static inline Wide WideMultiplyAdd(Wide sum, uint64_t left, uint64_t right)
{
    return WideAdd(sum, WideMultiply(left, right));
}
