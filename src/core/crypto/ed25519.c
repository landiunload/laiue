#include "crypto/ed25519.h"

#include "crypto/field25519.h"
#include "crypto/scalar25519.h"
#include "crypto/sha512.h"
#include "crypto/wipe.h"

#include <string.h>

/* Сравнение без ветвлений по содержимому. */
static bool BytesEqual(const uint8_t *left, const uint8_t *right, size_t size)
{
    uint8_t difference = 0u;
    for (size_t index = 0u; index < size; ++index)
        difference |= (uint8_t)(left[index] ^ right[index]);
    return difference == 0u;
}

/* ---- Группа точек ------------------------------------------------------------
 *
 * Скрученная кривая Эдвардса -x^2 + y^2 = 1 + d x^2 y^2 над полем 2^255 - 19.
 * Точка хранится в расширенных координатах (X : Y : Z : T): x = X/Z,
 * y = Y/Z, xy = T/Z. Сложение — add-2008-hwcd-3, удвоение — dbl-2008-hwcd
 * (Hisil, Wong, Carter, Dawson). При a = -1 и неквадратном d сложение
 * полное: верно для любых двух точек, включая равные и нейтральную.
 * Координаты точек всегда почти приведены. */

typedef struct EdPoint
{
    Field x;
    Field y;
    Field z;
    Field t;
} EdPoint;

/* Промежуточные значения сложения и удвоения. В них бывают кратные
 * секретного скаляра, поэтому лестница стирает их вместе со своими
 * точками. */
typedef struct EdScratch
{
    Field a;
    Field b;
    Field c;
    Field d;
    Field e;
    Field f;
    Field g;
    Field h;
} EdScratch;

/* d = -121665 / 121666. */
static const Field edD = {UINT64_C(0x34dca135978a3), UINT64_C(0x1a8283b156ebd),
                          UINT64_C(0x5e7a26001c029), UINT64_C(0x739c663a03cbb),
                          UINT64_C(0x52036cee2b6ff)};
/* 2d — множитель сложения. */
static const Field edD2 = {UINT64_C(0x69b9426b2f159), UINT64_C(0x35050762add7a),
                           UINT64_C(0x3cf44c0038052), UINT64_C(0x6738cc7407977),
                           UINT64_C(0x2406d9dc56dff)};
/* Квадратный корень из -1: 2^((p - 1) / 4). */
static const Field fieldSqrtMinusOne = {UINT64_C(0x61b274a0ea0b0), UINT64_C(0x0d5a5fc8f189d),
                                        UINT64_C(0x7ef5e9cbd0c60), UINT64_C(0x78595a6804c9e),
                                        UINT64_C(0x2b8324804fc1d)};
static const Field fieldOne = {1u};

/* Базовая точка B: y = 4/5, канонический x чётный. */
static const EdPoint edBase = {
    {UINT64_C(0x62d608f25d51a), UINT64_C(0x412a4b4f6592a), UINT64_C(0x75b7171a4b31d),
     UINT64_C(0x1ff60527118fe), UINT64_C(0x216936d3cd6e5)},
    {UINT64_C(0x6666666666658), UINT64_C(0x4cccccccccccc), UINT64_C(0x1999999999999),
     UINT64_C(0x3333333333333), UINT64_C(0x6666666666666)},
    {1u},
    {UINT64_C(0x68ab3a5b7dda3), UINT64_C(0x00eea2a5eadbb), UINT64_C(0x2af8df483c27e),
     UINT64_C(0x332b375274732), UINT64_C(0x67875f0fd78b7)},
};

static const EdPoint edIdentity = {{0u}, {1u}, {1u}, {0u}};

/* Результат пишется последним, поэтому output может совпадать с left или
 * right. */
