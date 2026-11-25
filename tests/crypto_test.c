/* Криптография движка: SHA-512 по FIPS 180-4, скаляры по модулю L и подписи
 * Ed25519 по RFC 8032 со строгой проверкой. Эталонные значения лежат в
 * tests/crypto_fixtures.h, его готовит tests/crypto_fixtures.py. */

#include "crypto/ed25519.h"
#include "crypto/scalar25519.h"
#include "crypto/sha512.h"
#include "crypto_fixtures.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Release LTO не должен сплющить сценарии в точку входа: кадр перерос бы
 * страницу и потребовал __chkstk, которого в no-CRT сборке нет. */
#if defined(_MSC_VER)
#define CRYPTO_TEST_NOINLINE __declspec(noinline)
#else
#define CRYPTO_TEST_NOINLINE __attribute__((noinline))
#endif

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Crypto failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Same(const void *first, const void *second, size_t size)
{
    return memcmp(first, second, size) == 0;
}

static uint64_t randomState = UINT64_C(0x452821E638D01377);

/* splitmix64: тест повторяется байт в байт. */
static uint64_t NextRandom(void)
{
    randomState += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t value = randomState;
    value = (value ^ (value >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27u)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31u);
}

static void FillRandom(uint8_t *bytes, size_t size)
{
    for (size_t index = 0u; index < size; ++index)
        bytes[index] = (uint8_t)(NextRandom() >> 56u);
}

