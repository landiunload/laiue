"""Собирает mp3_fixtures.h: настоящий MP3 плюс ожидаемые сэмплы.

Ожидания снимаются чужим декодером (ffmpeg), а не тем же кодом, который
проверяется: иначе тест сверял бы декодер с самим собой. Сборкой скрипт
не запускается — заголовок лежит в дереве, как скомпилированные шейдеры
и образцы текстур. Запускать вручную при смене набора образцов; нужны
ffmpeg с libmp3lame в PATH:

    python tests/mp3_fixtures.py
"""

import io
import math
import os
import struct
import subprocess
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 32000
SECONDS = 0.06


def emit_bytes(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for start in range(0, len(data), 16):
        chunk = data[start:start + 16]
        lines.append("    " + " ".join("%3du," % byte for byte in chunk))
    lines.append("};")
    return "\n".join(lines)


def emit_samples(name, data):
    values = struct.unpack("<%dh" % (len(data) // 2), data)
    lines = ["static const int16_t %s[%d] = {" % (name, len(values))]
    for start in range(0, len(values), 8):
        chunk = values[start:start + 8]
        lines.append("    " + " ".join("%6d," % value for value in chunk))
    lines.append("};")
    return "\n".join(lines)


def source_frames():
    # Резкая атака в середине заставляет кодировщик перейти на короткие
    # блоки, а разные каналы — задействовать стерео.
    frames = bytearray()
    total = int(RATE * SECONDS)
    for index in range(total):
        seconds = index / RATE
        attack = 1.0 if 0.02 <= seconds < 0.021 else 0.08
        left = attack * (0.6 * math.sin(2 * math.pi * 500 * seconds) +
                         0.3 * math.sin(2 * math.pi * 5500 * seconds))
        right = attack * (0.5 * math.sin(2 * math.pi * 2000 * seconds) +
                          0.25 * math.sin(2 * math.pi * 11000 * seconds))
        frames += struct.pack("<hh",
                              max(-32768, min(32767, int(left * 32767))),
                              max(-32768, min(32767, int(right * 32767))))
    return bytes(frames), total


def build():
    frames, total = source_frames()
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "source.wav")
        encoded = os.path.join(work, "sound.mp3")
        decoded = os.path.join(work, "sound.raw")
        with wave.open(source, "wb") as handle:
            handle.setnchannels(2)
            handle.setsampwidth(2)
            handle.setframerate(RATE)
            handle.writeframes(frames)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", source,
                        "-codec:a", "libmp3lame", "-b:a", "128k", encoded], check=True)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", encoded,
                        "-f", "s16le", "-acodec", "pcm_s16le", decoded], check=True)
        data = open(encoded, "rb").read()
        expected = open(decoded, "rb").read()

    print("mp3 %d bytes, expected %d frames" % (len(data), len(expected) // 4))
    header = ("// Образцы для tests/mp3_test.c: настоящий MP3 и ожидаемые\n"
              "// сэмплы. Собран tests/mp3_fixtures.py, сборкой не создаётся.\n"
              "// Ожидания сняты сторонним декодером, а не проверяемым кодом.\n"
              "#pragma once\n\n"
              "#include <stdint.h>\n\n"
              "#define MP3_SOUND_RATE %du\n"
              "#define MP3_SOUND_CHANNELS 2u\n\n" % RATE +
              emit_bytes("MP3_SOUND_FILE", data) + "\n\n" +
              emit_samples("MP3_SOUND_PCM", expected) + "\n")
    with io.open(os.path.join(HERE, "mp3_fixtures.h"), "w",
                 encoding="utf-8", newline="\n") as handle:
        handle.write(header)
    print("mp3_fixtures.h written")


if __name__ == "__main__":
    build()
