/* Криптография laiue.net по тестовым векторам стандартов: ChaCha20, Poly1305
 * и AEAD — RFC 8439 (разделы 2.3.2, 2.4.2, 2.5.2, 2.8.2 и приложение A.3),
 * X25519 — RFC 7748 (разделы 5.2 и 6.1), HMAC-SHA-256 — RFC 4231 (случаи 1,
 * 2 и 6), HKDF — RFC 5869 (приложение A.1). Байты векторов выгружены из
 * текстов стандартов, а не набраны вручную. */

#include "net/net_crypto.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Release LTO не должен сплющить эталон и сценарии в точку входа: кадр
 * перерос бы страницу и потребовал __chkstk, которого в no-CRT сборке нет. */
#if defined(_MSC_VER)
#define CRYPTO_TEST_NOINLINE __declspec(noinline)
#else
#define CRYPTO_TEST_NOINLINE __attribute__((noinline))
#endif

typedef struct PolyCase
{
    uint8_t key[32];
    uint8_t data[64];
    uint32_t size;
    uint8_t tag[16];
} PolyCase;

static const uint8_t chachaBlock[64] = {
    0x10u, 0xf1u, 0xe7u, 0xe4u, 0xd1u, 0x3bu, 0x59u, 0x15u, 0x50u, 0x0fu, 0xddu, 0x1fu, 0xa3u,
    0x20u, 0x71u, 0xc4u, 0xc7u, 0xd1u, 0xf4u, 0xc7u, 0x33u, 0xc0u, 0x68u, 0x03u, 0x04u, 0x22u,
    0xaau, 0x9au, 0xc3u, 0xd4u, 0x6cu, 0x4eu, 0xd2u, 0x82u, 0x64u, 0x46u, 0x07u, 0x9fu, 0xaau,
    0x09u, 0x14u, 0xc2u, 0xd7u, 0x05u, 0xd9u, 0x8bu, 0x02u, 0xa2u, 0xb5u, 0x12u, 0x9cu, 0xd1u,
    0xdeu, 0x16u, 0x4eu, 0xb9u, 0xcbu, 0xd0u, 0x83u, 0xe8u, 0xa2u, 0x50u, 0x3cu, 0x4eu};
static const uint8_t sunscreenCipher[114] = {
    0x6eu, 0x2eu, 0x35u, 0x9au, 0x25u, 0x68u, 0xf9u, 0x80u, 0x41u, 0xbau, 0x07u, 0x28u, 0xddu,
    0x0du, 0x69u, 0x81u, 0xe9u, 0x7eu, 0x7au, 0xecu, 0x1du, 0x43u, 0x60u, 0xc2u, 0x0au, 0x27u,
    0xafu, 0xccu, 0xfdu, 0x9fu, 0xaeu, 0x0bu, 0xf9u, 0x1bu, 0x65u, 0xc5u, 0x52u, 0x47u, 0x33u,
    0xabu, 0x8fu, 0x59u, 0x3du, 0xabu, 0xcdu, 0x62u, 0xb3u, 0x57u, 0x16u, 0x39u, 0xd6u, 0x24u,
    0xe6u, 0x51u, 0x52u, 0xabu, 0x8fu, 0x53u, 0x0cu, 0x35u, 0x9fu, 0x08u, 0x61u, 0xd8u, 0x07u,
    0xcau, 0x0du, 0xbfu, 0x50u, 0x0du, 0x6au, 0x61u, 0x56u, 0xa3u, 0x8eu, 0x08u, 0x8au, 0x22u,
    0xb6u, 0x5eu, 0x52u, 0xbcu, 0x51u, 0x4du, 0x16u, 0xccu, 0xf8u, 0x06u, 0x81u, 0x8cu, 0xe9u,
    0x1au, 0xb7u, 0x79u, 0x37u, 0x36u, 0x5au, 0xf9u, 0x0bu, 0xbfu, 0x74u, 0xa3u, 0x5bu, 0xe6u,
    0xb4u, 0x0bu, 0x8eu, 0xedu, 0xf2u, 0x78u, 0x5eu, 0x42u, 0x87u, 0x4du};
static const uint8_t polyKey[32] = {0x85u, 0xd6u, 0xbeu, 0x78u, 0x57u, 0x55u, 0x6du, 0x33u,
                                    0x7fu, 0x44u, 0x52u, 0xfeu, 0x42u, 0xd5u, 0x06u, 0xa8u,
                                    0x01u, 0x03u, 0x80u, 0x8au, 0xfbu, 0x0du, 0xb2u, 0xfdu,
                                    0x4au, 0xbfu, 0xf6u, 0xafu, 0x41u, 0x49u, 0xf5u, 0x1bu};
static const uint8_t polyTag[16] = {0xa8u, 0x06u, 0x1du, 0xc1u, 0x30u, 0x51u, 0x36u, 0xc6u,
                                    0xc2u, 0x2bu, 0x8bu, 0xafu, 0x0cu, 0x01u, 0x27u, 0xa9u};
