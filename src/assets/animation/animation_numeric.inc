/* Private shared numeric implementation; no platform or libm calls. */
static bool Finite(double value)
{
    return value >= -FLT_MAX && value <= FLT_MAX;
}

static bool Overlap(const void *a, uint64_t aBytes, const void *b, uint64_t bBytes)
{
    if (aBytes == 0u || bBytes == 0u)
        return false;
    const uintptr_t left = (uintptr_t)a;
    const uintptr_t right = (uintptr_t)b;
    if (aBytes > UINTPTR_MAX - left || bBytes > UINTPTR_MAX - right)
        return true;
    return left < right + bBytes && right < left + aBytes;
}

/* No libm or platform dependency. All normalization inputs are bounded
 * doubles built from finite floats; exponent-based initial estimate also
 * handles tiny values without a data-dependent halving loop. */
static double SquareRoot(double value)
{
    if (!(value > 0.0))
        return 0.0;
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits = (bits >> 1) + UINT64_C(0x1ff8000000000000);
    double estimate;
    memcpy(&estimate, &bits, sizeof(estimate));
    for (uint32_t step = 0u; step < 8u; ++step)
        estimate = 0.5 * (estimate + value / estimate);
    return estimate;
}
