/* Each translation unit specializes one arithmetic implementation. */
static bool NormalMatrix(const float *m, double *out)
{
    /* Cofactors are the inverse transpose multiplied by determinant. */
    out[0] = (double)m[5] * m[10] - (double)m[6] * m[9];
    out[1] = (double)m[6] * m[8] - (double)m[4] * m[10];
    out[2] = (double)m[4] * m[9] - (double)m[5] * m[8];
    out[3] = (double)m[2] * m[9] - (double)m[1] * m[10];
    out[4] = (double)m[0] * m[10] - (double)m[2] * m[8];
    out[5] = (double)m[1] * m[8] - (double)m[0] * m[9];
    out[6] = (double)m[1] * m[6] - (double)m[2] * m[5];
    out[7] = (double)m[2] * m[4] - (double)m[0] * m[6];
    out[8] = (double)m[0] * m[5] - (double)m[1] * m[4];
    const double determinant =
        (double)m[0] * out[0] + (double)m[1] * out[1] + (double)m[2] * out[2];
    if (determinant == 0.0)
        return false;
    for (uint32_t i = 0u; i < 9u; ++i)
        out[i] /= determinant;
    return true;
}

#if defined(ANIMATION_SKIN_CACHED)
static bool SkinOneCached(const LaiueAnimationSkinVertexV1 *vertex,
                          const LaiueAnimationMatrixV1 *palette, uint32_t jointCount,
                          const double *normalMatrices, const uint8_t *normalValid,
                          LaiueAnimationSkinnedVertexV1 *out)
#else
static bool SkinOne(const LaiueAnimationSkinVertexV1 *vertex, const LaiueAnimationMatrixV1 *palette,
                    uint32_t jointCount, LaiueAnimationSkinnedVertexV1 *out)
#endif
{
    double total = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
        if (!Finite(vertex->position[component]) || !Finite(vertex->normal[component]))
            return false;
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
        if (vertex->joints[influence] >= jointCount || !Finite(vertex->weights[influence]) ||
            vertex->weights[influence] < 0.0f)
            return false;
        total += vertex->weights[influence];
    }
    if (total < 0.999 || total > 1.001)
        return false;
    double position[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 0.0};
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
        if (vertex->weights[influence] == 0.0f)
            continue;
        const double weight = (double)vertex->weights[influence] / total;
        const float *matrix = palette[vertex->joints[influence]].m;
#if defined(ANIMATION_SKIN_CACHED)
        const uint32_t joint = vertex->joints[influence];
        if (normalValid[joint] == 0u)
            return false;
        const double *normalMatrix = normalMatrices + (size_t)joint * 9u;
#else
        double normalMatrix[9];
        if (!NormalMatrix(matrix, normalMatrix))
            return false;
#endif
        for (uint32_t row = 0u; row < 3u; ++row)
        {
            double p = matrix[row * 4u + 3u];
            double n = 0.0;
            for (uint32_t component = 0u; component < 3u; ++component)
            {
                p += (double)matrix[row * 4u + component] * vertex->position[component];
                n += normalMatrix[row * 3u + component] * vertex->normal[component];
            }
            position[row] += p * weight;
            normal[row] += n * weight;
        }
    }
    const double length =
        SquareRoot(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    for (uint32_t component = 0u; component < 3u; ++component)
    {
        const double n = length > 0.0 ? normal[component] / length : 0.0;
        if (!Finite(position[component]) || !Finite(n))
            return false;
        out->position[component] = (float)position[component];
        out->normal[component] = (float)n;
    }
    return true;
}
