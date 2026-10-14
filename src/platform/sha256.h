#pragma once

#include <stdbool.h>
#include <stdint.h>

bool LaiueSha256Compute(const void *bytes, uint64_t size, uint8_t output[32]);
