// Ресемплер изображений: проверка, что быстрые пути (копия 1:1 и точное
// уменьшение вдвое) побайтово совпадают с прежней общей реализацией на
// случайных данных, и что полная mip-цепочка 256 → 1 даёт ту же сумму,
// что давала неизменённая реализация.
//
// Общий путь берётся не из проверяемой функции, а из локальной копии
// исходного кода: иначе ошибка в общем пути маскировала бы ошибку в
// быстром. Контрольная сумма зафиксирована константой, снятой со старого
// кода стендом build/peer/image_resample_bench.c.

#include "media/image.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("image resample test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

// Эталон: дословная копия общей реализации до правки.
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

static void ExpectSame(const uint8_t *actual, const uint8_t *expected, size_t count,
                       const char *message)
{
    for (size_t index = 0; index < count; ++index)
    {
        Expect(actual[index] == expected[index], message);
    }
}

// Детерминированный ГПСЧ: тот же xorshift и то же зерно, что в стенде,
// поэтому mip-сумма воспроизводится с точностью до байта.
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
    for (size_t index = 0; index < count; ++index)
    {
        bytes[index] = (uint8_t)(XorShift(state) & 0xFFu);
    }
}

typedef void (*ResampleFn)(const uint8_t *, uint32_t, uint32_t, uint8_t *, uint32_t, uint32_t);

#define CHAIN_SIDE 256u
#define CHAIN_BYTES (CHAIN_SIDE * CHAIN_SIDE * 4u)

// Контрольная сумма mip-цепочки 256 → 1, снятая стендом с неизменённой
// реализации на случайном изображении с зерном 0x12345678.
#define CHAIN_CHECKSUM 0x9ad25e9b6cea9c76ull

static uint8_t g_chainSource[CHAIN_BYTES];
static uint8_t g_newA[CHAIN_BYTES];
static uint8_t g_newB[CHAIN_BYTES];
static uint8_t g_refA[CHAIN_BYTES];
static uint8_t g_refB[CHAIN_BYTES];

