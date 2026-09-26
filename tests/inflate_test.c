// Прямой тест InflateZlib: несжатые (stored), фиксированный Хаффман,
// смешанный поток и поток, где дозаправка битов читает вперёд в заголовок
// несжатого блока. Проверяются распакованное содержимое, копирование
// несжатого блока через границы отрезков, усечение, повреждённый NLEN и
// выход за размер выходного буфера. Эталон — RFC 1951, а не наш декодер.
//
// Буферы статические: кадр с рабочими таблицами и выходом не помещается в
// бюджет стека no-CRT исполняемого файла.
#include "media/inflate.h"

#include "test_runtime.h"
#include "inflate_fixtures.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define INFLATE_TEST_OUTPUT 2048u

static uint8_t g_output[INFLATE_TEST_OUTPUT];
static uint8_t g_short[sizeof(INFLATE_STORED_RAW) - 1u];
static uint8_t g_damaged[sizeof(INFLATE_STORED_ZLIB)];
static InflateWork g_work;

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("inflate test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectInflates(const uint8_t *stream, uint32_t streamBytes, const uint8_t *expected,
                           uint32_t expectedBytes, const char *message)
{
    uint32_t written = 0u;
    InflateSegment segment = {stream, streamBytes};
    ImageStatus status = InflateZlib(&segment, 1u, g_output, sizeof(g_output), &g_work,
                                     sizeof(g_work), &written);
    Expect(status == IMAGE_OK, message);
    Expect(written == expectedBytes, message);
    Expect(memcmp(g_output, expected, expectedBytes) == 0, message);
}

// Тот же поток, разрезанный на три отрезка: копирование несжатого блока
// обязано пережить границы IDAT.
static void ExpectInflatesSplitAt(const uint8_t *stream, uint32_t streamBytes,
                                  const uint8_t *expected, uint32_t expectedBytes,
                                  uint32_t firstCut, uint32_t secondCut, const char *message)
{
    Expect(firstCut > 0u && firstCut < secondCut && secondCut < streamBytes, message);
    uint32_t written = 0u;
    InflateSegment segments[3] = {
        {stream, firstCut},
        {stream + firstCut, secondCut - firstCut},
        {stream + secondCut, streamBytes - secondCut},
    };
    ImageStatus status =
        InflateZlib(segments, 3u, g_output, sizeof(g_output), &g_work, sizeof(g_work), &written);
    Expect(status == IMAGE_OK, message);
    Expect(written == expectedBytes, message);
    Expect(memcmp(g_output, expected, expectedBytes) == 0, message);
}

static void ExpectInflatesSplit(const uint8_t *stream, uint32_t streamBytes,
                                const uint8_t *expected, uint32_t expectedBytes,
                                const char *message)
{
    Expect(streamBytes >= 8u, message);
    ExpectInflatesSplitAt(stream, streamBytes, expected, expectedBytes, streamBytes / 4u,
                          streamBytes / 2u, message);
}

static void ExpectTruncated(const uint8_t *stream, uint32_t streamBytes)
{
    for (uint32_t cut = 0u; cut < streamBytes; ++cut)
    {
        uint32_t written = 0u;
        InflateSegment segment = {stream, cut};
        ImageStatus status = InflateZlib(&segment, 1u, g_output, sizeof(g_output), &g_work,
                                         sizeof(g_work), &written);
        Expect(status != IMAGE_OK, "a truncated stream must never decode as complete");
        Expect(status == IMAGE_TRUNCATED || status == IMAGE_CORRUPT,
               "a truncated stream must be reported as truncated or corrupt");
    }
}

