# Готовит tests/crypto_fixtures.h: хеши SHA-512, приведение скаляров по
# модулю L, подписи Ed25519 и подписи, которые строгая проверка обязана
# отвергнуть. Запускается вручную, как tests/inflate_fixtures.py; заголовок
# с результатом лежит рядом в дереве.
#
# Эталон Ed25519 использует аффинные координаты и Python int, хеш — hashlib.
# Случайные данные выводятся из SHA-512 счётчика, поэтому выход не меняется
# от запуска к запуску.
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, 'crypto_fixtures.h')

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, -1, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)
MESSAGE_CAPACITY = 300


def sha512(data):
    return hashlib.sha512(data).digest()


def stream(label, size):
    """Детерминированные байты: SHA-512 метки и счётчика."""
    out = b''
    counter = 0
    while len(out) < size:
        out += sha512(b'laiue crypto fixtures/%s/%d' % (label.encode(), counter))
        counter += 1
    return out[:size]


# ---- Эталон Ed25519 в аффинных координатах ---------------------------------


IDENTITY = (0, 1)


def point_add(left, right):
    x1, y1 = left
    x2, y2 = right
    dxy = D * x1 * x2 * y1 * y2 % P
    x = (x1 * y2 + y1 * x2) * pow(1 + dxy, -1, P) % P
    y = (y1 * y2 + x1 * x2) * pow(1 - dxy, -1, P) % P
    return x, y


def point_mul(scalar, point):
    result = IDENTITY
    for bit in bin(scalar)[2:]:
        result = point_add(result, result)
        if bit == '1':
            result = point_add(result, point)
    return result


def point_neg(point):
    x, y = point
    return -x % P, y


def recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, -1, P) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


G_Y = 4 * pow(5, -1, P) % P
G = (recover_x(G_Y, 0), G_Y)


def compress(point):
    x, y = point
    return (y | ((x & 1) << 255)).to_bytes(32, 'little')


def decompress(s):
    y = int.from_bytes(s, 'little')
    sign = y >> 255
    y &= (1 << 255) - 1
    x = recover_x(y, sign)
    if x is None:
        return None
    return x, y


def expand(seed):
    hashed = bytearray(sha512(seed))
    hashed[0] &= 0xF8
    hashed[31] = (hashed[31] & 0x7F) | 0x40
    return int.from_bytes(hashed[:32], 'little'), bytes(hashed[32:])


def public_key(seed):
    return compress(point_mul(expand(seed)[0], G))


def sign(seed, message):
    a, prefix = expand(seed)
    key = compress(point_mul(a, G))
    r = int.from_bytes(sha512(prefix + message), 'little') % L
    commitment = compress(point_mul(r, G))
    k = int.from_bytes(sha512(commitment + key + message), 'little') % L
    return commitment + int.to_bytes((r + k * a) % L, 32, 'little')


def equation_holds(key, message, signature):
    """Нестрогая проверка: только [S]B = R + [k]A, без запретов на S >= L и
    точки малого порядка."""
    a = decompress(key)
    r = decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], 'little')
    k = int.from_bytes(sha512(signature[:32] + key + message), 'little') % L
    return point_mul(s, G) == point_add(r, point_mul(k, a))


def is_small_order(point):
    return point_mul(8, point) == IDENTITY


def strict_verify(key, message, signature):
    s = int.from_bytes(signature[32:], 'little')
    a = decompress(key)
    r = decompress(signature[:32])
    if s >= L or a is None or r is None:
        return False
    if is_small_order(a) or is_small_order(r):
        return False
    return equation_holds(key, message, signature)


# ---- Форматирование --------------------------------------------------------


def c_bytes(data, indent):
    items = ['0x%02xu' % b for b in data]
    lines = []
    for start in range(0, len(items), 12):
        lines.append(indent + ', '.join(items[start:start + 12]) + ',')
    return '\n'.join(lines)


def c_array(data, indent):
    if not data:
        return '{0u}'
    return '{\n' + c_bytes(data, indent + '    ') + '\n' + indent + '}'


def le(value, size):
    return int.to_bytes(value, size, 'little')


# ---- SHA-512 ---------------------------------------------------------------


