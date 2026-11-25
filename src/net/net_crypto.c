#include "net/net_crypto.h"

#include "crypto/field25519.h"
#include "crypto/wide.h"
#include "crypto/wipe.h"
#include "platform/system.h"

#include <string.h>

/* Векторы ChaCha20 и Poly1305. LAIUE_CRYPTO_PORTABLE отключает их вместе с
 * быстрым 128-битным умножением: так тест проверяет и переносимый код. */
#if !defined(LAIUE_CRYPTO_PORTABLE) && defined(__AVX2__)
#define NET_CHACHA_AVX2 1
#define NET_CHACHA_LANES 8u
#include <immintrin.h>
#elif !defined(LAIUE_CRYPTO_PORTABLE) && (defined(__SSE2__) || defined(_M_X64))
#define NET_CHACHA_SSE2 1
#define NET_CHACHA_LANES 4u
#include <emmintrin.h>
#if defined(__SSSE3__) || defined(__AVX__)
#include <tmmintrin.h>
#endif
#elif !defined(LAIUE_CRYPTO_PORTABLE) && (defined(__ARM_NEON) || defined(_M_ARM64))
#define NET_CHACHA_NEON 1
#define NET_CHACHA_LANES 4u
#include <arm_neon.h>
#endif

#if defined(_MSC_VER)
#define NET_NOINLINE __declspec(noinline)
#else
#define NET_NOINLINE __attribute__((noinline))
#endif

/* Встраивание обязательно только с оптимизацией. Без неё clang всё равно
 * встраивает, но каждой копии отводит свои слоты стека: кадр перерастает
 * страницу и требует __chkstk, которого в сборке без CRT нет. MSVC без
 * оптимизации не встраивает ничего. */
#if (defined(__clang__) || defined(__GNUC__)) && defined(__OPTIMIZE__)
#define NET_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER) && !defined(__clang__)
#define NET_FORCE_INLINE __forceinline
#else
#define NET_FORCE_INLINE inline
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
    CryptoWipe(bytes, size);
}

static uint64_t Load64(const uint8_t *bytes)
{
    return (uint64_t)Load32(bytes) | ((uint64_t)Load32(bytes + 4u) << 32u);
}

/* ---- ChaCha20 (RFC 8439, раздел 2.3) -----------------------------------------
 *
 * Поток считается группами блоков, где есть векторы: AVX2 — по восемь, SSE2 и
 * NEON — по четыре. Каждое слово состояния лежит в своём векторе, по блоку в
 * дорожке, поэтому четверть раунда — те же сложения, исключающие ИЛИ и
 * повороты, только над векторами. Скалярный код считает одиночный последний
 * блок и всё там, где векторов нет. */

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

#if defined(NET_CHACHA_LANES)

#if defined(NET_CHACHA_AVX2)

typedef __m256i Lane;

static Lane LaneSet(uint32_t value)
{
    return _mm256_set1_epi32((int)value);
}

static Lane LaneAdd(Lane left, Lane right)
{
    return _mm256_add_epi32(left, right);
}

static Lane LaneXor(Lane left, Lane right)
{
    return _mm256_xor_si256(left, right);
}

/* Повороты на 16 и 8 бит — перестановки байтов внутри слова. */
static Lane LaneRotate16(Lane value)
{
    return _mm256_shuffle_epi8(value, _mm256_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1,
                                                      0, 3, 2, 13, 12, 15, 14, 9, 8, 11, 10, 5, 4,
                                                      7, 6, 1, 0, 3, 2));
}

static Lane LaneRotate12(Lane value)
{
    return _mm256_or_si256(_mm256_slli_epi32(value, 12), _mm256_srli_epi32(value, 20));
}

static Lane LaneRotate8(Lane value)
{
    return _mm256_shuffle_epi8(value, _mm256_set_epi8(14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2,
                                                      1, 0, 3, 14, 13, 12, 15, 10, 9, 8, 11, 6, 5,
                                                      4, 7, 2, 1, 0, 3));
}

static Lane LaneRotate7(Lane value)
{
    return _mm256_or_si256(_mm256_slli_epi32(value, 7), _mm256_srli_epi32(value, 25));
}

static Lane LaneCounters(void)
{
    return _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
}

static __m128i Load16(const uint8_t *bytes)
{
    return _mm_loadu_si128((const __m128i *)(const void *)bytes);
}

static void XorStore(const uint8_t *input, uint8_t *output, __m128i stream)
{
    _mm_storeu_si128((__m128i *)(void *)output, _mm_xor_si128(Load16(input), stream));
}

/* Слова 4g..4g+3 всех восьми блоков: транспонирование четвёрки внутри
 * каждой 128-битной половины. Нижняя половина — блоки 0..3, верхняя —
 * 4..7. */
static void LaneStoreGroup(const Lane x[4], size_t group, const uint8_t *input, uint8_t *output)
{
    const Lane ab01 = _mm256_unpacklo_epi32(x[0], x[1]);
    const Lane cd01 = _mm256_unpacklo_epi32(x[2], x[3]);
    const Lane ab23 = _mm256_unpackhi_epi32(x[0], x[1]);
    const Lane cd23 = _mm256_unpackhi_epi32(x[2], x[3]);
    const Lane blocks[4] = {_mm256_unpacklo_epi64(ab01, cd01), _mm256_unpackhi_epi64(ab01, cd01),
                            _mm256_unpacklo_epi64(ab23, cd23), _mm256_unpackhi_epi64(ab23, cd23)};
    for (size_t block = 0u; block < 4u; ++block)
    {
        const size_t low = block * 64u + group * 16u;
        const size_t high = low + 256u;
        XorStore(input + low, output + low, _mm256_castsi256_si128(blocks[block]));
        XorStore(input + high, output + high, _mm256_extracti128_si256(blocks[block], 1));
    }
}

#elif defined(NET_CHACHA_SSE2)

typedef __m128i Lane;

static Lane LaneSet(uint32_t value)
{
    return _mm_set1_epi32((int)value);
}

