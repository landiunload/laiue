#!/usr/bin/env python3
"""Create small seamless, dependency-free PNG tiles for the Android walk demo."""

from __future__ import annotations

import random
import struct
import zlib
from pathlib import Path


SIZE = 32
OUTPUT = Path(__file__).resolve().parents[1] / "assets" / "textures"


def periodic_noise(cells: int, seed: int) -> list[list[float]]:
    rng = random.Random(seed)
    grid = [[rng.random() for _ in range(cells)] for _ in range(cells)]
    result = [[0.0] * SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        gy = y * cells / SIZE
        y0 = int(gy)
        fy = gy - y0
        fy = fy * fy * (3.0 - 2.0 * fy)
        for x in range(SIZE):
            gx = x * cells / SIZE
            x0 = int(gx)
            fx = gx - x0
            fx = fx * fx * (3.0 - 2.0 * fx)
            a = grid[y0 % cells][x0 % cells]
            b = grid[y0 % cells][(x0 + 1) % cells]
            c = grid[(y0 + 1) % cells][x0 % cells]
            d = grid[(y0 + 1) % cells][(x0 + 1) % cells]
            result[y][x] = (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (
                c * (1.0 - fx) + d * fx
            ) * fy
    return result


def write_png(path: Path, rgb_rows: list[list[tuple[int, int, int]]]) -> None:
    raw = b"".join(
        b"\0" + bytes(channel for rgb in row for channel in (*rgb, 255))
        for row in rgb_rows
    )

    def chunk(name: bytes, data: bytes) -> bytes:
        body = name + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">2I5B", SIZE, SIZE, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def make_tile(name: str, base: tuple[int, int, int], seed: int, contrast: float) -> None:
    broad = periodic_noise(4, seed)
    medium = periodic_noise(11, seed + 1)
    fine = periodic_noise(SIZE, seed + 2)
    rows: list[list[tuple[int, int, int]]] = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            variation = ((broad[y][x] - 0.5) * 0.58 +
                         (medium[y][x] - 0.5) * 0.27 +
                         (fine[y][x] - 0.5) * 0.15) * contrast
            fleck = (medium[y][x] > 0.84 and fine[y][x] > 0.7) - (
                medium[y][x] < 0.16 and fine[y][x] < 0.3
            )
            row.append(tuple(max(0, min(255, int(c + variation + fleck * 13)))
                             for c in base))
        rows.append(row)
    write_png(OUTPUT / name, rows)


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    make_tile("grass.png", (78, 139, 52), 4101, 46.0)
    make_tile("dirt.png", (128, 83, 48), 7207, 42.0)
    make_tile("stone.png", (112, 121, 130), 9919, 38.0)
    print(f"Wrote three seamless {SIZE}x{SIZE} RGBA tiles to {OUTPUT}")


if __name__ == "__main__":
    main()