static uint64_t HashBytes(uint64_t hash, const uint8_t *bytes, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void CheckMipChain(void)
{
    uint32_t state = 0x12345678u;
    FillRandom(g_chainSource, CHAIN_BYTES, &state);

    uint64_t newHash = 0xcbf29ce484222325ull;
    uint64_t refHash = 0xcbf29ce484222325ull;

    // Первый уровень — приведение источника к 256×256 (быстрый путь 1:1).
    ImageResample(g_chainSource, CHAIN_SIDE, CHAIN_SIDE, g_newA, CHAIN_SIDE, CHAIN_SIDE);
    ReferenceResample(g_chainSource, CHAIN_SIDE, CHAIN_SIDE, g_refA, CHAIN_SIDE, CHAIN_SIDE);
    ExpectSame(g_newA, g_refA, CHAIN_BYTES, "the 256 copy must match the general path");
    newHash = HashBytes(newHash, g_newA, CHAIN_BYTES);
    refHash = HashBytes(refHash, g_refA, CHAIN_BYTES);

    const uint8_t *newCurrent = g_newA;
    uint8_t *newNext = g_newB;
    const uint8_t *refCurrent = g_refA;
    uint8_t *refNext = g_refB;
    uint32_t size = CHAIN_SIDE;

    while (size > 1u)
    {
        uint32_t half = size / 2u;
        size_t bytes = (size_t)half * half * 4u;
        ImageResample(newCurrent, size, size, newNext, half, half);
        ReferenceResample(refCurrent, size, size, refNext, half, half);
        ExpectSame(newNext, refNext, bytes, "every mip level must match the general path");
        newHash = HashBytes(newHash, newNext, bytes);
        refHash = HashBytes(refHash, refNext, bytes);

        const uint8_t *newSwap = newCurrent;
        newCurrent = newNext;
        newNext = (uint8_t *)newSwap;
        const uint8_t *refSwap = refCurrent;
        refCurrent = refNext;
        refNext = (uint8_t *)refSwap;
        size = half;
    }

    Expect(newHash == CHAIN_CHECKSUM, "the mip chain must keep the old checksum");
    Expect(refHash == CHAIN_CHECKSUM, "the reference chain must reproduce the checksum");
}

#define MAX_SIDE 64u
#define MAX_BYTES (MAX_SIDE * MAX_SIDE * 4u)

static uint8_t g_source[MAX_BYTES];
static uint8_t g_output[MAX_BYTES];
static uint8_t g_reference[MAX_BYTES];

static void CheckRandom(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t destWidth,
                        uint32_t destHeight, const char *message)
{
    size_t destBytes = (size_t)destWidth * destHeight * 4u;
    ImageResample(g_source, sourceWidth, sourceHeight, g_output, destWidth, destHeight);
    ReferenceResample(g_source, sourceWidth, sourceHeight, g_reference, destWidth, destHeight);
    ExpectSame(g_output, g_reference, destBytes, message);
}

LAIUE_TEST_ENTRY(ImageResampleTestEntryPoint)
{
    uint32_t state = 0xC0FFEE01u;
    uint32_t cases = 0u;

    // Все размеры 2..64: копия 1:1 и, где делится, точное уменьшение вдвое.
    for (uint32_t sourceWidth = 2u; sourceWidth <= MAX_SIDE; ++sourceWidth)
    {
        for (uint32_t sourceHeight = 2u; sourceHeight <= MAX_SIDE; ++sourceHeight)
        {
            size_t sourceBytes = (size_t)sourceWidth * sourceHeight * 4u;
            FillRandom(g_source, sourceBytes, &state);

            CheckRandom(sourceWidth, sourceHeight, sourceWidth, sourceHeight,
                        "the 1:1 copy must match the general path");
            ++cases;

            if ((sourceWidth % 2u) == 0u && (sourceHeight % 2u) == 0u)
            {
                // Полностью прозрачный блок 2×2 с ненулевым цветом: цвет
                // обязан усредниться обычным образом, а не пропасть.
                for (uint32_t channel = 0u; channel < 4u; ++channel)
                {
                    g_source[channel] = (uint8_t)(200u + channel);
                    g_source[4u + channel] = (uint8_t)(100u + channel);
                    g_source[(size_t)sourceWidth * 4u + channel] = (uint8_t)(50u + channel);
                    g_source[(size_t)sourceWidth * 4u + 4u + channel] = (uint8_t)1u;
                }
                g_source[3] = 0u;
                g_source[7] = 0u;
                g_source[(size_t)sourceWidth * 4u + 3u] = 0u;
                g_source[(size_t)sourceWidth * 4u + 7u] = 0u;

                CheckRandom(sourceWidth, sourceHeight, sourceWidth / 2u, sourceHeight / 2u,
                            "the halving path must match the general path");
                ++cases;
            }
        }
    }

    // Крайние случаи уменьшения вдвое: целиком прозрачный блок и блок
    // из максимальных значений.
    static const uint8_t transparentQuad[16] = {
        200u, 10u, 20u, 0u, 100u, 30u, 40u, 0u,
        50u,  60u, 70u, 0u, 1u,   2u,  3u,  0u,
    };
    ImageResample(transparentQuad, 2u, 2u, g_output, 1u, 1u);
    ReferenceResample(transparentQuad, 2u, 2u, g_reference, 1u, 1u);
    ExpectSame(g_output, g_reference, 4u, "a transparent block must average its colour");
    Expect(g_output[0] == 88u && g_output[1] == 26u && g_output[2] == 33u && g_output[3] == 0u,
           "the transparent block must keep the plain average");

    static const uint8_t maximumQuad[16] = {
        255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u,
        255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u,
    };
    ImageResample(maximumQuad, 2u, 2u, g_output, 1u, 1u);
    ReferenceResample(maximumQuad, 2u, 2u, g_reference, 1u, 1u);
    ExpectSame(g_output, g_reference, 4u, "a maximum block must stay maximum");

    // Случайные пары размеров 2..64 в обе стороны, включая увеличение.
    for (uint32_t iteration = 0u; iteration < 10000u; ++iteration)
    {
        uint32_t sourceWidth = 2u + XorShift(&state) % (MAX_SIDE - 1u);
        uint32_t sourceHeight = 2u + XorShift(&state) % (MAX_SIDE - 1u);
        uint32_t destWidth = 1u + XorShift(&state) % MAX_SIDE;
        uint32_t destHeight = 1u + XorShift(&state) % MAX_SIDE;

        FillRandom(g_source, (size_t)sourceWidth * sourceHeight * 4u, &state);
        CheckRandom(sourceWidth, sourceHeight, destWidth, destHeight,
                    "a random resample must match the general path");
        ++cases;
    }

    Expect(cases >= 10000u, "the random sweep must cover at least ten thousand images");

    CheckMipChain();

    LaiueTestRuntimeWrite("image resample test passed\n");
    LAIUE_TEST_SUCCESS();
}
