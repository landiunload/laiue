// Целый корень эвристики A*: результат обязан быть ровно floor(sqrt(value)) на
// всём диапазоне uint64. Эталон считает побитово, без плавающей точки.
#include "navigation/nav_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static uint64_t ExactRoot(uint64_t value)
{
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62u;
    while (bit > value)
        bit >>= 2u;
    while (bit != 0u)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1u) + bit;
        }
        else
            result >>= 1u;
        bit >>= 2u;
    }
    return result;
}

static void Check(uint64_t value)
{
    if (NavIntegerSqrt(value) == ExactRoot(value))
        return;
    LaiueTestRuntimeWrite("Integer square root check failed\n");
    LaiueTestRuntimeExit(1);
}

static void CheckAround(uint64_t value)
{
    Check(value - 2u);
    Check(value - 1u);
    Check(value);
    Check(value + 1u);
    Check(value + 2u);
}

LAIUE_TEST_ENTRY(NavigationSqrtTestEntryPoint)
{
    for (uint64_t value = 0u; value < (UINT64_C(1) << 20u); ++value)
        Check(value);

    // Степени двойки: здесь меняется порядок и теряется младший бит.
    for (uint32_t power = 2u; power < 64u; ++power)
        CheckAround(UINT64_C(1) << power);

    // Точные квадраты и их соседи: только тут оценка может уйти на единицу вверх.
    // Шаг растёт, чтобы пройти все порядки корня, а не только малые.
    for (uint64_t root = 2u; root <= UINT64_C(0xFFFFFFFF); root += 1u + root / 4096u)
        CheckAround(root * root);

    // Верх диапазона: самые большие квадраты и последнее значение.
    for (uint64_t distance = 0u; distance < 4096u; ++distance)
    {
        const uint64_t root = UINT64_C(0xFFFFFFFF) - distance;
        CheckAround(root * root);
    }
    Check(UINT64_MAX - 2u);
    Check(UINT64_MAX - 1u);
    Check(UINT64_MAX);

    // Случайные значения всех порядков.
    uint64_t state = UINT64_C(0x9E3779B97F4A7C15);
    for (uint32_t round = 0u; round < 1048576u; ++round)
    {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        Check(state >> (round % 64u));
    }

    LaiueTestRuntimeWrite("Integer square root checks passed\n");
    LaiueTestRuntimeExit(0);
}
