// Быстрый цикл распаковки. Остальные тесты inflate коротки и до него не
// доходят: циклу нужен запас на входе и сотни байт запаса на выходе.
//
// Длинные потоки собирает кодировщик прямо здесь, поэтому ожидаемый выход
// известен без декодера. Блоки чередуются: фиксированные коды, динамические
// с кодами длиннее таблицы прямого поиска и несжатые, которые читают байты,
// возвращённые циклом во вход. Проверяются разрезы потока на отрезки, обрыв,
// выход ровно по размеру и на байт меньше, защитная зона за выходом и ошибки
// посреди блока, где цикл обязан уступить разбор общему коду.
#include "media/inflate.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RAW_CAPACITY (1u << 17)
#define STREAM_CAPACITY (RAW_CAPACITY * 2u)
#define GUARD_BYTES 64u
#define GUARD_VALUE 0xA5u
#define MAX_SEGMENTS 8u
#define BLOCK_RAW_BYTES 12000u
#define BLOCK_COUNT 5u
#define TRUNCATION_POINTS 32u

#define LITERAL_ALPHABET 288u
#define DISTANCE_ALPHABET 30u
#define CODE_LENGTH_ALPHABET 19u
#define DYNAMIC_LITERALS 286u
#define END_OF_BLOCK 256u
#define INVALID_LENGTH_SYMBOL 286u
#define INVALID_DISTANCE_CODE 30u

typedef enum BlockKind
{
    BLOCK_STORED = 0,
    BLOCK_FIXED = 1,
    BLOCK_DYNAMIC = 2,
} BlockKind;

// Доли в процентах: литералы среди символов, перекрывающиеся match среди
// match и match длиннее широкого копирования цикла.
typedef struct Scenario
{
    uint32_t literalPercent;
    uint32_t shortDistancePercent;
    uint32_t longLengthPercent;
} Scenario;

static const Scenario SCENARIOS[] = {
    {100u, 0u, 0u}, {90u, 10u, 10u}, {50u, 20u, 10u}, {10u, 30u, 30u}, {0u, 100u, 100u}, {0u, 0u, 0u},
};