static const uint8_t aeadCipher[114] = {
    0xd3u, 0x1au, 0x8du, 0x34u, 0x64u, 0x8eu, 0x60u, 0xdbu, 0x7bu, 0x86u, 0xafu, 0xbcu, 0x53u,
    0xefu, 0x7eu, 0xc2u, 0xa4u, 0xadu, 0xedu, 0x51u, 0x29u, 0x6eu, 0x08u, 0xfeu, 0xa9u, 0xe2u,
    0xb5u, 0xa7u, 0x36u, 0xeeu, 0x62u, 0xd6u, 0x3du, 0xbeu, 0xa4u, 0x5eu, 0x8cu, 0xa9u, 0x67u,
    0x12u, 0x82u, 0xfau, 0xfbu, 0x69u, 0xdau, 0x92u, 0x72u, 0x8bu, 0x1au, 0x71u, 0xdeu, 0x0au,
    0x9eu, 0x06u, 0x0bu, 0x29u, 0x05u, 0xd6u, 0xa5u, 0xb6u, 0x7eu, 0xcdu, 0x3bu, 0x36u, 0x92u,
    0xddu, 0xbdu, 0x7fu, 0x2du, 0x77u, 0x8bu, 0x8cu, 0x98u, 0x03u, 0xaeu, 0xe3u, 0x28u, 0x09u,
    0x1bu, 0x58u, 0xfau, 0xb3u, 0x24u, 0xe4u, 0xfau, 0xd6u, 0x75u, 0x94u, 0x55u, 0x85u, 0x80u,
    0x8bu, 0x48u, 0x31u, 0xd7u, 0xbcu, 0x3fu, 0xf4u, 0xdeu, 0xf0u, 0x8eu, 0x4bu, 0x7au, 0x9du,
    0xe5u, 0x76u, 0xd2u, 0x65u, 0x86u, 0xceu, 0xc6u, 0x4bu, 0x61u, 0x16u};
static const uint8_t aeadTag[16] = {0x1au, 0xe1u, 0x0bu, 0x59u, 0x4fu, 0x09u, 0xe2u, 0x6au,
                                    0x7eu, 0x90u, 0x2eu, 0xcbu, 0xd0u, 0x60u, 0x06u, 0x91u};
