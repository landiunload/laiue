# Generates tests/r2_26_inflate_fixtures.h: zlib streams made of fixed-Huffman
# blocks whose contents are fully controlled, so every (distance, length)
# backreference case can be checked. Run manually, like tests/inflate_fixtures.py.
#
# The streams deliberately exercise CopyMatch: distance 1 (RLE), distances 2..7
# (periodic doubling), distance 8 and other boundaries, and the maximum match
# length 258. Expected output is computed here by the plain byte-by-byte rule,
# independently of the C decoder. Every stream is also verified with
# zlib.decompress before it is written.
import os
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, 'r2_26_inflate_fixtures.h')

LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
               51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
                4, 4, 5, 5, 5, 5, 0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
             513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
              10, 11, 11, 12, 12, 13, 13]


class Bits:
    def __init__(self):
        self.bits = []

    def lsb(self, value, count):
        for i in range(count):
            self.bits.append((value >> i) & 1)

    def msb(self, value, count):
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def bytes(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for j in range(8):
                byte |= self.bits[i + j] << j
            out.append(byte)
        return bytes(out)


def fixed_code(symbol):
    if symbol <= 143:
        return 0x30 + symbol, 8
    if symbol <= 255:
        return 0x190 + (symbol - 144), 9
    if symbol <= 279:
        return symbol - 256, 7
    return 0xC0 + (symbol - 280), 8


def emit_symbol(bits, symbol):
    code, n = fixed_code(symbol)
    bits.msb(code, n)


def emit_match(bits, distance, length):
    for idx, base in enumerate(LENGTH_BASE):
        extra = LENGTH_EXTRA[idx]
        if length < base + (1 << extra) or idx == len(LENGTH_BASE) - 1:
            emit_symbol(bits, 257 + idx)
            bits.lsb(length - base, extra)
            break
    for idx, base in enumerate(DIST_BASE):
        extra = DIST_EXTRA[idx]
        if distance < base + (1 << extra) or idx == len(DIST_BASE) - 1:
            # В fixed-блоках у дистанций собственная таблица: 5-битные коды.
            bits.msb(idx, 5)
            bits.lsb(distance - base, extra)
            break


def build(prefix, script):
    """prefix: bytes to emit as literals. script: list of (distance, length)."""
    bits = Bits()
    bits.lsb(1, 1)  # BFINAL
    bits.lsb(1, 2)  # BTYPE = 01 fixed
    raw = bytearray(prefix)
    for byte in prefix:
        emit_symbol(bits, byte)
    for distance, length in script:
        assert distance <= len(raw), (distance, len(raw))
        emit_match(bits, distance, length)
        for _ in range(length):
            raw.append(raw[len(raw) - distance])
    emit_symbol(bits, 256)  # end of block
    deflate = bits.bytes()
    stream = bytes([0x78, 0x01]) + deflate + \
        (zlib.adler32(bytes(raw)) & 0xFFFFFFFF).to_bytes(4, 'big')
    assert zlib.decompress(stream) == bytes(raw)
    return stream, bytes(raw), bytes(deflate)


def lcg_bytes(count, seed):
    out = bytearray()
    state = seed
    for _ in range(count):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((state >> 16) & 0xFF)
    return bytes(out)


def fmt(name, data):
    lines = ['static const uint8_t %s[%d] = {' % (name, len(data))]
    row = []
    for i, b in enumerate(data):
        row.append('%4du,' % b)
        if len(row) == 16:
            lines.append('    ' + ' '.join(row))
            row = []
    if row:
        lines.append('    ' + ' '.join(row))
    lines.append('};')
    return '\n'.join(lines)


def main():
    streams = []

    # Stream A: random literals, then matches across every distance boundary.
    prefix_a = lcg_bytes(1200, 12345)
    script_a = [
        (1, 258), (1, 3), (2, 258), (2, 7), (3, 258), (3, 8), (4, 17), (5, 258),
        (6, 9), (7, 258), (8, 258), (9, 255), (15, 16), (16, 258), (17, 3), (31, 258),
        (32, 258), (64, 100), (128, 258), (257, 258), (300, 258), (512, 258), (1024, 258),
        (1, 4), (3, 5), (5, 6), (7, 7), (2, 8),
    ]
    stream_a, raw_a, _ = build(prefix_a, script_a)
    streams.append(('R2_INFLATE_MATCH_A_ZLIB', stream_a, 'R2_INFLATE_MATCH_A_RAW', raw_a))

    # Stream B: distance-1 RLE across every length regime.
    prefix_b = lcg_bytes(8, 777)
    lengths_b = [3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128,
                 129, 200, 255, 256, 257, 258]
    script_b = [(1, length) for length in lengths_b]
    stream_b, raw_b, _ = build(prefix_b, script_b)
    streams.append(('R2_INFLATE_MATCH_B_ZLIB', stream_b, 'R2_INFLATE_MATCH_B_RAW', raw_b))

    # Stream C: small distances 2..7 with lengths that are not multiples.
    prefix_c = lcg_bytes(64, 4242)
    lengths_c = [3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 23, 24, 25, 31, 32, 33, 39]
    script_c = [(distance, length) for distance in range(2, 8) for length in lengths_c]
    stream_c, raw_c, _ = build(prefix_c, script_c)
    streams.append(('R2_INFLATE_MATCH_C_ZLIB', stream_c, 'R2_INFLATE_MATCH_C_RAW', raw_c))

    body = []
    body.append('// Сгенерировано tests/r2_26_inflate_fixtures.py. zlib-потоки')
    body.append('// из fixed-Huffman блоков с управляемыми backreference:')
    body.append('// distance 1..7, границы 8/16/32, большие distance и длина 258.')
    body.append('// Ожидаемый выход посчитан независимо побайтовым правилом.')
    body.append('#pragma once')
    body.append('')
    body.append('#include <stdint.h>')
    body.append('')
    for zname, zdata, rname, rdata in streams:
        body.append(fmt(zname, zdata))
        body.append('')
        body.append(fmt(rname, rdata))
        body.append('')
    body.append('typedef struct R2InflateFixture')
    body.append('{')
    body.append('    const uint8_t *stream;')
    body.append('    uint32_t streamBytes;')
    body.append('    const uint8_t *raw;')
    body.append('    uint32_t rawBytes;')
    body.append('} R2InflateFixture;')
    body.append('')
    body.append('static const R2InflateFixture R2_INFLATE_FIXTURES[] = {')
    for zname, zdata, rname, rdata in streams:
        body.append('    {%s, %d, %s, %d},' % (zname, len(zdata), rname, len(rdata)))
    body.append('};')
    body.append('')
    with open(TARGET, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(body) + '\n')
    for zname, zdata, rname, rdata in streams:
        print('%-28s zlib=%d raw=%d' % (zname, len(zdata), len(rdata)))
    print('wrote', TARGET, os.path.getsize(TARGET))


if __name__ == '__main__':
    main()