static Lane LaneAdd(Lane left, Lane right)
{
    return _mm_add_epi32(left, right);
}

static Lane LaneXor(Lane left, Lane right)
{
    return _mm_xor_si128(left, right);
}

static Lane LaneRotate16(Lane value)
{
#if defined(__SSSE3__) || defined(__AVX__)
    return _mm_shuffle_epi8(value,
                            _mm_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2));
#else
    return _mm_or_si128(_mm_slli_epi32(value, 16), _mm_srli_epi32(value, 16));
#endif
}

static Lane LaneRotate12(Lane value)
{
    return _mm_or_si128(_mm_slli_epi32(value, 12), _mm_srli_epi32(value, 20));
}

static Lane LaneRotate8(Lane value)
{
#if defined(__SSSE3__) || defined(__AVX__)
    return _mm_shuffle_epi8(value,
                            _mm_set_epi8(14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3));
#else
    return _mm_or_si128(_mm_slli_epi32(value, 8), _mm_srli_epi32(value, 24));
#endif
}

static Lane LaneRotate7(Lane value)
{
    return _mm_or_si128(_mm_slli_epi32(value, 7), _mm_srli_epi32(value, 25));
}

static Lane LaneCounters(void)
{
    return _mm_set_epi32(3, 2, 1, 0);
}

static Lane Load16(const uint8_t *bytes)
{
    return _mm_loadu_si128((const __m128i *)(const void *)bytes);
}

static void XorStore(const uint8_t *input, uint8_t *output, Lane stream)
{
    _mm_storeu_si128((__m128i *)(void *)output, _mm_xor_si128(Load16(input), stream));
}

/* Слова 4g..4g+3 четырёх блоков: транспонирование четвёрки. */
static void LaneStoreGroup(const Lane x[4], size_t group, const uint8_t *input, uint8_t *output)
{
    const Lane ab01 = _mm_unpacklo_epi32(x[0], x[1]);
    const Lane cd01 = _mm_unpacklo_epi32(x[2], x[3]);
    const Lane ab23 = _mm_unpackhi_epi32(x[0], x[1]);
    const Lane cd23 = _mm_unpackhi_epi32(x[2], x[3]);
    const Lane blocks[4] = {_mm_unpacklo_epi64(ab01, cd01), _mm_unpackhi_epi64(ab01, cd01),
                            _mm_unpacklo_epi64(ab23, cd23), _mm_unpackhi_epi64(ab23, cd23)};
    for (size_t block = 0u; block < 4u; ++block)
    {
        const size_t offset = block * 64u + group * 16u;
        XorStore(input + offset, output + offset, blocks[block]);
    }
}

#else

typedef uint32x4_t Lane;

static Lane LaneSet(uint32_t value)
{
    return vdupq_n_u32(value);
}

static Lane LaneAdd(Lane left, Lane right)
{
    return vaddq_u32(left, right);
}

static Lane LaneXor(Lane left, Lane right)
{
    return veorq_u32(left, right);
}

static Lane LaneRotate16(Lane value)
{
    return vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(value)));
}

static Lane LaneRotate12(Lane value)
{
    return vsriq_n_u32(vshlq_n_u32(value, 12), value, 20);
}

static Lane LaneRotate8(Lane value)
{
    return vsriq_n_u32(vshlq_n_u32(value, 8), value, 24);
}

static Lane LaneRotate7(Lane value)
{
    return vsriq_n_u32(vshlq_n_u32(value, 7), value, 25);
}

static Lane LaneCounters(void)
{
    static const uint32_t counters[4] = {0u, 1u, 2u, 3u};
    return vld1q_u32(counters);
}

static Lane Load16(const uint8_t *bytes)
{
    return vreinterpretq_u32_u8(vld1q_u8(bytes));
}

static void XorStore(const uint8_t *input, uint8_t *output, Lane stream)
{
    vst1q_u8(output, veorq_u8(vld1q_u8(input), vreinterpretq_u8_u32(stream)));
}

/* Слова 4g..4g+3 четырёх блоков: транспонирование четвёрки. */
static void LaneStoreGroup(const Lane x[4], size_t group, const uint8_t *input, uint8_t *output)
{
    const uint32x4x2_t ab = vtrnq_u32(x[0], x[1]);
    const uint32x4x2_t cd = vtrnq_u32(x[2], x[3]);
    const Lane blocks[4] = {vcombine_u32(vget_low_u32(ab.val[0]), vget_low_u32(cd.val[0])),
                            vcombine_u32(vget_low_u32(ab.val[1]), vget_low_u32(cd.val[1])),
                            vcombine_u32(vget_high_u32(ab.val[0]), vget_high_u32(cd.val[0])),
                            vcombine_u32(vget_high_u32(ab.val[1]), vget_high_u32(cd.val[1]))};
    for (size_t block = 0u; block < 4u; ++block)
    {
        const size_t offset = block * 64u + group * 16u;
        XorStore(input + offset, output + offset, blocks[block]);
    }
}

#endif

#define LANE_QUARTER_ROUND(a, b, c, d)                                                             \
    do                                                                                             \
    {                                                                                              \
        x[a] = LaneAdd(x[a], x[b]);                                                                \
        x[d] = LaneRotate16(LaneXor(x[d], x[a]));                                                  \
        x[c] = LaneAdd(x[c], x[d]);                                                                \
        x[b] = LaneRotate12(LaneXor(x[b], x[c]));                                                  \
        x[a] = LaneAdd(x[a], x[b]);                                                                \
        x[d] = LaneRotate8(LaneXor(x[d], x[a]));                                                   \
        x[c] = LaneAdd(x[c], x[d]);                                                                \
        x[b] = LaneRotate7(LaneXor(x[b], x[c]));                                                   \
    } while (0)

