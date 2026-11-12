#include "net/net_crypto.h"

#include "platform/system.h"

#include <string.h>

/* Векторы ChaCha20. NET_CRYPTO_PORTABLE отключает их вместе с быстрым
 * 128-битным умножением: так тест проверяет и переносимый код. */
#if !defined(NET_CRYPTO_PORTABLE) && defined(__AVX2__)
#define NET_CHACHA_AVX2 1
#define NET_CHACHA_LANES 8u
#include <immintrin.h>
#elif !defined(NET_CRYPTO_PORTABLE) && (defined(__SSE2__) || defined(_M_X64))
#define NET_CHACHA_SSE2 1
#define NET_CHACHA_LANES 4u
#include <emmintrin.h>
#if defined(__SSSE3__) || defined(__AVX__)
#include <tmmintrin.h>
#endif
#elif !defined(NET_CRYPTO_PORTABLE) && (defined(__ARM_NEON) || defined(_M_ARM64))
#define NET_CHACHA_NEON 1
#define NET_CHACHA_LANES 4u
#include <arm_neon.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#if defined(_MSC_VER)
#define NET_NOINLINE __declspec(noinline)
#else
#define NET_NOINLINE __attribute__((noinline))
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

/* memset через volatile-указатель: компилятор не знает, какую функцию
 * вызывает, и не может выбросить обнуление как мёртвую запись. Побайтовый
 * volatile-цикл тоже не выбрасывается, но пишет по байту за такт. */
static void *(*const volatile wipeMemset)(void *, int, size_t) = memset;

void NetWipe(void *bytes, size_t size)
{
    if (size != 0u)
        (void)wipeMemset(bytes, 0, size);
}

static uint64_t Load64(const uint8_t *bytes)
{
    return (uint64_t)Load32(bytes) | ((uint64_t)Load32(bytes + 4u) << 32u);
}

/* ---- 128-битные произведения ------------------------------------------------
 *
 * Poly1305 и X25519 копят суммы произведений 64 на 64 бита в Wide. Тест
 * собирает переносимый путь и там, где компилятор умеет быстрый. */

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

static uint64_t WideLow(Wide value)
{
    return (uint64_t)value;
}

/* value >> count при 0 < count < 64; результат обязан помещаться в 64 бита. */
static uint64_t WideShift(Wide value, uint32_t count)
{
    return (uint64_t)(value >> count);
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

static uint64_t WideLow(Wide value)
{
    return value.low;
}

/* value >> count при 0 < count < 64; результат обязан помещаться в 64 бита. */
static uint64_t WideShift(Wide value, uint32_t count)
{
    return (value.high << (64u - count)) | (value.low >> count);
}

#endif

static Wide WideMultiplyAdd(Wide sum, uint64_t left, uint64_t right)
{
    return WideAdd(sum, WideMultiply(left, right));
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
static void PolyBlocks(NetPoly1305 *state, const uint8_t *bytes, size_t count, uint64_t highBit)
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

/* Суммы произведений для пяти разрядов становятся почти приведённым
 * элементом. Каждая сумма меньше 2^115. */
static void FieldReduce(Field output, Wide t0, Wide t1, Wide t2, Wide t3, Wide t4)
{
    t1 = WideAddSmall(t1, WideShift(t0, 51u));
    t2 = WideAddSmall(t2, WideShift(t1, 51u));
    t3 = WideAddSmall(t3, WideShift(t2, 51u));
    t4 = WideAddSmall(t4, WideShift(t3, 51u));
    /* 2^255 = 19 по модулю p. Сумма старшего разряда — не больше пяти
     * произведений без множителя 19, перенос из неё меньше 5 * 2^57 + 2^13,
     * и младший разряд с ним остаётся меньше 2^64. */
    const uint64_t low = (WideLow(t0) & FIELD_MASK) + 19u * WideShift(t4, 51u);
    output[0] = low & FIELD_MASK;
    output[1] = (WideLow(t1) & FIELD_MASK) + (low >> 51u);
    output[2] = WideLow(t2) & FIELD_MASK;
    output[3] = WideLow(t3) & FIELD_MASK;
    output[4] = WideLow(t4) & FIELD_MASK;
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
