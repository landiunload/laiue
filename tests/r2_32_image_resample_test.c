// ROUND 2, агент 32-image: эквивалентность общего пути ImageResample.
//
// Правка переводит границы столбцов общего пути с деления на накопительный
// счётчик. Эталон — дословная копия прежнего общего пути (с делением на
// каждый выходной пиксель); сравнение побайтовое. Покрытие:
//   * все квадратные пары сторон 1..64;
//   * прямоугольники 1..64 в обе стороны при разных соотношениях;
//   * смешанный масштаб (одна ось вниз, другая вверх), включая 1×N/N×1;
//   * случайные пары сторон 1..512 на случайных данных.
// Отдельно проверяется старая mip-сумма 256 → 1.

#include "media/image.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("r2 image resample test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

// Эталон: дословная копия общего пути до правки.
static void ReferenceResample(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
                              uint8_t *destination, uint32_t destinationWidth,
                              uint32_t destinationHeight)
{
    if (source == NULL || destination == NULL) return;
    if (sourceWidth == 0u || sourceHeight == 0u) return;
    if (destinationWidth == 0u || destinationHeight == 0u) return;

    for (uint32_t row = 0; row < destinationHeight; ++row)
    {
        uint32_t firstRow = (uint32_t)((uint64_t)row * sourceHeight / destinationHeight);
        uint32_t lastRow = (uint32_t)(((uint64_t)row + 1u) * sourceHeight / destinationHeight);
        if (lastRow <= firstRow) lastRow = firstRow + 1u;

        for (uint32_t column = 0; column < destinationWidth; ++column)
        {
            uint32_t firstColumn =
                (uint32_t)((uint64_t)column * sourceWidth / destinationWidth);
            uint32_t lastColumn =
                (uint32_t)(((uint64_t)column + 1u) * sourceWidth / destinationWidth);
            if (lastColumn <= firstColumn) lastColumn = firstColumn + 1u;

            uint64_t weightedRed = 0u;
            uint64_t weightedGreen = 0u;
            uint64_t weightedBlue = 0u;
            uint64_t plainRed = 0u;
            uint64_t plainGreen = 0u;
            uint64_t plainBlue = 0u;
            uint64_t alphaSum = 0u;
            uint64_t samples = 0u;

            for (uint32_t sourceRow = firstRow; sourceRow < lastRow; ++sourceRow)
            {
                const uint8_t *line = source + (size_t)sourceRow * sourceWidth * 4u;
                for (uint32_t sourceColumn = firstColumn; sourceColumn < lastColumn;
                     ++sourceColumn)
                {
                    const uint8_t *texel = line + (size_t)sourceColumn * 4u;
                    uint32_t alpha = texel[3];
                    weightedRed += (uint64_t)texel[0] * alpha;
                    weightedGreen += (uint64_t)texel[1] * alpha;
                    weightedBlue += (uint64_t)texel[2] * alpha;
                    plainRed += texel[0];
                    plainGreen += texel[1];
                    plainBlue += texel[2];
                    alphaSum += alpha;
                    ++samples;
                }
            }

            uint8_t *out = destination + ((size_t)row * destinationWidth + column) * 4u;
            if (alphaSum != 0u)
            {
                out[0] = (uint8_t)((weightedRed + alphaSum / 2u) / alphaSum);
                out[1] = (uint8_t)((weightedGreen + alphaSum / 2u) / alphaSum);
                out[2] = (uint8_t)((weightedBlue + alphaSum / 2u) / alphaSum);
            }
            else
            {
                out[0] = (uint8_t)((plainRed + samples / 2u) / samples);
                out[1] = (uint8_t)((plainGreen + samples / 2u) / samples);
                out[2] = (uint8_t)((plainBlue + samples / 2u) / samples);
            }
            out[3] = (uint8_t)((alphaSum + samples / 2u) / samples);
        }
    }
}

