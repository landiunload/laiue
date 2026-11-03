#include "media/inflate.h"

#include <stddef.h>
#include <string.h>

#if defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#else
#include <emmintrin.h>
#endif

// Разбор кода: младшие биты буфера берутся из таблицы прямого поиска,
// более длинные коды читаются каноническим циклом по одному биту.
// Таблица строится из тех же длин, поэтому символ всегда тот же, что
// дал бы побитовый разбор: меняется только способ его найти.

#define LITERAL_SYMBOLS INFLATE_LITERAL_SYMBOLS
#define DISTANCE_SYMBOLS 30u
#define CODE_LENGTH_SYMBOLS 19u
#define MAX_CODE_BITS INFLATE_MAX_CODE_BITS
#define FAST_BITS INFLATE_FAST_BITS
#define FAST_SIZE INFLATE_FAST_SIZE

// Сколько бит общий разбор ждёт перед обращением к таблице и коды какой
// длины берёт из неё. Число своё, а не FAST_BITS: от него зависит, каким
// статусом и на каком байте закончится оборванный поток, а настройка
// скорости не должна менять ни того ни другого.
#define CAREFUL_BITS 9u
_Static_assert(CAREFUL_BITS <= FAST_BITS, "careful decoding must find its codes in the table");

// Запись быстрой таблицы, от младших бит к старшим: длина кода (0 — кода
// такой длины в таблице нет), число дополнительных бит, символ, основание
// длины или расстояния. Одна запись заменяет пять обращений к таблицам,
// а у неверных символов, конца блока и пустых индексов основание равно
// нулю: все исключения быстрого цикла проверяются одним сравнением.
#define ENTRY_CODE_BITS_MASK 15u
#define ENTRY_EXTRA_SHIFT 4u
#define ENTRY_EXTRA_MASK 15u
#define ENTRY_SYMBOL_SHIFT 8u
#define ENTRY_SYMBOL_MASK 0x1FFu
#define ENTRY_BASE_SHIFT 17u
#define ENTRY_NOT_LITERAL (256u << ENTRY_SYMBOL_SHIFT)
// Символа 511 нет ни в одном алфавите, а запись остаётся «не литералом».
#define ENTRY_MISS (ENTRY_SYMBOL_MASK << ENTRY_SYMBOL_SHIFT)

// Один шаг быстрого цикла разбирает три литерала либо до двух литералов
// и match, а кодов длиннее FAST_BITS он не разбирает. Первой дозаправки
// хватает на литерал и match, поэтому после двух литералов нужна вторая.
#define REFILL_MIN_BITS 56u
_Static_assert(3u * FAST_BITS + 5u + 13u <= REFILL_MIN_BITS,
               "literal and match must fit into one refill");

// Запас цикла: две дозаправки сдвигают вход не больше чем на семь байт и
// читают по восемь байт, а шаг пишет не больше двух литералов и match.
// Широкое копирование занимает два слова и в запас входит.
#define FAST_INPUT_MARGIN 16u
#define FAST_OUTPUT_MARGIN (2u + 258u)
#define COPY_WORD_BYTES 8u

// Основания и дополнительные биты символов алфавита, начиная с first.
typedef struct SymbolValues
{
    const uint16_t *base;
    const uint8_t *extra;
    uint32_t first;
    uint32_t count;
} SymbolValues;

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

static const SymbolValues LITERAL_VALUES = {LENGTH_BASE, LENGTH_EXTRA, 257u, 29u};
static const SymbolValues DISTANCE_VALUES = {DISTANCE_BASE, DISTANCE_EXTRA, 0u, DISTANCE_SYMBOLS};
static const SymbolValues NO_VALUES = {NULL, NULL, 0u, 0u};