def sha512_pattern(size):
    return bytes((index * 167 + 13) & 0xFF for index in range(size))


SHA512_SIZES = [0, 1, 2, 3, 55, 56, 63, 64, 111, 112, 113, 119, 120, 127, 128, 129,
                200, 239, 240, 255, 256, 257, 383, 384, 1000, 4096]

# FIPS 180-4, примеры из NIST CSVP: "abc", сообщение из 896 бит и миллион
# букв "a".
FIPS_ABC = ('ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a'
            '2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f')
FIPS_896 = ('8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018'
            '501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909')
FIPS_MILLION_A = ('e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb'
                  'de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b')
assert sha512(b'abc').hex() == FIPS_ABC
assert sha512(b'a' * 1000000).hex() == FIPS_MILLION_A
assert sha512(b'abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn'
              b'hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu').hex() == FIPS_896

# ---- Скаляры ---------------------------------------------------------------


def barrett_subtractions(value):
    """Сколько вычитаний L понадобится после оценки частного (HAC 14.42).
    Код делает одно: оценка ошибается меньше чем на 0,23."""
    mu = (1 << 512) // L
    q3 = ((value >> 192) * mu) >> 320
    r = (value - q3 * L) % (1 << 320)
    count = 0
    while r >= L:
        r -= L
        count += 1
    assert count <= 1
    return count


def reduce_inputs():
    values = [0, 1, L - 1, L, L + 1, 2 * L - 1, 2 * L, 2 * L + 1, 3 * L - 1, 3 * L,
              2 ** 252, 2 ** 253 - 1, 2 ** 255, 2 ** 256 - 1, 2 ** 256,
              2 ** 320 - 1, 2 ** 320, 2 ** 448 - 1, 2 ** 511, 2 ** 512 - 1]
    top = (2 ** 512 - 1) // L
    values += [top * L, top * L - 1]
    # Разряды из одних единиц и одних нулей вперемешку.
    for pattern in range(8):
        value = 0
        for limb in range(8):
            if (pattern >> (limb % 3)) & 1:
                value |= (2 ** 64 - 1) << (64 * limb)
        values.append(value)
    # Случайные частные с остатками у краёв: малому остатку при большом
    # value нужно вычитание L, остатку у самой L — нет.
    found = {0: 0, 1: 0}
    counter = 0
    while min(found.values()) < 4:
        quotient = int.from_bytes(stream('quotient/%d' % counter, 33), 'little') % (top + 1)
        remainder = int.from_bytes(stream('remainder/%d' % counter, 32), 'little') % L
        remainder = remainder >> 8 if counter % 2 else L - 1 - (remainder >> 8)
        value = quotient * L + remainder
        counter += 1
        if value >= 2 ** 512:
            continue
        count = barrett_subtractions(value)
        if found[count] < 4:
            found[count] += 1
            values.append(value)
    for value in values:
        barrett_subtractions(value)
    for index in range(12):
        values.append(int.from_bytes(stream('reduce/%d' % index, 64), 'little'))
    assert all(0 <= value < 2 ** 512 for value in values)
    return values


def multiply_add_inputs():
    top = 2 ** 256 - 1
    cases = [(0, 0, 0), (1, 1, 0), (L - 1, L - 1, L - 1), (top, top, top), (top, 1, 0),
             (1, top, L), (L, L, L), (2 ** 255, 2 ** 255, 2 ** 255 - 1)]
    for index in range(12):
        data = stream('multiply_add/%d' % index, 96)
        cases.append(tuple(int.from_bytes(data[32 * part:32 * part + 32], 'little')
                           for part in range(3)))
    return cases


# ---- Ed25519 ---------------------------------------------------------------

# RFC 8032, 7.1: TEST 1, 2, 3 и SHA(abc).
RFC_VECTORS = [
    ('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
     'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a',
     '',
     'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155'
     '5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'),
    ('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb',
     '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c',
     '72',
     '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da'
     '085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00'),
    ('c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7',
     'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025',
     'af82',
     '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac'
     '18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a'),
    ('833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42',
     'ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf',
     sha512(b'abc').hex(),
     'dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589'
     '09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704'),
]

RANDOM_SIZES = [0, 1, 31, 47, 48, 49, 63, 64, 65, 79, 80, 81, 95, 96, 97, 111, 112, 127,
                128, 200, 300]