/* NET_CHACHA_LANES блоков подряд, начиная с state[12]; счётчик сдвигается. */
static NET_NOINLINE void ChaChaXorLanes(uint32_t state[16], const uint8_t *input, uint8_t *output)
{
    Lane initial[16];
    Lane x[16];
    for (size_t index = 0u; index < 16u; ++index)
        initial[index] = LaneSet(state[index]);
    initial[12] = LaneAdd(initial[12], LaneCounters());
    for (size_t index = 0u; index < 16u; ++index)
        x[index] = initial[index];
    for (uint32_t round = 0u; round < 10u; ++round)
    {
        LANE_QUARTER_ROUND(0, 4, 8, 12);
        LANE_QUARTER_ROUND(1, 5, 9, 13);
        LANE_QUARTER_ROUND(2, 6, 10, 14);
        LANE_QUARTER_ROUND(3, 7, 11, 15);
        LANE_QUARTER_ROUND(0, 5, 10, 15);
        LANE_QUARTER_ROUND(1, 6, 11, 12);
        LANE_QUARTER_ROUND(2, 7, 8, 13);
        LANE_QUARTER_ROUND(3, 4, 9, 14);
    }
    for (size_t index = 0u; index < 16u; ++index)
        x[index] = LaneAdd(x[index], initial[index]);
    for (size_t group = 0u; group < 4u; ++group)
        LaneStoreGroup(x + group * 4u, group, input, output);
    state[12] += NET_CHACHA_LANES;
}

/* Поток группы без исходного текста: исключающее ИЛИ с нулями. */
static const uint8_t chachaZeros[(size_t)NET_CHACHA_LANES * 64u] = {0};

#define CHACHA_GROUP_BYTES ((size_t)NET_CHACHA_LANES * 64u)
#define CHACHA_FIRST_BYTES CHACHA_GROUP_BYTES

#else

#define CHACHA_FIRST_BYTES ((size_t)64u)

#endif

/* output = input ^ stream; output может совпадать с input. Цикл по байтам
 * компилятор не всегда векторизует: буферы могут пересекаться. */
static void XorBytes(uint8_t *output, const uint8_t *input, const uint8_t *stream, size_t size)
{
    size_t index = 0u;
#if defined(NET_CHACHA_LANES)
    for (; index + 16u <= size; index += 16u)
        XorStore(input + index, output + index, Load16(stream + index));
#endif
    for (; index < size; ++index)
        output[index] = (uint8_t)(input[index] ^ stream[index]);
}

/* Первые CHACHA_FIRST_BYTES потока с блока state[12]: группа векторов или
 * один блок. Счётчик сдвигается. */
static void ChaChaFirst(uint32_t state[16], uint8_t stream[CHACHA_FIRST_BYTES])
{
#if defined(NET_CHACHA_LANES)
    ChaChaXorLanes(state, chachaZeros, stream);
#else
    ChaChaBlock(state, stream);
    ++state[12];
#endif
}

/* Поток с блока state[12]; счётчик сдвигается. Где есть векторы, неполную
 * группу в конце тоже считают они, через поток целой группы: уже два блока
 * так дешевле, чем по одному. Последний одиночный блок — скалярный. */
static void ChaChaXorFrom(uint32_t state[16], const uint8_t *input, uint8_t *output, size_t size)
{
#if defined(NET_CHACHA_LANES)
    while (size >= CHACHA_GROUP_BYTES)
    {
        ChaChaXorLanes(state, input, output);
        input += CHACHA_GROUP_BYTES;
        output += CHACHA_GROUP_BYTES;
        size -= CHACHA_GROUP_BYTES;
    }
    if (size > 64u)
    {
        uint8_t stream[CHACHA_GROUP_BYTES];
        ChaChaXorLanes(state, chachaZeros, stream);
        XorBytes(output, input, stream, size);
        NetWipe(stream, sizeof(stream));
        return;
    }
#endif
    if (size == 0u)
        return;
    uint8_t block[64];
    while (size != 0u)
    {
        ChaChaBlock(state, block);
        ++state[12];
        const size_t part = size < sizeof(block) ? size : sizeof(block);
        XorBytes(output, input, block, part);
        input += part;
        output += part;
        size -= part;
    }
    NetWipe(block, sizeof(block));
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
    ChaChaSetup(state, key, counter, nonce);
    ChaChaXorFrom(state, input, output, size);
    NetWipe(state, sizeof(state));
}

/* ---- Poly1305 (RFC 8439, раздел 2.5) ------------------------------------------
 *
 * Аккумулятор и r хранятся разрядами по 44, 44 и 42 бита. Произведение
 * разрядов меньше 2^92, и суммы трёх таких копятся в Wide. По модулю
 * 2^130 - 5 перенос за 2^130 возвращается вниз умножением на 5, а
 * произведения с весом 2^132 — умножением на 20. */

#define POLY_MASK44 ((UINT64_C(1) << 44u) - 1u)
#define POLY_MASK42 ((UINT64_C(1) << 42u) - 1u)

void NetPoly1305Begin(NetPoly1305 *state, const uint8_t key[NET_KEY_SIZE])
{
    /* Обрезка r из стандарта: старшие 4 бита байтов 3, 7, 11, 15 и младшие
     * 2 бита байтов 4, 8, 12 равны нулю. */
    const uint64_t t0 = Load64(key) & UINT64_C(0x0FFFFFFC0FFFFFFF);
    const uint64_t t1 = Load64(key + 8u) & UINT64_C(0x0FFFFFFC0FFFFFFC);
    state->r[0] = t0 & POLY_MASK44;
    state->r[1] = ((t0 >> 44u) | (t1 << 20u)) & POLY_MASK44;
    state->r[2] = t1 >> 24u;
    for (uint32_t index = 0u; index < 3u; ++index)
        state->h[index] = 0u;
    state->pad[0] = Load64(key + 16u);
    state->pad[1] = Load64(key + 24u);
    state->buffered = 0u;
}

/* h = (h + блок) * r для count блоков подряд. highBit — бит 2^128 полного
 * блока; у дополненного последнего блока единица уже стоит внутри 16 байт. */
