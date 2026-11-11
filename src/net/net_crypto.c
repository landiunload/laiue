#include "net/net_crypto.h"

#include "platform/system.h"

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

static uint32_t Load32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static void Store32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void Store64(uint8_t *bytes, uint64_t value)
{
    Store32(bytes, (uint32_t)value);
    Store32(bytes + 4u, (uint32_t)(value >> 32u));
}

void NetWipe(void *bytes, size_t size)
{
    volatile uint8_t *cursor = bytes;
    while (size-- != 0u)
        *cursor++ = 0u;
}

/* ---- ChaCha20 (RFC 8439, раздел 2.3) ----------------------------------------- */

static uint32_t RotateLeft(uint32_t value, uint32_t count)
{
    return (value << count) | (value >> (32u - count));
}

#define QUARTER_ROUND(a, b, c, d)                                                                  \
    do                                                                                             \
    {                                                                                              \
        x[a] += x[b];                                                                              \
        x[d] = RotateLeft(x[d] ^ x[a], 16u);                                                       \
        x[c] += x[d];                                                                              \
        x[b] = RotateLeft(x[b] ^ x[c], 12u);                                                       \
        x[a] += x[b];                                                                              \
        x[d] = RotateLeft(x[d] ^ x[a], 8u);                                                        \
        x[c] += x[d];                                                                              \
        x[b] = RotateLeft(x[b] ^ x[c], 7u);                                                        \
    } while (0)

static void ChaChaSetup(uint32_t state[16], const uint8_t key[NET_KEY_SIZE], uint32_t counter,
                        const uint8_t nonce[NET_NONCE_SIZE])
{
    /* "expand 32-byte k" */
    state[0] = UINT32_C(0x61707865);
    state[1] = UINT32_C(0x3320646e);
    state[2] = UINT32_C(0x79622d32);
    state[3] = UINT32_C(0x6b206574);
    for (size_t index = 0u; index < 8u; ++index)
        state[4u + index] = Load32(key + index * 4u);
    state[12] = counter;
    state[13] = Load32(nonce);
    state[14] = Load32(nonce + 4u);
    state[15] = Load32(nonce + 8u);
}

static void ChaChaBlock(const uint32_t state[16], uint8_t output[64])
{
    uint32_t x[16];
    memcpy(x, state, sizeof(x));
    for (uint32_t round = 0u; round < 10u; ++round)
    {
        QUARTER_ROUND(0, 4, 8, 12);
        QUARTER_ROUND(1, 5, 9, 13);
        QUARTER_ROUND(2, 6, 10, 14);
        QUARTER_ROUND(3, 7, 11, 15);
        QUARTER_ROUND(0, 5, 10, 15);
        QUARTER_ROUND(1, 6, 11, 12);
        QUARTER_ROUND(2, 7, 8, 13);
        QUARTER_ROUND(3, 4, 9, 14);
    }
    for (size_t index = 0u; index < 16u; ++index)
        Store32(output + index * 4u, x[index] + state[index]);
}

void NetChaCha20Block(const uint8_t key[NET_KEY_SIZE], uint32_t counter,
                      const uint8_t nonce[NET_NONCE_SIZE], uint8_t output[64])
{
    uint32_t state[16];
    ChaChaSetup(state, key, counter, nonce);
    ChaChaBlock(state, output);
    NetWipe(state, sizeof(state));
}

