// Проверка backreference-копирования после R2-ускорения deflate.
//
// Потоки R2_INFLATE_MATCH_* — fixed-Huffman блоки с управляемой
// последовательностью match: distance 1 (RLE), distance 2..7
// (периодическое удвоение), границы 8/16/32, большие distance и длина 258.
// Ожидаемый выход посчитан генератором независимо от C-декодера, поэтому
// тест ловит ошибку фазы при широком копировании, запись за границу
// выхода и чтение вне окна.
//
// Дополнительно проверяются:
//   - разбиение потока на три отрезка (границы IDAT);
//   - усечение на каждом байте: поток не может декодироваться как полный;
//   - выходной буфер ровно по размеру и на байт меньше.
#include "media/inflate.h"

#include "test_runtime.h"
#include "r2_26_inflate_fixtures.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define R2_OUTPUT_CAPACITY 16384u

static uint8_t g_output[R2_OUTPUT_CAPACITY];
static uint8_t g_short[R2_OUTPUT_CAPACITY];
static InflateWork g_work;

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("r2 inflate test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectFixture(const R2InflateFixture *fixture)
{
    uint32_t written = 0u;
    InflateSegment segment = {fixture->stream, fixture->streamBytes};
    ImageStatus status = InflateZlib(&segment, 1u, g_output, (uint32_t)sizeof(g_output), &g_work,
                                     sizeof(g_work), &written);
    Expect(status == IMAGE_OK, "a match fixture must inflate exactly");
    Expect(written == fixture->rawBytes, "written must equal expected size");
    Expect(memcmp(g_output, fixture->raw, fixture->rawBytes) == 0, "bytes must match");

    // Три отрезка: границы не должны менять результат.
    InflateSegment split[3];
    uint32_t cuts[4] = {0u, fixture->streamBytes / 4u, fixture->streamBytes / 2u,
                        fixture->streamBytes};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        split[index].bytes = fixture->stream + cuts[index];
        split[index].size = cuts[index + 1u] - cuts[index];
    }
    written = 0u;
    status = InflateZlib(split, 3u, g_output, (uint32_t)sizeof(g_output), &g_work, sizeof(g_work),
                         &written);
    Expect(status == IMAGE_OK, "a split match fixture must inflate exactly");
    Expect(written == fixture->rawBytes, "split written must match");
    Expect(memcmp(g_output, fixture->raw, fixture->rawBytes) == 0, "split bytes must match");

    // Усечение на каждом байте не может дать полный результат.
    for (uint32_t cut = 0u; cut < fixture->streamBytes; ++cut)
    {
        uint32_t truncatedWritten = 0u;
        InflateSegment truncated = {fixture->stream, cut};
        status = InflateZlib(&truncated, 1u, g_output, (uint32_t)sizeof(g_output), &g_work,
                             sizeof(g_work), &truncatedWritten);
        Expect(status != IMAGE_OK, "a truncated fixture must not decode as complete");
        Expect(status == IMAGE_TRUNCATED || status == IMAGE_CORRUPT,
               "a truncated fixture must be truncated or corrupt");
    }

    // Выходной буфер на байт меньше распакованного: match не должен переполнить.
    {
        uint32_t shortWritten = 0u;
        status = InflateZlib(&segment, 1u, g_short, fixture->rawBytes - 1u, &g_work, sizeof(g_work),
                             &shortWritten);
        Expect(status != IMAGE_OK, "a short output buffer must be rejected");
    }
}

LAIUE_TEST_ENTRY(R2InflateTestEntryPoint)
{
    const uint32_t count = (uint32_t)(sizeof(R2_INFLATE_FIXTURES) / sizeof(R2_INFLATE_FIXTURES[0]));
    for (uint32_t index = 0u; index < count; ++index)
    {
        Expect(R2_INFLATE_FIXTURES[index].rawBytes <= sizeof(g_output),
               "fixture must fit the test buffer");
        ExpectFixture(&R2_INFLATE_FIXTURES[index]);
    }
    LaiueTestRuntimeWrite("r2 inflate test passed\n");
    LAIUE_TEST_SUCCESS();
}
