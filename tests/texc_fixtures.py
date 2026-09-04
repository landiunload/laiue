"""Собирает texc_fixtures.h: настоящие PNG, GIF и JPEG плюс ожидаемые пиксели.

Ожидания снимаются чужим декодером (Pillow), а не тем же кодом, который
проверяется: иначе тест сверял бы декодер с самим собой. Сборкой скрипт
не запускается — заголовок лежит в дереве, как и скомпилированные
шейдеры. Запускать вручную при смене набора образцов:

    python tests/texc_fixtures.py
"""

import io
import os

from PIL import Image, ImageSequence

HERE = os.path.dirname(os.path.abspath(__file__))


def emit_bytes(name, data):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for start in range(0, len(data), 16):
        chunk = data[start:start + 16]
        lines.append("    " + " ".join("%3du," % byte for byte in chunk))
    lines.append("};")
    return "\n".join(lines)


def rgba_of(image):
    return image.convert("RGBA").tobytes()


def png_fixture(name, image, expected=None, **save):
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", **save)
    data = buffer.getvalue()
    if expected is None:
        decoded = Image.open(io.BytesIO(data))
        expected = rgba_of(decoded)
    return (emit_bytes(name + "_FILE", data) + "\n\n" +
            emit_bytes(name + "_RGBA", expected) + "\n")


def gif_fixture(name, frames, duration, transparency):
    buffer = io.BytesIO()
    options = {"duration": duration, "disposal": 1, "loop": 0}
    if transparency is not None:
        options["transparency"] = transparency
    frames[0].save(buffer, format="GIF", save_all=True, append_images=frames[1:], **options)
    data = buffer.getvalue()

    opened = Image.open(io.BytesIO(data))
    composited = b""
    count = 0
    for frame in ImageSequence.Iterator(opened):
        composited += rgba_of(frame)
        count += 1
    return (emit_bytes(name + "_FILE", data) + "\n\n" +
            emit_bytes(name + "_RGBA", composited) + "\n\n" +
            "#define %s_FRAMES %du\n" % (name, count))


def jpeg_fixture(name, image, expected=None, **save):
    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", **save)
    data = buffer.getvalue()
    parts = [emit_bytes(name + "_FILE", data)]
    if expected is None:
        parts.append(emit_bytes(name + "_RGBA", rgba_of(Image.open(io.BytesIO(data)))))
    else:
        # Ожидание общее с другим образцом: перекладка того же кадра
        # обязана дать те же пиксели, и второй массив байтов только
        # прятал бы это равенство.
        decoded = rgba_of(Image.open(io.BytesIO(data)))
        assert decoded == expected, name + " must decode to the shared expectation"
    return "\n\n".join(parts) + "\n"