LAIUE_TEST_ENTRY(InflateTestEntryPoint)
{
    // Несжатые блоки, в том числе разрезанные на отрезки.
    ExpectInflates(INFLATE_STORED_ZLIB, (uint32_t)sizeof(INFLATE_STORED_ZLIB),
                   INFLATE_STORED_RAW, (uint32_t)sizeof(INFLATE_STORED_RAW),
                   "a stored stream must inflate exactly");
    ExpectInflatesSplit(INFLATE_STORED_ZLIB, (uint32_t)sizeof(INFLATE_STORED_ZLIB),
                        INFLATE_STORED_RAW, (uint32_t)sizeof(INFLATE_STORED_RAW),
                        "a stored stream split across segments must inflate exactly");

    // Фиксированный Хаффман.
    ExpectInflates(INFLATE_FIXED_ZLIB, (uint32_t)sizeof(INFLATE_FIXED_ZLIB), INFLATE_FIXED_RAW,
                   (uint32_t)sizeof(INFLATE_FIXED_RAW),
                   "a fixed huffman stream must inflate exactly");
    ExpectInflatesSplit(INFLATE_FIXED_ZLIB, (uint32_t)sizeof(INFLATE_FIXED_ZLIB),
                        INFLATE_FIXED_RAW, (uint32_t)sizeof(INFLATE_FIXED_RAW),
                        "a fixed huffman stream split across segments must inflate exactly");

    // Дозаправка битов читает вперёд в заголовок следующего несжатого
    // блока: его первые байты приходят из битового буфера.
    ExpectInflates(INFLATE_DRAIN_ZLIB, (uint32_t)sizeof(INFLATE_DRAIN_ZLIB), INFLATE_DRAIN_RAW,
                   (uint32_t)sizeof(INFLATE_DRAIN_RAW),
                   "a stored block after a huffman block must inflate exactly");
    ExpectInflatesSplit(INFLATE_DRAIN_ZLIB, (uint32_t)sizeof(INFLATE_DRAIN_ZLIB),
                        INFLATE_DRAIN_RAW, (uint32_t)sizeof(INFLATE_DRAIN_RAW),
                        "a drained stored block split across segments must inflate exactly");
    // Разрез внутри байтов, уже забранных дозаправкой в битовый буфер.
    ExpectInflatesSplitAt(INFLATE_DRAIN_ZLIB, (uint32_t)sizeof(INFLATE_DRAIN_ZLIB),
                          INFLATE_DRAIN_RAW, (uint32_t)sizeof(INFLATE_DRAIN_RAW), 3u, 10u,
                          "a drained stored block cut inside the bit buffer must inflate");

    // Смешанный поток: фиксированный блок, затем несжатые.
    ExpectInflates(INFLATE_MIXED_ZLIB, (uint32_t)sizeof(INFLATE_MIXED_ZLIB), INFLATE_MIXED_RAW,
                   (uint32_t)sizeof(INFLATE_MIXED_RAW),
                   "a mixed stream must inflate exactly");
    ExpectInflatesSplit(INFLATE_MIXED_ZLIB, (uint32_t)sizeof(INFLATE_MIXED_ZLIB),
                        INFLATE_MIXED_RAW, (uint32_t)sizeof(INFLATE_MIXED_RAW),
                        "a mixed stream split across segments must inflate exactly");

    ExpectTruncated(INFLATE_STORED_ZLIB, (uint32_t)sizeof(INFLATE_STORED_ZLIB));
    ExpectTruncated(INFLATE_FIXED_ZLIB, (uint32_t)sizeof(INFLATE_FIXED_ZLIB));

    // Повреждённый NLEN: заголовок zlib (2), байт блока (1), LEN (2),
    // затем NLEN. Инвертируем младший байт NLEN.
    {
        for (uint32_t index = 0; index < (uint32_t)sizeof(g_damaged); ++index)
            g_damaged[index] = INFLATE_STORED_ZLIB[index];
        g_damaged[5] ^= 0xFFu;
        uint32_t written = 0u;
        InflateSegment segment = {g_damaged, (uint32_t)sizeof(g_damaged)};
        Expect(InflateZlib(&segment, 1u, g_output, sizeof(g_output), &g_work, sizeof(g_work),
                           &written) == IMAGE_CORRUPT,
               "a stored block with a wrong NLEN must be corrupt");
    }

    // Выходной буфер на байт меньше распакованного: несжатый блок обязан
    // быть отвергнут, а не переполнить буфер.
    {
        uint32_t written = 0u;
        InflateSegment segment = {INFLATE_STORED_ZLIB, (uint32_t)sizeof(INFLATE_STORED_ZLIB)};
        Expect(InflateZlib(&segment, 1u, g_short, (uint32_t)sizeof(g_short), &g_work,
                           sizeof(g_work), &written) == IMAGE_CORRUPT,
               "a short output buffer must be reported");
    }

    // Защитные проверки аргументов.
    {
        uint32_t written = 0u;
        InflateSegment segment = {INFLATE_STORED_ZLIB, (uint32_t)sizeof(INFLATE_STORED_ZLIB)};
        Expect(InflateZlib(NULL, 1u, g_output, sizeof(g_output), &g_work, sizeof(g_work),
                           &written) == IMAGE_INVALID_ARGUMENT,
               "null segments must be rejected");
        Expect(InflateZlib(&segment, 0u, g_output, sizeof(g_output), &g_work, sizeof(g_work),
                           &written) == IMAGE_INVALID_ARGUMENT,
               "zero segments must be rejected");
        Expect(InflateZlib(&segment, 1u, NULL, sizeof(g_output), &g_work, sizeof(g_work),
                           &written) == IMAGE_INVALID_ARGUMENT,
               "null output must be rejected");
        Expect(InflateZlib(&segment, 1u, g_output, sizeof(g_output), NULL, sizeof(g_work),
                           &written) == IMAGE_INVALID_ARGUMENT,
               "null work must be rejected");
        Expect(InflateZlib(&segment, 1u, g_output, sizeof(g_output), &g_work, 4u, &written) ==
                   IMAGE_INVALID_ARGUMENT,
               "a short work buffer must be rejected");
    }

    LaiueTestRuntimeWrite("inflate test passed\n");
    LAIUE_TEST_SUCCESS();
}