static void FlipRandomBit(uint8_t *bytes, size_t size)
{
    const uint64_t bit = NextRandom() % (size * 8u);
    bytes[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
}

/* ---- SHA-512 ---------------------------------------------------------------- */

static CRYPTO_TEST_NOINLINE void TestSha512Vectors(void)
{
    static const char fips896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    uint8_t digest[CRYPTO_SHA512_SIZE];
    CryptoSha512Hash((const uint8_t *)"abc", 3u, digest);
    Expect(Same(digest, sha512Abc, sizeof(digest)), "SHA-512 of \"abc\"");
    CryptoSha512Hash((const uint8_t *)fips896, sizeof(fips896) - 1u, digest);
    Expect(Same(digest, sha512Fips896, sizeof(digest)), "SHA-512 of the 896-bit message");

    static uint8_t letters[1000];
    memset(letters, 'a', sizeof(letters));
    CryptoSha512 hash;
    CryptoSha512Begin(&hash);
    for (uint32_t round = 0u; round < 1000u; ++round)
        CryptoSha512Update(&hash, letters, sizeof(letters));
    CryptoSha512Finish(&hash, digest);
    Expect(Same(digest, sha512MillionA, sizeof(digest)), "SHA-512 of a million letters");

    static uint8_t message[4096];
    for (uint32_t index = 0u; index < sizeof(message); ++index)
        message[index] = (uint8_t)(index * 167u + 13u);
    for (uint32_t index = 0u; index < sizeof(sha512Fixtures) / sizeof(sha512Fixtures[0]); ++index)
    {
        const Sha512Fixture *fixture = &sha512Fixtures[index];
        CryptoSha512Hash(message, fixture->size, digest);
        Expect(Same(digest, fixture->digest, sizeof(digest)),
               "SHA-512 matches the reference around block boundaries");
    }
}

/* Куски любого размера дают тот же хеш, что и целое сообщение, а Finish
 * стирает состояние. */
static CRYPTO_TEST_NOINLINE void TestSha512Streaming(void)
{
    static uint8_t message[2048];
    FillRandom(message, sizeof(message));
    uint8_t whole[CRYPTO_SHA512_SIZE];
    uint8_t pieces[CRYPTO_SHA512_SIZE];
    CryptoSha512 hash;
    for (uint32_t round = 0u; round < 200u; ++round)
    {
        const size_t size = (size_t)(NextRandom() % (sizeof(message) + 1u));
        CryptoSha512Hash(message, size, whole);
        CryptoSha512Begin(&hash);
        size_t offset = 0u;
        while (offset < size)
        {
            size_t part = (size_t)(NextRandom() % 300u);
            if (part > size - offset)
                part = size - offset;
            CryptoSha512Update(&hash, message + offset, part);
            offset += part;
        }
        CryptoSha512Update(&hash, NULL, 0u);
        CryptoSha512Finish(&hash, pieces);
        Expect(Same(whole, pieces, sizeof(whole)), "SHA-512 in pieces matches the whole");
    }

    static const uint8_t zero[sizeof(hash)] = {0u};
    CryptoSha512Begin(&hash);
    hash.bitsLow = UINT64_MAX - 7u;
    hash.bitsHigh = 9u;
    CryptoSha512Update(&hash, message, 2u);
    Expect(hash.bitsLow == 8u && hash.bitsHigh == 10u, "SHA-512 carries the bit count");

    CryptoSha512Begin(&hash);
    CryptoSha512Update(&hash, message, 77u);
    CryptoSha512Finish(&hash, pieces);
    Expect(Same(&hash, zero, sizeof(hash)), "SHA-512 finish wipes the state");
}

/* ---- Скаляры ---------------------------------------------------------------- */

static CRYPTO_TEST_NOINLINE void TestScalars(void)
{
    uint8_t scalar[CRYPTO_SCALAR_SIZE];
    for (uint32_t index = 0u;
         index < sizeof(scalarReduceFixtures) / sizeof(scalarReduceFixtures[0]); ++index)
    {
        const ScalarReduceFixture *fixture = &scalarReduceFixtures[index];
        CryptoScalarReduce(scalar, fixture->value);
        Expect(Same(scalar, fixture->reduced, sizeof(scalar)), "reduction modulo L");
        Expect(CryptoScalarIsCanonical(scalar), "a reduced scalar is canonical");
    }
    for (uint32_t index = 0u;
         index < sizeof(scalarMultiplyAddFixtures) / sizeof(scalarMultiplyAddFixtures[0]); ++index)
    {
        const ScalarMultiplyAddFixture *fixture = &scalarMultiplyAddFixtures[index];
        CryptoScalarMultiplyAdd(scalar, fixture->left, fixture->right, fixture->addend);
        Expect(Same(scalar, fixture->result, sizeof(scalar)), "multiply-add modulo L");
        /* Результат пишется последним: выход может совпадать со входом. */
        memcpy(scalar, fixture->left, sizeof(scalar));
        CryptoScalarMultiplyAdd(scalar, scalar, fixture->right, fixture->addend);
        Expect(Same(scalar, fixture->result, sizeof(scalar)), "multiply-add in place");
    }

    static const uint8_t order[CRYPTO_SCALAR_SIZE] = {
        0xedu, 0xd3u, 0xf5u, 0x5cu, 0x1au, 0x63u, 0x12u, 0x58u, 0xd6u, 0x9cu, 0xf7u,
        0xa2u, 0xdeu, 0xf9u, 0xdeu, 0x14u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u};
    memcpy(scalar, order, sizeof(scalar));
    Expect(!CryptoScalarIsCanonical(scalar), "L itself is not canonical");
    scalar[0] = 0xecu;
    Expect(CryptoScalarIsCanonical(scalar), "L - 1 is canonical");
    memset(scalar, 0, sizeof(scalar));
    Expect(CryptoScalarIsCanonical(scalar), "zero is canonical");
    memset(scalar, 0xFF, sizeof(scalar));
    Expect(!CryptoScalarIsCanonical(scalar), "2^256 - 1 is not canonical");
}

/* ---- Ed25519 ---------------------------------------------------------------- */

static CRYPTO_TEST_NOINLINE void TestEd25519Vectors(void)
{
    uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
    uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE];
    uint8_t message[CRYPTO_FIXTURE_MESSAGE_CAPACITY];
    for (uint32_t index = 0u; index < sizeof(ed25519Fixtures) / sizeof(ed25519Fixtures[0]); ++index)
    {
        const Ed25519Fixture *fixture = &ed25519Fixtures[index];
        CryptoEd25519PublicKey(fixture->seed, publicKey);
        Expect(Same(publicKey, fixture->publicKey, sizeof(publicKey)), "Ed25519 public key");
        Expect(CryptoEd25519PublicKeyIsValid(publicKey), "a derived public key is valid");
        CryptoEd25519Sign(fixture->seed, fixture->message, fixture->size, signature);
        Expect(Same(signature, fixture->signature, sizeof(signature)), "Ed25519 signature");
        Expect(CryptoEd25519Verify(fixture->publicKey, fixture->message, fixture->size,
                                   fixture->signature),
               "Ed25519 accepts the reference signature");

        memcpy(message, fixture->message, fixture->size);
        if (fixture->size != 0u)
        {
            FlipRandomBit(message, fixture->size);
            Expect(!CryptoEd25519Verify(fixture->publicKey, message, fixture->size,
                                        fixture->signature),
                   "Ed25519 rejects a changed message");
        }
        Expect(fixture->size == 0u || !CryptoEd25519Verify(fixture->publicKey, fixture->message,
                                                           fixture->size - 1u, fixture->signature),
               "Ed25519 rejects a shortened message");
        memcpy(signature, fixture->signature, sizeof(signature));
        FlipRandomBit(signature, 32u);
        Expect(!CryptoEd25519Verify(fixture->publicKey, fixture->message, fixture->size, signature),
               "Ed25519 rejects a changed R");
        memcpy(signature, fixture->signature, sizeof(signature));
        FlipRandomBit(signature + 32u, 32u);
        Expect(!CryptoEd25519Verify(fixture->publicKey, fixture->message, fixture->size, signature),
               "Ed25519 rejects a changed S");
        memcpy(publicKey, fixture->publicKey, sizeof(publicKey));
        FlipRandomBit(publicKey, sizeof(publicKey));
        Expect(!CryptoEd25519Verify(publicKey, fixture->message, fixture->size, fixture->signature),
               "Ed25519 rejects another key");
    }
}