static void PolyBlocksScalar(NetPoly1305 *state, const uint8_t *bytes, size_t count,
                             uint64_t highBit)
{
    const uint64_t r0 = state->r[0];
    const uint64_t r1 = state->r[1];
    const uint64_t r2 = state->r[2];
    const uint64_t s1 = r1 * 20u;
    const uint64_t s2 = r2 * 20u;
    uint64_t h0 = state->h[0];
    uint64_t h1 = state->h[1];
    uint64_t h2 = state->h[2];
    for (; count != 0u; --count)
    {
        const uint64_t t0 = Load64(bytes);
        const uint64_t t1 = Load64(bytes + 8u);
        bytes += 16u;
        h0 += t0 & POLY_MASK44;
        h1 += ((t0 >> 44u) | (t1 << 20u)) & POLY_MASK44;
        h2 += (t1 >> 24u) | (highBit << 40u);
        Wide d0 = WideMultiply(h0, r0);
        d0 = WideMultiplyAdd(d0, h1, s2);
        d0 = WideMultiplyAdd(d0, h2, s1);
        Wide d1 = WideMultiply(h0, r1);
        d1 = WideMultiplyAdd(d1, h1, r0);
        d1 = WideMultiplyAdd(d1, h2, s2);
        Wide d2 = WideMultiply(h0, r2);
        d2 = WideMultiplyAdd(d2, h1, r1);
        d2 = WideMultiplyAdd(d2, h2, r0);
        d1 = WideAddSmall(d1, WideShift(d0, 44u));
        d2 = WideAddSmall(d2, WideShift(d1, 44u));
        const uint64_t low = (WideLow(d0) & POLY_MASK44) + WideShift(d2, 42u) * 5u;
        h0 = low & POLY_MASK44;
        h1 = (WideLow(d1) & POLY_MASK44) + (low >> 44u);
        h2 = WideLow(d2) & POLY_MASK42;
    }
    state->h[0] = h0;
    state->h[1] = h1;
    state->h[2] = h2;
}

/* ---- Poly1305 векторами ------------------------------------------------------
 *
 * Длинный вход считают NET_POLY_LANES независимых аккумуляторов: дорожка
 * берёт каждый NET_POLY_LANES-й блок и после него умножается на r^L, где L —
 * число дорожек. Последнюю группу дорожки умножают на r^L, ..., r^1 — каждая
 * на степень, которая оставалась бы её блокам до конца, — и сумма дорожек
 * равна последовательному счёту. В дорожках числа — пять разрядов по 26 бит:
 * векторное умножение 32 на 32 бита даёт 64, а сумма пяти произведений
 * меньше 2^58. AVX2 ведёт четыре дорожки, SSE2 и NEON — две. */

#if defined(NET_CHACHA_AVX2)
#define NET_POLY_LANES 4u
#elif defined(NET_CHACHA_LANES)
#define NET_POLY_LANES 2u
#endif

#if defined(NET_POLY_LANES)

#define POLY_MASK26 ((UINT64_C(1) << 26u) - 1u)

typedef uint64_t Poly26[5];

/* Почти приведённое значение: все разряды меньше 2^26, кроме второго — он
 * меньше 2^26 + 2^11. Разряды входа меньше 2^60. */
static void Poly26Carry(Poly26 value)
{
    uint64_t carry = value[0] >> 26u;
    value[0] &= POLY_MASK26;
    value[1] += carry;
    carry = value[1] >> 26u;
    value[1] &= POLY_MASK26;
    value[2] += carry;
    carry = value[2] >> 26u;
    value[2] &= POLY_MASK26;
    value[3] += carry;
    carry = value[3] >> 26u;
    value[3] &= POLY_MASK26;
    value[4] += carry;
    carry = value[4] >> 26u;
    value[4] &= POLY_MASK26;
    value[0] += carry * 5u;
    carry = value[0] >> 26u;
    value[0] &= POLY_MASK26;
    value[1] += carry;
}

/* output = left * right по модулю 2^130 - 5; разряды обоих меньше 2^27. */
static void Poly26Multiply(Poly26 output, const Poly26 left, const Poly26 right)
{
    const uint64_t s1 = right[1] * 5u;
    const uint64_t s2 = right[2] * 5u;
    const uint64_t s3 = right[3] * 5u;
    const uint64_t s4 = right[4] * 5u;
    Poly26 d;
    d[0] = left[0] * right[0] + left[1] * s4 + left[2] * s3 + left[3] * s2 + left[4] * s1;
    d[1] = left[0] * right[1] + left[1] * right[0] + left[2] * s4 + left[3] * s3 + left[4] * s2;
    d[2] =
        left[0] * right[2] + left[1] * right[1] + left[2] * right[0] + left[3] * s4 + left[4] * s3;
    d[3] = left[0] * right[3] + left[1] * right[2] + left[2] * right[1] + left[3] * right[0] +
           left[4] * s4;
    d[4] = left[0] * right[4] + left[1] * right[3] + left[2] * right[2] + left[3] * right[1] +
           left[4] * right[0];
    Poly26Carry(d);
    for (size_t limb = 0u; limb < 5u; ++limb)
        output[limb] = d[limb];
}

/* Из разрядов по 44 бита: первые два меньше 2^44, третий меньше 2^43. */
static void Poly26From44(Poly26 output, uint64_t low, uint64_t middle, uint64_t high)
{
    output[0] = low & POLY_MASK26;
    output[1] = ((low >> 26u) | (middle << 18u)) & POLY_MASK26;
    output[2] = (middle >> 8u) & POLY_MASK26;
    output[3] = ((middle >> 34u) | (high << 10u)) & POLY_MASK26;
    output[4] = high >> 16u;
}

/* Обратно в разряды по 44 бита после Poly26Carry; третий выходит меньше
 * 2^42 + 2^17. */
static void Poly26To44(const Poly26 value, uint64_t output[3])
{
    uint64_t t = value[0] + (value[1] << 26u);
    output[0] = t & POLY_MASK44;
    t = (t >> 44u) + (value[2] << 8u) + (value[3] << 34u);
    output[1] = t & POLY_MASK44;
    output[2] = (t >> 44u) + (value[4] << 16u);
}

