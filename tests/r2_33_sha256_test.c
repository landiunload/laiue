// ROUND2 33-sha256: прямая регрессия LaiueSha256Compute (src/platform/sha256.c).
//
// На Windows production-путь PlatformSha256 идёт через BCrypt, поэтому общий
// platform_crypto_test этот файл не покрывает. Здесь sha256.c компилируется
// прямо в тест и сверяется с: (1) опубликованными векторами FIPS 180-4,
// включая границы padding 55/56/64/119; (2) реализацией PlatformSha256
// (BCrypt на Windows) на диапазоне размеров; (3) самим собой на выровненном и
// невыровненном входе одинакового содержимого.

#include "platform/sha256.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool BytesEqual(const uint8_t *first, const uint8_t *second, uint32_t size)
{
    for (uint32_t index = 0U; index < size; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

static void ExpectDigest(const uint8_t *bytes, uint64_t size, const uint8_t expected[32],
                         const char *message)
{
    uint8_t digest[32];
    Expect(LaiueSha256Compute(bytes, size, digest) && BytesEqual(digest, expected, 32U), message);
}

LAIUE_TEST_ENTRY(R2Sha256TestEntryPoint)
{
    static const uint8_t emptyExpected[32] = {
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U, 0x9aU, 0xfbU, 0xf4U,
        0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U, 0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU,
        0x93U, 0x4cU, 0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U,
    };
    static const uint8_t abcExpected[32] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU, 0x41U, 0x41U, 0x40U,
        0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U, 0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U,
        0x7aU, 0x9cU, 0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
    };
    static const uint8_t fipsTwoBlockExpected[32] = {
        0x24U, 0x8dU, 0x6aU, 0x61U, 0xd2U, 0x06U, 0x38U, 0xb8U, 0xe5U, 0xc0U, 0x26U,
        0x93U, 0x0cU, 0x3eU, 0x60U, 0x39U, 0xa3U, 0x3cU, 0xe4U, 0x59U, 0x64U, 0xffU,
        0x21U, 0x67U, 0xf6U, 0xecU, 0xedU, 0xd4U, 0x19U, 0xdbU, 0x06U, 0xc1U,
    };
    static const uint8_t a55Expected[32] = {
        0x9fU, 0x43U, 0x90U, 0xf8U, 0xd3U, 0x0cU, 0x2dU, 0xd9U, 0x2eU, 0xc9U, 0xf0U,
        0x95U, 0xb6U, 0x5eU, 0x2bU, 0x9aU, 0xe9U, 0xb0U, 0xa9U, 0x25U, 0xa5U, 0x25U,
        0x8eU, 0x24U, 0x1cU, 0x9fU, 0x1eU, 0x91U, 0x0fU, 0x73U, 0x43U, 0x18U,
    };
    static const uint8_t a56Expected[32] = {
        0xb3U, 0x54U, 0x39U, 0xa4U, 0xacU, 0x6fU, 0x09U, 0x48U, 0xb6U, 0xd6U, 0xf9U,
        0xe3U, 0xc6U, 0xafU, 0x0fU, 0x5fU, 0x59U, 0x0cU, 0xe2U, 0x0fU, 0x1bU, 0xdeU,
        0x70U, 0x90U, 0xefU, 0x79U, 0x70U, 0x68U, 0x6eU, 0xc6U, 0x73U, 0x8aU,
    };
    static const uint8_t a64Expected[32] = {
        0xffU, 0xe0U, 0x54U, 0xfeU, 0x7aU, 0xe0U, 0xcbU, 0x6dU, 0xc6U, 0x5cU, 0x3aU,
        0xf9U, 0xb6U, 0x1dU, 0x52U, 0x09U, 0xf4U, 0x39U, 0x85U, 0x1dU, 0xb4U, 0x3dU,
        0x0bU, 0xa5U, 0x99U, 0x73U, 0x37U, 0xdfU, 0x15U, 0x46U, 0x68U, 0xebU,
    };
    static const uint8_t a119Expected[32] = {
        0x31U, 0xebU, 0xa5U, 0x1cU, 0x31U, 0x3aU, 0x5cU, 0x08U, 0x22U, 0x6aU, 0xdfU,
        0x18U, 0xd4U, 0xa3U, 0x59U, 0xcfU, 0xdfU, 0xd8U, 0xd2U, 0xe8U, 0x16U, 0xb1U,
        0x3fU, 0x4aU, 0xf9U, 0x52U, 0xf7U, 0xeaU, 0x65U, 0x84U, 0xdcU, 0xfbU,
    };
    static const uint8_t patternExpected[32] = {
        0x1eU, 0x9bU, 0xc3U, 0x8cU, 0xbfU, 0x86U, 0x0bU, 0x9eU, 0xc3U, 0x19U, 0x18U,
        0xb0U, 0x65U, 0xf9U, 0xb5U, 0x24U, 0x76U, 0xc5U, 0x49U, 0xa7U, 0x82U, 0xe0U,
        0xe7U, 0x99U, 0x0bU, 0xedU, 0x8cU, 0xe3U, 0x86U, 0x8dU, 0x23U, 0x71U,
    };
    static const uint8_t millionAExpected[32] = {
        0xcdU, 0xc7U, 0x6eU, 0x5cU, 0x99U, 0x14U, 0xfbU, 0x92U, 0x81U, 0xa1U, 0xc7U,
        0xe2U, 0x84U, 0xd7U, 0x3eU, 0x67U, 0xf1U, 0x80U, 0x9aU, 0x48U, 0xa4U, 0x97U,
        0x20U, 0x0eU, 0x04U, 0x6dU, 0x39U, 0xccU, 0xc7U, 0x11U, 0x2cU, 0xd0U,
    };

    uint8_t digest[32];
    Expect(LaiueSha256Compute(NULL, 0U, digest) && BytesEqual(digest, emptyExpected, 32U),
           "r2 sha256 empty vector mismatch");

    static const uint8_t abc[] = {'a', 'b', 'c'};
    ExpectDigest(abc, sizeof(abc), abcExpected, "r2 sha256 abc vector mismatch");

    static const uint8_t fipsTwoBlock[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ExpectDigest(fipsTwoBlock, (uint64_t)(sizeof(fipsTwoBlock) - 1U), fipsTwoBlockExpected,
                 "r2 sha256 FIPS two-block vector mismatch");

    static uint8_t repeated[119];
    memset(repeated, 'a', sizeof(repeated));
    ExpectDigest(repeated, 55U, a55Expected, "r2 sha256 55-byte padding mismatch");
    ExpectDigest(repeated, 56U, a56Expected, "r2 sha256 56-byte padding mismatch");
    ExpectDigest(repeated, 64U, a64Expected, "r2 sha256 full-block mismatch");
    ExpectDigest(repeated, 119U, a119Expected, "r2 sha256 two-block mismatch");

    uint8_t pattern[1000];
    for (uint32_t index = 0U; index < (uint32_t)sizeof(pattern); ++index)
    {
        pattern[index] = (uint8_t)(index * 7U + 3U);
    }
    ExpectDigest(pattern, (uint64_t)sizeof(pattern), patternExpected,
                 "r2 sha256 multi-block pattern mismatch");

    // Невыровненный вход одинакового содержимого обязан дать тот же digest:
    // это проверяет прямой путь Transform по полным блокам в Update.
    static uint8_t unaligned[600];
    uint8_t copy[600];
    for (uint32_t start = 0U; start < 3U; ++start)
    {
        for (uint32_t size = 0U; size <= 512U; size += 1U)
        {
            for (uint32_t index = 0U; index < size; ++index)
            {
                unaligned[start + index] = (uint8_t)(index * 131U + start * 7U + 5U);
                copy[index] = unaligned[start + index];
            }
            uint8_t alignedDigest[32];
            uint8_t shiftedDigest[32];
            Expect(LaiueSha256Compute(copy, size, alignedDigest), "r2 sha256 aligned compute failed");
            Expect(LaiueSha256Compute(unaligned + start, size, shiftedDigest),
                   "r2 sha256 shifted compute failed");
            Expect(BytesEqual(alignedDigest, shiftedDigest, 32U),
                   "r2 sha256 unaligned digest mismatch");
        }
    }

    // Перекрёстная проверка с PlatformSha256 (на Windows — BCrypt) на всех
    // размерах через границы блока.
    static uint8_t crossBuffer[1024];
    for (uint32_t index = 0U; index < (uint32_t)sizeof(crossBuffer); ++index)
    {
        crossBuffer[index] = (uint8_t)(index * 29U + 11U);
    }
    for (uint32_t size = 0U; size <= 256U; ++size)
    {
        uint8_t portDigest[32];
        uint8_t platformDigest[32];
        Expect(LaiueSha256Compute(crossBuffer, size, portDigest),
               "r2 sha256 cross compute failed");
        Expect(PlatformSha256(crossBuffer, size, platformDigest),
               "r2 sha256 PlatformSha256 failed");
        Expect(BytesEqual(portDigest, platformDigest, 32U),
               "r2 sha256 digest differs from PlatformSha256");
    }

    static uint8_t million[1000000];
    memset(million, 'a', sizeof(million));
    ExpectDigest(million, (uint64_t)sizeof(million), millionAExpected,
                 "r2 sha256 million-a vector mismatch");

    LaiueTestRuntimeWrite("r2_33_sha256_test passed\n");
    LAIUE_TEST_SUCCESS();
}
