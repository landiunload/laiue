#pragma once

#include <stdint.h>

// Детерминированный градиентный 2D-шум для высоты ландшафта
// (fractal brownian motion, 4 октавы). Возвращает значение [0..1].
// Эквивалентен прежнему 3D-шуму при y = 0 — рельеф не изменился.
float GenerateTerrainNoise(int64_t seed, float x, float z);