def build():
    parts = []

    # RGBA8 с настоящей прозрачностью по диагонали.
    rgba = Image.new("RGBA", (4, 4))
    for y in range(4):
        for x in range(4):
            rgba.putpixel((x, y), (x * 64, y * 64, 128, 0 if x == y else 255))
    parts.append(png_fixture("PNG_RGBA", rgba))

    # Палитра с прозрачным нулевым индексом.
    palette = Image.new("P", (4, 4))
    table = []
    for index in range(16):
        table += [index * 16, 255 - index * 16, index * 8]
    palette.putpalette(table + [0] * (768 - len(table)))
    for y in range(4):
        for x in range(4):
            palette.putpixel((x, y), x + y * 4)
    parts.append(png_fixture("PNG_PALETTE", palette, transparency=0))

    # Шестнадцать бит на канал: младший байт обязан отброситься ровно.
    # Ожидание здесь считается по спецификации, а не снимается с
    # Pillow: его перевод режима I в RGBA обрезает значения по 255
    # вместо масштабирования, и эталоном для глубины 16 он не годится.
    gray16 = Image.new("I;16", (2, 2))
    levels = [0, 21845, 43690, 65535]
    gray16.putpixel((0, 0), levels[0])
    gray16.putpixel((1, 0), levels[1])
    gray16.putpixel((0, 1), levels[2])
    gray16.putpixel((1, 1), levels[3])
    gray16_expected = b""
    for level in levels:
        high = level >> 8
        gray16_expected += bytes([high, high, high, 255])
    parts.append(png_fixture("PNG_GRAY16", gray16, expected=gray16_expected))

    # Чересстрочная развёртка Adam7.
    interlaced = Image.new("RGB", (8, 8))
    for y in range(8):
        for x in range(8):
            interlaced.putpixel((x, y), (x * 32, y * 32, (x + y) * 16))
    parts.append(png_fixture("PNG_INTERLACED", interlaced, interlace=True))

    # Одноцветный PNG: по нему проверяется цвет пикселя на экране, а
    # для этого текстура обязана быть однородной.
    solid = Image.new("RGBA", (4, 4), (0, 255, 0, 255))
    parts.append(png_fixture("PNG_SOLID", solid))

    # Анимация: три кадра, у второго прозрачный угол — он обязан
    # показать пиксель предыдущего кадра, а не пустоту.
    frames = []
    for index in range(3):
        frame = Image.new("P", (4, 4))
        table = [0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255]
        frame.putpalette(table + [0] * (768 - len(table)))
        for y in range(4):
            for x in range(4):
                frame.putpixel((x, y), index + 1)
        if index == 1:
            frame.putpixel((0, 0), 0)
        frames.append(frame)
    parts.append(gif_fixture("GIF_ANIMATED", frames, 60, 0))

    # Разные задержки у разных кадров: в GIF так можно, и один интервал
    # на всю анимацию это молча ломал бы.
    variable = []
    for index in range(3):
        frame = Image.new("P", (4, 4))
        table = [255, 0, 0, 0, 255, 0, 0, 0, 255]
        frame.putpalette(table + [0] * (768 - len(table)))
        for y in range(4):
            for x in range(4):
                frame.putpixel((x, y), index)
        variable.append(frame)
    parts.append(gif_fixture("GIF_VARIABLE", variable, [40, 120, 60], None))

    # JPEG. Ожидания снимает Pillow (libjpeg), поэтому сверяется наш
    # разбор с чужим. Совпадение здесь не побайтовое: обратное ДКП у
    # нас своё, и допуск задан в самом тесте.
    colour = Image.new("RGB", (16, 16))
    for y in range(16):
        for x in range(16):
            colour.putpixel((x, y), ((x * 16) % 256, (y * 16) % 256, ((x + y) * 8) % 256))
    parts.append(jpeg_fixture("JPEG_BASELINE", colour, quality=90, subsampling=0))

    # Прогрессивная запись того же кадра: коэффициенты те же, меняется
    # только порядок их передачи, поэтому и пиксели обязаны совпасть.
    baseline_pixels = rgba_of(Image.open(io.BytesIO(
        (lambda buffer: (colour.save(buffer, format="JPEG", quality=90, subsampling=0),
                         buffer.getvalue())[1])(io.BytesIO()))))
    parts.append(jpeg_fixture("JPEG_PROGRESSIVE", colour, expected=baseline_pixels,
                              quality=90, subsampling=0, progressive=True))

    # Прореживание цветности 4:2:0 на размере, не кратном MCU: кадр 20
    # на 12 занимает два MCU по горизонтали и один по вертикали, и
    # лишние блоки обязаны отброситься. Цвет здесь серый, то есть
    # цветность постоянна: восстановление половинного разрешения не
    # зависит от выбранного фильтра, и сравнение остаётся строгим.
    neutral = Image.new("RGB", (20, 12))
    for y in range(12):
        for x in range(20):
            level = (x * 13 + y * 29) % 256
            neutral.putpixel((x, y), (level, level, level))
    parts.append(jpeg_fixture("JPEG_SUBSAMPLED", neutral, quality=90, subsampling=2))

    # Маркеры рестарта не меняют коэффициенты: они только сбрасывают
    # предсказание, и кодировщик это учитывает. Значит, результат
    # обязан совпасть с тем же кадром без них.
    subsampled_pixels = rgba_of(Image.open(io.BytesIO(
        (lambda buffer: (neutral.save(buffer, format="JPEG", quality=90, subsampling=2),
                         buffer.getvalue())[1])(io.BytesIO()))))
    parts.append(jpeg_fixture("JPEG_RESTART", neutral, expected=subsampled_pixels, quality=90,
                              subsampling=2, restart_marker_blocks=1))

    # Резкая цветность при прореживании 4:2:0: полосы красного и синего
    # меняются каждые два пикселя, то есть ровно на шаге прореживания.
    # На таком кадре повторение ближайшего отсчёта вместо треугольного
    # фильтра расходится с эталоном на десятки единиц, и допуск теста
    # это ловит.
    chroma = Image.new("RGB", (16, 16))
    for y in range(16):
        for x in range(16):
            chroma.putpixel((x, y), (230, 30, 40) if (x // 2 + y // 2) % 2 == 0 else (20, 60, 220))
    parts.append(jpeg_fixture("JPEG_CHROMA", chroma, quality=92, subsampling=2))

    # Одноцветный JPEG: по нему проверяется цвет пикселя на экране,
    # поэтому текстура обязана быть однородной.
    solidJpeg = Image.new("RGB", (8, 8), (0, 255, 255))
    parts.append(jpeg_fixture("JPEG_SOLID", solidJpeg, quality=95, subsampling=0))

    # Один компонент: серый JPEG не проходит через преобразование цвета.
    gray = Image.new("L", (8, 8))
    for y in range(8):
        for x in range(8):
            gray.putpixel((x, y), (x * 31 + y * 7) % 256)
    parts.append(jpeg_fixture("JPEG_GRAY", gray, quality=88))

    header = ("// Образцы для tests/texc_test.c: настоящие файлы и ожидаемые\n"
              "// пиксели. Собран tests/texc_fixtures.py, сборкой не создаётся.\n"
              "// Ожидания сняты сторонним декодером, а не проверяемым кодом.\n"
              "#pragma once\n\n"
              "#include <stdint.h>\n\n" + "\n".join(parts))
    with io.open(os.path.join(HERE, "texc_fixtures.h"), "w",
                 encoding="utf-8", newline="\n") as handle:
        handle.write(header)
    print("texc_fixtures.h written")


if __name__ == "__main__":
    build()
