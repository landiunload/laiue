#pragma once

/* Подписи Ed25519 (RFC 8032, раздел 5.1). Внутренний заголовок криптографии
 * движка.
 *
 * Закрытый ключ — 32 случайных байта seed, открытый — 32 байта точки,
 * подпись — 64 байта. Подпись детерминирована: случайность нужна только
 * при создании seed.
 *
 * Подпись и вывод ключа используют одинаковую последовательность операций
 * для всех значений скаляра. Проверка работает только с открытыми данными.
 * Для сообщения длины 0 допустим message == NULL. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRYPTO_ED25519_SEED_SIZE 32u
#define CRYPTO_ED25519_PUBLIC_KEY_SIZE 32u
#define CRYPTO_ED25519_SIGNATURE_SIZE 64u

void CryptoEd25519PublicKey(const uint8_t seed[CRYPTO_ED25519_SEED_SIZE],
                            uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE]);

/* true, если ключ записан канонически, лежит на кривой и не малого порядка:
 * Verify не отвергнет его заранее. Ключ, которому будут доверять, стоит
 * проверить так при сохранении. */
bool CryptoEd25519PublicKeyIsValid(const uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE]);

/* Открытый ключ выводится из seed заново: подписать с чужим открытым
 * ключом нельзя. Буфер подписи может перекрывать сообщение. */
void CryptoEd25519Sign(const uint8_t seed[CRYPTO_ED25519_SEED_SIZE], const uint8_t *message,
                       size_t size, uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE]);

/* Строгая проверка: true, только если S < L, точки A и R записаны
 * канонически и не малого порядка, а [S]B - [k]A в точности равна R. Так у
 * подписи нет второго вида, а ключ малого порядка не подходит сразу ко
 * многим сообщениям. */
bool CryptoEd25519Verify(const uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE],
                         const uint8_t *message, size_t size,
                         const uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE]);
