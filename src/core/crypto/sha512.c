#include "crypto/sha512.h"

#include "crypto/wipe.h"

#include <string.h>

/* Первые 64 бита дробных частей кубических корней первых 80 простых. */
static const uint64_t sha512Rounds[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd), UINT64_C(0xb5c0fbcfec4d3b2f),
    UINT64_C(0xe9b5dba58189dbbc), UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118), UINT64_C(0xd807aa98a3030242),
    UINT64_C(0x12835b0145706fbe), UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1), UINT64_C(0x9bdc06a725c71235),
    UINT64_C(0xc19bf174cf692694), UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65), UINT64_C(0x2de92c6f592b0275),
    UINT64_C(0x4a7484aa6ea6e483), UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210), UINT64_C(0xb00327c898fb213f),
    UINT64_C(0xbf597fc7beef0ee4), UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70), UINT64_C(0x27b70a8546d22ffc),
    UINT64_C(0x2e1b21385c26c926), UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8), UINT64_C(0x81c2c92e47edaee6),
    UINT64_C(0x92722c851482353b), UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30), UINT64_C(0xd192e819d6ef5218),
    UINT64_C(0xd69906245565a910), UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53), UINT64_C(0x2748774cdf8eeb99),
    UINT64_C(0x34b0bcb5e19b48a8), UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3), UINT64_C(0x748f82ee5defb2fc),
    UINT64_C(0x78a5636f43172f60), UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9), UINT64_C(0xbef9a3f7b2c67915),
    UINT64_C(0xc67178f2e372532b), UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178), UINT64_C(0x06f067aa72176fba),
    UINT64_C(0x0a637dc5a2c898a6), UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493), UINT64_C(0x3c9ebe0a15c9bebc),
    UINT64_C(0x431d67c49c100d4c), UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817),
};

/* Первые 64 бита дробных частей квадратных корней первых восьми простых. */
static const uint64_t sha512Initial[8] = {
    UINT64_C(0x6a09e667f3bcc908), UINT64_C(0xbb67ae8584caa73b), UINT64_C(0x3c6ef372fe94f82b),
    UINT64_C(0xa54ff53a5f1d36f1), UINT64_C(0x510e527fade682d1), UINT64_C(0x9b05688c2b3e6c1f),
    UINT64_C(0x1f83d9abfb41bd6b), UINT64_C(0x5be0cd19137e2179),
};

static uint64_t RotateRight(uint64_t value, uint32_t count)
{
    return (value >> count) | (value << (64u - count));
}

static uint64_t LoadBigEndian64(const uint8_t *bytes)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value = (value << 8u) | bytes[index];
    return value;
}

static void StoreBigEndian64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (56u - 8u * index));
}

/* Сжимает count блоков подряд. Расписание — кольцо из 16 слов: расширение
 * переиспользует слот index - 16. */
