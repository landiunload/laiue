# Готовит tests/inflate_fixtures.h: zlib-потоки с известным распакованным
# содержимым и разными типами DEFLATE блоков. Запускается вручную, как
# tests/texc_fixtures.py; заголовок с результатом лежит рядом в дереве.
import os
import random
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, 'inflate_fixtures.h')


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


def manual_drain_stream(data):
    """Фиксированный блок с одним литералом, за которым сразу идёт
    несжатый блок. Так дозаправка битов читает вперёд в заголовок
    stored-блока, и его первые байты обязаны прийти из битового буфера."""
    bits = []

    def lsb(value, count):
        for i in range(count):
            bits.append((value >> i) & 1)

    def msb(value, count):
        for i in range(count - 1, -1, -1):
            bits.append((value >> i) & 1)

    lsb(0, 1)            # BFINAL = 0
    lsb(1, 2)            # BTYPE = 01 (fixed)
    msb(0x30 + 0x58, 8)  # литерал 'X'
    msb(0x00, 7)         # конец блока (символ 256)
    lsb(1, 1)            # BFINAL = 1
    lsb(0, 2)            # BTYPE = 00 (stored)
    while len(bits) % 8 != 0:
        bits.append(0)
    out = bytearray([0x78, 0x01])   # заголовок zlib, (0x7801 % 31) == 0
    for i in range(0, len(bits), 8):
        byte = 0
        for j in range(8):
            byte |= bits[i + j] << j
        out.append(byte)
    length = len(data)
    out.append(length & 0xFF)
    out.append((length >> 8) & 0xFF)
    out.append((~length) & 0xFF)
    out.append(((~length) >> 8) & 0xFF)
    out += data
    raw = b'X' + data
    out += (zlib.adler32(raw) & 0xFFFFFFFF).to_bytes(4, 'big')
    return bytes(out), raw


def main():
    rng = random.Random(20240921)

    stored_raw = bytes((i * 7 + 3) & 0xFF for i in range(1024))
    stored_z = zlib.compress(stored_raw, 0)

    fixed_raw = bytes((i * 3) % 17 for i in range(1024))
    fixed_c = zlib.compressobj(9, zlib.DEFLATED, 15, 8, zlib.Z_FIXED)
    fixed_z = fixed_c.compress(fixed_raw) + fixed_c.flush()

    drain_data = bytes(rng.randrange(256) for _ in range(1024))
    drain_z, drain_raw = manual_drain_stream(drain_data)

    prefix = b'A' * 512
    tail = bytes(rng.randrange(256) for _ in range(1024))
    mixed_raw = prefix + tail
    mixed_c = zlib.compressobj(6)
    mixed_z = mixed_c.compress(prefix) + mixed_c.flush(zlib.Z_SYNC_FLUSH) + \
        mixed_c.compress(tail) + mixed_c.flush()

    for name, stream, raw in (('stored', stored_z, stored_raw), ('fixed', fixed_z, fixed_raw),
                              ('drain', drain_z, drain_raw), ('mixed', mixed_z, mixed_raw)):
        assert zlib.decompress(stream) == raw, name
        print('%-8s zlib=%d raw=%d' % (name, len(stream), len(raw)))

    body = []
    body.append('// Сгенерировано tests/inflate_fixtures.py. Потоки zlib с')
    body.append('// известным распакованным содержимым: несжатый, фиксированный')
    body.append('// Хаффман, смешанный и поток с дозаправкой битов через границу')
    body.append('// несжатого блока. Проверяются InflateZlib и копирование')
    body.append('// несжатых блоков через границы отрезков IDAT.')
    body.append('#pragma once')
    body.append('')
    body.append('#include <stdint.h>')
    body.append('')
    pairs = (('INFLATE_STORED_ZLIB', stored_z, 'INFLATE_STORED_RAW', stored_raw),
             ('INFLATE_FIXED_ZLIB', fixed_z, 'INFLATE_FIXED_RAW', fixed_raw),
             ('INFLATE_DRAIN_ZLIB', drain_z, 'INFLATE_DRAIN_RAW', drain_raw),
             ('INFLATE_MIXED_ZLIB', mixed_z, 'INFLATE_MIXED_RAW', mixed_raw))
    for zname, zdata, rname, rdata in pairs:
        body.append(fmt(zname, zdata))
        body.append('')
        body.append(fmt(rname, rdata))
        body.append('')
    with open(TARGET, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(body) + '\n')
    print('wrote', TARGET, os.path.getsize(TARGET))


if __name__ == '__main__':
    main()