static void EdAdd(EdPoint *output, const EdPoint *left, const EdPoint *right, EdScratch *scratch)
{
    FieldSubtract(scratch->a, left->y, left->x);
    FieldSubtract(scratch->b, right->y, right->x);
    FieldMultiply(scratch->a, scratch->a, scratch->b);
    FieldAdd(scratch->b, left->y, left->x);
    FieldAdd(scratch->c, right->y, right->x);
    FieldMultiply(scratch->b, scratch->b, scratch->c);
    FieldMultiply(scratch->c, left->t, right->t);
    FieldMultiply(scratch->c, scratch->c, edD2);
    FieldMultiply(scratch->d, left->z, right->z);
    FieldAdd(scratch->d, scratch->d, scratch->d);
    /* E = B - A, F = D - C, G = D + C, H = B + A. Разряды всех четырёх
     * меньше 2^54, как требует умножение. */
    FieldSubtract(scratch->e, scratch->b, scratch->a);
    FieldSubtract(scratch->f, scratch->d, scratch->c);
    FieldAdd(scratch->g, scratch->d, scratch->c);
    FieldAdd(scratch->h, scratch->b, scratch->a);
    FieldMultiply(output->x, scratch->e, scratch->f);
    FieldMultiply(output->y, scratch->g, scratch->h);
    FieldMultiply(output->t, scratch->e, scratch->h);
    FieldMultiply(output->z, scratch->f, scratch->g);
}

/* Удвоение с a = -1. Формула даёт F и H; здесь считаются -F и -H, и все
 * четыре координаты выходят с обратным знаком — та же точка. Вычитаемое
 * каждый раз почти приведено. output может совпадать с point. */
static void EdDouble(EdPoint *output, const EdPoint *point, EdScratch *scratch)
{
    FieldSquare(scratch->a, point->x);
    FieldSquare(scratch->b, point->y);
    FieldSquare(scratch->c, point->z);
    FieldAdd(scratch->c, scratch->c, scratch->c);
    FieldAdd(scratch->d, point->x, point->y);
    FieldSquare(scratch->d, scratch->d);
    /* -H = X^2 + Y^2, G = Y^2 - X^2, -F = 2Z^2 - G, E = (X + Y)^2 - X^2 - Y^2. */
    FieldAdd(scratch->h, scratch->a, scratch->b);
    FieldSubtract(scratch->g, scratch->b, scratch->a);
    FieldSubtract(scratch->f, scratch->c, scratch->b);
    FieldAdd(scratch->f, scratch->f, scratch->a);
    FieldSubtract(scratch->e, scratch->d, scratch->a);
    FieldSubtract(scratch->e, scratch->e, scratch->b);
    FieldMultiply(output->x, scratch->e, scratch->f);
    FieldMultiply(output->y, scratch->g, scratch->h);
    FieldMultiply(output->t, scratch->e, scratch->h);
    FieldMultiply(output->z, scratch->f, scratch->g);
}

/* -(x, y) = (-x, y): меняют знак X и T. */
static void EdNegate(EdPoint *point)
{
    FieldNegate(point->x, point->x);
    FieldNegate(point->t, point->t);
}

/* Обмен без ветвления: swap равен 0 или 1. */
static void EdSwap(EdPoint *left, EdPoint *right, uint64_t swap)
{
    FieldSwap(left->x, right->x, swap);
    FieldSwap(left->y, right->y, swap);
    FieldSwap(left->z, right->z, swap);
    FieldSwap(left->t, right->t, swap);
}

/* [scalar]point для 32 байт little-endian, все 256 бит. Лестница Монтгомери:
 * на каждом бите одно сложение и одно удвоение, а точки меняются местами
 * без ветвлений по битам скаляра. */
static void EdMultiply(EdPoint *output, const uint8_t scalar[CRYPTO_SCALAR_SIZE],
                       const EdPoint *point)
{
    EdPoint low = edIdentity;
    EdPoint high = *point;
    EdScratch scratch;
    uint64_t swap = 0u;
    /* Всегда high = low + point. */
    for (int32_t bit = 255; bit >= 0; --bit)
    {
        const uint64_t scalarBit = (uint64_t)(scalar[bit >> 3] >> (bit & 7)) & 1u;
        swap ^= scalarBit;
        EdSwap(&low, &high, swap);
        swap = scalarBit;
        EdAdd(&high, &low, &high, &scratch);
        EdDouble(&low, &low, &scratch);
    }
    EdSwap(&low, &high, swap);
    *output = low;
    CryptoWipe(&low, sizeof(low));
    CryptoWipe(&high, sizeof(high));
    CryptoWipe(&scratch, sizeof(scratch));
}

/* RFC 8032, 5.1.2: y и знак x в старшем бите. */
static void EdEncode(uint8_t bytes[FIELD_BYTES], const EdPoint *point)
{
    Field inverse;
    Field x;
    Field y;
    FieldInvert(inverse, point->z);
    FieldMultiply(x, point->x, inverse);
    FieldMultiply(y, point->y, inverse);
    FieldPack(bytes, y);
    bytes[31] |= (uint8_t)(FieldIsNegative(x) << 7u);
    CryptoWipe(inverse, sizeof(Field));
    CryptoWipe(x, sizeof(Field));
    CryptoWipe(y, sizeof(Field));
}

