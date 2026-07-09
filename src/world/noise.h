#pragma once

#include <stdint.h>

// Детерминированный градиентный шум (fractal brownian motion, 4 октавы).
// Возвращает значение в диапазоне [0..1].
float GenerateNoise(int64_t seed, float x, float y, float z);
