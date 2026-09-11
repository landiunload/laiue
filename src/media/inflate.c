#include "media/inflate.h"

#include <stddef.h>

// Разбор кода: девять старших бит берутся из таблицы прямого поиска,
// более длинные коды читаются каноническим циклом по одному биту.
// Таблица строится из тех же длин, поэтому символ всегда тот же, что
// дал бы побитовый разбор: меняется только способ его найти.

#define LITERAL_SYMBOLS INFLATE_LITERAL_SYMBOLS
#define DISTANCE_SYMBOLS 30u
#define CODE_LENGTH_SYMBOLS 19u
#define MAX_CODE_BITS INFLATE_MAX_CODE_BITS
#define FAST_BITS INFLATE_FAST_BITS
#define FAST_SIZE INFLATE_FAST_SIZE

typedef struct InflateState
{
    const InflateSegment *segments;
    uint32_t segmentCount;
    uint32_t segmentIndex;
    uint32_t position;

    uint32_t bitBuffer;
    uint32_t bitCount;

    uint8_t *output;
    uint32_t outputBytes;
    uint32_t written;

    bool truncated;
    bool corrupt;
} InflateState;

static const uint16_t LENGTH_BASE[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const uint8_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint16_t DISTANCE_BASE[DISTANCE_SYMBOLS] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577,
};

static const uint8_t DISTANCE_EXTRA[DISTANCE_SYMBOLS] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

