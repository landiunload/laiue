#pragma once

/* Скаляры по модулю L = 2^252 + 27742317777372353535851937790883648493 —
 * порядка базовой точки Ed25519. Внутренний заголовок криптографии движка.
 *
 * Скаляр — 32 байта little-endian. Время работы не зависит от значений. */

#include <stdbool.h>
#include <stdint.h>

#define CRYPTO_SCALAR_SIZE 32u
#define CRYPTO_SCALAR_WIDE_SIZE 64u

/* value mod L для 64 байт little-endian, например хеша SHA-512. */
void CryptoScalarReduce(uint8_t output[CRYPTO_SCALAR_SIZE],
                        const uint8_t value[CRYPTO_SCALAR_WIDE_SIZE]);

/* (left * right + addend) mod L для любых 256-битных аргументов. */
void CryptoScalarMultiplyAdd(uint8_t output[CRYPTO_SCALAR_SIZE],
                             const uint8_t left[CRYPTO_SCALAR_SIZE],
                             const uint8_t right[CRYPTO_SCALAR_SIZE],
                             const uint8_t addend[CRYPTO_SCALAR_SIZE]);

/* true, если scalar < L: запись каноническая. */
bool CryptoScalarIsCanonical(const uint8_t scalar[CRYPTO_SCALAR_SIZE]);
