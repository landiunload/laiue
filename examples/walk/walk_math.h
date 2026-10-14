#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Keep the no-CRT walk executables independent of UCRT math imports in both
 * Debug and Release; optimized builds can otherwise hide these dependencies. */
static inline double WalkMathAbs(double value)
{
    return value < 0.0 ? -value : value;
}

static inline bool WalkMathFinite(double value)
{
    union
    {
        double number;
        uint64_t bits;
    } representation = {value};
    return (representation.bits & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

static inline double WalkMathFloor(double value)
{
    if (!(value >= -0x1p63 && value < 0x1p63))
        return value;
    const int64_t truncated = (int64_t)value;
    return (double)truncated > value ? (double)(truncated - 1) : (double)truncated;
}