static const PolyCase polyCases[] = {
    {{0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu},
     16u,
     {0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
    {{0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu},
     {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u},
     16u,
     {0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
    {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu, 0xffu, 0xf0u, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x11u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     48u,
     {0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
    {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu, 0xffu, 0xfbu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu,
      0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0xfeu, 0x01u, 0x01u, 0x01u, 0x01u,
      0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u, 0x01u},
     48u,
     {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
    {{0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xfdu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu},
     16u,
     {0xfau, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
      0xffu, 0xffu, 0xffu}},
    {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xe3u, 0x35u, 0x94u, 0xd7u, 0x50u, 0x5eu, 0x43u, 0xb9u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x33u, 0x94u, 0xd7u, 0x50u, 0x5eu, 0x43u, 0x79u, 0xcdu, 0x01u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     64u,
     {0x14u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x55u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
    {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     {0xe3u, 0x35u, 0x94u, 0xd7u, 0x50u, 0x5eu, 0x43u, 0xb9u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x33u, 0x94u, 0xd7u, 0x50u, 0x5eu, 0x43u, 0x79u, 0xcdu,
      0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u},
     48u,
     {0x13u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u}},
};
static const uint8_t curveScalar0[32] = {0xa5u, 0x46u, 0xe3u, 0x6bu, 0xf0u, 0x52u, 0x7cu, 0x9du,
                                         0x3bu, 0x16u, 0x15u, 0x4bu, 0x82u, 0x46u, 0x5eu, 0xddu,
                                         0x62u, 0x14u, 0x4cu, 0x0au, 0xc1u, 0xfcu, 0x5au, 0x18u,
                                         0x50u, 0x6au, 0x22u, 0x44u, 0xbau, 0x44u, 0x9au, 0xc4u};
static const uint8_t curvePoint0[32] = {0xe6u, 0xdbu, 0x68u, 0x67u, 0x58u, 0x30u, 0x30u, 0xdbu,
                                        0x35u, 0x94u, 0xc1u, 0xa4u, 0x24u, 0xb1u, 0x5fu, 0x7cu,
                                        0x72u, 0x66u, 0x24u, 0xecu, 0x26u, 0xb3u, 0x35u, 0x3bu,
                                        0x10u, 0xa9u, 0x03u, 0xa6u, 0xd0u, 0xabu, 0x1cu, 0x4cu};
static const uint8_t curveOutput0[32] = {0xc3u, 0xdau, 0x55u, 0x37u, 0x9du, 0xe9u, 0xc6u, 0x90u,
                                         0x8eu, 0x94u, 0xeau, 0x4du, 0xf2u, 0x8du, 0x08u, 0x4fu,
                                         0x32u, 0xecu, 0xcfu, 0x03u, 0x49u, 0x1cu, 0x71u, 0xf7u,
                                         0x54u, 0xb4u, 0x07u, 0x55u, 0x77u, 0xa2u, 0x85u, 0x52u};
static const uint8_t curveScalar1[32] = {0x4bu, 0x66u, 0xe9u, 0xd4u, 0xd1u, 0xb4u, 0x67u, 0x3cu,
                                         0x5au, 0xd2u, 0x26u, 0x91u, 0x95u, 0x7du, 0x6au, 0xf5u,
                                         0xc1u, 0x1bu, 0x64u, 0x21u, 0xe0u, 0xeau, 0x01u, 0xd4u,
                                         0x2cu, 0xa4u, 0x16u, 0x9eu, 0x79u, 0x18u, 0xbau, 0x0du};
static const uint8_t curvePoint1[32] = {0xe5u, 0x21u, 0x0fu, 0x12u, 0x78u, 0x68u, 0x11u, 0xd3u,
                                        0xf4u, 0xb7u, 0x95u, 0x9du, 0x05u, 0x38u, 0xaeu, 0x2cu,
                                        0x31u, 0xdbu, 0xe7u, 0x10u, 0x6fu, 0xc0u, 0x3cu, 0x3eu,
                                        0xfcu, 0x4cu, 0xd5u, 0x49u, 0xc7u, 0x15u, 0xa4u, 0x93u};
static const uint8_t curveOutput1[32] = {0x95u, 0xcbu, 0xdeu, 0x94u, 0x76u, 0xe8u, 0x90u, 0x7du,
                                         0x7au, 0xadu, 0xe4u, 0x5cu, 0xb4u, 0xb8u, 0x73u, 0xf8u,
                                         0x8bu, 0x59u, 0x5au, 0x68u, 0x79u, 0x9fu, 0xa1u, 0x52u,
                                         0xe6u, 0xf8u, 0xf7u, 0x64u, 0x7au, 0xacu, 0x79u, 0x57u};
static const uint8_t curveOnce[32] = {0x42u, 0x2cu, 0x8eu, 0x7au, 0x62u, 0x27u, 0xd7u, 0xbcu,
                                      0xa1u, 0x35u, 0x0bu, 0x3eu, 0x2bu, 0xb7u, 0x27u, 0x9fu,
                                      0x78u, 0x97u, 0xb8u, 0x7bu, 0xb6u, 0x85u, 0x4bu, 0x78u,
                                      0x3cu, 0x60u, 0xe8u, 0x03u, 0x11u, 0xaeu, 0x30u, 0x79u};
static const uint8_t curveThousand[32] = {0x68u, 0x4cu, 0xf5u, 0x9bu, 0xa8u, 0x33u, 0x09u, 0x55u,
                                          0x28u, 0x00u, 0xefu, 0x56u, 0x6fu, 0x2fu, 0x4du, 0x3cu,
                                          0x1cu, 0x38u, 0x87u, 0xc4u, 0x93u, 0x60u, 0xe3u, 0x87u,
                                          0x5fu, 0x2eu, 0xb9u, 0x4du, 0x99u, 0x53u, 0x2cu, 0x51u};
static const uint8_t alicePrivate[32] = {0x77u, 0x07u, 0x6du, 0x0au, 0x73u, 0x18u, 0xa5u, 0x7du,
                                         0x3cu, 0x16u, 0xc1u, 0x72u, 0x51u, 0xb2u, 0x66u, 0x45u,
                                         0xdfu, 0x4cu, 0x2fu, 0x87u, 0xebu, 0xc0u, 0x99u, 0x2au,
                                         0xb1u, 0x77u, 0xfbu, 0xa5u, 0x1du, 0xb9u, 0x2cu, 0x2au};
static const uint8_t alicePublic[32] = {0x85u, 0x20u, 0xf0u, 0x09u, 0x89u, 0x30u, 0xa7u, 0x54u,
                                        0x74u, 0x8bu, 0x7du, 0xdcu, 0xb4u, 0x3eu, 0xf7u, 0x5au,
                                        0x0du, 0xbfu, 0x3au, 0x0du, 0x26u, 0x38u, 0x1au, 0xf4u,
                                        0xebu, 0xa4u, 0xa9u, 0x8eu, 0xaau, 0x9bu, 0x4eu, 0x6au};
static const uint8_t bobPrivate[32] = {0x5du, 0xabu, 0x08u, 0x7eu, 0x62u, 0x4au, 0x8au, 0x4bu,
                                       0x79u, 0xe1u, 0x7fu, 0x8bu, 0x83u, 0x80u, 0x0eu, 0xe6u,
                                       0x6fu, 0x3bu, 0xb1u, 0x29u, 0x26u, 0x18u, 0xb6u, 0xfdu,
                                       0x1cu, 0x2fu, 0x8bu, 0x27u, 0xffu, 0x88u, 0xe0u, 0xebu};
static const uint8_t bobPublic[32] = {0xdeu, 0x9eu, 0xdbu, 0x7du, 0x7bu, 0x7du, 0xc1u, 0xb4u,
                                      0xd3u, 0x5bu, 0x61u, 0xc2u, 0xecu, 0xe4u, 0x35u, 0x37u,
                                      0x3fu, 0x83u, 0x43u, 0xc8u, 0x5bu, 0x78u, 0x67u, 0x4du,
                                      0xadu, 0xfcu, 0x7eu, 0x14u, 0x6fu, 0x88u, 0x2bu, 0x4fu};
static const uint8_t sharedSecret[32] = {0x4au, 0x5du, 0x9du, 0x5bu, 0xa4u, 0xceu, 0x2du, 0xe1u,
                                         0x72u, 0x8eu, 0x3bu, 0xf4u, 0x80u, 0x35u, 0x0fu, 0x25u,
                                         0xe0u, 0x7eu, 0x21u, 0xc9u, 0x47u, 0xd1u, 0x9eu, 0x33u,
                                         0x76u, 0xf0u, 0x9bu, 0x3cu, 0x1eu, 0x16u, 0x17u, 0x42u};
static const uint8_t hmacCase1[32] = {0xb0u, 0x34u, 0x4cu, 0x61u, 0xd8u, 0xdbu, 0x38u, 0x53u,
                                      0x5cu, 0xa8u, 0xafu, 0xceu, 0xafu, 0x0bu, 0xf1u, 0x2bu,
                                      0x88u, 0x1du, 0xc2u, 0x00u, 0xc9u, 0x83u, 0x3du, 0xa7u,
                                      0x26u, 0xe9u, 0x37u, 0x6cu, 0x2eu, 0x32u, 0xcfu, 0xf7u};
static const uint8_t hmacCase2[32] = {0x5bu, 0xdcu, 0xc1u, 0x46u, 0xbfu, 0x60u, 0x75u, 0x4eu,
                                      0x6au, 0x04u, 0x24u, 0x26u, 0x08u, 0x95u, 0x75u, 0xc7u,
                                      0x5au, 0x00u, 0x3fu, 0x08u, 0x9du, 0x27u, 0x39u, 0x83u,
                                      0x9du, 0xecu, 0x58u, 0xb9u, 0x64u, 0xecu, 0x38u, 0x43u};
static const uint8_t hmacCase6[32] = {0x60u, 0xe4u, 0x31u, 0x59u, 0x1eu, 0xe0u, 0xb6u, 0x7fu,
                                      0x0du, 0x8au, 0x26u, 0xaau, 0xcbu, 0xf5u, 0xb7u, 0x7fu,
                                      0x8eu, 0x0bu, 0xc6u, 0x21u, 0x37u, 0x28u, 0xc5u, 0x14u,
                                      0x05u, 0x46u, 0x04u, 0x0fu, 0x0eu, 0xe3u, 0x7fu, 0x54u};
static const uint8_t hkdfPrk[32] = {0x07u, 0x77u, 0x09u, 0x36u, 0x2cu, 0x2eu, 0x32u, 0xdfu,
                                    0x0du, 0xdcu, 0x3fu, 0x0du, 0xc4u, 0x7bu, 0xbau, 0x63u,
                                    0x90u, 0xb6u, 0xc7u, 0x3bu, 0xb5u, 0x0fu, 0x9cu, 0x31u,
                                    0x22u, 0xecu, 0x84u, 0x4au, 0xd7u, 0xc2u, 0xb3u, 0xe5u};
static const uint8_t hkdfOkm[32] = {0x3cu, 0xb2u, 0x5fu, 0x25u, 0xfau, 0xacu, 0xd5u, 0x7au,
                                    0x90u, 0x43u, 0x4fu, 0x64u, 0xd0u, 0x36u, 0x2fu, 0x2au,
                                    0x2du, 0x2du, 0x0au, 0x90u, 0xcfu, 0x1au, 0x5au, 0x4cu,
                                    0x5du, 0xb0u, 0x2du, 0x56u, 0xecu, 0xc4u, 0xc5u, 0xbfu};

#define SUNSCREEN_SIZE 114u
static const char sunscreen[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                "only one tip for the future, sunscreen would be it.";

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Net crypto failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Same(const void *first, const void *second, size_t size)
{
    return memcmp(first, second, size) == 0;
}

static void FillSequence(uint8_t *bytes, uint32_t size, uint8_t first)
{
    for (uint32_t index = 0u; index < size; ++index)
        bytes[index] = (uint8_t)(first + index);
}

static void TestChaCha20(void)
{
    static const uint8_t blockNonce[NET_NONCE_SIZE] = {0u, 0u, 0u, 9u, 0u, 0u, 0u, 0x4au};
    static const uint8_t streamNonce[NET_NONCE_SIZE] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x4au};
    uint8_t key[NET_KEY_SIZE];
    uint8_t block[64];
    uint8_t text[SUNSCREEN_SIZE];
    FillSequence(key, sizeof(key), 0u);
    NetChaCha20Block(key, 1u, blockNonce, block);
    Expect(Same(block, chachaBlock, sizeof(block)), "ChaCha20 block");
    NetChaCha20Xor(key, 1u, streamNonce, (const uint8_t *)sunscreen, text, sizeof(text));
    Expect(Same(text, sunscreenCipher, sizeof(text)), "ChaCha20 encryption");
    NetChaCha20Xor(key, 1u, streamNonce, text, text, sizeof(text));
    Expect(Same(text, sunscreen, sizeof(text)), "ChaCha20 decrypts in place");
}

static void TestPoly1305(void)
{
    static const char message[] = "Cryptographic Forum Research Group";
    const size_t size = sizeof(message) - 1u;
    uint8_t tag[NET_TAG_SIZE];
    NetPoly1305 mac;
    /* Тег не зависит от того, какими кусками пришло сообщение. */
    for (size_t split = 0u; split <= size; ++split)
    {
        NetPoly1305Begin(&mac, polyKey);
        NetPoly1305Update(&mac, (const uint8_t *)message, split);
        NetPoly1305Update(&mac, (const uint8_t *)message + split, size - split);
        NetPoly1305Finish(&mac, tag);
        Expect(Same(tag, polyTag, sizeof(tag)), "Poly1305 tag");
    }
    NetPoly1305Begin(&mac, polyKey);
    for (size_t index = 0u; index < size; ++index)
        NetPoly1305Update(&mac, (const uint8_t *)message + index, 1u);
    NetPoly1305Finish(&mac, tag);
    Expect(Same(tag, polyTag, sizeof(tag)), "Poly1305 byte by byte");

    /* Переносы и приведение на границах 2^130 - 5. */
    for (size_t index = 0u; index < sizeof(polyCases) / sizeof(polyCases[0]); ++index)
    {
        const PolyCase *vector = &polyCases[index];
        NetPoly1305Begin(&mac, vector->key);
        NetPoly1305Update(&mac, vector->data, vector->size);
        NetPoly1305Finish(&mac, tag);
        Expect(Same(tag, vector->tag, sizeof(tag)), "Poly1305 reduction edge case");
    }
}

static void TestAead(void)
{
    static const uint8_t nonce[NET_NONCE_SIZE] = {7u,    0u,    0u,    0u,    0x40u, 0x41u,
                                                  0x42u, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u};
    static const uint8_t aad[12] = {0x50u, 0x51u, 0x52u, 0x53u, 0xc0u, 0xc1u,
                                    0xc2u, 0xc3u, 0xc4u, 0xc5u, 0xc6u, 0xc7u};
    uint8_t key[NET_KEY_SIZE];
    uint8_t cipher[SUNSCREEN_SIZE];
    uint8_t opened[SUNSCREEN_SIZE];
    uint8_t tag[NET_TAG_SIZE];
    FillSequence(key, sizeof(key), 0x80u);
    NetAeadSeal(key, nonce, aad, sizeof(aad), (const uint8_t *)sunscreen, sizeof(cipher), cipher,
                tag);
    Expect(Same(cipher, aeadCipher, sizeof(cipher)), "AEAD ciphertext");
    Expect(Same(tag, aeadTag, sizeof(tag)), "AEAD tag");
    Expect(NetAeadOpen(key, nonce, aad, sizeof(aad), cipher, sizeof(cipher), tag, opened),
           "AEAD opens its own seal");
    Expect(Same(opened, sunscreen, sizeof(opened)), "AEAD plaintext");

    /* Любой изменённый бит шифротекста, тега, заголовка, nonce или ключа
     * отвергается, и выход при этом не тронут. */
    memset(opened, 0xA5, sizeof(opened));
    for (uint32_t bit = 0u; bit < sizeof(cipher) * 8u; bit += 7u)
    {
        cipher[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        Expect(!NetAeadOpen(key, nonce, aad, sizeof(aad), cipher, sizeof(cipher), tag, opened),
               "AEAD rejects a changed ciphertext");
        cipher[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
    for (uint32_t bit = 0u; bit < sizeof(tag) * 8u; ++bit)
    {
        tag[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        Expect(!NetAeadOpen(key, nonce, aad, sizeof(aad), cipher, sizeof(cipher), tag, opened),
               "AEAD rejects a changed tag");
        tag[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
    uint8_t changed[NET_KEY_SIZE];
    memcpy(changed, aad, sizeof(aad));
    changed[3] ^= 1u;
    Expect(!NetAeadOpen(key, nonce, changed, sizeof(aad), cipher, sizeof(cipher), tag, opened),
           "AEAD rejects changed associated data");
    Expect(!NetAeadOpen(key, nonce, aad, sizeof(aad) - 1u, cipher, sizeof(cipher), tag, opened),
           "AEAD rejects shortened associated data");
    memcpy(changed, nonce, sizeof(nonce));
    changed[11] ^= 0x80u;
    Expect(!NetAeadOpen(key, changed, aad, sizeof(aad), cipher, sizeof(cipher), tag, opened),
           "AEAD rejects another nonce");
    memcpy(changed, key, sizeof(key));
    changed[0] ^= 1u;
    Expect(!NetAeadOpen(changed, nonce, aad, sizeof(aad), cipher, sizeof(cipher), tag, opened),
           "AEAD rejects another key");
    Expect(!NetAeadOpen(key, nonce, aad, sizeof(aad), cipher, sizeof(cipher) - 1u, tag, opened),
           "AEAD rejects a truncated ciphertext");
    for (size_t index = 0u; index < sizeof(opened); ++index)
        Expect(opened[index] == 0xA5u, "a rejected packet leaves the output untouched");

    /* Пустой текст и пустой заголовок — законный пакет. */
    NetAeadSeal(key, nonce, NULL, 0u, NULL, 0u, cipher, tag);
    Expect(NetAeadOpen(key, nonce, NULL, 0u, cipher, 0u, tag, opened), "AEAD of nothing");
    tag[0] ^= 1u;
    Expect(!NetAeadOpen(key, nonce, NULL, 0u, cipher, 0u, tag, opened),
           "AEAD of nothing still checks the tag");

    /* В обе стороны на всех длинах вокруг границ блоков 16 и 64 байта. */
    uint8_t buffer[200];
    uint8_t sealed[200];
    for (uint32_t size = 0u; size <= sizeof(buffer); ++size)
    {
        FillSequence(buffer, size, (uint8_t)size);
        NetAeadSeal(key, nonce, aad, size % 13u, buffer, size, sealed, tag);
        Expect(size < 4u || !Same(sealed, buffer, size), "AEAD changes the text");
        Expect(NetAeadOpen(key, nonce, aad, size % 13u, sealed, size, tag, sealed),
               "AEAD round trip in place");
        Expect(Same(sealed, buffer, size), "AEAD round trip text");
    }
}

static void TestX25519(void)
{
    uint8_t output[NET_KEY_SIZE];
    Expect(NetX25519(curveScalar0, curvePoint0, output) &&
               Same(output, curveOutput0, sizeof(output)),
           "X25519 vector 1");
    Expect(NetX25519(curveScalar1, curvePoint1, output) &&
               Same(output, curveOutput1, sizeof(output)),
           "X25519 vector 2");

    /* Итерации из раздела 5.2: k и u начинаются с девятки, затем
     * k = X25519(k, u), а u — прежний k. */
    uint8_t scalar[NET_KEY_SIZE] = {9u};
    uint8_t point[NET_KEY_SIZE] = {9u};
    for (uint32_t iteration = 1u; iteration <= 1000u; ++iteration)
    {
        Expect(NetX25519(scalar, point, output), "X25519 iteration");
        memcpy(point, scalar, sizeof(point));
        memcpy(scalar, output, sizeof(scalar));
        if (iteration == 1u)
            Expect(Same(scalar, curveOnce, sizeof(scalar)), "X25519 after one iteration");
    }
    Expect(Same(scalar, curveThousand, sizeof(scalar)), "X25519 after a thousand iterations");

    /* Обмен ключами из раздела 6.1. */
    uint8_t publicKey[NET_KEY_SIZE];
    uint8_t shared[NET_KEY_SIZE];
    NetX25519Public(alicePrivate, publicKey);
    Expect(Same(publicKey, alicePublic, sizeof(publicKey)), "Alice's public key");
    NetX25519Public(bobPrivate, publicKey);
    Expect(Same(publicKey, bobPublic, sizeof(publicKey)), "Bob's public key");
    Expect(NetX25519(alicePrivate, bobPublic, shared) && Same(shared, sharedSecret, sizeof(shared)),
           "Alice's shared secret");
    Expect(NetX25519(bobPrivate, alicePublic, shared) && Same(shared, sharedSecret, sizeof(shared)),
           "Bob's shared secret");

    /* Точки малого порядка дают нулевой секрет, и он отвергается. */
    uint8_t low[NET_KEY_SIZE] = {0u};
    Expect(!NetX25519(alicePrivate, low, shared), "the zero point is rejected");
    low[0] = 1u;
    Expect(!NetX25519(alicePrivate, low, shared), "the point of order four is rejected");
    /* Старший бит u-координаты не участвует: p + 9 с ним читается как 9. */
    uint8_t masked[NET_KEY_SIZE] = {9u};
    masked[31] = 0x80u;
    Expect(NetX25519(alicePrivate, masked, publicKey) &&
               Same(publicKey, alicePublic, sizeof(publicKey)),
           "the top bit of a point is ignored");
    /* Неканоническая запись: p + 9 то же, что 9. */
    uint8_t wrapped[NET_KEY_SIZE];
    memset(wrapped, 0xFF, sizeof(wrapped));
    wrapped[0] = 0xF6u;
    wrapped[31] = 0x7Fu;
    Expect(NetX25519(alicePrivate, wrapped, publicKey) &&
               Same(publicKey, alicePublic, sizeof(publicKey)),
           "a point above the prime is reduced");
}

/* ---- Эталон X25519 ------------------------------------------------------------
 *
 * Прямолинейная медленная лестница: шестнадцать разрядов по 16 бит, умножение
 * в столбик, обращение возведением в степень p - 2 бит за битом. Быстрая
 * реализация модуля обязана совпадать с ней на любых входах. */

typedef uint64_t ReferenceField[16];

static void ReferenceCarry(ReferenceField value)
{
    for (uint32_t index = 0u; index < 15u; ++index)
    {
        value[index + 1u] += value[index] >> 16u;
        value[index] &= 0xFFFFu;
    }
    const uint64_t top = value[15] >> 16u;
    value[15] &= 0xFFFFu;
    value[0] += 38u * top;
}

static void ReferenceAdd(ReferenceField output, const ReferenceField left,
                         const ReferenceField right)
{
    for (uint32_t index = 0u; index < 16u; ++index)
        output[index] = left[index] + right[index];
}

/* left + 8p - right; right почти приведён. */
static void ReferenceSubtract(ReferenceField output, const ReferenceField left,
                              const ReferenceField right)
{
    output[0] = left[0] + UINT64_C(0x7FF68) - right[0];
    for (uint32_t index = 1u; index < 15u; ++index)
        output[index] = left[index] + UINT64_C(0x7FFF8) - right[index];
    output[15] = left[15] + UINT64_C(0x3FFF8) - right[15];
}

static void ReferenceMultiply(ReferenceField output, const ReferenceField left,
                              const ReferenceField right)
{
    uint64_t product[31] = {0};
    for (uint32_t i = 0u; i < 16u; ++i)
        for (uint32_t j = 0u; j < 16u; ++j)
            product[i + j] += left[i] * right[j];
    for (uint32_t index = 0u; index < 15u; ++index)
        product[index] += 38u * product[index + 16u];
    for (uint32_t index = 0u; index < 16u; ++index)
        output[index] = product[index];
    ReferenceCarry(output);
    ReferenceCarry(output);
}

static void ReferenceSwap(ReferenceField left, ReferenceField right, uint64_t swap)
{
    const uint64_t mask = 0u - swap;
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        const uint64_t difference = mask & (left[index] ^ right[index]);
        left[index] ^= difference;
        right[index] ^= difference;
    }
}

/* value^(2^255 - 21): единицы во всех битах показателя, кроме битов 2 и 4. */
static void ReferenceInvert(ReferenceField output, const ReferenceField value)
{
    ReferenceField result;
    memcpy(result, value, sizeof(ReferenceField));
    for (int32_t bit = 253; bit >= 0; --bit)
    {
        ReferenceMultiply(result, result, result);
        if (bit != 2 && bit != 4)
            ReferenceMultiply(result, result, value);
    }
    memcpy(output, result, sizeof(ReferenceField));
}

static CRYPTO_TEST_NOINLINE void ReferenceX25519(const uint8_t scalar[NET_KEY_SIZE],
                                                 const uint8_t point[NET_KEY_SIZE],
                                                 uint8_t output[NET_KEY_SIZE])
{
    static const ReferenceField a24 = {0xDB41u, 1u};
    uint8_t clamped[NET_KEY_SIZE];
    memcpy(clamped, scalar, NET_KEY_SIZE);
    clamped[0] &= 248u;
    clamped[31] = (uint8_t)((clamped[31] & 127u) | 64u);
    ReferenceField x1;
    for (size_t index = 0u; index < 16u; ++index)
        x1[index] = (uint64_t)point[index * 2u] | ((uint64_t)point[index * 2u + 1u] << 8u);
    x1[15] &= 0x7FFFu;
    ReferenceField x2 = {1u};
    ReferenceField z2 = {0u};
    ReferenceField x3;
    ReferenceField z3 = {1u};
    ReferenceField a;
    ReferenceField b;
    ReferenceField c;
    ReferenceField d;
    ReferenceField e;
    ReferenceField aa;
    ReferenceField bb;
    memcpy(x3, x1, sizeof(ReferenceField));
    uint64_t swap = 0u;
    for (int32_t bit = 254; bit >= 0; --bit)
    {
        const uint64_t scalarBit = (uint64_t)(clamped[bit >> 3] >> (bit & 7)) & 1u;
        swap ^= scalarBit;
        ReferenceSwap(x2, x3, swap);
        ReferenceSwap(z2, z3, swap);
        swap = scalarBit;
        ReferenceAdd(a, x2, z2);
        ReferenceSubtract(b, x2, z2);
        ReferenceAdd(c, x3, z3);
        ReferenceSubtract(d, x3, z3);
        ReferenceMultiply(aa, a, a);
        ReferenceMultiply(bb, b, b);
        ReferenceSubtract(e, aa, bb);
        ReferenceMultiply(d, d, a);
        ReferenceMultiply(c, c, b);
        ReferenceAdd(x3, d, c);
        ReferenceMultiply(x3, x3, x3);
        ReferenceSubtract(z3, d, c);
        ReferenceMultiply(z3, z3, z3);
        ReferenceMultiply(z3, z3, x1);
        ReferenceMultiply(x2, aa, bb);
        ReferenceMultiply(z2, a24, e);
        ReferenceAdd(z2, z2, aa);
        ReferenceMultiply(z2, z2, e);
    }
    ReferenceSwap(x2, x3, swap);
    ReferenceSwap(z2, z3, swap);
    ReferenceInvert(z2, z2);
    ReferenceMultiply(x2, x2, z2);
    /* Канонический вид: после двух переносов значение меньше 2p + 38, и p
     * вычитается не больше двух раз. */
    ReferenceCarry(x2);
    ReferenceCarry(x2);
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        ReferenceField lowered;
        uint64_t borrow = 0u;
        for (uint32_t index = 0u; index < 16u; ++index)
        {
            const uint64_t prime = index == 0u ? 0xFFEDu : index == 15u ? 0x7FFFu : 0xFFFFu;
            const uint64_t difference = x2[index] - prime - borrow;
            borrow = difference >> 63u;
            lowered[index] = difference & 0xFFFFu;
        }
        if (borrow == 0u)
            memcpy(x2, lowered, sizeof(ReferenceField));
    }
    for (size_t index = 0u; index < 16u; ++index)
    {
        output[index * 2u] = (uint8_t)x2[index];
        output[index * 2u + 1u] = (uint8_t)(x2[index] >> 8u);
    }
}

static uint64_t randomState = UINT64_C(0x243F6A8885A308D3);

/* splitmix64: тест повторяется байт в байт. */
static uint64_t NextRandom(void)
{
    randomState += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t value = randomState;
    value = (value ^ (value >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27u)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31u);
}

static void FillRandom(uint8_t *bytes, uint32_t size)
{
    for (uint32_t index = 0u; index < size; ++index)
        bytes[index] = (uint8_t)(NextRandom() >> 56u);
}

static bool AllZero(const uint8_t *bytes, uint32_t size)
{
    uint8_t any = 0u;
    for (uint32_t index = 0u; index < size; ++index)
        any |= bytes[index];
    return any == 0u;
}

/* Сравнивает быструю лестницу с эталоном; ноль на выходе отвергается. */
static void ExpectReference(const uint8_t scalar[NET_KEY_SIZE], const uint8_t point[NET_KEY_SIZE],
                            const char *message)
{
    uint8_t fast[NET_KEY_SIZE];
    uint8_t slow[NET_KEY_SIZE];
    const bool accepted = NetX25519(scalar, point, fast);
    ReferenceX25519(scalar, point, slow);
    Expect(Same(fast, slow, NET_KEY_SIZE) && accepted == !AllZero(slow, NET_KEY_SIZE), message);
}

/* Значение 2^bits - 1 либо 2^bits, little-endian. */
static void PowerOfTwo(uint8_t bytes[NET_KEY_SIZE], uint32_t bits, bool minusOne)
{
    memset(bytes, 0, NET_KEY_SIZE);
    if (minusOne)
    {
        for (uint32_t bit = 0u; bit < bits; ++bit)
            bytes[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    }
    else if (bits < 256u)
    {
        bytes[bits / 8u] = (uint8_t)(1u << (bits % 8u));
    }
}

/* Точки, умножение на которые даёт ноль при любом скаляре: их порядок делит 8.
 * Ноль, единица, две u-координаты точек порядка 8, p - 1, p и p + 1. */
static const uint8_t smallOrder[7][NET_KEY_SIZE] = {
    {0x00u},
    {0x01u},
    {0xe0u, 0xebu, 0x7au, 0x7cu, 0x3bu, 0x41u, 0xb8u, 0xaeu, 0x16u, 0x56u, 0xe3u,
     0xfau, 0xf1u, 0x9fu, 0xc4u, 0x6au, 0xdau, 0x09u, 0x8du, 0xebu, 0x9cu, 0x32u,
     0xb1u, 0xfdu, 0x86u, 0x62u, 0x05u, 0x16u, 0x5fu, 0x49u, 0xb8u, 0x00u},
    {0x5fu, 0x9cu, 0x95u, 0xbcu, 0xa3u, 0x50u, 0x8cu, 0x24u, 0xb1u, 0xd0u, 0xb1u,
     0x55u, 0x9cu, 0x83u, 0xefu, 0x5bu, 0x04u, 0x44u, 0x5cu, 0xc4u, 0x58u, 0x1cu,
     0x8eu, 0x86u, 0xd8u, 0x22u, 0x4eu, 0xddu, 0xd0u, 0x9fu, 0x11u, 0x57u},
    {0xecu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x7fu},
    {0xedu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x7fu},
    {0xeeu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,
     0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x7fu},
};

/* Быстрая лестница совпадает с эталоном: случайные скаляры и точки, в том
 * числе не меньше p и с поднятым старшим битом, границы 51-битных разрядов,
 * значения рядом с p и точки малого порядка. */
static CRYPTO_TEST_NOINLINE void TestX25519AgainstReference(void)
{
    uint8_t scalar[NET_KEY_SIZE];
    uint8_t point[NET_KEY_SIZE];
    for (uint32_t round = 0u; round < 256u; ++round)
    {
        FillRandom(scalar, NET_KEY_SIZE);
        FillRandom(point, NET_KEY_SIZE);
        ExpectReference(scalar, point, "X25519 matches the reference on random input");
    }

    static uint8_t points[40][NET_KEY_SIZE];
    uint32_t count = 0u;
    for (uint32_t limb = 1u; limb <= 5u; ++limb)
    {
        PowerOfTwo(points[count++], 51u * limb, true);
        PowerOfTwo(points[count++], 51u * limb, false);
        PowerOfTwo(points[count++], 51u * limb - 1u, false);
    }
    PowerOfTwo(points[count++], 256u, true);
    /* p + 2 .. p + 18 и 2^255 - 1 = p + 18: неканоническая запись малых
     * чисел. */
    for (uint32_t above = 2u; above <= 18u; above += 4u)
    {
        memcpy(points[count], smallOrder[5], NET_KEY_SIZE);
        points[count++][0] = (uint8_t)(0xEDu + above);
    }
    /* p - 2 .. p - 38: самые большие канонические. */
    for (uint32_t below = 2u; below <= 38u; below += 9u)
    {
        memcpy(points[count], smallOrder[5], NET_KEY_SIZE);
        points[count++][0] = (uint8_t)(0xEDu - below);
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        for (uint32_t round = 0u; round < 3u; ++round)
        {
            FillRandom(scalar, NET_KEY_SIZE);
            ExpectReference(scalar, points[index], "X25519 matches the reference at the edges");
        }
        memset(scalar, 0xFF, sizeof(scalar));
        ExpectReference(scalar, points[index], "X25519 matches the reference with the top scalar");
    }

    uint8_t output[NET_KEY_SIZE];
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        memcpy(point, smallOrder[index], NET_KEY_SIZE);
        for (uint32_t high = 0u; high < 2u; ++high)
        {
            point[31] = (uint8_t)(smallOrder[index][31] | (high << 7u));
            FillRandom(scalar, NET_KEY_SIZE);
            uint8_t slow[NET_KEY_SIZE];
            ReferenceX25519(scalar, point, slow);
            Expect(AllZero(slow, NET_KEY_SIZE) && !NetX25519(scalar, point, output) &&
                       AllZero(output, NET_KEY_SIZE),
                   "a point of small order gives zero and is rejected");
        }
    }
}

static void TestHmacAndHkdf(void)
{
    uint8_t key[131];
    uint8_t digest[NET_HASH_SIZE];
    memset(key, 0x0b, 20u);
    Expect(NetHmacSha256(key, 20u, (const uint8_t *)"Hi There", 8u, digest) &&
               Same(digest, hmacCase1, sizeof(digest)),
           "HMAC test case 1");
    Expect(NetHmacSha256((const uint8_t *)"Jefe", 4u,
                         (const uint8_t *)"what do ya want for nothing?", 28u, digest) &&
               Same(digest, hmacCase2, sizeof(digest)),
           "HMAC test case 2");
    memset(key, 0xaa, sizeof(key));
    Expect(NetHmacSha256(key, sizeof(key),
                         (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First",
                         54u, digest) &&
               Same(digest, hmacCase6, sizeof(digest)),
           "HMAC test case 6");
    uint8_t large[NET_HMAC_MESSAGE_CAPACITY + 1u] = {0u};
    Expect(NetHmacSha256(key, 20u, large, NET_HMAC_MESSAGE_CAPACITY, digest),
           "HMAC takes a message of the documented size");
    Expect(!NetHmacSha256(key, 20u, large, sizeof(large), digest),
           "HMAC refuses a message it cannot hold");

    uint8_t salt[13];
    uint8_t prk[NET_HASH_SIZE];
    uint8_t okm[NET_KEY_SIZE];
    static const char info[] = "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9";
    FillSequence(salt, sizeof(salt), 0u);
    memset(key, 0x0b, 22u);
    Expect(NetHkdfExtract(salt, sizeof(salt), key, 22u, prk) && Same(prk, hkdfPrk, sizeof(prk)),
           "HKDF extract");
    Expect(NetHkdfExpand(prk, info, okm) && Same(okm, hkdfOkm, sizeof(okm)), "HKDF expand");
    uint8_t other[NET_KEY_SIZE];
    Expect(NetHkdfExpand(prk, "another label", other) && !Same(other, okm, sizeof(okm)),
           "labels separate keys");
}

static void TestWipe(void)
{
    uint8_t secret[40];
    memset(secret, 0x5A, sizeof(secret));
    NetWipe(secret + 4u, 32u);
    for (uint32_t index = 0u; index < sizeof(secret); ++index)
        Expect(secret[index] == (index < 4u || index >= 36u ? 0x5Au : 0u), "wipe clears its range");
}

LAIUE_TEST_ENTRY(NetCryptoTestEntryPoint)
{
    TestChaCha20();
    TestPoly1305();
    TestAead();
    TestX25519();
    TestX25519AgainstReference();
    TestHmacAndHkdf();
    TestWipe();
    LaiueTestRuntimeWrite("net crypto: passed\n");
    LAIUE_TEST_SUCCESS();
}