static uint32_t NextByte(InflateState *state)
{
    // Общий случай — байт из текущего отрезка. Проверка границы вынесена
    // первой: на горячем пути сегмент не кончается, и второй while не нужен.
    uint32_t index = state->segmentIndex;
    uint32_t position = state->position;
    if (index < state->segmentCount && position < state->segments[index].size)
    {
        state->position = position + 1u;
        return state->segments[index].bytes[position];
    }
    while (index < state->segmentCount && position >= state->segments[index].size)
    {
        position = 0u;
        ++index;
    }
    if (index >= state->segmentCount)
    {
        state->segmentIndex = index;
        state->position = position;
        state->truncated = true;
        return 0u;
    }
    state->segmentIndex = index;
    state->position = position + 1u;
    return state->segments[index].bytes[position];
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
    EnsureBits(state, CAREFUL_BITS);
    if (state->bitCount >= CAREFUL_BITS)
    {
        uint32_t entry = table->fast[state->bitBuffer & (FAST_SIZE - 1u)];
        uint32_t length = entry & ENTRY_CODE_BITS_MASK;
        if (length != 0u && length <= CAREFUL_BITS)
        {
            state->bitBuffer >>= length;
            state->bitCount -= length;
            return (int32_t)((entry >> ENTRY_SYMBOL_SHIFT) & ENTRY_SYMBOL_MASK);
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
static void FillFast(InflateHuffmanTable *table, const SymbolValues *values, uint32_t code,
                     uint32_t length, uint32_t symbol)
{
    uint32_t reversed = 0u;
    for (uint32_t bit = 0; bit < length; ++bit)
    {
        reversed = (reversed << 1) | (code & 1u);
        code >>= 1;
    }
    uint32_t entry = length | (symbol << ENTRY_SYMBOL_SHIFT);
    if (symbol - values->first < values->count)
    {
        const uint32_t offset = symbol - values->first;
        entry |= (uint32_t)values->extra[offset] << ENTRY_EXTRA_SHIFT;
        entry |= (uint32_t)values->base[offset] << ENTRY_BASE_SHIFT;
    }
    for (uint32_t index = reversed; index < FAST_SIZE; index += 1u << length)
    {
        table->fast[index] = entry;
    }
}

// Строит канонический код по длинам. Недогруженный набор (кодов меньше,
// чем позволяет длина) допустим только в вырожденном случае с одним
// символом; перегруженный — всегда повреждение.
static bool BuildTable(InflateHuffmanTable *table, const SymbolValues *values,
                       const uint8_t *lengths, uint32_t symbolCount)
{
    for (uint32_t length = 0; length <= MAX_CODE_BITS; ++length) table->count[length] = 0;
    for (uint32_t index = 0; index < FAST_SIZE; ++index) table->fast[index] = ENTRY_MISS;
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
            if (length <= FAST_BITS) FillFast(table, values, codeValue, length, symbol);
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

    uint32_t remaining = length;
    // Целые байты, уже забранные дозаправкой в битовый буфер, идут
    // первыми: остаток текущего байта отброшен выравниванием выше.
    while (remaining != 0u && state->bitCount >= 8u)
    {
        state->output[state->written++] = (uint8_t)(state->bitBuffer & 0xFFu);
        state->bitBuffer >>= 8u;
        state->bitCount -= 8u;
        --remaining;
    }
    // Дальше данные лежат в отрезках IDAT непрерывно, поэтому копируются
    // кусками, а не по байту через ReadAlignedByte.
    while (remaining != 0u)
    {
        while (state->segmentIndex < state->segmentCount &&
               state->position >= state->segments[state->segmentIndex].size)
        {
            state->position = 0u;
            ++state->segmentIndex;
        }
        if (state->segmentIndex >= state->segmentCount)
        {
            // Прежний ReadAlignedByte на исчерпанном входе отдавал ноль,
            // поэтому остаток блока дописывается нулями, а счётчик
            // выданных байт доходит до конца блока — ровно как раньше.
            memset(state->output + state->written, 0, remaining);
            state->written += remaining;
            state->truncated = true;
            return false;
        }
        const InflateSegment *segment = &state->segments[state->segmentIndex];
        uint32_t available = segment->size - state->position;
        uint32_t take = remaining < available ? remaining : available;
        memcpy(state->output + state->written, segment->bytes + state->position, take);
        state->written += take;
        state->position += take;
        remaining -= take;
    }
    return true;
}

// Копирование match. Источник вправе перекрываться с приёмником: DEFLATE
// именно так кодирует повторяющийся узор, когда длина больше distance.
// Разбор по перекрытию:
//   distance >= 8 — внутри одного 8-байтового блока источник не догоняет
//                   приёмник (source+8 <= destination), широкое копирование;
//   distance == 1 — байт множится, memset;
//   distance < 8 и length <= distance — источник целиком до приёмника,
//                   побайтовое копирование (короткие match дешевле вызова memcpy);
//   1 < distance < 8 — первые unit байт (кратное distance, не меньше 8)
//                   выводятся побайтово, затем выданный период удваивается
//                   широким копированием.
// За конец выхода запись невозможна: length уже проверен против остатка
// буфера, а хвост копируется побайтово.
//
// Функция не встраивается: быстрому циклу она нужна редко, а внутри него
// заняла бы регистры, которых не хватает горячему пути.
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void CopyMatch(uint8_t *output, uint32_t written, uint32_t distance, uint32_t length)
{
    uint8_t *destination = output + written;
    const uint8_t *source = destination - distance;

    if (distance >= 8u)
    {
        uint32_t index = 0u;
        while (index + 8u <= length)
        {
            memcpy(destination + index, source + index, 8u);
            index += 8u;
        }
        while (index < length)
        {
            destination[index] = source[index];
            ++index;
        }
        return;
    }
    if (distance == 1u)
    {
        memset(destination, source[0], length);
        return;
    }
    if (length <= distance)
    {
        for (uint32_t index = 0u; index < length; ++index)
        {
            destination[index] = source[index];
        }
        return;
    }

    // 1 < distance < 8 и length > distance. Материализуем первые `unit`
    // байт (unit — кратное distance, не меньше 8), затем удваиваем уже
    // выданный период. Удвоение корректно, потому что длина выданной части
    // всегда кратна distance, а байт i равен байту i-distance.
    uint32_t unit = distance;
    while (unit < 8u) unit += distance;
    uint32_t index = 0u;
    while (index < unit && index < length)
    {
        destination[index] = index < distance ? source[index] : destination[index - distance];
        ++index;
    }
    while (index < length)
    {
        uint32_t remaining = length - index;
        uint32_t copy = index < remaining ? index : remaining;
        memcpy(destination + index, destination, copy);
        index += copy;
    }
}

// Восемь байт младшим вперёд. Сборка по байтам не зависит от порядка
// байт процессора, а Clang и GCC сводят её к одному чтению.
static uint64_t ReadWord(const uint8_t *bytes)
{
    return (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8) | ((uint64_t)bytes[2] << 16) |
           ((uint64_t)bytes[3] << 24) | ((uint64_t)bytes[4] << 32) | ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) | ((uint64_t)bytes[7] << 56);
}

// Без ветвлений: bitCount становится не меньше REFILL_MIN_BITS, а input
// сдвигается на столько целых байт, сколько вошло в буфер.
static void Refill(uint64_t *buffer, uint32_t *bitCount, const uint8_t **input)
{
    *buffer |= ReadWord(*input) << *bitCount;
    *input += (63u - *bitCount) >> 3;
    *bitCount |= REFILL_MIN_BITS;
}

static void CopyWord(uint8_t *destination, const uint8_t *source)
{
    uint64_t word;
    memcpy(&word, source, sizeof(word));
    memcpy(destination, &word, sizeof(word));
}

static void TakeLiteral(uint32_t entry, uint64_t *buffer, uint32_t *bitCount, uint8_t **cursor)
{
    const uint32_t bits = entry & ENTRY_CODE_BITS_MASK;
    *buffer >>= bits;
    *bitCount -= bits;
    *(*cursor)++ = (uint8_t)(entry >> ENTRY_SYMBOL_SHIFT);
}

static uint32_t ExtraValue(uint64_t buffer, uint32_t extraBits)
{
    return (uint32_t)buffer & ((1u << extraBits) - 1u);
}

// Нужно ли вообще входить в быстрый цикл: на краю входа или выхода его
// шаг не поместится, и символы разбирает InflateBlock.
static bool FastLoopFits(const InflateState *state)
{
    if (state->segmentIndex >= state->segmentCount) return false;
    return state->segments[state->segmentIndex].size - state->position > FAST_INPUT_MARGIN &&
           state->outputBytes - state->written > FAST_OUTPUT_MARGIN;
}

// Середина блока: вход и выход заведомо не кончатся за шаг, поэтому их
// границы проверяются раз на шаг, а не на символ. Всё нештатное — конец
// блока, код длиннее таблицы, ошибка в потоке, край входа или выхода —
// цикл оставляет разбору в InflateBlock, не трогая этот символ: статусы и
// счётчики на повреждённых данных определяет один код. Поэтому match
// разбирается без сдвига буфера, а биты снимаются только после проверок.
//
// Цикл дочитывает вход и пишет выход с запасом: биты выше bitCount в
// buffer совпадают с теми, что прочитаются следом, а байты после match
// перекроет следующий символ. Отрезок читается только внутри его границ.
static void InflateFast(InflateState *state, const InflateHuffmanTable *literalTable,
                        const InflateHuffmanTable *distanceTable)
{
    const InflateSegment *segment = &state->segments[state->segmentIndex];
    const uint8_t *const start = segment->bytes + state->position;
    const uint8_t *const inputLimit = segment->bytes + segment->size - FAST_INPUT_MARGIN;
    const uint8_t *input = start;
    uint8_t *const output = state->output;
    uint8_t *const outputLimit = output + state->outputBytes - FAST_OUTPUT_MARGIN;
    uint8_t *cursor = output + state->written;
    const uint32_t *const literals = literalTable->fast;
    const uint32_t *const distances = distanceTable->fast;
    uint64_t buffer = state->bitBuffer;
    uint32_t bitCount = state->bitCount;

    while (input < inputLimit && cursor < outputLimit)
    {
        Refill(&buffer, &bitCount, &input);

        uint32_t entry = literals[buffer & (FAST_SIZE - 1u)];
        if ((entry & ENTRY_NOT_LITERAL) == 0u)
        {
            TakeLiteral(entry, &buffer, &bitCount, &cursor);
            entry = literals[buffer & (FAST_SIZE - 1u)];
            if ((entry & ENTRY_NOT_LITERAL) == 0u)
            {
                TakeLiteral(entry, &buffer, &bitCount, &cursor);
                entry = literals[buffer & (FAST_SIZE - 1u)];
                if ((entry & ENTRY_NOT_LITERAL) == 0u)
                {
                    TakeLiteral(entry, &buffer, &bitCount, &cursor);
                    continue;
                }
                Refill(&buffer, &bitCount, &input);
            }
        }
        if (entry >> ENTRY_BASE_SHIFT == 0u) break;

        uint32_t used = entry & ENTRY_CODE_BITS_MASK;
        uint32_t extraBits = (entry >> ENTRY_EXTRA_SHIFT) & ENTRY_EXTRA_MASK;
        const uint32_t length = (entry >> ENTRY_BASE_SHIFT) + ExtraValue(buffer >> used, extraBits);
        used += extraBits;

        entry = distances[(buffer >> used) & (FAST_SIZE - 1u)];
        if (entry >> ENTRY_BASE_SHIFT == 0u) break;
        used += entry & ENTRY_CODE_BITS_MASK;
        extraBits = (entry >> ENTRY_EXTRA_SHIFT) & ENTRY_EXTRA_MASK;
        const uint32_t distance = (entry >> ENTRY_BASE_SHIFT) + ExtraValue(buffer >> used, extraBits);
        used += extraBits;
        if (distance > (size_t)(cursor - output)) break;

        buffer >>= used;
        bitCount -= used;
        if (distance >= COPY_WORD_BYTES && length <= 2u * COPY_WORD_BYTES)
        {
            CopyWord(cursor, cursor - distance);
            CopyWord(cursor + COPY_WORD_BYTES, cursor + COPY_WORD_BYTES - distance);
        }
        else
        {
            CopyMatch(output, (uint32_t)(cursor - output), distance, length);
        }
        cursor += length;
    }

    // Целые байты, до которых разбор не дошёл, возвращаются во вход. Биты,
    // принесённые из прежнего отрезка, вернуть некуда: они остаются в буфере.
    uint32_t unread = bitCount >> 3;
    const uint32_t loaded = (uint32_t)(input - start);
    if (unread > loaded) unread = loaded;
    input -= unread;
    bitCount -= unread * 8u;
    state->bitBuffer = (uint32_t)buffer & ((1u << bitCount) - 1u);
    state->bitCount = bitCount;
    state->position = (uint32_t)(input - segment->bytes);
    state->written = (uint32_t)(cursor - output);
}

static bool InflateBlock(InflateState *state, const InflateHuffmanTable *literals,
                         const InflateHuffmanTable *distances)
{
    for (;;)
    {
        if (FastLoopFits(state)) InflateFast(state, literals, distances);

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

        // Копирование через CopyMatch: длина вправе перекрывать источник,
        // и именно так поток кодирует повторяющийся узор.
        CopyMatch(state->output, state->written, distance, length);
        state->written += length;
    }
}

static void BuildFixedTables(InflateHuffmanTable *literals, InflateHuffmanTable *distances)
{
    uint8_t lengths[LITERAL_SYMBOLS];
    for (uint32_t symbol = 0; symbol < 144u; ++symbol) lengths[symbol] = 8u;
    for (uint32_t symbol = 144u; symbol < 256u; ++symbol) lengths[symbol] = 9u;
    for (uint32_t symbol = 256u; symbol < 280u; ++symbol) lengths[symbol] = 7u;
    for (uint32_t symbol = 280u; symbol < LITERAL_SYMBOLS; ++symbol) lengths[symbol] = 8u;
    BuildTable(literals, &LITERAL_VALUES, lengths, LITERAL_SYMBOLS);

    for (uint32_t symbol = 0; symbol < DISTANCE_SYMBOLS; ++symbol) lengths[symbol] = 5u;
    BuildTable(distances, &DISTANCE_VALUES, lengths, DISTANCE_SYMBOLS);
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

    if (!BuildTable(codeLengths, &NO_VALUES, lengths, CODE_LENGTH_SYMBOLS))
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

    if (!BuildTable(literals, &LITERAL_VALUES, lengths, literalCount) ||
        !BuildTable(distances, &DISTANCE_VALUES, lengths + literalCount, distanceCount))
    {
        state->corrupt = true;
        return false;
    }
    return true;
}

// Шестнадцать байт одной формулой вместо шестнадцати шагов low += byte,
// high += low: каждый байт входит в high с весом, равным числу байт от
// него до конца куска включительно.
#define ADLER_CHUNK_BYTES 16u

#if defined(_M_ARM64) || defined(__aarch64__)

static void AdlerAddChunk(const uint8_t *chunk, uint32_t *low, uint32_t *high)
{
    static const uint16_t FIRST_WEIGHTS[8] = {16u, 15u, 14u, 13u, 12u, 11u, 10u, 9u};
    static const uint16_t SECOND_WEIGHTS[8] = {8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u};
    const uint8x16_t bytes = vld1q_u8(chunk);
    const uint16x8_t first = vmulq_u16(vmovl_u8(vget_low_u8(bytes)), vld1q_u16(FIRST_WEIGHTS));
    const uint16x8_t second = vmulq_u16(vmovl_u8(vget_high_u8(bytes)), vld1q_u16(SECOND_WEIGHTS));
    *high += ADLER_CHUNK_BYTES * *low + vaddvq_u16(vaddq_u16(first, second));
    *low += vaddlvq_u8(bytes);
}

#else

static void AdlerAddChunk(const uint8_t *chunk, uint32_t *low, uint32_t *high)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i bytes = _mm_loadu_si128((const __m128i *)chunk);
    const __m128i sums = _mm_sad_epu8(bytes, zero);
    const __m128i first = _mm_madd_epi16(_mm_unpacklo_epi8(bytes, zero),
                                         _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10, 9));
    const __m128i second =
        _mm_madd_epi16(_mm_unpackhi_epi8(bytes, zero), _mm_setr_epi16(8, 7, 6, 5, 4, 3, 2, 1));
    __m128i weighted = _mm_add_epi32(first, second);
    weighted = _mm_add_epi32(weighted, _mm_shuffle_epi32(weighted, _MM_SHUFFLE(1, 0, 3, 2)));
    weighted = _mm_add_epi32(weighted, _mm_shuffle_epi32(weighted, _MM_SHUFFLE(2, 3, 0, 1)));
    *high += ADLER_CHUNK_BYTES * *low + (uint32_t)_mm_cvtsi128_si32(weighted);
    *low += (uint32_t)_mm_cvtsi128_si32(sums) + (uint32_t)_mm_extract_epi16(sums, 4);
}

#endif

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
        uint32_t step = 0u;
        for (; block - step >= ADLER_CHUNK_BYTES; step += ADLER_CHUNK_BYTES)
        {
            AdlerAddChunk(bytes + index + step, &low, &high);
        }
        for (; step < block; ++step)
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