static void Sha512Blocks(uint64_t state[8], const uint8_t *bytes, size_t count)
{
    uint64_t schedule[16];
    for (; count != 0u; --count, bytes += CRYPTO_SHA512_BLOCK)
    {
        for (size_t index = 0u; index < 16u; ++index)
            schedule[index] = LoadBigEndian64(bytes + 8u * index);
        uint64_t a = state[0];
        uint64_t b = state[1];
        uint64_t c = state[2];
        uint64_t d = state[3];
        uint64_t e = state[4];
        uint64_t f = state[5];
        uint64_t g = state[6];
        uint64_t h = state[7];
        for (uint32_t index = 0u; index < 80u; ++index)
        {
            uint64_t word;
            if (index < 16u)
            {
                word = schedule[index];
            }
            else
            {
                const uint64_t previous15 = schedule[(index + 1u) & 15u];
                const uint64_t previous2 = schedule[(index + 14u) & 15u];
                const uint64_t sigma0 =
                    RotateRight(previous15, 1u) ^ RotateRight(previous15, 8u) ^ (previous15 >> 7u);
                const uint64_t sigma1 =
                    RotateRight(previous2, 19u) ^ RotateRight(previous2, 61u) ^ (previous2 >> 6u);
                word = schedule[index & 15u] + sigma0 + schedule[(index + 9u) & 15u] + sigma1;
                schedule[index & 15u] = word;
            }
            const uint64_t sum1 = RotateRight(e, 14u) ^ RotateRight(e, 18u) ^ RotateRight(e, 41u);
            const uint64_t choose = (e & f) ^ (~e & g);
            const uint64_t temporary1 = h + sum1 + choose + sha512Rounds[index] + word;
            const uint64_t sum0 = RotateRight(a, 28u) ^ RotateRight(a, 34u) ^ RotateRight(a, 39u);
            const uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + sum0 + majority;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
    CryptoWipe(schedule, sizeof(schedule));
}

void CryptoSha512Begin(CryptoSha512 *hash)
{
    memcpy(hash->state, sha512Initial, sizeof(sha512Initial));
    hash->bitsLow = 0u;
    hash->bitsHigh = 0u;
    hash->buffered = 0u;
}

void CryptoSha512Update(CryptoSha512 *hash, const uint8_t *bytes, size_t size)
{
    if (size == 0u)
        return;
    const uint64_t bitsLow = hash->bitsLow + ((uint64_t)size << 3u);
    hash->bitsHigh += ((uint64_t)size >> 61u) + (uint64_t)(bitsLow < hash->bitsLow);
    hash->bitsLow = bitsLow;
    if (hash->buffered != 0u)
    {
        const size_t space = CRYPTO_SHA512_BLOCK - hash->buffered;
        const size_t part = size < space ? size : space;
        memcpy(hash->block + hash->buffered, bytes, part);
        hash->buffered += (uint32_t)part;
        bytes += part;
        size -= part;
        if (hash->buffered < CRYPTO_SHA512_BLOCK)
            return;
        Sha512Blocks(hash->state, hash->block, 1u);
        hash->buffered = 0u;
    }
    /* Полные блоки сжимаются прямо из входа, без копии в буфер. */
    const size_t blocks = size / CRYPTO_SHA512_BLOCK;
    if (blocks != 0u)
    {
        Sha512Blocks(hash->state, bytes, blocks);
        bytes += blocks * CRYPTO_SHA512_BLOCK;
        size -= blocks * CRYPTO_SHA512_BLOCK;
    }
    if (size != 0u)
    {
        memcpy(hash->block, bytes, size);
        hash->buffered = (uint32_t)size;
    }
}

void CryptoSha512Finish(CryptoSha512 *hash, uint8_t digest[CRYPTO_SHA512_SIZE])
{
    uint32_t buffered = hash->buffered;
    hash->block[buffered++] = 0x80u;
    if (buffered > CRYPTO_SHA512_BLOCK - 16u)
    {
        memset(hash->block + buffered, 0, CRYPTO_SHA512_BLOCK - buffered);
        Sha512Blocks(hash->state, hash->block, 1u);
        buffered = 0u;
    }
    memset(hash->block + buffered, 0, CRYPTO_SHA512_BLOCK - 16u - buffered);
    StoreBigEndian64(hash->block + CRYPTO_SHA512_BLOCK - 16u, hash->bitsHigh);
    StoreBigEndian64(hash->block + CRYPTO_SHA512_BLOCK - 8u, hash->bitsLow);
    Sha512Blocks(hash->state, hash->block, 1u);
    for (size_t index = 0u; index < 8u; ++index)
        StoreBigEndian64(digest + 8u * index, hash->state[index]);
    CryptoWipe(hash, sizeof(*hash));
}

void CryptoSha512Hash(const uint8_t *bytes, size_t size, uint8_t digest[CRYPTO_SHA512_SIZE])
{
    CryptoSha512 hash;
    CryptoSha512Begin(&hash);
    CryptoSha512Update(&hash, bytes, size);
    CryptoSha512Finish(&hash, digest);
}