def ed25519_cases():
    cases = []
    for seed_hex, key_hex, message_hex, signature_hex in RFC_VECTORS:
        seed = bytes.fromhex(seed_hex)
        message = bytes.fromhex(message_hex)
        key = public_key(seed)
        signature = sign(seed, message)
        assert key.hex() == key_hex and signature.hex() == signature_hex
        cases.append((seed, key, message, signature))
    for index, size in enumerate(RANDOM_SIZES):
        seed = stream('seed/%d' % index, 32)
        message = stream('message/%d' % index, size)
        key = public_key(seed)
        signature = sign(seed, message)
        assert strict_verify(key, message, signature)
        cases.append((seed, key, message, signature))
    assert all(len(case[2]) <= MESSAGE_CAPACITY for case in cases)
    return cases


def torsion_points():
    """Все восемь точек малого порядка: кратные точки порядка 8."""
    y = 2
    while True:
        point = decompress(le(y, 32))
        y += 1
        if point is None:
            continue
        torsion = point_mul(L, point)
        if point_mul(4, torsion) != IDENTITY:
            break
    points = [point_mul(index, torsion) for index in range(8)]
    encodings = [compress(point) for point in points]
    assert len(set(encodings)) == 8
    return points, encodings


def order_of(point):
    for order in (1, 2, 4, 8):
        if point_mul(order, point) == IDENTITY:
            return order
    raise AssertionError('not a small-order point')


def forge_small_order_key(point, encoding, index):
    """Подпись под ключом малого порядка, верная по нестрогому уравнению:
    [k]A зависит лишь от k по модулю порядка A, поэтому R = [s]B - [j]A
    подходит, когда k mod order = j."""
    order = order_of(point)
    for attempt in range(1000):
        message = b'small order key %d, attempt %d' % (index, attempt)
        s = int.from_bytes(sha512(b'response' + message), 'little') % L
        for guess in range(order):
            commitment = point_add(point_mul(s, G), point_neg(point_mul(guess, point)))
            commitment_bytes = compress(commitment)
            k = int.from_bytes(sha512(commitment_bytes + encoding + message), 'little') % L
            if k % order == guess:
                signature = commitment_bytes + le(s, 32)
                assert equation_holds(encoding, message, signature)
                assert not strict_verify(encoding, message, signature)
                return encoding, message, signature
    raise AssertionError('no forgery found')


def forgeries():
    """Подписи, верные по уравнению [S]B = R + [k]A, но запрещённые строгими
    правилами: ключ малого порядка, R малого порядка и S >= L."""
    result = []
    comments = []
    points, encodings = torsion_points()
    for index, (point, encoding) in enumerate(zip(points, encodings)):
        result.append(forge_small_order_key(point, encoding, index))
        comments.append('ключ малого порядка %d' % order_of(point))

    # R — нейтральная точка: [S]B = [k]A при S = k a.
    seed = stream('identity commitment', 32)
    a, _ = expand(seed)
    key = public_key(seed)
    message = b'identity commitment'
    commitment = compress(IDENTITY)
    k = int.from_bytes(sha512(commitment + key + message), 'little') % L
    signature = commitment + le(k * a % L, 32)
    assert equation_holds(key, message, signature) and not strict_verify(key, message, signature)
    result.append((key, message, signature))
    comments.append('R — нейтральная точка')

    # S + mL для верной подписи RFC 8032 TEST 1: та же точка [S]B.
    seed = bytes.fromhex(RFC_VECTORS[0][0])
    key = public_key(seed)
    signature = sign(seed, b'')
    s = int.from_bytes(signature[32:], 'little')
    top = (2 ** 256 - 1 - s) // L
    for multiple in (1, 2, top):
        spoiled = signature[:32] + le(s + multiple * L, 32)
        assert equation_holds(key, b'', spoiled) and not strict_verify(key, b'', spoiled)
        result.append((key, b'', spoiled))
        comments.append('S + L' if multiple == 1 else 'S + %dL' % multiple)
    return result, comments