void NetChaCha20Xor(const uint8_t key[NET_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[NET_NONCE_SIZE], const uint8_t *input, uint8_t *output,
                    size_t size)
{
    uint32_t state[16];
    uint8_t block[64];
    ChaChaSetup(state, key, counter, nonce);
    while (size != 0u)
    {
        ChaChaBlock(state, block);
        ++state[12];
        const size_t part = size < sizeof(block) ? size : sizeof(block);
        for (size_t index = 0u; index < part; ++index)
            output[index] = (uint8_t)(input[index] ^ block[index]);
        input += part;
        output += part;
        size -= part;
    }
    NetWipe(state, sizeof(state));
    NetWipe(block, sizeof(block));
}

/* ---- Poly1305 (RFC 8439, раздел 2.5) ------------------------------------------
 *
 * Аккумулятор и r хранятся пятью разрядами по 26 бит, поэтому произведения
 * и их суммы помещаются в 64 бита без переноса. По модулю 2^130 - 5 старшая
 * часть возвращается вниз умножением на 5. */

#define POLY_MASK UINT32_C(0x03FFFFFF)

void NetPoly1305Begin(NetPoly1305 *state, const uint8_t key[NET_KEY_SIZE])
{
    uint8_t r[16];
    memcpy(r, key, sizeof(r));
    /* Обрезка r из стандарта: старшие 4 бита байтов 3, 7, 11, 15 и младшие
     * 2 бита байтов 4, 8, 12 равны нулю. */
    r[3] &= 15u;
    r[7] &= 15u;
    r[11] &= 15u;
    r[15] &= 15u;
    r[4] &= 252u;
    r[8] &= 252u;
    r[12] &= 252u;
    const uint32_t t0 = Load32(r);
    const uint32_t t1 = Load32(r + 4u);
    const uint32_t t2 = Load32(r + 8u);
    const uint32_t t3 = Load32(r + 12u);
    state->r[0] = t0 & POLY_MASK;
    state->r[1] = ((t0 >> 26u) | (t1 << 6u)) & POLY_MASK;
    state->r[2] = ((t1 >> 20u) | (t2 << 12u)) & POLY_MASK;
    state->r[3] = ((t2 >> 14u) | (t3 << 18u)) & POLY_MASK;
    state->r[4] = t3 >> 8u;
    for (uint32_t index = 0u; index < 5u; ++index)
        state->h[index] = 0u;
    for (size_t index = 0u; index < 4u; ++index)
        state->pad[index] = Load32(key + 16u + index * 4u);
    state->buffered = 0u;
    NetWipe(r, sizeof(r));
}

/* h = (h + блок) * r. highBit — бит 2^128 полного блока; у дополненного
 * последнего блока единица уже стоит внутри 16 байт. */
static void PolyBlock(NetPoly1305 *state, const uint8_t block[16], uint32_t highBit)
{
    const uint32_t t0 = Load32(block);
    const uint32_t t1 = Load32(block + 4u);
    const uint32_t t2 = Load32(block + 8u);
    const uint32_t t3 = Load32(block + 12u);
    const uint64_t h0 = (uint64_t)state->h[0] + (t0 & POLY_MASK);
    const uint64_t h1 = (uint64_t)state->h[1] + (((t0 >> 26u) | (t1 << 6u)) & POLY_MASK);
    const uint64_t h2 = (uint64_t)state->h[2] + (((t1 >> 20u) | (t2 << 12u)) & POLY_MASK);
    const uint64_t h3 = (uint64_t)state->h[3] + (((t2 >> 14u) | (t3 << 18u)) & POLY_MASK);
    const uint64_t h4 = (uint64_t)state->h[4] + ((t3 >> 8u) | (highBit << 24u));
    const uint64_t r0 = state->r[0];
    const uint64_t r1 = state->r[1];
    const uint64_t r2 = state->r[2];
    const uint64_t r3 = state->r[3];
    const uint64_t r4 = state->r[4];
    const uint64_t s1 = r1 * 5u;
    const uint64_t s2 = r2 * 5u;
    const uint64_t s3 = r3 * 5u;
    const uint64_t s4 = r4 * 5u;
    uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
    uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
    uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
    uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
    uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
    d1 += d0 >> 26u;
    d2 += d1 >> 26u;
    d3 += d2 >> 26u;
    d4 += d3 >> 26u;
    d0 = (d0 & POLY_MASK) + (d4 >> 26u) * 5u;
    d1 = (d1 & POLY_MASK) + (d0 >> 26u);
    state->h[0] = (uint32_t)d0 & POLY_MASK;
    state->h[1] = (uint32_t)d1;
    state->h[2] = (uint32_t)d2 & POLY_MASK;
    state->h[3] = (uint32_t)d3 & POLY_MASK;
    state->h[4] = (uint32_t)d4 & POLY_MASK;
}

void NetPoly1305Update(NetPoly1305 *state, const uint8_t *bytes, size_t size)
{
    if (state->buffered != 0u)
    {
        while (size != 0u && state->buffered < 16u)
        {
            state->buffer[state->buffered++] = *bytes++;
            --size;
        }
        if (state->buffered < 16u)
            return;
        PolyBlock(state, state->buffer, 1u);
        state->buffered = 0u;
    }
    while (size >= 16u)
    {
        PolyBlock(state, bytes, 1u);
        bytes += 16u;
        size -= 16u;
    }
    while (size != 0u)
    {
        state->buffer[state->buffered++] = *bytes++;
        --size;
    }
}

void NetPoly1305Finish(NetPoly1305 *state, uint8_t tag[NET_TAG_SIZE])
{
    if (state->buffered != 0u)
    {
        state->buffer[state->buffered++] = 1u;
        while (state->buffered < 16u)
            state->buffer[state->buffered++] = 0u;
        PolyBlock(state, state->buffer, 0u);
    }
    uint32_t h0 = state->h[0];
    uint32_t h1 = state->h[1];
    uint32_t h2 = state->h[2];
    uint32_t h3 = state->h[3];
    uint32_t h4 = state->h[4];
    uint32_t carry = h1 >> 26u;
    h1 &= POLY_MASK;
    h2 += carry;
    carry = h2 >> 26u;
    h2 &= POLY_MASK;
    h3 += carry;
    carry = h3 >> 26u;
    h3 &= POLY_MASK;
    h4 += carry;
    carry = h4 >> 26u;
    h4 &= POLY_MASK;
    h0 += carry * 5u;
    carry = h0 >> 26u;
    h0 &= POLY_MASK;
    h1 += carry;
    /* g = h + 5 - 2^130: неотрицательно ровно тогда, когда h >= 2^130 - 5. */
    uint32_t g0 = h0 + 5u;
    carry = g0 >> 26u;
    g0 &= POLY_MASK;
    uint32_t g1 = h1 + carry;
    carry = g1 >> 26u;
    g1 &= POLY_MASK;
    uint32_t g2 = h2 + carry;
    carry = g2 >> 26u;
    g2 &= POLY_MASK;
    uint32_t g3 = h3 + carry;
    carry = g3 >> 26u;
    g3 &= POLY_MASK;
    const uint32_t g4 = h4 + carry - (UINT32_C(1) << 26u);
    /* Все единицы, если заём не случился, то есть нужно взять g. */
    const uint32_t takeG = (g4 >> 31u) - 1u;
    h0 = (h0 & ~takeG) | (g0 & takeG);
    h1 = (h1 & ~takeG) | (g1 & takeG);
    h2 = (h2 & ~takeG) | (g2 & takeG);
    h3 = (h3 & ~takeG) | (g3 & takeG);
    h4 = (h4 & ~takeG) | (g4 & takeG);
    const uint32_t word0 = h0 | (h1 << 26u);
    const uint32_t word1 = (h1 >> 6u) | (h2 << 20u);
    const uint32_t word2 = (h2 >> 12u) | (h3 << 14u);
    const uint32_t word3 = (h3 >> 18u) | (h4 << 8u);
    uint64_t sum = (uint64_t)word0 + state->pad[0];
    Store32(tag, (uint32_t)sum);
    sum = (uint64_t)word1 + state->pad[1] + (sum >> 32u);
    Store32(tag + 4u, (uint32_t)sum);
    sum = (uint64_t)word2 + state->pad[2] + (sum >> 32u);
    Store32(tag + 8u, (uint32_t)sum);
    sum = (uint64_t)word3 + state->pad[3] + (sum >> 32u);
    Store32(tag + 12u, (uint32_t)sum);
    NetWipe(state, sizeof(*state));
}

/* ---- AEAD (RFC 8439, раздел 2.8) ---------------------------------------------- */

static void AeadTag(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                    const uint8_t *aad, size_t aadSize, const uint8_t *ciphertext, size_t size,
                    uint8_t tag[NET_TAG_SIZE])
{
    static const uint8_t zeros[16] = {0};
    uint8_t block[64];
    uint8_t lengths[16];
    NetPoly1305 mac;
    /* Одноразовый ключ Poly1305 — первые 32 байта блока со счётчиком 0. */
    NetChaCha20Block(key, 0u, nonce, block);
    NetPoly1305Begin(&mac, block);
    NetPoly1305Update(&mac, aad, aadSize);
    NetPoly1305Update(&mac, zeros, (16u - (aadSize & 15u)) & 15u);
    NetPoly1305Update(&mac, ciphertext, size);
    NetPoly1305Update(&mac, zeros, (16u - (size & 15u)) & 15u);
    Store64(lengths, (uint64_t)aadSize);
    Store64(lengths + 8u, (uint64_t)size);
    NetPoly1305Update(&mac, lengths, sizeof(lengths));
    NetPoly1305Finish(&mac, tag);
    NetWipe(block, sizeof(block));
}

void NetAeadSeal(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *plaintext, size_t size,
                 uint8_t *ciphertext, uint8_t tag[NET_TAG_SIZE])
{
    NetChaCha20Xor(key, 1u, nonce, plaintext, ciphertext, size);
    AeadTag(key, nonce, aad, aadSize, ciphertext, size, tag);
}

bool NetAeadOpen(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *ciphertext, size_t size,
                 const uint8_t tag[NET_TAG_SIZE], uint8_t *plaintext)
{
    uint8_t expected[NET_TAG_SIZE];
    AeadTag(key, nonce, aad, aadSize, ciphertext, size, expected);
    if (!PlatformConstantTimeEqual(expected, tag, NET_TAG_SIZE))
        return false;
    NetChaCha20Xor(key, 1u, nonce, ciphertext, plaintext, size);
    return true;
}

/* ---- X25519 (RFC 7748) ----------------------------------------------------------
 *
 * Элемент поля 2^255 - 19 — пять разрядов по 51 биту в uint64_t: разряд i
 * весит 2^(51i). Произведение двух разрядов занимает до 108 бит, и суммы
 * таких произведений копятся в 128-битном Wide. Разряды всегда
 * неотрицательны: вычитание прибавляет 2p, поэтому знаковых сдвигов нет.
 *
 * «Почти приведённый» элемент — выход умножения и возведения в квадрат: все
 * разряды меньше 2^51, кроме второго, который меньше 2^51 + 2^13. Сумма двух
 * таких элементов и разность почти приведённых дают разряды меньше 2^53, а
 * умножение принимает разряды до 2^54: в сумме для разряда пять
 * произведений, и у четырёх из них множитель 19, поэтому она меньше 2^115, а
 * перенос из неё помещается в 64 бита. */

#define FIELD_MASK ((UINT64_C(1) << 51u) - 1u)
/* (486662 - 2) / 4 */
#define FIELD_A24 UINT64_C(121665)

typedef uint64_t Field[5];

/* Тест собирает переносимое 128-битное умножение и там, где компилятор
 * умеет быстрое. */
#if defined(__SIZEOF_INT128__) && !defined(NET_CRYPTO_PORTABLE)

/* GCC и Clang, включая clang-cl, держат 128-битное целое на 64-битных целях:
 * произведение становится одной инструкцией, сумма — парой. */
#if defined(__GNUC__)
__extension__ typedef unsigned __int128 Wide;
#else
typedef unsigned __int128 Wide;
#endif

static Wide WideMultiply(uint64_t left, uint64_t right)
{
    return (Wide)left * right;
}

static Wide WideAdd(Wide sum, Wide value)
{
    return sum + value;
}

static Wide WideAddSmall(Wide sum, uint64_t value)
{
    return sum + value;
}

static uint64_t WideLow51(Wide value)
{
    return (uint64_t)value & FIELD_MASK;
}

/* value >> 51; value меньше 2^115. */
static uint64_t WideHigh51(Wide value)
{
    return (uint64_t)(value >> 51u);
}

#else

typedef struct Wide
{
    uint64_t low;
    uint64_t high;
} Wide;

#if defined(_MSC_VER) && !defined(__clang__) && !defined(NET_CRYPTO_PORTABLE) &&                   \
    (defined(_M_X64) || defined(_M_ARM64))
#define NET_MSVC_WIDE 1
#endif

static Wide WideMultiply(uint64_t left, uint64_t right)
{
    Wide product;
#if defined(NET_MSVC_WIDE) && defined(_M_X64)
    product.low = _umul128(left, right, &product.high);
#elif defined(NET_MSVC_WIDE)
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

static Wide WideAdd(Wide sum, Wide value)
{
#if defined(NET_MSVC_WIDE) && defined(_M_X64)
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

static Wide WideAddSmall(Wide sum, uint64_t value)
{
    const Wide wide = {value, 0u};
    return WideAdd(sum, wide);
}

static uint64_t WideLow51(Wide value)
{
    return value.low & FIELD_MASK;
}

/* value >> 51; value меньше 2^115. */
static uint64_t WideHigh51(Wide value)
{
    return (value.high << 13u) | (value.low >> 51u);
}

#endif

static Wide WideMultiplyAdd(Wide sum, uint64_t left, uint64_t right)
{
    return WideAdd(sum, WideMultiply(left, right));
}

static uint64_t Load64(const uint8_t *bytes)
{
    return (uint64_t)Load32(bytes) | ((uint64_t)Load32(bytes + 4u) << 32u);
}

/* Суммы произведений для пяти разрядов становятся почти приведённым
 * элементом. Каждая сумма меньше 2^115. */
static void FieldReduce(Field output, Wide t0, Wide t1, Wide t2, Wide t3, Wide t4)
{
    t1 = WideAddSmall(t1, WideHigh51(t0));
    t2 = WideAddSmall(t2, WideHigh51(t1));
    t3 = WideAddSmall(t3, WideHigh51(t2));
    t4 = WideAddSmall(t4, WideHigh51(t3));
    /* 2^255 = 19 по модулю p. Сумма старшего разряда — не больше пяти
     * произведений без множителя 19, перенос из неё меньше 5 * 2^57 + 2^13,
     * и младший разряд с ним остаётся меньше 2^64. */
    const uint64_t low = WideLow51(t0) + 19u * WideHigh51(t4);
    output[0] = low & FIELD_MASK;
    output[1] = WideLow51(t1) + (low >> 51u);
    output[2] = WideLow51(t2);
    output[3] = WideLow51(t3);
    output[4] = WideLow51(t4);
}

static void FieldAdd(Field output, const Field left, const Field right)
{
    for (uint32_t index = 0u; index < 5u; ++index)
        output[index] = left[index] + right[index];
}

/* left + 2p - right. right обязан быть почти приведённым: тогда каждый его
 * разряд не больше разряда числа 2p. */
static void FieldSubtract(Field output, const Field left, const Field right)
{
    output[0] = left[0] + ((UINT64_C(1) << 52u) - 38u) - right[0];
    for (uint32_t index = 1u; index < 5u; ++index)
        output[index] = left[index] + ((UINT64_C(1) << 52u) - 2u) - right[index];
}

static void FieldMultiply(Field output, const Field left, const Field right)
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
static void FieldSquare(Field output, const Field value)
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

/* value^(2^count), count не меньше единицы. */
static void FieldSquareTimes(Field output, const Field value, uint32_t count)
{
    FieldSquare(output, value);
    for (uint32_t index = 1u; index < count; ++index)
        FieldSquare(output, output);
}

static void FieldMultiplyA24(Field output, const Field value)
{
    FieldReduce(output, WideMultiply(value[0], FIELD_A24), WideMultiply(value[1], FIELD_A24),
                WideMultiply(value[2], FIELD_A24), WideMultiply(value[3], FIELD_A24),
                WideMultiply(value[4], FIELD_A24));
}

/* Обмен без ветвления: swap равен 0 или 1. */
static void FieldSwap(Field left, Field right, uint64_t swap)
{
    const uint64_t mask = 0u - swap;
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        const uint64_t difference = mask & (left[index] ^ right[index]);
        left[index] ^= difference;
        right[index] ^= difference;
    }
}

/* value^(p - 2) = value^(2^255 - 21): 254 возведения в квадрат и 11
 * умножений. onesN — value^(2^N - 1), показатель из N единиц. */
static void FieldInvert(Field output, const Field value)
{
    Field square;
    Field power9;
    Field power11;
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
    FieldMultiply(power, power, ones50);
    /* 2^255 - 32, затем + 11. */
    FieldSquareTimes(power, power, 5u);
    FieldMultiply(output, power, power11);
    NetWipe(square, sizeof(Field));
    NetWipe(power9, sizeof(Field));
    NetWipe(power11, sizeof(Field));
    NetWipe(ones5, sizeof(Field));
    NetWipe(ones10, sizeof(Field));
    NetWipe(ones20, sizeof(Field));
    NetWipe(ones50, sizeof(Field));
    NetWipe(ones100, sizeof(Field));
    NetWipe(power, sizeof(Field));
}

static void FieldUnpack(Field output, const uint8_t bytes[NET_KEY_SIZE])
{
    /* Разряд i начинается с бита 51i. Старший бит u-координаты не
     * используется: маска старшего разряда его отбрасывает. */
    output[0] = Load64(bytes) & FIELD_MASK;
    output[1] = (Load64(bytes + 6u) >> 3u) & FIELD_MASK;
    output[2] = (Load64(bytes + 12u) >> 6u) & FIELD_MASK;
    output[3] = (Load64(bytes + 19u) >> 1u) & FIELD_MASK;
    output[4] = (Load64(bytes + 24u) >> 12u) & FIELD_MASK;
}

/* Канонический вид: значение строго меньше p. value почти приведён, то есть
 * меньше 2^255 + 2^64 и тем более меньше 2p. */
static void FieldPack(uint8_t bytes[NET_KEY_SIZE], const Field value)
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
    Store64(bytes, f0 | (f1 << 51u));
    Store64(bytes + 8u, (f1 >> 13u) | (f2 << 38u));
    Store64(bytes + 16u, (f2 >> 26u) | (f3 << 25u));
    Store64(bytes + 24u, (f3 >> 39u) | (f4 << 12u));
}

bool NetX25519(const uint8_t scalar[NET_KEY_SIZE], const uint8_t point[NET_KEY_SIZE],
               uint8_t output[NET_KEY_SIZE])
{
    uint8_t clamped[NET_KEY_SIZE];
    memcpy(clamped, scalar, NET_KEY_SIZE);
    clamped[0] &= 248u;
    clamped[31] = (uint8_t)((clamped[31] & 127u) | 64u);

    Field x1;
    Field x2 = {1u};
    Field z2 = {0u};
    Field x3;
    Field z3 = {1u};
    Field a;
    Field b;
    Field c;
    Field d;
    Field e;
    Field aa;
    Field bb;
    FieldUnpack(x1, point);
    memcpy(x3, x1, sizeof(Field));
    uint64_t swap = 0u;
    /* Лестница Монтгомери из раздела 5 стандарта. */
    for (int32_t bit = 254; bit >= 0; --bit)
    {
        const uint64_t scalarBit = (uint64_t)(clamped[bit >> 3] >> (bit & 7)) & 1u;
        swap ^= scalarBit;
        FieldSwap(x2, x3, swap);
        FieldSwap(z2, z3, swap);
        swap = scalarBit;
        FieldAdd(a, x2, z2);
        FieldSubtract(b, x2, z2);
        FieldAdd(c, x3, z3);
        FieldSubtract(d, x3, z3);
        FieldSquare(aa, a);
        FieldSquare(bb, b);
        FieldSubtract(e, aa, bb);
        FieldMultiply(d, d, a);
        FieldMultiply(c, c, b);
        FieldAdd(x3, d, c);
        FieldSquare(x3, x3);
        FieldSubtract(z3, d, c);
        FieldSquare(z3, z3);
        FieldMultiply(z3, z3, x1);
        FieldMultiply(x2, aa, bb);
        FieldMultiplyA24(z2, e);
        FieldAdd(z2, z2, aa);
        FieldMultiply(z2, z2, e);
    }
    FieldSwap(x2, x3, swap);
    FieldSwap(z2, z3, swap);
    FieldInvert(z2, z2);
    FieldMultiply(x2, x2, z2);
    FieldPack(output, x2);

    uint8_t nonZero = 0u;
    for (uint32_t index = 0u; index < NET_KEY_SIZE; ++index)
        nonZero |= output[index];
    NetWipe(clamped, sizeof(clamped));
    NetWipe(x1, sizeof(Field));
    NetWipe(x2, sizeof(Field));
    NetWipe(z2, sizeof(Field));
    NetWipe(x3, sizeof(Field));
    NetWipe(z3, sizeof(Field));
    NetWipe(a, sizeof(Field));
    NetWipe(b, sizeof(Field));
    NetWipe(c, sizeof(Field));
    NetWipe(d, sizeof(Field));
    NetWipe(e, sizeof(Field));
    NetWipe(aa, sizeof(Field));
    NetWipe(bb, sizeof(Field));
    return nonZero != 0u;
}

void NetX25519Public(const uint8_t privateKey[NET_KEY_SIZE], uint8_t publicKey[NET_KEY_SIZE])
{
    static const uint8_t base[NET_KEY_SIZE] = {9u};
    (void)NetX25519(privateKey, base, publicKey);
}

/* ---- HMAC-SHA-256 и HKDF --------------------------------------------------------- */

#define HMAC_BLOCK 64u

bool NetHmacSha256(const uint8_t *key, size_t keySize, const uint8_t *message, size_t size,
                   uint8_t output[NET_HASH_SIZE])
{
    uint8_t keyBlock[HMAC_BLOCK] = {0};
    uint8_t buffer[HMAC_BLOCK + NET_HMAC_MESSAGE_CAPACITY];
    uint8_t inner[NET_HASH_SIZE];
    if (size > NET_HMAC_MESSAGE_CAPACITY)
        return false;
    if (keySize > HMAC_BLOCK)
    {
        if (!PlatformSha256(key, keySize, keyBlock))
            return false;
    }
    else if (keySize != 0u)
    {
        memcpy(keyBlock, key, keySize);
    }
    for (uint32_t index = 0u; index < HMAC_BLOCK; ++index)
        buffer[index] = (uint8_t)(keyBlock[index] ^ 0x36u);
    if (size != 0u)
        memcpy(buffer + HMAC_BLOCK, message, size);
    bool succeeded = PlatformSha256(buffer, HMAC_BLOCK + size, inner);
    for (uint32_t index = 0u; index < HMAC_BLOCK; ++index)
        buffer[index] = (uint8_t)(keyBlock[index] ^ 0x5cu);
    memcpy(buffer + HMAC_BLOCK, inner, NET_HASH_SIZE);
    succeeded = succeeded && PlatformSha256(buffer, HMAC_BLOCK + NET_HASH_SIZE, output);
    NetWipe(keyBlock, sizeof(keyBlock));
    NetWipe(buffer, sizeof(buffer));
    NetWipe(inner, sizeof(inner));
    return succeeded;
}

bool NetHkdfExtract(const uint8_t *salt, size_t saltSize, const uint8_t *input, size_t inputSize,
                    uint8_t prk[NET_HASH_SIZE])
{
    return NetHmacSha256(salt, saltSize, input, inputSize, prk);
}

bool NetHkdfExpand(const uint8_t prk[NET_HASH_SIZE], const char *label,
                   uint8_t output[NET_KEY_SIZE])
{
    /* T(1) = HMAC(PRK, info | 0x01): одного блока хватает на ключ. */
    uint8_t info[33];
    size_t length = 0u;
    while (label[length] != '\0' && length < sizeof(info) - 1u)
    {
        info[length] = (uint8_t)label[length];
        ++length;
    }
    info[length] = 1u;
    return NetHmacSha256(prk, NET_HASH_SIZE, info, length + 1u, output);
}
