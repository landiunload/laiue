#pragma once

/* SHA-512 (FIPS 180-4). Внутренний заголовок криптографии движка.
 *
 * Реализация собственная: одинаковая на всех платформах, без ОС и CRT.
 * Константы и тестовые векторы сверены с определением стандарта в
 * tests/crypto_test.c. */

#include <stddef.h>
#include <stdint.h>

#define CRYPTO_SHA512_SIZE 64u
#define CRYPTO_SHA512_BLOCK 128u

/* Потоковое состояние: данные можно подавать кусками любого размера. */
typedef struct CryptoSha512
{
    uint64_t state[8];
    /* Длина сообщения в битах, как в последнем блоке SHA-512. */
    uint64_t bitsLow;
    uint64_t bitsHigh;
    uint8_t block[CRYPTO_SHA512_BLOCK];
    uint32_t buffered;
} CryptoSha512;

void CryptoSha512Begin(CryptoSha512 *hash);
void CryptoSha512Update(CryptoSha512 *hash, const uint8_t *bytes, size_t size);
/* Пишет хеш и стирает состояние: в нём могли остаться секретные данные. */
void CryptoSha512Finish(CryptoSha512 *hash, uint8_t digest[CRYPTO_SHA512_SIZE]);
/* Хеш одного куска. */
void CryptoSha512Hash(const uint8_t *bytes, size_t size, uint8_t digest[CRYPTO_SHA512_SIZE]);