def invalid_keys():
    """Ключи, которые строгая проверка отвергает до уравнения."""
    keys = []
    comments = []
    _, encodings = torsion_points()
    for encoding in encodings:
        keys.append(encoding)
        comments.append('малый порядок')
    # Неканонические y = p + small и «отрицательный ноль» x = 0.
    for small in range(0, 19):
        encoding = le(P + small, 32)
        keys.append(encoding)
        comments.append('y = p + %d' % small)
    keys.append(le(1 | (1 << 255), 32))
    comments.append('x = 0 со знаком, y = 1')
    keys.append(le((P - 1) | (1 << 255), 32))
    comments.append('x = 0 со знаком, y = -1')
    # y, для которых x^2 = (y^2 - 1) / (d y^2 + 1) не квадрат.
    y = 2
    off_curve = 0
    while off_curve < 4:
        if recover_x(y, 0) is None:
            keys.append(le(y, 32))
            comments.append('y = %d не на кривой' % y)
            keys.append(le(y | (1 << 255), 32))
            comments.append('y = %d не на кривой, со знаком' % y)
            off_curve += 1
        y += 1
    for key in keys:
        point = decompress(key)
        assert point is None or is_small_order(point)
    return keys, comments


def valid_keys():
    """Ключи, которые проверка принимает: обычные, с малым y и смешанного
    порядка. Ключ смешанного порядка — сумма обычного и точки порядка 8.
    Проверка записи ключа отвергает только точки малого порядка."""
    keys = [public_key(bytes.fromhex(vector[0])) for vector in RFC_VECTORS]
    comments = ['RFC 8032'] * len(keys)
    for small in range(2, 19):
        point = decompress(le(small, 32))
        if point is not None and not is_small_order(point):
            keys.append(le(small, 32))
            comments.append('y = %d' % small)
            break
    points, _ = torsion_points()
    mixed = point_add(decompress(keys[0]), points[1])
    keys.append(compress(mixed))
    comments.append('смешанный порядок')
    for key in keys:
        point = decompress(key)
        assert point is not None and not is_small_order(point)
    return keys, comments


# ---- Заголовок -------------------------------------------------------------