/* Подписи, верные по уравнению [S]B = R + [k]A, и ключи, которые строгая
 * проверка запрещает до него. */
static CRYPTO_TEST_NOINLINE void TestEd25519Strictness(void)
{
    for (uint32_t index = 0u; index < sizeof(ed25519Forgeries) / sizeof(ed25519Forgeries[0]);
         ++index)
    {
        const Ed25519Forgery *forgery = &ed25519Forgeries[index];
        Expect(!CryptoEd25519Verify(forgery->publicKey, forgery->message, forgery->size,
                                    forgery->signature),
               "Ed25519 rejects small-order points and S >= L");
    }

    const Ed25519Fixture *valid = &ed25519Fixtures[0];
    uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE];
    for (uint32_t index = 0u; index < sizeof(ed25519InvalidKeys) / sizeof(ed25519InvalidKeys[0]);
         ++index)
    {
        Expect(!CryptoEd25519PublicKeyIsValid(ed25519InvalidKeys[index]),
               "an invalid public key is refused");
        Expect(!CryptoEd25519Verify(ed25519InvalidKeys[index], valid->message, valid->size,
                                    valid->signature),
               "Ed25519 rejects an invalid public key");
        /* Та же запись на месте R. */
        memcpy(signature, ed25519InvalidKeys[index], 32u);
        memcpy(signature + 32u, valid->signature + 32u, 32u);
        Expect(!CryptoEd25519Verify(valid->publicKey, valid->message, valid->size, signature),
               "Ed25519 rejects an invalid R");
    }
    for (uint32_t index = 0u; index < sizeof(ed25519ValidKeys) / sizeof(ed25519ValidKeys[0]);
         ++index)
    {
        Expect(CryptoEd25519PublicKeyIsValid(ed25519ValidKeys[index]),
               "a valid public key is accepted");
    }
}

/* Подпись случайных сообщений случайными ключами проходит проверку, в том
 * числе подпись поверх самого сообщения. */
static CRYPTO_TEST_NOINLINE void TestEd25519RoundTrip(void)
{
    uint8_t seed[CRYPTO_ED25519_SEED_SIZE];
    uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
    uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE];
    uint8_t other[CRYPTO_ED25519_SIGNATURE_SIZE];
    static uint8_t message[512];
    static uint8_t buffer[512];
    for (uint32_t round = 0u; round < 32u; ++round)
    {
        FillRandom(seed, sizeof(seed));
        const size_t size = 64u + (size_t)(NextRandom() % (sizeof(message) - 63u));
        FillRandom(message, size);
        CryptoEd25519PublicKey(seed, publicKey);
        CryptoEd25519Sign(seed, message, size, signature);
        Expect(CryptoEd25519Verify(publicKey, message, size, signature),
               "Ed25519 accepts its own signature");
        CryptoEd25519Sign(seed, message, size, other);
        Expect(Same(signature, other, sizeof(signature)), "Ed25519 signing is deterministic");

        memcpy(buffer, message, size);
        CryptoEd25519Sign(seed, buffer, size, buffer);
        Expect(Same(buffer, signature, sizeof(signature)), "Ed25519 signs over the message itself");

        FillRandom(seed, sizeof(seed));
        CryptoEd25519PublicKey(seed, publicKey);
        Expect(!CryptoEd25519Verify(publicKey, message, size, signature),
               "Ed25519 rejects the signature under another key");
    }
}

LAIUE_TEST_ENTRY(CryptoTestEntryPoint)
{
    TestSha512Vectors();
    TestSha512Streaming();
    TestScalars();
    TestEd25519Vectors();
    TestEd25519Strictness();
    TestEd25519RoundTrip();
    LaiueTestRuntimeWrite("crypto: passed\n");
    LAIUE_TEST_SUCCESS();
}
