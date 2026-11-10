#pragma once

/* Криптография транспорта laiue.net. Внутренний заголовок модуля.
 *
 * ChaCha20, Poly1305 и их AEAD-композиция — RFC 8439. X25519 — RFC 7748.
 * HMAC-SHA-256 — RFC 2104, HKDF — RFC 5869. Реализации собственные; из
 * стандартов взяты только константы и тестовые векторы, которыми их
 * проверяет tests/net_crypto_test.c.
 *
 * Время работы не зависит от секретов: нет ни ветвлений, ни обращений к
 * памяти по секретным данным. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_KEY_SIZE 32u
#define NET_NONCE_SIZE 12u
#define NET_TAG_SIZE 16u
#define NET_HASH_SIZE 32u

/* Один блок потока ChaCha20: 64 байта для ключа, счётчика и nonce. */
void NetChaCha20Block(const uint8_t key[NET_KEY_SIZE], uint32_t counter,
                      const uint8_t nonce[NET_NONCE_SIZE], uint8_t output[64]);
/* Складывает bytes с потоком, начиная с блока counter. Вход и выход могут
 * совпадать. */
void NetChaCha20Xor(const uint8_t key[NET_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[NET_NONCE_SIZE], const uint8_t *input, uint8_t *output,
                    size_t size);

typedef struct NetPoly1305
{
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t buffer[16];
    uint32_t buffered;
} NetPoly1305;

void NetPoly1305Begin(NetPoly1305 *state, const uint8_t key[NET_KEY_SIZE]);
void NetPoly1305Update(NetPoly1305 *state, const uint8_t *bytes, size_t size);
void NetPoly1305Finish(NetPoly1305 *state, uint8_t tag[NET_TAG_SIZE]);

/* AEAD_CHACHA20_POLY1305. Шифротекст той же длины, что и текст; вход и выход
 * могут совпадать. */
void NetAeadSeal(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *plaintext, size_t size,
                 uint8_t *ciphertext, uint8_t tag[NET_TAG_SIZE]);
/* false — подделка или повреждение; выход при этом не тронут. */
bool NetAeadOpen(const uint8_t key[NET_KEY_SIZE], const uint8_t nonce[NET_NONCE_SIZE],
                 const uint8_t *aad, size_t aadSize, const uint8_t *ciphertext, size_t size,
                 const uint8_t tag[NET_TAG_SIZE], uint8_t *plaintext);

/* X25519: скаляр на u-координату точки. false — результат нулевой, то есть
 * точка малого порядка; таким секретом пользоваться нельзя. */
bool NetX25519(const uint8_t scalar[NET_KEY_SIZE], const uint8_t point[NET_KEY_SIZE],
               uint8_t output[NET_KEY_SIZE]);
/* Открытый ключ: скаляр на базовую точку u = 9. */
void NetX25519Public(const uint8_t privateKey[NET_KEY_SIZE], uint8_t publicKey[NET_KEY_SIZE]);

/* HMAC-SHA-256 от одного куска до NET_HMAC_MESSAGE_CAPACITY байт. */
#define NET_HMAC_MESSAGE_CAPACITY 256u
bool NetHmacSha256(const uint8_t *key, size_t keySize, const uint8_t *message, size_t size,
                   uint8_t output[NET_HASH_SIZE]);
/* HKDF-Extract и однoблочный HKDF-Expand: до 32 байт на метку. */
bool NetHkdfExtract(const uint8_t *salt, size_t saltSize, const uint8_t *input, size_t inputSize,
                    uint8_t prk[NET_HASH_SIZE]);
bool NetHkdfExpand(const uint8_t prk[NET_HASH_SIZE], const char *label,
                   uint8_t output[NET_KEY_SIZE]);

/* Стирает секрет так, чтобы компилятор не выбросил запись. */
void NetWipe(void *bytes, size_t size);