// Порядок длин кодов служебного алфавита задан стандартом и подобран
// так, чтобы у типичного потока хвост списка можно было не передавать.
static const uint8_t CODE_LENGTH_ORDER[CODE_LENGTH_SYMBOLS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

static uint32_t NextByte(InflateState *state)
{
    while (state->segmentIndex < state->segmentCount &&
           state->position >= state->segments[state->segmentIndex].size)
    {
        state->position = 0u;
        ++state->segmentIndex;
    }
    if (state->segmentIndex >= state->segmentCount)
    {
        state->truncated = true;
        return 0u;
    }
    return state->segments[state->segmentIndex].bytes[state->position++];
}

static uint32_t ReadBits(InflateState *state, uint32_t need)
{
    while (state->bitCount < need)
    {
        state->bitBuffer |= NextByte(state) << state->bitCount;
        state->bitCount += 8u;
        if (state->truncated) return 0u;
    }
    uint32_t value = state->bitBuffer & ((1u << need) - 1u);
    state->bitBuffer >>= need;
    state->bitCount -= need;
    return value;
}

// Дозаправка до need бит. В отличие от ReadBits не выставляет мусорные
// биты при нехватке: fast-путь должен увидеть, что бит меньше need, и
// уйти в канонический разбор.
static void EnsureBits(InflateState *state, uint32_t need)
{
    while (state->bitCount < need)
    {
        uint32_t byte = NextByte(state);
        if (state->truncated) return;
        state->bitBuffer |= byte << state->bitCount;
        state->bitCount += 8u;
    }
}

static int32_t DecodeSymbol(InflateState *state, const InflateHuffmanTable *table)
{
    EnsureBits(state, FAST_BITS);
    if (state->bitCount >= FAST_BITS)
    {
        uint16_t entry = table->fast[state->bitBuffer & (FAST_SIZE - 1u)];
        uint32_t length = (uint32_t)entry >> 9;
        if (length != 0u)
        {
            state->bitBuffer >>= length;
            state->bitCount -= length;
            return (int32_t)(entry & 0x1FFu);
        }
    }

    // Канонический разбор. Если дозаправка уже заметила конец потока,
    // когда биты ещё оставались, флаг снимается: прежний разбор узнал бы
    // об обрыве только исчерпав эти биты. Иначе счётчик выданных байт на
    // усечённом потоке разошёлся бы.
    state->truncated = false;
    int32_t code = 0;
    int32_t first = 0;
    int32_t index = 0;
    for (uint32_t length = 1; length <= MAX_CODE_BITS; ++length)
    {
        code |= (int32_t)ReadBits(state, 1u);
        if (state->truncated) return -1;

        int32_t count = table->count[length];
        if (code - count < first) return table->symbol[index + (code - first)];

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    state->corrupt = true;
    return -1;
}

// Заполняет таблицу прямого поиска всеми индексами, у которых младшие
// length бит равны коду. Код записан старшим битом вперёд, а битовый
// буфер отдаёт его младшим битом вперёд, поэтому индекс — обратный код.
static void FillFast(InflateHuffmanTable *table, uint32_t code, uint32_t length, uint32_t symbol)
{
    uint32_t reversed = 0u;
    for (uint32_t bit = 0; bit < length; ++bit)
    {
        reversed = (reversed << 1) | (code & 1u);
        code >>= 1;
    }
    uint16_t entry = (uint16_t)((length << 9) | (symbol & 0x1FFu));
    for (uint32_t index = reversed; index < FAST_SIZE; index += 1u << length)
    {
        table->fast[index] = entry;
    }
}

// Строит канонический код по длинам. Недогруженный набор (кодов меньше,
// чем позволяет длина) допустим только в вырожденном случае с одним
// символом; перегруженный — всегда повреждение.
static bool BuildTable(InflateHuffmanTable *table, const uint8_t *lengths, uint32_t symbolCount)
{
    for (uint32_t length = 0; length <= MAX_CODE_BITS; ++length) table->count[length] = 0;
    for (uint32_t index = 0; index < FAST_SIZE; ++index) table->fast[index] = 0u;
    for (uint32_t symbol = 0; symbol < symbolCount; ++symbol)
    {
        ++table->count[lengths[symbol]];
    }
    if (table->count[0] == (int16_t)symbolCount) return true;   // пустой алфавит

    int32_t left = 1;
    for (uint32_t length = 1; length <= MAX_CODE_BITS; ++length)
    {
        left <<= 1;
        left -= table->count[length];
        if (left < 0) return false;
    }

    // nextCode[length] — первый канонический код этой длины. Длины 0
    // (неиспользуемые символы) в стандартную формулу не входят.
    uint32_t nextCode[MAX_CODE_BITS + 1u];
    uint32_t code = 0u;
    for (uint32_t length = 1; length <= MAX_CODE_BITS; ++length)
    {
        code = (code + (length > 1u ? (uint32_t)table->count[length - 1u] : 0u)) << 1;
        nextCode[length] = code;
    }

    int16_t offsets[MAX_CODE_BITS + 2u];
    offsets[1] = 0;
    for (uint32_t length = 1; length <= MAX_CODE_BITS; ++length)
    {
        offsets[length + 1u] = (int16_t)(offsets[length] + table->count[length]);
    }
    for (uint32_t symbol = 0; symbol < symbolCount; ++symbol)
    {
        uint32_t length = lengths[symbol];
        if (length != 0u)
        {
            table->symbol[offsets[length]++] = (int16_t)symbol;
            uint32_t codeValue = nextCode[length]++;
            if (length <= FAST_BITS) FillFast(table, codeValue, length, symbol);
        }
    }
    return true;
}

// Выравнивание по границе байта: остаток бит текущего байта
// отбрасывается, а целые байты, уже попавшие в буфер дозаправкой,
// сохраняются — иначе они потерялись бы при переходе к несжатому блоку
// и к контрольной сумме.
static void AlignToByte(InflateState *state)
{
    uint32_t drop = state->bitCount & 7u;
    state->bitBuffer >>= drop;
    state->bitCount -= drop;
}

static uint32_t ReadAlignedByte(InflateState *state)
{
    if (state->bitCount >= 8u)
    {
        uint32_t value = state->bitBuffer & 0xFFu;
        state->bitBuffer >>= 8u;
        state->bitCount -= 8u;
        return value;
    }
    return NextByte(state);
}

static bool CopyStored(InflateState *state)
{
    // Несжатый блок выровнен по байту: остаток текущего байта отбрасывается.
    AlignToByte(state);

    uint32_t length = ReadAlignedByte(state);
    length |= ReadAlignedByte(state) << 8;
    uint32_t inverted = ReadAlignedByte(state);
    inverted |= ReadAlignedByte(state) << 8;
    if (state->truncated) return false;
    if (((length ^ 0xFFFFu) & 0xFFFFu) != inverted)
    {
        state->corrupt = true;
        return false;
    }
    if (length > state->outputBytes - state->written)
    {
        state->corrupt = true;
        return false;
    }

    for (uint32_t index = 0; index < length; ++index)
    {
        state->output[state->written++] = (uint8_t)ReadAlignedByte(state);
    }
    return !state->truncated;
}

static bool InflateBlock(InflateState *state, const InflateHuffmanTable *literals,
                         const InflateHuffmanTable *distances)
{
    for (;;)
    {
        int32_t symbol = DecodeSymbol(state, literals);
        if (symbol < 0) return false;

        if (symbol < 256)
        {
            if (state->written >= state->outputBytes)
            {
                state->corrupt = true;
                return false;
            }
            state->output[state->written++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) return true;

        symbol -= 257;
        if (symbol >= 29)
        {
            state->corrupt = true;
            return false;
        }
        uint32_t length = LENGTH_BASE[symbol] + ReadBits(state, LENGTH_EXTRA[symbol]);

        int32_t distanceSymbol = DecodeSymbol(state, distances);
        if (distanceSymbol < 0 || distanceSymbol >= (int32_t)DISTANCE_SYMBOLS)
        {
            state->corrupt = true;
            return false;
        }
        uint32_t distance =
            DISTANCE_BASE[distanceSymbol] + ReadBits(state, DISTANCE_EXTRA[distanceSymbol]);
        if (state->truncated) return false;

        if (distance > state->written || length > state->outputBytes - state->written)
        {
            state->corrupt = true;
            return false;
        }

        // Копирование побайтово намеренно: длина вправе перекрывать
        // источник, и именно так поток кодирует повторяющийся узор.
        uint32_t source = state->written - distance;
        for (uint32_t index = 0; index < length; ++index)
        {
            state->output[state->written++] = state->output[source++];
        }
    }
}

static void BuildFixedTables(InflateHuffmanTable *literals, InflateHuffmanTable *distances)
{
    uint8_t lengths[LITERAL_SYMBOLS];
    for (uint32_t symbol = 0; symbol < 144u; ++symbol) lengths[symbol] = 8u;
    for (uint32_t symbol = 144u; symbol < 256u; ++symbol) lengths[symbol] = 9u;
    for (uint32_t symbol = 256u; symbol < 280u; ++symbol) lengths[symbol] = 7u;
    for (uint32_t symbol = 280u; symbol < LITERAL_SYMBOLS; ++symbol) lengths[symbol] = 8u;
    BuildTable(literals, lengths, LITERAL_SYMBOLS);

    for (uint32_t symbol = 0; symbol < DISTANCE_SYMBOLS; ++symbol) lengths[symbol] = 5u;
    BuildTable(distances, lengths, DISTANCE_SYMBOLS);
}

static bool BuildDynamicTables(InflateState *state, InflateHuffmanTable *literals,
                               InflateHuffmanTable *distances, InflateHuffmanTable *codeLengths)
{
    uint32_t literalCount = ReadBits(state, 5u) + 257u;
    uint32_t distanceCount = ReadBits(state, 5u) + 1u;
    uint32_t codeLengthCount = ReadBits(state, 4u) + 4u;
    if (state->truncated) return false;
    if (literalCount > LITERAL_SYMBOLS || distanceCount > DISTANCE_SYMBOLS)
    {
        state->corrupt = true;
        return false;
    }

    uint8_t lengths[LITERAL_SYMBOLS + DISTANCE_SYMBOLS];
    for (uint32_t index = 0; index < CODE_LENGTH_SYMBOLS; ++index) lengths[index] = 0u;
    for (uint32_t index = 0; index < codeLengthCount; ++index)
    {
        lengths[CODE_LENGTH_ORDER[index]] = (uint8_t)ReadBits(state, 3u);
    }
    if (state->truncated) return false;

    if (!BuildTable(codeLengths, lengths, CODE_LENGTH_SYMBOLS))
    {
        state->corrupt = true;
        return false;
    }

    uint32_t total = literalCount + distanceCount;
    uint32_t index = 0;
    while (index < total)
    {
        int32_t symbol = DecodeSymbol(state, codeLengths);
        if (symbol < 0) return false;

        if (symbol < 16)
        {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }

        uint32_t repeat = 0u;
        uint8_t value = 0u;
        if (symbol == 16)
        {
            if (index == 0u)
            {
                state->corrupt = true;
                return false;
            }
            value = lengths[index - 1u];
            repeat = 3u + ReadBits(state, 2u);
        }
        else if (symbol == 17)
        {
            repeat = 3u + ReadBits(state, 3u);
        }
        else
        {
            repeat = 11u + ReadBits(state, 7u);
        }
        if (state->truncated) return false;
        if (index + repeat > total)
        {
            state->corrupt = true;
            return false;
        }
        while (repeat-- > 0u) lengths[index++] = value;
    }

    if (!BuildTable(literals, lengths, literalCount) ||
        !BuildTable(distances, lengths + literalCount, distanceCount))
    {
        state->corrupt = true;
        return false;
    }
    return true;
}

static uint32_t Adler32(const uint8_t *bytes, uint32_t size)
{
    uint32_t low = 1u;
    uint32_t high = 0u;
    uint32_t index = 0u;
    while (index < size)
    {
        // 5552 байта — предел, после которого сумма перестала бы
        // помещаться в 32 бита. Остаток берётся раз в блок, а не на
        // каждом байте: деление здесь дороже всего остального цикла.
        uint32_t block = size - index < 5552u ? size - index : 5552u;
        for (uint32_t step = 0; step < block; ++step)
        {
            low += bytes[index + step];
            high += low;
        }
        index += block;
        low %= 65521u;
        high %= 65521u;
    }
    return (high << 16) | low;
}

ImageStatus InflateZlib(const InflateSegment *segments, uint32_t segmentCount, void *output,
                        uint32_t outputBytes, void *work, uint32_t workBytes,
                        uint32_t *outWritten)
{
    if (segments == NULL || output == NULL || segmentCount == 0u) return IMAGE_INVALID_ARGUMENT;
    if (work == NULL || workBytes < (uint32_t)sizeof(InflateWork)) return IMAGE_INVALID_ARGUMENT;
    InflateWork *tables = (InflateWork *)work;
    tables->fixedReady = false;

    InflateState state = {
        .segments = segments,
        .segmentCount = segmentCount,
        .output = (uint8_t *)output,
        .outputBytes = outputBytes,
    };

    uint32_t compressionMethod = NextByte(&state);
    uint32_t flags = NextByte(&state);
    if (state.truncated) return IMAGE_TRUNCATED;
    if ((compressionMethod & 0x0Fu) != 8u) return IMAGE_CORRUPT;
    if (((compressionMethod << 8) | flags) % 31u != 0u) return IMAGE_CORRUPT;
    // Предустановленный словарь в PNG запрещён и здесь не поддерживается.
    if ((flags & 0x20u) != 0u) return IMAGE_UNSUPPORTED_FEATURE;

    bool final = false;
    while (!final)
    {
        final = ReadBits(&state, 1u) != 0u;
        uint32_t type = ReadBits(&state, 2u);
        if (state.truncated) return IMAGE_TRUNCATED;

        bool ok = false;
        if (type == 0u)
        {
            ok = CopyStored(&state);
        }
        else if (type == 1u)
        {
            if (!tables->fixedReady)
            {
                BuildFixedTables(&tables->fixedLiterals, &tables->fixedDistances);
                tables->fixedReady = true;
            }
            ok = InflateBlock(&state, &tables->fixedLiterals, &tables->fixedDistances);
        }
        else if (type == 2u)
        {
            ok = BuildDynamicTables(&state, &tables->literals, &tables->distances,
                                    &tables->codeLengths) &&
                 InflateBlock(&state, &tables->literals, &tables->distances);
        }
        else
        {
            state.corrupt = true;
        }

        if (!ok)
        {
            if (state.truncated) return IMAGE_TRUNCATED;
            return IMAGE_CORRUPT;
        }
    }

    // Хвост потока выровнен по байту и несёт Adler-32 распакованных
    // данных: выравнивание сохраняет байты, уже забранные дозаправкой.
    AlignToByte(&state);
    uint32_t checksum = ReadAlignedByte(&state) << 24;
    checksum |= ReadAlignedByte(&state) << 16;
    checksum |= ReadAlignedByte(&state) << 8;
    checksum |= ReadAlignedByte(&state);
    if (state.truncated) return IMAGE_TRUNCATED;
    if (checksum != Adler32(state.output, state.written)) return IMAGE_CORRUPT;

    if (outWritten != NULL) *outWritten = state.written;
    return IMAGE_OK;
}