static uint32_t XorShift(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void FillRandom(uint8_t *bytes, size_t count, uint32_t *state)
{
    for (size_t index = 0; index < count; ++index) bytes[index] = (uint8_t)(XorShift(state) & 0xFFu);
}

#define MAX_SIDE 512u
#define MAX_BYTES (MAX_SIDE * MAX_SIDE * 4u)

static uint8_t g_source[MAX_BYTES];
static uint8_t g_output[MAX_BYTES];
static uint8_t g_reference[MAX_BYTES];

static void CheckOne(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t destWidth,
                     uint32_t destHeight, const char *message)
{
    if (sourceWidth == 0u || sourceHeight == 0u || destWidth == 0u || destHeight == 0u) return;
    size_t destBytes = (size_t)destWidth * destHeight * 4u;
    ImageResample(g_source, sourceWidth, sourceHeight, g_output, destWidth, destHeight);
    ReferenceResample(g_source, sourceWidth, sourceHeight, g_reference, destWidth, destHeight);
    for (size_t index = 0; index < destBytes; ++index)
    {
        if (g_output[index] != g_reference[index]) Expect(false, message);
    }
}

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

#define CHAIN_SIDE 256u
#define CHAIN_BYTES (CHAIN_SIDE * CHAIN_SIDE * 4u)
#define CHAIN_CHECKSUM 0x9ad25e9b6cea9c76ull

static uint8_t g_chainSource[CHAIN_BYTES];
static uint8_t g_chainA[CHAIN_BYTES];
static uint8_t g_chainB[CHAIN_BYTES];

static void CheckMipChain(void)
{
    uint32_t state = 0x12345678u;
    FillRandom(g_chainSource, CHAIN_BYTES, &state);
    uint64_t hash = 0xcbf29ce484222325ull;

    ImageResample(g_chainSource, CHAIN_SIDE, CHAIN_SIDE, g_chainA, CHAIN_SIDE, CHAIN_SIDE);
    hash = HashBytes(hash, g_chainA, CHAIN_BYTES);

    const uint8_t *current = g_chainA;
    uint8_t *next = g_chainB;
    uint32_t size = CHAIN_SIDE;
    while (size > 1u)
    {
        uint32_t half = size / 2u;
        size_t bytes = (size_t)half * half * 4u;
        ImageResample(current, size, size, next, half, half);
        hash = HashBytes(hash, next, bytes);
        const uint8_t *swap = current;
        current = next;
        next = (uint8_t *)swap;
        size = half;
    }
    Expect(hash == CHAIN_CHECKSUM, "the mip chain must keep the old checksum");
}

LAIUE_TEST_ENTRY(R2ImageResampleTestEntryPoint)
{
    uint32_t state = 0x51D0C0DEu;
    uint64_t cases = 0u;

    // Квадратные пары 1..64: все соотношения, включая 1:1.
    for (uint32_t source = 1u; source <= 64u; ++source)
    {
        for (uint32_t dest = 1u; dest <= 64u; ++dest)
        {
            FillRandom(g_source, (size_t)source * source * 4u, &state);
            CheckOne(source, source, dest, dest, "square pair must match the general path");
            ++cases;
        }
    }

    // Прямоугольники: 64×7 к 1..72×1..72 — широкие и высокие выходы,
    // смешанный масштаб в обе стороны.
    FillRandom(g_source, 64u * 7u * 4u, &state);
    for (uint32_t destWidth = 1u; destWidth <= 72u; ++destWidth)
    {
        for (uint32_t destHeight = 1u; destHeight <= 72u; ++destHeight)
        {
            CheckOne(64u, 7u, destWidth, destHeight, "wide source must match the general path");
            ++cases;
        }
    }

    FillRandom(g_source, 7u * 64u * 4u, &state);
    for (uint32_t destWidth = 1u; destWidth <= 72u; ++destWidth)
    {
        for (uint32_t destHeight = 1u; destHeight <= 72u; ++destHeight)
        {
            CheckOne(7u, 64u, destWidth, destHeight, "tall source must match the general path");
            ++cases;
        }
    }

    // Одна строка / один столбец в обе стороны.
    FillRandom(g_source, MAX_SIDE * 4u, &state);
    for (uint32_t source = 1u; source <= 200u; ++source)
    {
        for (uint32_t dest = 1u; dest <= 200u; ++dest)
        {
            CheckOne(source, 1u, dest, 1u, "single row must match the general path");
            CheckOne(1u, source, 1u, dest, "single column must match the general path");
            ++cases;
        }
    }

    // Случайные пары сторон 1..512 на случайных данных.
    for (uint32_t iteration = 0u; iteration < 3000u; ++iteration)
    {
        uint32_t sourceWidth = 1u + XorShift(&state) % MAX_SIDE;
        uint32_t sourceHeight = 1u + XorShift(&state) % MAX_SIDE;
        uint32_t destWidth = 1u + XorShift(&state) % MAX_SIDE;
        uint32_t destHeight = 1u + XorShift(&state) % MAX_SIDE;
        FillRandom(g_source, (size_t)sourceWidth * sourceHeight * 4u, &state);
        CheckOne(sourceWidth, sourceHeight, destWidth, destHeight,
                 "a random resample must match the general path");
        ++cases;
    }

    // Специально смешанный масштаб с большим коэффициентом.
    FillRandom(g_source, 512u * 8u * 4u, &state);
    CheckOne(512u, 8u, 8u, 512u, "mixed down/up must match the general path");
    CheckOne(8u, 512u, 512u, 8u, "mixed up/down must match the general path");
    ++cases;

    CheckMipChain();

    Expect(cases >= 20000u, "coverage must stay wide");
    LaiueTestRuntimeWrite("r2 image resample test passed\n");
    LAIUE_TEST_SUCCESS();
}