static const uint16_t LENGTH_BASE[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t DISTANCE_BASE[DISTANCE_ALPHABET] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const uint8_t DISTANCE_EXTRA[DISTANCE_ALPHABET] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
static const uint8_t CODE_LENGTH_ORDER[CODE_LENGTH_ALPHABET] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

static uint8_t g_raw[RAW_CAPACITY];
static uint8_t g_stream[STREAM_CAPACITY];
static uint8_t g_output[RAW_CAPACITY + GUARD_BYTES];
static InflateWork g_work;

static uint32_t g_rawBytes;
static uint32_t g_streamBytes;
static uint64_t g_bits;
static uint32_t g_bitCount;
static uint32_t g_random;

static uint8_t g_literalLengths[LITERAL_ALPHABET];
static uint16_t g_literalCodes[LITERAL_ALPHABET];
static uint8_t g_distanceLengths[DISTANCE_ALPHABET];
static uint16_t g_distanceCodes[DISTANCE_ALPHABET];

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("inflate fast test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t Random(uint32_t limit)
{
    g_random = g_random * 1664525u + 1013904223u;
    return (g_random >> 8) % limit;
}

static void PutBits(uint32_t value, uint32_t count)
{
    g_bits |= (uint64_t)value << g_bitCount;
    g_bitCount += count;
    while (g_bitCount >= 8u)
    {
        Expect(g_streamBytes < STREAM_CAPACITY, "the stream must fit its buffer");
        g_stream[g_streamBytes++] = (uint8_t)g_bits;
        g_bits >>= 8;
        g_bitCount -= 8u;
    }
}

// Код Хаффмана идёт в поток старшим битом вперёд.
static void PutCode(uint32_t code, uint32_t length)
{
    uint32_t reversed = 0u;
    for (uint32_t bit = 0u; bit < length; ++bit)
    {
        reversed |= ((code >> bit) & 1u) << (length - 1u - bit);
    }
    PutBits(reversed, length);
}

// Канонические коды по длинам, как в RFC 1951, раздел 3.2.2.
static void AssignCodes(const uint8_t *lengths, uint16_t *codes, uint32_t count)
{
    uint32_t lengthCount[INFLATE_MAX_CODE_BITS + 1u] = {0u};
    uint32_t nextCode[INFLATE_MAX_CODE_BITS + 1u] = {0u};
    for (uint32_t symbol = 0u; symbol < count; ++symbol) ++lengthCount[lengths[symbol]];
    lengthCount[0] = 0u;
    uint32_t code = 0u;
    for (uint32_t bits = 1u; bits <= INFLATE_MAX_CODE_BITS; ++bits)
    {
        code = (code + lengthCount[bits - 1u]) << 1;
        nextCode[bits] = code;
    }
    for (uint32_t symbol = 0u; symbol < count; ++symbol)
    {
        if (lengths[symbol] != 0u) codes[symbol] = (uint16_t)nextCode[lengths[symbol]]++;
    }
}

static void UseFixedCodes(void)
{
    for (uint32_t symbol = 0u; symbol < LITERAL_ALPHABET; ++symbol)
    {
        g_literalLengths[symbol] = symbol < 144u ? 8u : symbol < 256u ? 9u : symbol < 280u ? 7u : 8u;
    }
    AssignCodes(g_literalLengths, g_literalCodes, LITERAL_ALPHABET);
    for (uint32_t symbol = 0u; symbol < DISTANCE_ALPHABET; ++symbol) g_distanceLengths[symbol] = 5u;
    AssignCodes(g_distanceLengths, g_distanceCodes, DISTANCE_ALPHABET);
}

// Коды длиннее таблицы прямого поиска: так цикл покидает символ посреди
// литералов, на длине match и на расстоянии. Код неполный, для разбора
// это допустимо, а тесту не нужны лишние символы.
static void UseLongCodes(void)
{
    for (uint32_t symbol = 0u; symbol < DYNAMIC_LITERALS; ++symbol)
    {
        g_literalLengths[symbol] = symbol < 128u ? 8u : symbol < 248u ? 12u : symbol < 256u ? 15u : 8u;
    }
    AssignCodes(g_literalLengths, g_literalCodes, DYNAMIC_LITERALS);
    for (uint32_t symbol = 0u; symbol < DISTANCE_ALPHABET; ++symbol)
    {
        g_distanceLengths[symbol] = symbol < 20u ? 5u : symbol < 26u ? 11u : 15u;
    }
    AssignCodes(g_distanceLengths, g_distanceCodes, DISTANCE_ALPHABET);
}

static void BeginBlock(BlockKind kind, bool final)
{
    PutBits((final ? 1u : 0u) | ((uint32_t)kind << 1), 3u);
    if (kind == BLOCK_FIXED) UseFixedCodes();
    if (kind != BLOCK_DYNAMIC) return;

    UseLongCodes();
    PutBits(DYNAMIC_LITERALS - 257u, 5u);
    PutBits(DISTANCE_ALPHABET - 1u, 5u);
    PutBits(CODE_LENGTH_ALPHABET - 4u, 4u);
    uint8_t codeLengths[CODE_LENGTH_ALPHABET];
    uint16_t codes[CODE_LENGTH_ALPHABET];
    for (uint32_t symbol = 0u; symbol < CODE_LENGTH_ALPHABET; ++symbol)
    {
        codeLengths[symbol] = symbol < 13u ? 4u : 5u;
    }
    AssignCodes(codeLengths, codes, CODE_LENGTH_ALPHABET);
    for (uint32_t index = 0u; index < CODE_LENGTH_ALPHABET; ++index)
    {
        PutBits(codeLengths[CODE_LENGTH_ORDER[index]], 3u);
    }
    for (uint32_t symbol = 0u; symbol < DYNAMIC_LITERALS; ++symbol)
    {
        const uint32_t length = g_literalLengths[symbol];
        PutCode(codes[length], codeLengths[length]);
    }
    for (uint32_t symbol = 0u; symbol < DISTANCE_ALPHABET; ++symbol)
    {
        const uint32_t length = g_distanceLengths[symbol];
        PutCode(codes[length], codeLengths[length]);
    }
}

static void PutSymbol(uint32_t symbol)
{
    PutCode(g_literalCodes[symbol], g_literalLengths[symbol]);
}

static void EmitLiteral(uint32_t value)
{
    PutSymbol(value);
    g_raw[g_rawBytes++] = (uint8_t)value;
}

static void PutMatch(uint32_t length, uint32_t distance)
{
    uint32_t lengthIndex = 28u;
    while (LENGTH_BASE[lengthIndex] > length) --lengthIndex;
    PutSymbol(257u + lengthIndex);
    PutBits(length - LENGTH_BASE[lengthIndex], LENGTH_EXTRA[lengthIndex]);

    uint32_t distanceIndex = DISTANCE_ALPHABET - 1u;
    while (DISTANCE_BASE[distanceIndex] > distance) --distanceIndex;
    PutCode(g_distanceCodes[distanceIndex], g_distanceLengths[distanceIndex]);
    PutBits(distance - DISTANCE_BASE[distanceIndex], DISTANCE_EXTRA[distanceIndex]);
}

static void EmitMatch(uint32_t length, uint32_t distance)
{
    PutMatch(length, distance);
    for (uint32_t index = 0u; index < length; ++index)
    {
        g_raw[g_rawBytes] = g_raw[g_rawBytes - distance];
        ++g_rawBytes;
    }
}

static void EmitStored(uint32_t size)
{
    if (g_bitCount != 0u) PutBits(0u, 8u - g_bitCount);
    PutBits(size, 16u);
    PutBits(size ^ 0xFFFFu, 16u);
    for (uint32_t index = 0u; index < size; ++index)
    {
        const uint32_t value = Random(256u);
        PutBits(value, 8u);
        g_raw[g_rawBytes++] = (uint8_t)value;
    }
}

static uint32_t ChooseDistance(const Scenario *scenario)
{
    if (Random(100u) < scenario->shortDistancePercent)
    {
        const uint32_t distance = 1u + Random(7u);
        return distance < g_rawBytes ? distance : g_rawBytes;
    }
    const uint32_t reach = g_rawBytes < 32768u ? g_rawBytes : 32768u;
    const uint32_t span = Random(2u) == 0u && reach > 300u ? 300u : reach;
    return span <= 8u ? span : 8u + Random(span - 7u);
}

static void EmitSymbols(const Scenario *scenario, uint32_t size)
{
    const uint32_t target = g_rawBytes + size;
    while (g_rawBytes < target)
    {
        if (g_rawBytes == 0u || Random(100u) < scenario->literalPercent)
        {
            EmitLiteral(Random(8u) == 0u ? Random(256u) : Random(128u));
            continue;
        }
        const uint32_t distance = ChooseDistance(scenario);
        uint32_t length = 3u + Random(14u);
        if (Random(100u) < scenario->longLengthPercent)
        {
            length = Random(4u) == 0u ? 258u : 17u + Random(242u);
        }
        EmitMatch(length, distance);
    }
}

static void BeginStream(uint32_t seed)
{
    g_random = seed;
    g_rawBytes = 0u;
    g_streamBytes = 0u;
    g_bits = 0u;
    g_bitCount = 0u;
    g_stream[g_streamBytes++] = 0x78u;
    g_stream[g_streamBytes++] = 0x01u;
}

static uint32_t Adler32(const uint8_t *bytes, uint32_t size)
{
    uint32_t low = 1u;
    uint32_t high = 0u;
    for (uint32_t index = 0u; index < size; ++index)
    {
        low = (low + bytes[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

static void EndStream(void)
{
    if (g_bitCount != 0u) PutBits(0u, 8u - g_bitCount);
    const uint32_t checksum = Adler32(g_raw, g_rawBytes);
    for (uint32_t shift = 32u; shift != 0u; shift -= 8u)
    {
        PutBits((checksum >> (shift - 8u)) & 0xFFu, 8u);
    }
}

static ImageStatus Inflate(const InflateSegment *segments, uint32_t count, uint32_t outputBytes,
                           uint32_t *written)
{
    memset(g_output, GUARD_VALUE, sizeof(g_output));
    return InflateZlib(segments, count, g_output, outputBytes, &g_work, sizeof(g_work), written);
}

static void ExpectGuard(uint32_t outputBytes)
{
    for (uint32_t index = 0u; index < GUARD_BYTES; ++index)
    {
        Expect(g_output[outputBytes + index] == GUARD_VALUE, "nothing may land past the output");
    }
}

static void ExpectWhole(const InflateSegment *segments, uint32_t count, uint32_t outputBytes)
{
    uint32_t written = 0u;
    Expect(Inflate(segments, count, outputBytes, &written) == IMAGE_OK, "the stream must inflate");
    Expect(written == g_rawBytes, "the whole stream must be written");
    Expect(memcmp(g_output, g_raw, g_rawBytes) == 0, "the bytes must match");
    ExpectGuard(outputBytes);
}

// Случайные разрезы, в том числе пустые и очень короткие отрезки, на
// которых цикл не умещается и символ приходит с двух отрезков сразу.
static void ExpectSplits(void)
{
    InflateSegment segments[MAX_SEGMENTS];
    for (uint32_t pass = 0u; pass < 6u; ++pass)
    {
        const uint32_t tiny = pass % 2u == 0u ? 40u : 4000u;
        uint32_t position = 0u;
        for (uint32_t index = 0u; index + 1u < MAX_SEGMENTS; ++index)
        {
            const uint32_t left = g_streamBytes - position;
            const uint32_t limit = left < tiny ? left : tiny;
            const uint32_t size = Random(limit + 1u);
            segments[index].bytes = g_stream + position;
            segments[index].size = size;
            position += size;
        }
        segments[MAX_SEGMENTS - 1u].bytes = g_stream + position;
        segments[MAX_SEGMENTS - 1u].size = g_streamBytes - position;
        ExpectWhole(segments, MAX_SEGMENTS, g_rawBytes);
    }
}

static void ExpectFailures(void)
{
    uint32_t written = 0u;
    for (uint32_t index = 0u; index < TRUNCATION_POINTS; ++index)
    {
        const InflateSegment truncated = {g_stream, Random(g_streamBytes)};
        const ImageStatus status = Inflate(&truncated, 1u, g_rawBytes, &written);
        Expect(status == IMAGE_TRUNCATED || status == IMAGE_CORRUPT, "a cut stream must fail");
        ExpectGuard(g_rawBytes);
    }

    const InflateSegment whole = {g_stream, g_streamBytes};
    Expect(Inflate(&whole, 1u, g_rawBytes - 1u, &written) == IMAGE_CORRUPT,
           "a stream larger than the output must fail");
    ExpectGuard(g_rawBytes - 1u);

    g_stream[g_streamBytes - 1u] ^= 1u;
    Expect(Inflate(&whole, 1u, g_rawBytes, &written) == IMAGE_CORRUPT,
           "a wrong checksum must fail");
    g_stream[g_streamBytes - 1u] ^= 1u;
}

static void ExpectScenario(const Scenario *scenario, uint32_t seed)
{
    static const BlockKind KINDS[BLOCK_COUNT] = {
        BLOCK_DYNAMIC, BLOCK_FIXED, BLOCK_STORED, BLOCK_DYNAMIC, BLOCK_FIXED,
    };
    BeginStream(seed);
    for (uint32_t index = 0u; index < BLOCK_COUNT; ++index)
    {
        const bool final = index + 1u == BLOCK_COUNT;
        BeginBlock(KINDS[index], final);
        if (KINDS[index] == BLOCK_STORED)
        {
            EmitStored(BLOCK_RAW_BYTES);
            continue;
        }
        EmitSymbols(scenario, BLOCK_RAW_BYTES);
        PutSymbol(END_OF_BLOCK);
    }
    EndStream();
    Expect(g_rawBytes <= RAW_CAPACITY, "the output must fit its buffer");

    const InflateSegment whole = {g_stream, g_streamBytes};
    ExpectWhole(&whole, 1u, g_rawBytes);
    ExpectWhole(&whole, 1u, RAW_CAPACITY);
    ExpectSplits();
    ExpectFailures();
}

// Поток, где после тысячи литералов идёт ошибка, а за ней символы: цикл
// доходит до неё с запасом и обязан оставить решение общему разбору.
static void BeginDamagedStream(void)
{
    BeginStream(11u);
    BeginBlock(BLOCK_FIXED, true);
    for (uint32_t index = 0u; index < 1000u; ++index) EmitLiteral(Random(256u));
}

static void ExpectDamaged(const char *message)
{
    for (uint32_t index = 0u; index < 600u; ++index) PutSymbol(Random(256u));
    PutSymbol(END_OF_BLOCK);
    EndStream();
    const InflateSegment whole = {g_stream, g_streamBytes};
    uint32_t written = 0u;
    Expect(Inflate(&whole, 1u, RAW_CAPACITY, &written) == IMAGE_CORRUPT, message);
}

LAIUE_TEST_ENTRY(InflateFastTestEntryPoint)
{
    const uint32_t count = (uint32_t)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0]));
    for (uint32_t index = 0u; index < count; ++index)
    {
        ExpectScenario(&SCENARIOS[index], 1000u + index);
    }

    BeginDamagedStream();
    PutMatch(20u, 1001u);
    ExpectDamaged("a distance beyond the output must fail");

    BeginDamagedStream();
    PutSymbol(INVALID_LENGTH_SYMBOL);
    ExpectDamaged("a length symbol outside the alphabet must fail");

    BeginDamagedStream();
    PutSymbol(257u);
    PutCode(INVALID_DISTANCE_CODE, 5u);
    ExpectDamaged("a distance symbol outside the alphabet must fail");

    LaiueTestRuntimeWrite("inflate fast test passed\n");
    LAIUE_TEST_SUCCESS();
}