def main():
    out = []
    out.append('// Сгенерировано tests/crypto_fixtures.py. Хеши SHA-512, приведение')
    out.append('// скаляров по модулю L и подписи Ed25519 по RFC 8032,')
    out.append('// включая векторы раздела 7.1 и подписи, которые строгая проверка')
    out.append('// обязана отвергнуть.')
    out.append('#pragma once')
    out.append('')
    out.append('#include <stdint.h>')
    out.append('')
    out.append('#define CRYPTO_FIXTURE_MESSAGE_CAPACITY %du' % MESSAGE_CAPACITY)
    out.append('')

    out.append('/* FIPS 180-4: \"abc\", сообщение из 896 бит и миллион букв \"a\". */')
    out.append('static const uint8_t sha512Abc[64] = %s;' % c_array(bytes.fromhex(FIPS_ABC), ''))
    out.append('static const uint8_t sha512Fips896[64] = %s;' %
               c_array(bytes.fromhex(FIPS_896), ''))
    out.append('static const uint8_t sha512MillionA[64] = %s;' %
               c_array(bytes.fromhex(FIPS_MILLION_A), ''))
    out.append('')

    out.append('/* SHA-512 сообщения message[i] = (uint8_t)(i * 167 + 13). */')
    out.append('typedef struct Sha512Fixture')
    out.append('{')
    out.append('    uint32_t size;')
    out.append('    uint8_t digest[64];')
    out.append('} Sha512Fixture;')
    out.append('')
    out.append('static const Sha512Fixture sha512Fixtures[%d] = {' % len(SHA512_SIZES))
    for size in SHA512_SIZES:
        out.append('    {%du, %s},' % (size, c_array(sha512(sha512_pattern(size)), '    ')))
    out.append('};')
    out.append('')

    reduce_values = reduce_inputs()
    out.append('/* value mod L: края, разряды из единиц и остатки, которым после оценки')
    out.append(' * частного нужно ноль или одно вычитание L. */')
    out.append('typedef struct ScalarReduceFixture')
    out.append('{')
    out.append('    uint8_t value[64];')
    out.append('    uint8_t reduced[32];')
    out.append('} ScalarReduceFixture;')
    out.append('')
    out.append('static const ScalarReduceFixture scalarReduceFixtures[%d] = {' % len(reduce_values))
    for value in reduce_values:
        out.append('    {%s,' % c_array(le(value, 64), '     '))
        out.append('     %s},' % c_array(le(value % L, 32), '     '))
    out.append('};')
    out.append('')

    multiply_cases = multiply_add_inputs()
    out.append('/* (left * right + addend) mod L. */')
    out.append('typedef struct ScalarMultiplyAddFixture')
    out.append('{')
    out.append('    uint8_t left[32];')
    out.append('    uint8_t right[32];')
    out.append('    uint8_t addend[32];')
    out.append('    uint8_t result[32];')
    out.append('} ScalarMultiplyAddFixture;')
    out.append('')
    out.append('static const ScalarMultiplyAddFixture scalarMultiplyAddFixtures[%d] = {'
               % len(multiply_cases))
    for left, right, addend in multiply_cases:
        out.append('    {%s,' % c_array(le(left, 32), '     '))
        out.append('     %s,' % c_array(le(right, 32), '     '))
        out.append('     %s,' % c_array(le(addend, 32), '     '))
        out.append('     %s},' % c_array(le((left * right + addend) % L, 32), '     '))
    out.append('};')
    out.append('')

    cases = ed25519_cases()
    out.append('/* Первые %d — RFC 8032, 7.1 (TEST 1, 2, 3 и SHA(abc)), остальные —' %
               len(RFC_VECTORS))
    out.append(' * случайные seed и сообщения у границ блоков SHA-512. */')
    out.append('typedef struct Ed25519Fixture')
    out.append('{')
    out.append('    uint8_t seed[32];')
    out.append('    uint8_t publicKey[32];')
    out.append('    uint32_t size;')
    out.append('    uint8_t message[CRYPTO_FIXTURE_MESSAGE_CAPACITY];')
    out.append('    uint8_t signature[64];')
    out.append('} Ed25519Fixture;')
    out.append('')
    out.append('static const Ed25519Fixture ed25519Fixtures[%d] = {' % len(cases))
    for seed, key, message, signature in cases:
        out.append('    {%s,' % c_array(seed, '     '))
        out.append('     %s,' % c_array(key, '     '))
        out.append('     %du,' % len(message))
        out.append('     %s,' % c_array(message, '     '))
        out.append('     %s},' % c_array(signature, '     '))
    out.append('};')
    out.append('')

    forged, forged_comments = forgeries()
    out.append('/* Верны по уравнению [S]B = R + [k]A, но строгая проверка их отвергает. */')
    out.append('typedef struct Ed25519Forgery')
    out.append('{')
    out.append('    uint8_t publicKey[32];')
    out.append('    uint32_t size;')
    out.append('    uint8_t message[CRYPTO_FIXTURE_MESSAGE_CAPACITY];')
    out.append('    uint8_t signature[64];')
    out.append('} Ed25519Forgery;')
    out.append('')
    out.append('static const Ed25519Forgery ed25519Forgeries[%d] = {' % len(forged))
    for (key, message, signature), comment in zip(forged, forged_comments):
        out.append('    /* %s */' % comment)
        out.append('    {%s,' % c_array(key, '     '))
        out.append('     %du,' % len(message))
        out.append('     %s,' % c_array(message, '     '))
        out.append('     %s},' % c_array(signature, '     '))
    out.append('};')
    out.append('')

    keys, key_comments = invalid_keys()
    out.append('/* Ключи, которые проверка отвергает до уравнения. */')
    out.append('static const uint8_t ed25519InvalidKeys[%d][32] = {' % len(keys))
    for key, comment in zip(keys, key_comments):
        out.append('    /* %s */' % comment)
        out.append('    %s,' % c_array(key, '    '))
    out.append('};')
    out.append('')

    keys, key_comments = valid_keys()
    out.append('/* Ключи, которые проверка принимает. */')
    out.append('static const uint8_t ed25519ValidKeys[%d][32] = {' % len(keys))
    for key, comment in zip(keys, key_comments):
        out.append('    /* %s */' % comment)
        out.append('    %s,' % c_array(key, '    '))
    out.append('};')

    with open(TARGET, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