/* Строгое чтение точки (RFC 8032, 5.1.3): y < p, точка лежит на кривой, а
 * у x = 0 нет отрицательной записи. Данные открытые, поэтому ветвления
 * допустимы. */
static bool EdDecode(EdPoint *output, const uint8_t bytes[FIELD_BYTES])
{
    uint8_t canonical[FIELD_BYTES];
    FieldUnpack(output->y, bytes);
    FieldPack(canonical, output->y);
    canonical[31] |= (uint8_t)(bytes[31] & 0x80u);
    if (!BytesEqual(canonical, bytes, FIELD_BYTES))
        return false;
    const uint8_t sign = (uint8_t)(bytes[31] >> 7u);

    /* x^2 = u / v, u = y^2 - 1, v = d y^2 + 1. Кандидат в корень —
     * u v^3 (u v^7)^((p - 5) / 8). */
    Field u;
    Field v;
    Field v3;
    Field x;
    Field check;
    FieldSquare(u, output->y);
    FieldMultiply(v, u, edD);
    FieldSubtract(u, u, fieldOne);
    FieldCarry(u);
    FieldAdd(v, v, fieldOne);
    FieldSquare(v3, v);
    FieldMultiply(v3, v3, v);
    FieldSquare(x, v3);
    FieldMultiply(x, x, v);
    FieldMultiply(x, x, u);
    FieldPower22523(x, x);
    FieldMultiply(x, x, v3);
    FieldMultiply(x, x, u);

    /* v x^2 = u — корень найден; v x^2 = -u — его даёт x * sqrt(-1); иначе
     * u / v не квадрат, и точки с таким y нет. */
    FieldSquare(check, x);
    FieldMultiply(check, check, v);
    if (!FieldEqual(check, u))
    {
        Field negated;
        FieldNegate(negated, u);
        if (!FieldEqual(check, negated))
            return false;
        FieldMultiply(x, x, fieldSqrtMinusOne);
    }
    if (FieldIsZero(x) && sign != 0u)
        return false;
    if (FieldIsNegative(x) != sign)
        FieldNegate(x, x);
    memcpy(output->x, x, sizeof(Field));
    memcpy(output->z, fieldOne, sizeof(Field));
    FieldMultiply(output->t, x, output->y);
    return true;
}

/* Точка малого порядка обнуляет x после умножения на кофактор 8: у
 * кратного [8]P порядок делит простое L, и x = 0 только у нейтральной. */
static bool EdIsSmallOrder(const EdPoint *point)
{
    EdPoint multiple;
    EdScratch scratch;
    EdDouble(&multiple, point, &scratch);
    EdDouble(&multiple, &multiple, &scratch);
    EdDouble(&multiple, &multiple, &scratch);
    return FieldIsZero(multiple.x);
}

/* ---- Ed25519 ------------------------------------------------------------------ */

/* RFC 8032, 5.1.5: SHA-512 от seed — скаляр a с зажатыми битами и префикс
 * для nonce. */
static void Ed25519Expand(const uint8_t seed[CRYPTO_ED25519_SEED_SIZE],
                          uint8_t scalar[CRYPTO_SCALAR_SIZE], uint8_t prefix[CRYPTO_SCALAR_SIZE])
{
    uint8_t digest[CRYPTO_SHA512_SIZE];
    CryptoSha512Hash(seed, CRYPTO_ED25519_SEED_SIZE, digest);
    memcpy(scalar, digest, CRYPTO_SCALAR_SIZE);
    scalar[0] &= 248u;
    scalar[31] = (uint8_t)((scalar[31] & 127u) | 64u);
    memcpy(prefix, digest + CRYPTO_SCALAR_SIZE, CRYPTO_SCALAR_SIZE);
    CryptoWipe(digest, sizeof(digest));
}

void CryptoEd25519PublicKey(const uint8_t seed[CRYPTO_ED25519_SEED_SIZE],
                            uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE])
{
    uint8_t scalar[CRYPTO_SCALAR_SIZE];
    uint8_t prefix[CRYPTO_SCALAR_SIZE];
    EdPoint point;
    Ed25519Expand(seed, scalar, prefix);
    EdMultiply(&point, scalar, &edBase);
    EdEncode(publicKey, &point);
    CryptoWipe(scalar, sizeof(scalar));
    CryptoWipe(prefix, sizeof(prefix));
    CryptoWipe(&point, sizeof(point));
}