#if defined(NET_CHACHA_AVX2)

typedef __m256i PolyVector;
/* Множитель: значение в младших 32 битах каждой 64-битной дорожки. */
typedef __m256i PolyFactor;

/* Распаковка берёт по слову из каждой 128-битной половины, поэтому в
 * дорожках оказываются блоки 0, 2, 1 и 3: им в последней группе остаются
 * степени r^4, r^2, r^3 и r^1. */
static const uint32_t polyLastExponents[4] = {4u, 2u, 3u, 1u};

static PolyVector PolyAdd(PolyVector left, PolyVector right)
{
    return _mm256_add_epi64(left, right);
}

static PolyVector PolyLow26(PolyVector value)
{
    return _mm256_and_si256(value, _mm256_set1_epi64x((long long)POLY_MASK26));
}

static PolyVector PolyHigh26(PolyVector value)
{
    return _mm256_srli_epi64(value, 26);
}

static PolyVector PolyTimes5(PolyVector value)
{
    return _mm256_add_epi64(value, _mm256_slli_epi64(value, 2));
}

static PolyFactor PolyNarrow(PolyVector value)
{
    return value;
}

static PolyVector PolyMultiply(PolyFactor left, PolyFactor right)
{
    return _mm256_mul_epu32(left, right);
}

static PolyVector PolyMultiplyAdd(PolyVector sum, PolyFactor left, PolyFactor right)
{
    return _mm256_add_epi64(sum, _mm256_mul_epu32(left, right));
}

/* Разряд limb степеней exponent[0..3] — по дорожкам. */
static PolyVector PolyLanesOf(const Poly26 power[5], const uint32_t exponent[4], size_t limb)
{
    return _mm256_set_epi64x(
        (long long)power[exponent[3]][limb], (long long)power[exponent[2]][limb],
        (long long)power[exponent[1]][limb], (long long)power[exponent[0]][limb]);
}

static PolyVector PolyFirstLane(uint64_t value)
{
    return _mm256_set_epi64x(0, 0, 0, (long long)value);
}

static uint64_t PolySum(PolyVector value)
{
    uint64_t lanes[2];
    _mm_storeu_si128((__m128i *)(void *)lanes, _mm_add_epi64(_mm256_castsi256_si128(value),
                                                             _mm256_extracti128_si256(value, 1)));
    return lanes[0] + lanes[1];
}

/* Разряды четырёх блоков с битом 2^128. */
static void PolyLoad(const uint8_t *bytes, PolyVector m[5])
{
    const __m256i first = _mm256_loadu_si256((const __m256i *)(const void *)bytes);
    const __m256i second = _mm256_loadu_si256((const __m256i *)(const void *)(bytes + 32u));
    const __m256i low = _mm256_unpacklo_epi64(first, second);
    const __m256i high = _mm256_unpackhi_epi64(first, second);
    m[0] = PolyLow26(low);
    m[1] = PolyLow26(_mm256_srli_epi64(low, 26));
    m[2] = PolyLow26(_mm256_or_si256(_mm256_srli_epi64(low, 52), _mm256_slli_epi64(high, 12)));
    m[3] = PolyLow26(_mm256_srli_epi64(high, 14));
    m[4] = _mm256_or_si256(_mm256_srli_epi64(high, 40), _mm256_set1_epi64x(1 << 24));
}

#elif defined(NET_CHACHA_SSE2)

typedef __m128i PolyVector;
typedef __m128i PolyFactor;

static const uint32_t polyLastExponents[2] = {2u, 1u};

static PolyVector PolyAdd(PolyVector left, PolyVector right)
{
    return _mm_add_epi64(left, right);
}

static PolyVector PolyLow26(PolyVector value)
{
    return _mm_and_si128(value, _mm_set1_epi64x((long long)POLY_MASK26));
}

static PolyVector PolyHigh26(PolyVector value)
{
    return _mm_srli_epi64(value, 26);
}

static PolyVector PolyTimes5(PolyVector value)
{
    return _mm_add_epi64(value, _mm_slli_epi64(value, 2));
}

static PolyFactor PolyNarrow(PolyVector value)
{
    return value;
}

static PolyVector PolyMultiply(PolyFactor left, PolyFactor right)
{
    return _mm_mul_epu32(left, right);
}

static PolyVector PolyMultiplyAdd(PolyVector sum, PolyFactor left, PolyFactor right)
{
    return _mm_add_epi64(sum, _mm_mul_epu32(left, right));
}

static PolyVector PolyLanesOf(const Poly26 power[3], const uint32_t exponent[2], size_t limb)
{
    return _mm_set_epi64x((long long)power[exponent[1]][limb], (long long)power[exponent[0]][limb]);
}

static PolyVector PolyFirstLane(uint64_t value)
{
    return _mm_set_epi64x(0, (long long)value);
}

static uint64_t PolySum(PolyVector value)
{
    uint64_t lanes[2];
    _mm_storeu_si128((__m128i *)(void *)lanes, value);
    return lanes[0] + lanes[1];
}

static void PolyLoad(const uint8_t *bytes, PolyVector m[5])
{
    const __m128i first = Load16(bytes);
    const __m128i second = Load16(bytes + 16u);
    const __m128i low = _mm_unpacklo_epi64(first, second);
    const __m128i high = _mm_unpackhi_epi64(first, second);
    m[0] = PolyLow26(low);
    m[1] = PolyLow26(_mm_srli_epi64(low, 26));
    m[2] = PolyLow26(_mm_or_si128(_mm_srli_epi64(low, 52), _mm_slli_epi64(high, 12)));
    m[3] = PolyLow26(_mm_srli_epi64(high, 14));
    m[4] = _mm_or_si128(_mm_srli_epi64(high, 40), _mm_set1_epi64x(1 << 24));
}

#else

typedef uint64x2_t PolyVector;
/* У NEON произведение 32 на 32 бита берёт половинные векторы. */
typedef uint32x2_t PolyFactor;

static const uint32_t polyLastExponents[2] = {2u, 1u};

