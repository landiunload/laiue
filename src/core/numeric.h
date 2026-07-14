#pragma once

#include <stdint.h>

static inline int64_t FloorDoubleToInt64(double value)
{
    int64_t truncated = (int64_t)value;
    return (double)truncated > value ? truncated - 1 : truncated;
}