bool CryptoEd25519PublicKeyIsValid(const uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE])
{
    EdPoint key;
    return EdDecode(&key, publicKey) && !EdIsSmallOrder(&key);
}

void CryptoEd25519Sign(const uint8_t seed[CRYPTO_ED25519_SEED_SIZE], const uint8_t *message,
                       size_t size, uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE])
{
    uint8_t scalar[CRYPTO_SCALAR_SIZE];
    uint8_t prefix[CRYPTO_SCALAR_SIZE];
    uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
    uint8_t digest[CRYPTO_SHA512_SIZE];
    uint8_t nonce[CRYPTO_SCALAR_SIZE];
    uint8_t challenge[CRYPTO_SCALAR_SIZE];
    uint8_t encoded[FIELD_BYTES];
    uint8_t response[CRYPTO_SCALAR_SIZE];
    EdPoint point;
    CryptoSha512 hash;
    Ed25519Expand(seed, scalar, prefix);
    EdMultiply(&point, scalar, &edBase);
    EdEncode(publicKey, &point);

    /* r = SHA-512(prefix || M) mod L, R = [r]B. */
    CryptoSha512Begin(&hash);
    CryptoSha512Update(&hash, prefix, sizeof(prefix));
    CryptoSha512Update(&hash, message, size);
    CryptoSha512Finish(&hash, digest);
    CryptoScalarReduce(nonce, digest);
    EdMultiply(&point, nonce, &edBase);
    EdEncode(encoded, &point);

    /* k = SHA-512(R || A || M) mod L, S = (r + k a) mod L. Подпись пишется
     * последней: буфер может перекрывать сообщение. */
    CryptoSha512Begin(&hash);
    CryptoSha512Update(&hash, encoded, sizeof(encoded));
    CryptoSha512Update(&hash, publicKey, sizeof(publicKey));
    CryptoSha512Update(&hash, message, size);
    CryptoSha512Finish(&hash, digest);
    CryptoScalarReduce(challenge, digest);
    CryptoScalarMultiplyAdd(response, challenge, scalar, nonce);
    memcpy(signature, encoded, sizeof(encoded));
    memcpy(signature + FIELD_BYTES, response, sizeof(response));

    CryptoWipe(scalar, sizeof(scalar));
    CryptoWipe(prefix, sizeof(prefix));
    CryptoWipe(digest, sizeof(digest));
    CryptoWipe(nonce, sizeof(nonce));
    CryptoWipe(response, sizeof(response));
    CryptoWipe(&point, sizeof(point));
}

bool CryptoEd25519Verify(const uint8_t publicKey[CRYPTO_ED25519_PUBLIC_KEY_SIZE],
                         const uint8_t *message, size_t size,
                         const uint8_t signature[CRYPTO_ED25519_SIGNATURE_SIZE])
{
    const uint8_t *response = signature + FIELD_BYTES;
    EdPoint key;
    EdPoint commitment;
    if (!CryptoScalarIsCanonical(response) || !EdDecode(&key, publicKey) || EdIsSmallOrder(&key))
        return false;
    if (!EdDecode(&commitment, signature) || EdIsSmallOrder(&commitment))
        return false;

    uint8_t digest[CRYPTO_SHA512_SIZE];
    uint8_t challenge[CRYPTO_SCALAR_SIZE];
    CryptoSha512 hash;
    CryptoSha512Begin(&hash);
    CryptoSha512Update(&hash, signature, FIELD_BYTES);
    CryptoSha512Update(&hash, publicKey, CRYPTO_ED25519_PUBLIC_KEY_SIZE);
    CryptoSha512Update(&hash, message, size);
    CryptoSha512Finish(&hash, digest);
    CryptoScalarReduce(challenge, digest);

    /* [S]B - [k]A сравнивается с R побайтово: R уже проверена на
     * каноничность, так что это равенство точек. */
    EdPoint check;
    EdPoint product;
    EdScratch scratch;
    uint8_t encoded[FIELD_BYTES];
    EdMultiply(&check, response, &edBase);
    EdMultiply(&product, challenge, &key);
    EdNegate(&product);
    EdAdd(&check, &check, &product, &scratch);
    EdEncode(encoded, &check);
    return BytesEqual(encoded, signature, FIELD_BYTES);
}