static PolyVector PolyAdd(PolyVector left, PolyVector right)
{
    return vaddq_u64(left, right);
}

static PolyVector PolyLow26(PolyVector value)
{
    return vandq_u64(value, vdupq_n_u64(POLY_MASK26));
}

static PolyVector PolyHigh26(PolyVector value)
{
    return vshrq_n_u64(value, 26);
}

static PolyVector PolyTimes5(PolyVector value)
{
    return vaddq_u64(value, vshlq_n_u64(value, 2));
}

static PolyFactor PolyNarrow(PolyVector value)
{
    return vmovn_u64(value);
}

static PolyVector PolyMultiply(PolyFactor left, PolyFactor right)
{
    return vmull_u32(left, right);
}

static PolyVector PolyMultiplyAdd(PolyVector sum, PolyFactor left, PolyFactor right)
{
    return vmlal_u32(sum, left, right);
}

static PolyVector PolyLanesOf(const Poly26 power[3], const uint32_t exponent[2], size_t limb)
{
    return vcombine_u64(vcreate_u64(power[exponent[0]][limb]),
                        vcreate_u64(power[exponent[1]][limb]));
}

static PolyVector PolyFirstLane(uint64_t value)
{
    return vcombine_u64(vcreate_u64(value), vcreate_u64(0u));
}

static uint64_t PolySum(PolyVector value)
{
    return vgetq_lane_u64(value, 0) + vgetq_lane_u64(value, 1);
}

static void PolyLoad(const uint8_t *bytes, PolyVector m[5])
{
    const uint64x2_t first = vreinterpretq_u64_u8(vld1q_u8(bytes));
    const uint64x2_t second = vreinterpretq_u64_u8(vld1q_u8(bytes + 16u));
    const uint64x2_t low = vcombine_u64(vget_low_u64(first), vget_low_u64(second));
    const uint64x2_t high = vcombine_u64(vget_high_u64(first), vget_high_u64(second));
    m[0] = PolyLow26(low);
    m[1] = PolyLow26(vshrq_n_u64(low, 26));
    m[2] = PolyLow26(vorrq_u64(vshrq_n_u64(low, 52), vshlq_n_u64(high, 12)));
    m[3] = PolyLow26(vshrq_n_u64(high, 14));
    m[4] = vorrq_u64(vshrq_n_u64(high, 40), vdupq_n_u64(UINT64_C(1) << 24u));
}

#endif

/* Разряды степени r для каждой дорожки и они же, умноженные на 5: перенос
 * за 2^130 возвращается вниз умножением на 5. */
typedef struct PolyKey
{
    PolyFactor r[5];
    PolyFactor s[5];
} PolyKey;

/* Степени r^1..r^L и ключи дорожек из них. Они секретны, как сам r, и
 * обнуляются вместе, одним вызовом. */
typedef struct PolyPowers
{
    Poly26 power[NET_POLY_LANES + 1u];
    PolyKey step;
    PolyKey last;
} PolyPowers;

/* Степени берутся через указатель на неизменяемую структуру: массив
 * массивов без const до C23 не передаётся туда, где ждут его с const. */
static void PolyKeyOf(PolyKey *key, const PolyPowers *powers,
                      const uint32_t exponent[NET_POLY_LANES])
{
    for (size_t limb = 0u; limb < 5u; ++limb)
    {
        const PolyVector r = PolyLanesOf(powers->power, exponent, limb);
        key->r[limb] = PolyNarrow(r);
        key->s[limb] = PolyNarrow(PolyTimes5(r));
    }
}

/* Не встраивается: цикл читает ключи из памяти. Встроив подготовку, clang
 * держал ключи в регистрах и вытеснял из них аккумуляторы. */
static NET_NOINLINE void PolyPowersOf(PolyPowers *powers, const NetPoly1305 *state)
{
    Poly26From44(powers->power[1], state->r[0], state->r[1], state->r[2]);
    Poly26Multiply(powers->power[2], powers->power[1], powers->power[1]);
    /* r^3 = r * r^2 и r^4 = r^2 * r^2 не ждут друг друга. */
    for (uint32_t exponent = 3u; exponent <= NET_POLY_LANES; ++exponent)
        Poly26Multiply(powers->power[exponent], powers->power[exponent - 2u], powers->power[2]);
    uint32_t step[NET_POLY_LANES];
    for (size_t lane = 0u; lane < NET_POLY_LANES; ++lane)
        step[lane] = NET_POLY_LANES;
    PolyKeyOf(&powers->step, powers, step);
    PolyKeyOf(&powers->last, powers, polyLastExponents);
}

/* Встраивается всегда: MSVC без /Ob3 оставлял вызов в цикле и проводил h и d
 * через память. */
static NET_FORCE_INLINE void PolyMultiplyLanes(PolyVector d[5], const PolyVector h[5],
                                               const PolyKey *key)
{
    const PolyFactor h0 = PolyNarrow(h[0]);
    const PolyFactor h1 = PolyNarrow(h[1]);
    const PolyFactor h2 = PolyNarrow(h[2]);
    const PolyFactor h3 = PolyNarrow(h[3]);
    const PolyFactor h4 = PolyNarrow(h[4]);
    PolyVector d0 = PolyMultiply(h0, key->r[0]);
    d0 = PolyMultiplyAdd(d0, h1, key->s[4]);
    d0 = PolyMultiplyAdd(d0, h2, key->s[3]);
    d0 = PolyMultiplyAdd(d0, h3, key->s[2]);
    d0 = PolyMultiplyAdd(d0, h4, key->s[1]);
    PolyVector d1 = PolyMultiply(h0, key->r[1]);
    d1 = PolyMultiplyAdd(d1, h1, key->r[0]);
    d1 = PolyMultiplyAdd(d1, h2, key->s[4]);
    d1 = PolyMultiplyAdd(d1, h3, key->s[3]);
    d1 = PolyMultiplyAdd(d1, h4, key->s[2]);
    PolyVector d2 = PolyMultiply(h0, key->r[2]);
    d2 = PolyMultiplyAdd(d2, h1, key->r[1]);
    d2 = PolyMultiplyAdd(d2, h2, key->r[0]);
    d2 = PolyMultiplyAdd(d2, h3, key->s[4]);
    d2 = PolyMultiplyAdd(d2, h4, key->s[3]);
    PolyVector d3 = PolyMultiply(h0, key->r[3]);
    d3 = PolyMultiplyAdd(d3, h1, key->r[2]);
    d3 = PolyMultiplyAdd(d3, h2, key->r[1]);
    d3 = PolyMultiplyAdd(d3, h3, key->r[0]);
    d3 = PolyMultiplyAdd(d3, h4, key->s[4]);
    PolyVector d4 = PolyMultiply(h0, key->r[4]);
    d4 = PolyMultiplyAdd(d4, h1, key->r[3]);
    d4 = PolyMultiplyAdd(d4, h2, key->r[2]);
    d4 = PolyMultiplyAdd(d4, h3, key->r[1]);
    d4 = PolyMultiplyAdd(d4, h4, key->r[0]);
    d[0] = d0;
    d[1] = d1;
    d[2] = d2;
    d[3] = d3;
    d[4] = d4;
}

