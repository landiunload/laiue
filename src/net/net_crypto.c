#include "net/net_crypto.h"

#include "platform/system.h"

#include <string.h>

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
 * Элемент поля 2^255 - 19 — шестнадцать разрядов по 16 бит в uint64_t.
 * Разряды всегда неотрицательны: вычитание прибавляет 8p, поэтому знаковых
 * сдвигов нет. «Почти приведённый» элемент — выход умножения: все разряды
 * меньше 2^16, кроме младшего, который меньше 2^16 + 38. Сложение даёт
 * разряды меньше 2^18, вычитание — меньше 2^20, и умножение принимает
 * именно такие: шестнадцать произведений по 2^40 и свёртка старшей половины
 * множителем 38 остаются меньше 2^50. */

typedef uint64_t Field[16];

static void FieldCarry(Field value)
{
    for (uint32_t index = 0u; index < 15u; ++index)
    {
        value[index + 1u] += value[index] >> 16u;
        value[index] &= 0xFFFFu;
    }
    /* 2^256 = 38 по модулю p. */
    const uint64_t top = value[15] >> 16u;
    value[15] &= 0xFFFFu;
    value[0] += 38u * top;
}

static void FieldAdd(Field output, const Field left, const Field right)
{
    for (uint32_t index = 0u; index < 16u; ++index)
        output[index] = left[index] + right[index];
}

/* right обязан быть почти приведённым. */
static void FieldSubtract(Field output, const Field left, const Field right)
{
    /* Разряды числа 8p: вычитаемое их не превышает. */
    output[0] = left[0] + UINT64_C(0x7FF68) - right[0];
    for (uint32_t index = 1u; index < 15u; ++index)
        output[index] = left[index] + UINT64_C(0x7FFF8) - right[index];
    output[15] = left[15] + UINT64_C(0x3FFF8) - right[15];
}

static void FieldMultiply(Field output, const Field left, const Field right)
{
    uint64_t product[31] = {0};
    for (uint32_t i = 0u; i < 16u; ++i)
        for (uint32_t j = 0u; j < 16u; ++j)
            product[i + j] += left[i] * right[j];
    for (uint32_t index = 0u; index < 15u; ++index)
        product[index] += 38u * product[index + 16u];
    for (uint32_t index = 0u; index < 16u; ++index)
        output[index] = product[index];
    FieldCarry(output);
    FieldCarry(output);
}

/* Обмен без ветвления: swap равен 0 или 1. */
static void FieldSwap(Field left, Field right, uint64_t swap)
{
    const uint64_t mask = 0u - swap;
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        const uint64_t difference = mask & (left[index] ^ right[index]);
        left[index] ^= difference;
        right[index] ^= difference;
    }
}

/* value^(p - 2): показатель 2^255 - 21 состоит из единиц, кроме битов 2 и 4. */
static void FieldInvert(Field output, const Field value)
{
    Field result;
    memcpy(result, value, sizeof(Field));
    for (int32_t bit = 253; bit >= 0; --bit)
    {
        FieldMultiply(result, result, result);
        if (bit != 2 && bit != 4)
            FieldMultiply(result, result, value);
    }
    memcpy(output, result, sizeof(Field));
}

static void FieldUnpack(Field output, const uint8_t bytes[NET_KEY_SIZE])
{
    for (size_t index = 0u; index < 16u; ++index)
        output[index] = (uint64_t)bytes[index * 2u] | ((uint64_t)bytes[index * 2u + 1u] << 8u);
    /* Старший бит u-координаты не используется. */
    output[15] &= 0x7FFFu;
}

/* Канонический вид: значение строго меньше p. value почти приведён. */
static void FieldPack(uint8_t bytes[NET_KEY_SIZE], const Field value)
{
    Field reduced;
    memcpy(reduced, value, sizeof(Field));
    FieldCarry(reduced);
    FieldCarry(reduced);
    /* Теперь значение меньше 2^256 = 2p + 38: p вычитается не больше двух раз. */
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        Field lowered;
        uint64_t borrow = 0u;
        for (uint32_t index = 0u; index < 16u; ++index)
        {
            const uint64_t prime = index == 0u ? 0xFFEDu : index == 15u ? 0x7FFFu : 0xFFFFu;
            const uint64_t difference = reduced[index] - prime - borrow;
            borrow = difference >> 63u;
            lowered[index] = difference & 0xFFFFu;
        }
        /* Все единицы, если заёма нет и значение было не меньше p. */
        const uint64_t take = borrow - 1u;
        for (uint32_t index = 0u; index < 16u; ++index)
            reduced[index] = (reduced[index] & ~take) | (lowered[index] & take);
    }
    for (size_t index = 0u; index < 16u; ++index)
    {
        bytes[index * 2u] = (uint8_t)reduced[index];
        bytes[index * 2u + 1u] = (uint8_t)(reduced[index] >> 8u);
    }
}

bool NetX25519(const uint8_t scalar[NET_KEY_SIZE], const uint8_t point[NET_KEY_SIZE],
               uint8_t output[NET_KEY_SIZE])
{
    /* (486662 - 2) / 4 */
    static const Field a24 = {0xDB41u, 1u};
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
        FieldMultiply(aa, a, a);
        FieldMultiply(bb, b, b);
        FieldSubtract(e, aa, bb);
        FieldMultiply(d, d, a);
        FieldMultiply(c, c, b);
        FieldAdd(x3, d, c);
        FieldMultiply(x3, x3, x3);
        FieldSubtract(z3, d, c);
        FieldMultiply(z3, z3, z3);
        FieldMultiply(z3, z3, x1);
        FieldMultiply(x2, aa, bb);
        FieldMultiply(z2, a24, e);
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