/* Неполный перенос двумя цепочками сразу: разряды выходят меньше 2^26, второй
 * и пятый — чуть больше, и с разрядом блока остаются меньше 2^28. */
static NET_FORCE_INLINE void PolyCarryLanes(PolyVector h[5], PolyVector d[5])
{
    PolyVector carry = PolyHigh26(d[3]);
    d[3] = PolyLow26(d[3]);
    d[4] = PolyAdd(d[4], carry);
    carry = PolyHigh26(d[0]);
    d[0] = PolyLow26(d[0]);
    d[1] = PolyAdd(d[1], carry);
    carry = PolyHigh26(d[4]);
    d[4] = PolyLow26(d[4]);
    d[0] = PolyAdd(d[0], PolyTimes5(carry));
    carry = PolyHigh26(d[1]);
    d[1] = PolyLow26(d[1]);
    d[2] = PolyAdd(d[2], carry);
    carry = PolyHigh26(d[0]);
    d[0] = PolyLow26(d[0]);
    d[1] = PolyAdd(d[1], carry);
    carry = PolyHigh26(d[2]);
    d[2] = PolyLow26(d[2]);
    d[3] = PolyAdd(d[3], carry);
    carry = PolyHigh26(d[3]);
    d[3] = PolyLow26(d[3]);
    d[4] = PolyAdd(d[4], carry);
    for (size_t limb = 0u; limb < 5u; ++limb)
        h[limb] = d[limb];
}

/* groups групп по NET_POLY_LANES полных блоков, groups не меньше одной. */
static NET_NOINLINE void PolyBlocksWide(NetPoly1305 *state, const uint8_t *bytes, size_t groups)
{
    PolyPowers powers;
    PolyPowersOf(&powers, state);

    /* Накопленное значение ложится в первую дорожку: оно стоит перед первым
     * блоком. */
    Poly26 start;
    Poly26From44(start, state->h[0], state->h[1] & POLY_MASK44, state->h[2] + (state->h[1] >> 44u));
    PolyVector h[5];
    for (size_t limb = 0u; limb < 5u; ++limb)
        h[limb] = PolyFirstLane(start[limb]);
    PolyVector d[5];
    for (;;)
    {
        PolyVector m[5];
        PolyLoad(bytes, m);
        bytes += (size_t)NET_POLY_LANES * 16u;
        for (size_t limb = 0u; limb < 5u; ++limb)
            h[limb] = PolyAdd(h[limb], m[limb]);
        if (--groups == 0u)
            break;
        PolyMultiplyLanes(d, h, &powers.step);
        PolyCarryLanes(h, d);
    }
    PolyMultiplyLanes(d, h, &powers.last);
    Poly26 sum;
    for (size_t limb = 0u; limb < 5u; ++limb)
        sum[limb] = PolySum(d[limb]);
    Poly26Carry(sum);
    Poly26To44(sum, state->h);
    NetWipe(&powers, sizeof(powers));
}

/* С 12 блоков (192 байт) векторы окупают степени r у MSVC и clang, с SSE2 и
 * AVX2; на 128 байтах выигрыша ещё нет. */
#define POLY_WIDE_BLOCKS 12u

#endif

/* Полные блоки — векторами, если вход длинный, остаток — по одному. */
static void PolyBlocks(NetPoly1305 *state, const uint8_t *bytes, size_t count, uint64_t highBit)
{
#if defined(NET_POLY_LANES)
    if (highBit != 0u && count >= POLY_WIDE_BLOCKS)
    {
        const size_t groups = count / NET_POLY_LANES;
        PolyBlocksWide(state, bytes, groups);
        bytes += groups * NET_POLY_LANES * 16u;
        count -= groups * NET_POLY_LANES;
    }
#endif
    PolyBlocksScalar(state, bytes, count, highBit);
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
        PolyBlocks(state, state->buffer, 1u, 1u);
        state->buffered = 0u;
    }
    PolyBlocks(state, bytes, size / 16u, 1u);
    bytes += size & ~(size_t)15u;
    size &= 15u;
    while (size != 0u)
    {
        state->buffer[state->buffered++] = *bytes++;
        --size;
    }
}

/* Тег из аккумулятора всех блоков; состояние не обнуляется. */
static void PolyFinal(const NetPoly1305 *state, uint8_t tag[NET_TAG_SIZE])
{
    uint64_t h0 = state->h[0];
    uint64_t h1 = state->h[1];
    uint64_t h2 = state->h[2];
    /* Два полных переноса: разряды точно в своих границах. */
    uint64_t carry = 0u;
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        carry = h1 >> 44u;
        h1 &= POLY_MASK44;
        h2 += carry;
        carry = h2 >> 42u;
        h2 &= POLY_MASK42;
        h0 += carry * 5u;
        carry = h0 >> 44u;
        h0 &= POLY_MASK44;
        h1 += carry;
    }
    /* g = h + 5 - 2^130: неотрицательно ровно тогда, когда h >= 2^130 - 5. */
    uint64_t g0 = h0 + 5u;
    carry = g0 >> 44u;
    g0 &= POLY_MASK44;
    uint64_t g1 = h1 + carry;
    carry = g1 >> 44u;
    g1 &= POLY_MASK44;
    const uint64_t g2 = h2 + carry - (UINT64_C(1) << 42u);
    /* Все единицы, если заём не случился, то есть нужно взять g. */
    const uint64_t takeG = (g2 >> 63u) - 1u;
    h0 = (h0 & ~takeG) | (g0 & takeG);
    h1 = (h1 & ~takeG) | (g1 & takeG);
    h2 = (h2 & ~takeG) | (g2 & takeG);
    /* Тег — h + s по модулю 2^128. */
    const uint64_t s0 = state->pad[0];
    const uint64_t s1 = state->pad[1];
    h0 += s0 & POLY_MASK44;
    carry = h0 >> 44u;
    h0 &= POLY_MASK44;
    h1 += (((s0 >> 44u) | (s1 << 20u)) & POLY_MASK44) + carry;
    carry = h1 >> 44u;
    h1 &= POLY_MASK44;
    h2 += (s1 >> 24u) + carry;
    Store64(tag, h0 | (h1 << 44u));
    Store64(tag + 8u, (h1 >> 20u) | (h2 << 24u));
}

void NetPoly1305Finish(NetPoly1305 *state, uint8_t tag[NET_TAG_SIZE])
{
    if (state->buffered != 0u)
    {
        state->buffer[state->buffered++] = 1u;
        while (state->buffered < 16u)
            state->buffer[state->buffered++] = 0u;
        PolyBlocks(state, state->buffer, 1u, 0u);
    }
    PolyFinal(state, tag);
    NetWipe(state, sizeof(*state));
}

/* ---- AEAD (RFC 8439, раздел 2.8) ----------------------------------------------
 *
 * Одноразовый ключ Poly1305 — первые 32 байта блока 0, сообщение шифруется с
 * блока 1. Где есть векторы, блок 0 считается одной группой с первыми
 * блоками сообщения: мелкому пакету хватает одного прохода. MAC считает
 * дополненные нулями до 16 байт данные, то есть только полные блоки, и
 * буфер NetPoly1305 ему не нужен. */

typedef struct Aead
{
    uint32_t state[16];
    /* Ключ Poly1305 и с 64-го байта — поток начала сообщения. */
    uint8_t first[CHACHA_FIRST_BYTES];
    NetPoly1305 mac;
} Aead;

static void AeadBegin(Aead *aead, const uint8_t key[NET_KEY_SIZE],
                      const uint8_t nonce[NET_NONCE_SIZE])
{
    ChaChaSetup(aead->state, key, 0u, nonce);
    ChaChaFirst(aead->state, aead->first);
}

static void AeadXor(Aead *aead, const uint8_t *input, uint8_t *output, size_t size)
{
    const size_t ready = sizeof(aead->first) - 64u;
    const size_t head = size < ready ? size : ready;
    XorBytes(output, input, aead->first + 64u, head);
    ChaChaXorFrom(aead->state, input + head, output + head, size - head);
}

static void PolyPadded(NetPoly1305 *mac, const uint8_t *bytes, size_t size)
{
    const size_t whole = size & ~(size_t)15u;
    PolyBlocks(mac, bytes, whole / 16u, 1u);
    if (whole != size)
    {
        uint8_t block[16] = {0};
        for (size_t index = whole; index < size; ++index)
            block[index - whole] = bytes[index];
        PolyBlocks(mac, block, 1u, 1u);
    }
}

static void AeadMac(Aead *aead, const uint8_t *aad, size_t aadSize, const uint8_t *ciphertext,
                    size_t size, uint8_t tag[NET_TAG_SIZE])
{
    uint8_t lengths[16];
    NetPoly1305Begin(&aead->mac, aead->first);
    PolyPadded(&aead->mac, aad, aadSize);
    PolyPadded(&aead->mac, ciphertext, size);
    Store64(lengths, (uint64_t)aadSize);
    Store64(lengths + 8u, (uint64_t)size);
    PolyBlocks(&aead->mac, lengths, 1u, 1u);
    PolyFinal(&aead->mac, tag);
}

void NetAeadSeal(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *plaintext, size_t size,
                 uint8_t *ciphertext, uint8_t tag[NET_TAG_SIZE])
{
    Aead aead;
    AeadBegin(&aead, key, nonce);
    AeadXor(&aead, plaintext, ciphertext, size);
    AeadMac(&aead, aad, aadSize, ciphertext, size, tag);
    NetWipe(&aead, sizeof(aead));
}

bool NetAeadOpen(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *ciphertext, size_t size,
                 const uint8_t tag[NET_TAG_SIZE], uint8_t *plaintext)
{
    Aead aead;
    uint8_t expected[NET_TAG_SIZE];
    AeadBegin(&aead, key, nonce);
    AeadMac(&aead, aad, aadSize, ciphertext, size, expected);
    /* Открытый текст появляется только после проверки тега. */
    const bool valid = PlatformConstantTimeEqual(expected, tag, NET_TAG_SIZE);
    if (valid)
        AeadXor(&aead, ciphertext, plaintext, size);
    NetWipe(&aead, sizeof(aead));
    return valid;
}

/* ---- X25519 (RFC 7748) ----------------------------------------------------------
 *
 * Арифметика поля 2^255 - 19 общая с подписями Ed25519 и лежит в
 * crypto/field25519.h. */

/* (486662 - 2) / 4 */
#define FIELD_A24 UINT64_C(121665)

static void FieldMultiplyA24(Field output, const Field value)
{
    FieldReduce(output, WideMultiply(value[0], FIELD_A24), WideMultiply(value[1], FIELD_A24),
                WideMultiply(value[2], FIELD_A24), WideMultiply(value[3], FIELD_A24),
                WideMultiply(value[4], FIELD_A24));
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
