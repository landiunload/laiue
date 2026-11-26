#include "media/jpeg_decode.h"

#include <stddef.h>

#if defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#else
#include <emmintrin.h>
#endif

// Без оптимизации встраивание только складывает временные значения всех
// вызовов в один кадр: он перерастает страницу и требует __chkstk.
#if defined(_MSC_VER) && !defined(__clang__)
#define JPEG_FORCE_INLINE __forceinline
#define JPEG_NOINLINE __declspec(noinline)
#elif defined(__OPTIMIZE__)
#define JPEG_FORCE_INLINE inline __attribute__((always_inline))
#define JPEG_NOINLINE __attribute__((noinline))
#else
#define JPEG_FORCE_INLINE inline
#define JPEG_NOINLINE __attribute__((noinline))
#endif

#define JPEG_MAX_COMPONENTS 4u
#define JPEG_MAX_SAMPLING 4u
#define JPEG_HUFFMAN_TABLES 8u
#define JPEG_QUANT_TABLES 4u
#define JPEG_FAST_BITS 9u
#define JPEG_FAST_SIZE (1u << JPEG_FAST_BITS)

// Порядок обхода коэффициентов внутри блока: ZIGZAG[k] — естественное
// место k-го коэффициента. Прогрессивные проходы хранят коэффициенты
// зигзагом, потому что спектральный диапазон задан его номерами, и
// переставляют блок один раз, перед обратным ДКП. Последовательный проход
// сразу кладёт коэффициент на его место, а таблицы квантования хранятся
// в естественном порядке, как их читает обратное ДКП.
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Обратное ДКП — целочисленное islow из IJG libjpeg: бабочка Лёффлера,
// Лигтенберга и Мошица, константы в 13 битах, после первого прохода два
// бита запаса. Вместе с фильтром цветности и переводом цвета ниже оно
// даёт те же байты, что libjpeg-turbo с настройками по умолчанию, на
// любом процессоре: в целых числах нечему расходиться.
//
// Считается сразу восемь столбцов в 16-битных дорожках, а произведения в
// 32-битных. У настоящего файла значения туда умещаются, а на испорченном
// дорожки переполняются и насыщаются одинаково на SSE2 и NEON: команды
// ниже подобраны с одним поведением, и пиксели остаются теми же и там.
#define IDCT_CONST_BITS 13
#define IDCT_PASS1_BITS 2
#define IDCT_FIX_0_298631336 2446
#define IDCT_FIX_0_390180644 3196
#define IDCT_FIX_0_541196100 4433
#define IDCT_FIX_0_765366865 6270
#define IDCT_FIX_0_899976223 7373
#define IDCT_FIX_1_175875602 9633
#define IDCT_FIX_1_501321110 12299
#define IDCT_FIX_1_847759065 15137
#define IDCT_FIX_1_961570560 16069
#define IDCT_FIX_2_053119869 16819
#define IDCT_FIX_2_562915447 20995
#define IDCT_FIX_3_072711026 25172

typedef struct JpegHuffman
{
    bool present;
    // Каноническая таблица по T.81 F.15: коды длины length лежат между
    // minCode[length] и maxCode[length], а их значения начинаются с
    // valuePointer[length]. maxCode = -1 означает, что кодов такой длины
    // в таблице нет.
    int32_t minCode[17];
    int32_t maxCode[17];
    int32_t valuePointer[17];
    uint8_t values[256];
    // Таблица прямого поиска по старшим битам, запись даёт CodeEntry.
    // Ноль означает код длиннее JPEG_FAST_BITS: его разбирает
    // LongCodeEntry. Символ и длина при этом те же.
    uint16_t fast[JPEG_FAST_SIZE];
} JpegHuffman;

typedef struct JpegComponent
{
    uint8_t id;
    uint8_t horizontal;
    uint8_t vertical;
    uint8_t quantTable;
    uint8_t dcTable;
    uint8_t acTable;
    int32_t dcPrediction;
    // Блоков в буфере: округлено вверх до целого MCU, потому что
    // чередующийся проход пишет блоки и за краем картинки.
    uint32_t blocksPerLine;
    uint32_t blocksPerColumn;
    // Блоков в самой картинке: одиночный проход обходит только их.
    uint32_t scanBlocksPerLine;
    uint32_t scanBlocksPerColumn;
    // Отсчётов в самом компоненте: за их пределами в буфере лежит
    // продолжение края, которое кодировщик дописал до целого блока.
    uint32_t plainWidth;
    uint32_t plainHeight;
    uint32_t sampleWidth;
    uint8_t *samples;
    int16_t *coefficients;
} JpegComponent;

typedef struct JpegFrame
{
    uint32_t width;
    uint32_t height;
    uint32_t componentCount;
    uint32_t maxHorizontal;
    uint32_t maxVertical;
    uint32_t mcusPerLine;
    uint32_t mcusPerColumn;
    bool progressive;
    // Adobe APP14 отличает RGB, записанный без преобразования, от
    // обычного YCbCr. Без маркера три компонента всегда YCbCr.
    bool adobe;
    uint8_t adobeTransform;
    JpegComponent components[JPEG_MAX_COMPONENTS];
} JpegFrame;

typedef struct JpegLayout
{
    uint32_t huffman;
    uint32_t quant;
    uint32_t samples[JPEG_MAX_COMPONENTS];
    uint32_t coefficients[JPEG_MAX_COMPONENTS];
    uint32_t rows;
    uint32_t total;
} JpegLayout;

typedef struct JpegReader
{
    const uint8_t *bytes;
    uint32_t sizeBytes;
    uint32_t position;
    uint64_t bitBuffer;
    uint32_t bitCount;
    // Встретился маркер: энтропийные данные кончились, и дальше
    // выдаются нули. Так повреждённый файл не уходит за конец буфера.
    bool markerReached;
} JpegReader;

typedef struct JpegDecoder
{
    JpegFrame frame;
    JpegReader reader;
    JpegHuffman *huffman;
    uint16_t *quant;
    bool quantPresent[JPEG_QUANT_TABLES];
    uint32_t restartInterval;
    uint32_t eobRun;
} JpegDecoder;

// Биты на время блока лежат в регистрах: поля читателя компилятор
// перечитывал бы после каждой записи коэффициента. Следующий бит потока —
// старший в buffer, ниже count бит только нули.
typedef struct JpegWindow
{
    uint64_t buffer;
    uint32_t count;
} JpegWindow;

// Столько бит окно гарантирует после дозаправки: самый длинный код и
// значение за ним.
#define JPEG_WINDOW_BITS 32u

static uint32_t ReadU16Be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 8) | (uint32_t)bytes[1];
}

static uint32_t ReadU32Be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uint32_t AlignUp8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
{
    return (value + divisor - 1u) / divisor;
}

// Следующий байт энтропийных данных. Возвращает false, когда кончился
// буфер или встретился маркер: байт тогда не выдаётся, а позиция
// остаётся на маркере, чтобы его нашли рестарт и разбор сегмента.
static bool NextDataByte(JpegReader *reader, uint32_t *outByte)
{
    if (reader->markerReached || reader->position >= reader->sizeBytes)
    {
        reader->markerReached = true;
        return false;
    }
    uint8_t byte = reader->bytes[reader->position];
    reader->position += 1u;
    if (byte == 0xFFu)
    {
        // Заполняющих 0xFF перед маркером может быть сколько угодно.
        uint32_t look = reader->position;
        while (look < reader->sizeBytes && reader->bytes[look] == 0xFFu) look += 1u;
        uint8_t next = look < reader->sizeBytes ? reader->bytes[look] : 0xD9u;
        if (next == 0x00u)
        {
            reader->position = look + 1u;
        }
        else
        {
            reader->position -= 1u;
            reader->markerReached = true;
            return false;
        }
    }
    *outByte = byte;
    return true;
}

// Дозаправка: байты добавляются, пока их в буфере не станет больше 56.
// Маркер не проглатывается: на нём и за концом данных буфер добирается
// нулями, поэтому разбор испорченного или обрезанного файла доходит до
// конца прохода, не выходя за его данные.
static JPEG_NOINLINE void FillBits(JpegReader *reader)
{
    while (reader->bitCount <= 56u)
    {
        uint32_t byte = 0u;
        (void)NextDataByte(reader, &byte);
        reader->bitBuffer |= (uint64_t)byte << (56u - reader->bitCount);
        reader->bitCount += 8u;
    }
}

static JPEG_FORCE_INLINE JpegWindow LoadWindow(const JpegReader *reader)
{
    const JpegWindow window = {reader->bitBuffer, reader->bitCount};
    return window;
}

static JPEG_FORCE_INLINE void SaveWindow(JpegReader *reader, JpegWindow window)
{
    reader->bitBuffer = window.buffer;
    reader->bitCount = window.count;
}

// Четыре байта без 0xFF берутся разом: среди них нет ни маркера, ни
// вставленного нуля. Остальное идёт байт за байтом.
static JPEG_FORCE_INLINE void RefillWindow(JpegReader *reader, JpegWindow *window)
{
    if (window->count >= JPEG_WINDOW_BITS) return;
    if (reader->sizeBytes - reader->position >= 4u)
    {
        const uint32_t word = ReadU32Be(reader->bytes + reader->position);
        if (((~word - 0x01010101u) & word & 0x80808080u) == 0u)
        {
            window->buffer |= (uint64_t)word << (32u - window->count);
            window->count += 32u;
            reader->position += 4u;
            return;
        }
    }
    SaveWindow(reader, *window);
    FillBits(reader);
    *window = LoadWindow(reader);
}

// count — от 1 до 16: длиннее значений в JPEG нет.
static JPEG_FORCE_INLINE uint32_t TakeBits(JpegWindow *window, uint32_t count)
{
    const uint32_t bits = (uint32_t)(window->buffer >> (64u - count));
    window->buffer <<= count;
    window->count -= count;
    return bits;
}

// Запись таблицы прямого поиска: длина кода вместе с битами значения,
// которые объявляет младшая тетрада символа, и сам символ. Значение
// забирается заодно с кодом: сдвиг на обе длины сразу короче цепочки
// зависимостей от символа к символу.
static uint32_t CodeEntry(uint32_t length, uint32_t symbol)
{
    return ((length + (symbol & 15u)) << 8) | symbol;
}

// Код длиннее JPEG_FAST_BITS по каноническим таблицам, peek — следующие
// 16 бит. Ноль значит, что такого кода нет.
static JPEG_NOINLINE uint32_t LongCodeEntry(const JpegHuffman *table, uint32_t peek)
{
    for (uint32_t length = JPEG_FAST_BITS + 1u; length <= 16u; ++length)
    {
        const int32_t code = (int32_t)(peek >> (16u - length));
        if (table->maxCode[length] >= 0 && code <= table->maxCode[length])
        {
            const int32_t index = table->valuePointer[length] + code - table->minCode[length];
            if (index < 0 || index >= 256) return 0u;
            return CodeEntry(length, table->values[index]);
        }
    }
    return 0u;
}

// Символ следующего кода или -1, если такого кода нет. В младших битах
// outBits — значение, которое объявил символ. В окне должно быть не
// меньше JPEG_WINDOW_BITS бит.
static JPEG_FORCE_INLINE int32_t TakeCode(const JpegHuffman *table, JpegWindow *window,
                                          uint32_t *outBits)
{
    uint32_t entry = table->fast[window->buffer >> (64u - JPEG_FAST_BITS)];
    if (entry == 0u)
    {
        entry = LongCodeEntry(table, (uint32_t)(window->buffer >> 48));
        if (entry == 0u) return -1;
    }
    const uint32_t length = entry >> 8;
    *outBits = (uint32_t)(window->buffer >> (64u - length));
    window->buffer <<= length;
    window->count -= length;
    return (int32_t)(entry & 0xFFu);
}

// T.81 F.12: младшие length бит дают знаковое значение. Без ветвления:
// знак у каждого коэффициента непредсказуем.
static JPEG_FORCE_INLINE int32_t Extend(uint32_t bits, uint32_t length)
{
    const int32_t range = (int32_t)1 << length;
    const int32_t value = (int32_t)bits & (range - 1);
    const int32_t negative = (value - (range >> 1)) >> 31;
    return value + (negative & (1 - range));
}

// Заполняет таблицу прямого поиска всеми индексами, у которых старшие
// length бит равны коду: код JPEG читается старшим битом вперёд, как и
// лежит в буфере.
static void FillFast(JpegHuffman *table, uint32_t code, uint32_t length, uint8_t symbol)
{
    uint32_t step = 1u << (JPEG_FAST_BITS - length);
    uint32_t base = code << (JPEG_FAST_BITS - length);
    uint16_t entry = (uint16_t)CodeEntry(length, symbol);
    for (uint32_t index = 0; index < step; ++index) table->fast[base + index] = entry;
}

static ImageStatus BuildHuffman(JpegHuffman *table, const uint8_t *counts, const uint8_t *values,
                                uint32_t valueCount)
{
    int32_t code = 0;
    uint32_t taken = 0u;
    for (uint32_t index = 0; index < JPEG_FAST_SIZE; ++index) table->fast[index] = 0u;
    for (uint32_t length = 1u; length <= 16u; ++length)
    {
        uint32_t count = counts[length - 1u];
        table->valuePointer[length] = (int32_t)taken;
        table->minCode[length] = code;
        code += (int32_t)count;
        taken += count;
        if (count != 0u)
        {
            table->maxCode[length] = code - 1;
            // Кодов длины length не бывает больше, чем 2 в степени length.
            if (code > ((int32_t)1 << length)) return IMAGE_CORRUPT;
        }
        else
        {
            table->maxCode[length] = -1;
        }
        code <<= 1;
    }
    if (taken != valueCount) return IMAGE_CORRUPT;
    for (uint32_t index = 0; index < valueCount; ++index) table->values[index] = values[index];
    // Быстрый путь строится после проверки: до неё counts вправе
    // ссылаться за пределы списка значений.
    for (uint32_t length = 1u; length <= JPEG_FAST_BITS; ++length)
    {
        uint32_t count = counts[length - 1u];
        for (uint32_t index = 0; index < count; ++index)
        {
            FillFast(table, (uint32_t)(table->minCode[length] + (int32_t)index), length,
                     values[table->valuePointer[length] + (int32_t)index]);
        }
    }
    table->present = true;
    return IMAGE_OK;
}

// Сдвиг вправо с округлением к ближайшему. У отрицательного сдвиг
// арифметический, как и в переводе цвета.
static int64_t IdctDescale(int64_t value, uint32_t bits)
{
    return (value + ((int64_t)1 << (bits - 1u))) >> bits;
}

// Отсчёт после обратного ДКП: возврат уровня 128 и насыщение.
static uint8_t IdctSample(int64_t value)
{
    value += 128;
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

#if defined(_M_ARM64) || defined(__aarch64__)

typedef uint8x16_t Lanes8;
typedef int16x8_t Lanes16;
typedef struct Lanes32
{
    int32x4_t low;
    int32x4_t high;
} Lanes32;

static JPEG_FORCE_INLINE Lanes16 LoadProduct(const int16_t *block, const uint16_t *quant)
{
    return vmulq_s16(vld1q_s16(block), vreinterpretq_s16_u16(vld1q_u16(quant)));
}

static JPEG_FORCE_INLINE void StoreSamples(uint8_t *line, Lanes16 lanes)
{
    const uint8x8_t centred = vreinterpret_u8_s8(vqmovn_s16(lanes));
    vst1_u8(line, vadd_u8(centred, vdup_n_u8(128u)));
}

static JPEG_FORCE_INLINE Lanes16 Add16(Lanes16 first, Lanes16 second)
{
    return vaddq_s16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 Subtract16(Lanes16 first, Lanes16 second)
{
    return vsubq_s16(first, second);
}

// first * firstFactor + second * secondFactor.
static JPEG_FORCE_INLINE Lanes32 Combine(Lanes16 first, Lanes16 second, int16_t firstFactor,
                                         int16_t secondFactor)
{
    const Lanes32 result = {
        vmlal_n_s16(vmull_n_s16(vget_low_s16(first), firstFactor), vget_low_s16(second),
                    secondFactor),
        vmlal_n_s16(vmull_n_s16(vget_high_s16(first), firstFactor), vget_high_s16(second),
                    secondFactor),
    };
    return result;
}

// Умножение на 2^IDCT_CONST_BITS.
static JPEG_FORCE_INLINE Lanes32 Scale(Lanes16 lanes)
{
    const Lanes32 result = {
        vshll_n_s16(vget_low_s16(lanes), IDCT_CONST_BITS),
        vshll_n_s16(vget_high_s16(lanes), IDCT_CONST_BITS),
    };
    return result;
}

static JPEG_FORCE_INLINE Lanes32 Add32(Lanes32 first, Lanes32 second)
{
    const Lanes32 result = {vaddq_s32(first.low, second.low), vaddq_s32(first.high, second.high)};
    return result;
}

static JPEG_FORCE_INLINE Lanes32 Subtract32(Lanes32 first, Lanes32 second)
{
    const Lanes32 result = {vsubq_s32(first.low, second.low), vsubq_s32(first.high, second.high)};
    return result;
}

// Сдвиг вправо с округлением и насыщением до 16 бит. Сложение и сдвиг
// отдельными командами: готовая vqrshrn не переполняется при округлении,
// а paddd на SSE2 переполняется.
static JPEG_FORCE_INLINE Lanes16 Narrow(Lanes32 lanes, int32_t bits)
{
    const int32x4_t half = vdupq_n_s32((int32_t)1 << (bits - 1));
    const int32x4_t shift = vdupq_n_s32(-bits);
    return vcombine_s16(vqmovn_s32(vshlq_s32(vaddq_s32(lanes.low, half), shift)),
                        vqmovn_s32(vshlq_s32(vaddq_s32(lanes.high, half), shift)));
}

static JPEG_FORCE_INLINE Lanes16 ZipLow16(Lanes16 first, Lanes16 second)
{
    return vzip1q_s16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh16(Lanes16 first, Lanes16 second)
{
    return vzip2q_s16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipLow32(Lanes16 first, Lanes16 second)
{
    return vreinterpretq_s16_s32(
        vzip1q_s32(vreinterpretq_s32_s16(first), vreinterpretq_s32_s16(second)));
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh32(Lanes16 first, Lanes16 second)
{
    return vreinterpretq_s16_s32(
        vzip2q_s32(vreinterpretq_s32_s16(first), vreinterpretq_s32_s16(second)));
}

static JPEG_FORCE_INLINE Lanes16 ZipLow64(Lanes16 first, Lanes16 second)
{
    return vreinterpretq_s16_s64(
        vzip1q_s64(vreinterpretq_s64_s16(first), vreinterpretq_s64_s16(second)));
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh64(Lanes16 first, Lanes16 second)
{
    return vreinterpretq_s16_s64(
        vzip2q_s64(vreinterpretq_s64_s16(first), vreinterpretq_s64_s16(second)));
}

static JPEG_FORCE_INLINE Lanes16 Repeat16(int16_t value)
{
    return vdupq_n_s16(value);
}

static JPEG_FORCE_INLINE Lanes8 LoadBytes(const uint8_t *bytes)
{
    return vld1q_u8(bytes);
}

static JPEG_FORCE_INLINE Lanes16 WidenLow(Lanes8 bytes)
{
    return vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(bytes)));
}

static JPEG_FORCE_INLINE Lanes16 WidenHigh(Lanes8 bytes)
{
    return vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(bytes)));
}

// Восемь байт в восемь дорожек.
static JPEG_FORCE_INLINE Lanes16 LoadWidened(const uint8_t *bytes)
{
    return vreinterpretq_s16_u16(vmovl_u8(vld1_u8(bytes)));
}

static JPEG_FORCE_INLINE Lanes8 NarrowToBytes(Lanes16 low, Lanes16 high)
{
    return vcombine_u8(vqmovun_s16(low), vqmovun_s16(high));
}

// Дорожки от 0 до 255: пары байт even[i], odd[i].
static JPEG_FORCE_INLINE void StorePairs(uint8_t *out, Lanes16 even, Lanes16 odd)
{
    vst1q_u8(out, vreinterpretq_u8_s16(vorrq_s16(even, vshlq_n_s16(odd, 8))));
}

static JPEG_FORCE_INLINE void StoreRgba(uint8_t *out, Lanes8 red, Lanes8 green, Lanes8 blue)
{
    const uint8x16x4_t pixels = {{red, green, blue, vdupq_n_u8(255u)}};
    vst4q_u8(out, pixels);
}

// Сдвиг вправо неотрицательных дорожек.
static JPEG_FORCE_INLINE Lanes16 ShiftDown16(Lanes16 lanes, int16_t bits)
{
    return vshlq_s16(lanes, vdupq_n_s16((int16_t)-bits));
}

// Значения дорожки слева от текущего: первое берётся из предыдущего шага.
static JPEG_FORCE_INLINE Lanes16 LanesBefore(Lanes16 previous, Lanes16 current)
{
    return vextq_s16(previous, current, 7);
}

// Значения дорожки справа: последнее берётся из следующего шага.
static JPEG_FORCE_INLINE Lanes16 LanesAfter(Lanes16 current, Lanes16 next)
{
    return vextq_s16(current, next, 1);
}

// (value * factor + 2^15) >> 16 для |value| меньше 2^14.
static JPEG_FORCE_INLINE Lanes16 MultiplyFraction(Lanes16 value, int16_t factor)
{
    return vrshrq_n_s16(vqdmulhq_s16(value, vdupq_n_s16(factor)), 1);
}

#else

typedef __m128i Lanes8;
typedef __m128i Lanes16;
typedef struct Lanes32
{
    __m128i low;
    __m128i high;
} Lanes32;

static JPEG_FORCE_INLINE Lanes16 LoadProduct(const int16_t *block, const uint16_t *quant)
{
    return _mm_mullo_epi16(_mm_loadu_si128((const __m128i *)block),
                           _mm_loadu_si128((const __m128i *)quant));
}

static JPEG_FORCE_INLINE void StoreSamples(uint8_t *line, Lanes16 lanes)
{
    const __m128i centred = _mm_packs_epi16(lanes, lanes);
    _mm_storel_epi64((__m128i *)line, _mm_add_epi8(centred, _mm_set1_epi8(-128)));
}

static JPEG_FORCE_INLINE Lanes16 Add16(Lanes16 first, Lanes16 second)
{
    return _mm_add_epi16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 Subtract16(Lanes16 first, Lanes16 second)
{
    return _mm_sub_epi16(first, second);
}

// first * firstFactor + second * secondFactor.
static JPEG_FORCE_INLINE Lanes32 Combine(Lanes16 first, Lanes16 second, int16_t firstFactor,
                                         int16_t secondFactor)
{
    const __m128i factors = _mm_set1_epi32((int32_t)(((uint32_t)(uint16_t)secondFactor << 16) |
                                                     (uint16_t)firstFactor));
    const Lanes32 result = {
        _mm_madd_epi16(_mm_unpacklo_epi16(first, second), factors),
        _mm_madd_epi16(_mm_unpackhi_epi16(first, second), factors),
    };
    return result;
}

// Умножение на 2^IDCT_CONST_BITS: значение ложится в старшую половину
// 32-битной дорожки и спускается арифметическим сдвигом.
static JPEG_FORCE_INLINE Lanes32 Scale(Lanes16 lanes)
{
    const __m128i zero = _mm_setzero_si128();
    const Lanes32 result = {
        _mm_srai_epi32(_mm_unpacklo_epi16(zero, lanes), 16 - IDCT_CONST_BITS),
        _mm_srai_epi32(_mm_unpackhi_epi16(zero, lanes), 16 - IDCT_CONST_BITS),
    };
    return result;
}

static JPEG_FORCE_INLINE Lanes32 Add32(Lanes32 first, Lanes32 second)
{
    const Lanes32 result = {_mm_add_epi32(first.low, second.low),
                            _mm_add_epi32(first.high, second.high)};
    return result;
}

static JPEG_FORCE_INLINE Lanes32 Subtract32(Lanes32 first, Lanes32 second)
{
    const Lanes32 result = {_mm_sub_epi32(first.low, second.low),
                            _mm_sub_epi32(first.high, second.high)};
    return result;
}

// Сдвиг вправо с округлением и насыщением до 16 бит.
static JPEG_FORCE_INLINE Lanes16 Narrow(Lanes32 lanes, int32_t bits)
{
    const __m128i half = _mm_set1_epi32((int32_t)1 << (bits - 1));
    return _mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(lanes.low, half), bits),
                           _mm_srai_epi32(_mm_add_epi32(lanes.high, half), bits));
}

static JPEG_FORCE_INLINE Lanes16 ZipLow16(Lanes16 first, Lanes16 second)
{
    return _mm_unpacklo_epi16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh16(Lanes16 first, Lanes16 second)
{
    return _mm_unpackhi_epi16(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipLow32(Lanes16 first, Lanes16 second)
{
    return _mm_unpacklo_epi32(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh32(Lanes16 first, Lanes16 second)
{
    return _mm_unpackhi_epi32(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipLow64(Lanes16 first, Lanes16 second)
{
    return _mm_unpacklo_epi64(first, second);
}

static JPEG_FORCE_INLINE Lanes16 ZipHigh64(Lanes16 first, Lanes16 second)
{
    return _mm_unpackhi_epi64(first, second);
}

static JPEG_FORCE_INLINE Lanes16 Repeat16(int16_t value)
{
    return _mm_set1_epi16(value);
}

static JPEG_FORCE_INLINE Lanes8 LoadBytes(const uint8_t *bytes)
{
    return _mm_loadu_si128((const __m128i *)bytes);
}

static JPEG_FORCE_INLINE Lanes16 WidenLow(Lanes8 bytes)
{
    return _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
}

static JPEG_FORCE_INLINE Lanes16 WidenHigh(Lanes8 bytes)
{
    return _mm_unpackhi_epi8(bytes, _mm_setzero_si128());
}

// Восемь байт в восемь дорожек.
static JPEG_FORCE_INLINE Lanes16 LoadWidened(const uint8_t *bytes)
{
    return WidenLow(_mm_loadl_epi64((const __m128i *)bytes));
}

static JPEG_FORCE_INLINE Lanes8 NarrowToBytes(Lanes16 low, Lanes16 high)
{
    return _mm_packus_epi16(low, high);
}

// Дорожки от 0 до 255: пары байт even[i], odd[i].
static JPEG_FORCE_INLINE void StorePairs(uint8_t *out, Lanes16 even, Lanes16 odd)
{
    _mm_storeu_si128((__m128i *)out, _mm_or_si128(even, _mm_slli_epi16(odd, 8)));
}

static JPEG_FORCE_INLINE void StoreRgba(uint8_t *out, Lanes8 red, Lanes8 green, Lanes8 blue)
{
    const __m128i alpha = _mm_set1_epi8(-1);
    const __m128i lowColors = _mm_unpacklo_epi8(red, green);
    const __m128i highColors = _mm_unpackhi_epi8(red, green);
    const __m128i lowRest = _mm_unpacklo_epi8(blue, alpha);
    const __m128i highRest = _mm_unpackhi_epi8(blue, alpha);
    _mm_storeu_si128((__m128i *)out, _mm_unpacklo_epi16(lowColors, lowRest));
    _mm_storeu_si128((__m128i *)(out + 16), _mm_unpackhi_epi16(lowColors, lowRest));
    _mm_storeu_si128((__m128i *)(out + 32), _mm_unpacklo_epi16(highColors, highRest));
    _mm_storeu_si128((__m128i *)(out + 48), _mm_unpackhi_epi16(highColors, highRest));
}

// Сдвиг вправо неотрицательных дорожек.
static JPEG_FORCE_INLINE Lanes16 ShiftDown16(Lanes16 lanes, int16_t bits)
{
    return _mm_srli_epi16(lanes, bits);
}

// Значения дорожки слева от текущего: первое берётся из предыдущего шага.
static JPEG_FORCE_INLINE Lanes16 LanesBefore(Lanes16 previous, Lanes16 current)
{
    return _mm_or_si128(_mm_srli_si128(previous, 14), _mm_slli_si128(current, 2));
}

// Значения дорожки справа: последнее берётся из следующего шага.
static JPEG_FORCE_INLINE Lanes16 LanesAfter(Lanes16 current, Lanes16 next)
{
    return _mm_or_si128(_mm_srli_si128(current, 2), _mm_slli_si128(next, 14));
}

// (value * factor + 2^15) >> 16 для |value| меньше 2^14: удвоенное
// значение даёт старшую половину с лишним битом для округления.
static JPEG_FORCE_INLINE Lanes16 MultiplyFraction(Lanes16 value, int16_t factor)
{
    const __m128i doubled = _mm_mulhi_epi16(_mm_add_epi16(value, value), _mm_set1_epi16(factor));
    return _mm_srai_epi16(_mm_add_epi16(doubled, _mm_set1_epi16(1)), 1);
}

#endif

// Одномерное islow над восемью входами, по значению в каждой дорожке.
// Формула раскрыта так, чтобы каждое произведение брало по два 16-битных
// входа: суммы вроде (in[2] + in[6]) * c в 16 бит не всегда поместились бы
// вместе с константой.
static JPEG_FORCE_INLINE void InverseTransform8(const Lanes16 *in, Lanes16 *out, int32_t descale)
{
    // Чётная часть, поворот на sqrt(2) * c6.
    const Lanes32 even2 = Combine(in[2], in[6], IDCT_FIX_0_541196100,
                                  IDCT_FIX_0_541196100 - IDCT_FIX_1_847759065);
    const Lanes32 even3 = Combine(in[2], in[6], IDCT_FIX_0_541196100 + IDCT_FIX_0_765366865,
                                  IDCT_FIX_0_541196100);
    const Lanes32 even0 = Scale(Add16(in[0], in[4]));
    const Lanes32 even1 = Scale(Subtract16(in[0], in[4]));
    const Lanes32 sum10 = Add32(even0, even3);
    const Lanes32 sum13 = Subtract32(even0, even3);
    const Lanes32 sum11 = Add32(even1, even2);
    const Lanes32 sum12 = Subtract32(even1, even2);

    // Нечётная часть: входы 7, 5, 3 и 1.
    const Lanes16 z3 = Add16(in[7], in[3]);
    const Lanes16 z4 = Add16(in[5], in[1]);
    const Lanes32 rotated3 = Combine(z3, z4, IDCT_FIX_1_175875602 - IDCT_FIX_1_961570560,
                                     IDCT_FIX_1_175875602);
    const Lanes32 rotated4 = Combine(z3, z4, IDCT_FIX_1_175875602,
                                     IDCT_FIX_1_175875602 - IDCT_FIX_0_390180644);
    const Lanes32 odd0 =
        Add32(Combine(in[7], in[1], IDCT_FIX_0_298631336 - IDCT_FIX_0_899976223,
                      -IDCT_FIX_0_899976223),
              rotated3);
    const Lanes32 odd1 =
        Add32(Combine(in[5], in[3], IDCT_FIX_2_053119869 - IDCT_FIX_2_562915447,
                      -IDCT_FIX_2_562915447),
              rotated4);
    const Lanes32 odd2 =
        Add32(Combine(in[5], in[3], -IDCT_FIX_2_562915447,
                      IDCT_FIX_3_072711026 - IDCT_FIX_2_562915447),
              rotated3);
    const Lanes32 odd3 =
        Add32(Combine(in[7], in[1], -IDCT_FIX_0_899976223,
                      IDCT_FIX_1_501321110 - IDCT_FIX_0_899976223),
              rotated4);

    out[0] = Narrow(Add32(sum10, odd3), descale);
    out[7] = Narrow(Subtract32(sum10, odd3), descale);
    out[1] = Narrow(Add32(sum11, odd2), descale);
    out[6] = Narrow(Subtract32(sum11, odd2), descale);
    out[2] = Narrow(Add32(sum12, odd1), descale);
    out[5] = Narrow(Subtract32(sum12, odd1), descale);
    out[3] = Narrow(Add32(sum13, odd0), descale);
    out[4] = Narrow(Subtract32(sum13, odd0), descale);
}

static JPEG_FORCE_INLINE void Transpose(const Lanes16 *in, Lanes16 *out)
{
    const Lanes16 pair0 = ZipLow16(in[0], in[1]);
    const Lanes16 pair1 = ZipHigh16(in[0], in[1]);
    const Lanes16 pair2 = ZipLow16(in[2], in[3]);
    const Lanes16 pair3 = ZipHigh16(in[2], in[3]);
    const Lanes16 pair4 = ZipLow16(in[4], in[5]);
    const Lanes16 pair5 = ZipHigh16(in[4], in[5]);
    const Lanes16 pair6 = ZipLow16(in[6], in[7]);
    const Lanes16 pair7 = ZipHigh16(in[6], in[7]);
    const Lanes16 quad0 = ZipLow32(pair0, pair2);
    const Lanes16 quad1 = ZipHigh32(pair0, pair2);
    const Lanes16 quad2 = ZipLow32(pair1, pair3);
    const Lanes16 quad3 = ZipHigh32(pair1, pair3);
    const Lanes16 quad4 = ZipLow32(pair4, pair6);
    const Lanes16 quad5 = ZipHigh32(pair4, pair6);
    const Lanes16 quad6 = ZipLow32(pair5, pair7);
    const Lanes16 quad7 = ZipHigh32(pair5, pair7);
    out[0] = ZipLow64(quad0, quad4);
    out[1] = ZipHigh64(quad0, quad4);
    out[2] = ZipLow64(quad1, quad5);
    out[3] = ZipHigh64(quad1, quad5);
    out[4] = ZipLow64(quad2, quad6);
    out[5] = ZipHigh64(quad2, quad6);
    out[6] = ZipLow64(quad3, quad7);
    out[7] = ZipHigh64(quad3, quad7);
}

// Обратное ДКП блока. Коэффициенты и таблица квантования лежат в
// естественном порядке. Первый проход идёт по столбцам, второй снимает
// запас первого прохода и множитель 8 самого ДКП.
static void InverseTransform(const int16_t *block, const uint16_t *quant, uint8_t *destination,
                             uint32_t stride)
{
    Lanes16 rows[8];
    Lanes16 work[8];
    for (size_t y = 0; y < 8u; ++y) rows[y] = LoadProduct(block + y * 8u, quant + y * 8u);
    InverseTransform8(rows, work, IDCT_CONST_BITS - IDCT_PASS1_BITS);
    Transpose(work, rows);
    InverseTransform8(rows, work, IDCT_CONST_BITS + IDCT_PASS1_BITS + 3);
    Transpose(work, rows);
    for (size_t y = 0; y < 8u; ++y) StoreSamples(destination + y * stride, rows[y]);
}

// Между сегментами допустимы заполняющие 0xFF, а некоторые кодировщики
// оставляют там же мусор. Ищем ближайший настоящий маркер.
static ImageStatus NextMarker(const uint8_t *bytes, uint32_t sizeBytes, uint32_t *position,
                              uint8_t *outMarker)
{
    uint32_t at = *position;
    do
    {
        while (at < sizeBytes && bytes[at] != 0xFFu) at += 1u;
        while (at < sizeBytes && bytes[at] == 0xFFu) at += 1u;
        if (at >= sizeBytes) return IMAGE_TRUNCATED;
        // 0xFF с нулём — байт данных: за проходом мог остаться их хвост.
    } while (bytes[at] == 0x00u);
    *outMarker = bytes[at];
    *position = at + 1u;
    return IMAGE_OK;
}

static bool MarkerIsStandalone(uint8_t marker)
{
    // SOI, EOI, RSTn и TEM идут без длины.
    return marker == 0xD8u || marker == 0xD9u || marker == 0x01u ||
           (marker >= 0xD0u && marker <= 0xD7u);
}

static ImageStatus ParseFrameSegment(const uint8_t *segment, uint32_t length, bool progressive,
                                     JpegFrame *frame)
{
    if (length < 6u) return IMAGE_CORRUPT;
    uint32_t precision = segment[0];
    // Двенадцать бит на отсчёт стандарт допускает, но наш выход
    // восьмибитный, и молча терять четыре бита хуже, чем отказаться.
    if (precision != 8u) return IMAGE_UNSUPPORTED_FEATURE;

    uint32_t height = ReadU16Be(segment + 1);
    uint32_t width = ReadU16Be(segment + 3);
    uint32_t componentCount = segment[5];
    // Нулевая высота означает маркер DNL после первого прохода: размер
    // становится известен только в конце файла.
    if (width == 0u || height == 0u) return IMAGE_UNSUPPORTED_FEATURE;
    if (width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION) return IMAGE_TOO_LARGE;
    if (componentCount != 1u && componentCount != 3u) return IMAGE_UNSUPPORTED_FEATURE;
    if (length < 6u + componentCount * 3u) return IMAGE_CORRUPT;

    frame->width = width;
    frame->height = height;
    frame->componentCount = componentCount;
    frame->progressive = progressive;
    frame->maxHorizontal = 1u;
    frame->maxVertical = 1u;

    for (uint32_t index = 0; index < componentCount; ++index)
    {
        const uint8_t *entry = segment + 6u + (size_t)(index * 3u);
        JpegComponent *component = &frame->components[index];
        component->id = entry[0];
        component->horizontal = (uint8_t)(entry[1] >> 4);
        component->vertical = (uint8_t)(entry[1] & 15u);
        component->quantTable = entry[2];
        if (component->horizontal == 0u || component->horizontal > JPEG_MAX_SAMPLING) return IMAGE_CORRUPT;
        if (component->vertical == 0u || component->vertical > JPEG_MAX_SAMPLING) return IMAGE_CORRUPT;
        if (component->quantTable >= JPEG_QUANT_TABLES) return IMAGE_CORRUPT;
        if (component->horizontal > frame->maxHorizontal) frame->maxHorizontal = component->horizontal;
        if (component->vertical > frame->maxVertical) frame->maxVertical = component->vertical;
    }

    frame->mcusPerLine = DivideRoundUp(width, 8u * frame->maxHorizontal);
    frame->mcusPerColumn = DivideRoundUp(height, 8u * frame->maxVertical);

    for (uint32_t index = 0; index < componentCount; ++index)
    {
        JpegComponent *component = &frame->components[index];
        uint32_t componentWidth =
            DivideRoundUp(width * component->horizontal, frame->maxHorizontal);
        uint32_t componentHeight = DivideRoundUp(height * component->vertical, frame->maxVertical);
        component->plainWidth = componentWidth;
        component->plainHeight = componentHeight;
        component->scanBlocksPerLine = DivideRoundUp(componentWidth, 8u);
        component->scanBlocksPerColumn = DivideRoundUp(componentHeight, 8u);
        component->blocksPerLine = frame->mcusPerLine * component->horizontal;
        component->blocksPerColumn = frame->mcusPerColumn * component->vertical;
        component->sampleWidth = component->blocksPerLine * 8u;
    }
    return IMAGE_OK;
}

static ImageStatus ComputeLayout(const JpegFrame *frame, JpegLayout *outLayout)
{
    uint64_t offset = 0u;
    outLayout->huffman = 0u;
    offset += (uint64_t)JPEG_HUFFMAN_TABLES * sizeof(JpegHuffman);
    offset = AlignUp8((uint32_t)offset);
    outLayout->quant = (uint32_t)offset;
    offset += (uint64_t)JPEG_QUANT_TABLES * 64u * sizeof(uint16_t);

    for (uint32_t index = 0; index < frame->componentCount; ++index)
    {
        const JpegComponent *component = &frame->components[index];
        offset = (offset + 7u) & ~(uint64_t)7u;
        if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
        outLayout->samples[index] = (uint32_t)offset;
        offset += (uint64_t)component->sampleWidth * component->blocksPerColumn * 8u;
    }
    if (frame->progressive)
    {
        for (uint32_t index = 0; index < frame->componentCount; ++index)
        {
            const JpegComponent *component = &frame->components[index];
            offset = (offset + 7u) & ~(uint64_t)7u;
            if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
            outLayout->coefficients[index] = (uint32_t)offset;
            offset += (uint64_t)component->blocksPerLine * component->blocksPerColumn * 64u *
                      sizeof(int16_t);
        }
    }
    else
    {
        for (uint32_t index = 0; index < JPEG_MAX_COMPONENTS; ++index)
        {
            outLayout->coefficients[index] = 0u;
        }
    }
    offset = (offset + 7u) & ~(uint64_t)7u;
    if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
    outLayout->rows = (uint32_t)offset;
    offset += (uint64_t)frame->width * frame->componentCount;
    if (offset > 0xFFFFFFFFu) return IMAGE_TOO_LARGE;
    outLayout->total = (uint32_t)offset;
    return IMAGE_OK;
}

// Доходит до заголовка кадра и останавливается: размеры и расклад
// памяти известны уже там, а разбирать таблицы ради Inspect незачем.
static ImageStatus ParseFrameHeader(const uint8_t *bytes, uint32_t sizeBytes, JpegFrame *frame)
{
    if (sizeBytes < 4u) return IMAGE_TRUNCATED;
    if (bytes[0] != 0xFFu || bytes[1] != 0xD8u) return IMAGE_NOT_RECOGNISED;

    for (uint32_t index = 0; index < JPEG_MAX_COMPONENTS; ++index)
    {
        frame->components[index].samples = NULL;
        frame->components[index].coefficients = NULL;
    }
    frame->adobe = false;
    frame->adobeTransform = 0u;

    uint32_t position = 2u;
    for (;;)
    {
        uint8_t marker = 0u;
        ImageStatus status = NextMarker(bytes, sizeBytes, &position, &marker);
        if (status != IMAGE_OK) return status;
        if (marker == 0xD9u) return IMAGE_CORRUPT;   // EOI до заголовка кадра
        if (MarkerIsStandalone(marker)) continue;

        if (position + 2u > sizeBytes) return IMAGE_TRUNCATED;
        uint32_t length = ReadU16Be(bytes + position);
        if (length < 2u) return IMAGE_CORRUPT;
        if (position + length > sizeBytes) return IMAGE_TRUNCATED;
        const uint8_t *segment = bytes + position + 2u;
        uint32_t payload = length - 2u;

        if (marker == 0xC0u || marker == 0xC1u || marker == 0xC2u)
        {
            return ParseFrameSegment(segment, payload, marker == 0xC2u, frame);
        }
        if ((marker >= 0xC3u && marker <= 0xCFu) && marker != 0xC4u && marker != 0xC8u &&
            marker != 0xCCu)
        {
            // Иерархический режим, потери без ДКП и арифметическое
            // кодирование: разбирать их наполовину хуже, чем сказать
            // об этом прямо.
            return IMAGE_UNSUPPORTED_FEATURE;
        }
        if (marker == 0xEEu && payload >= 12u && segment[0] == 'A' && segment[1] == 'd' &&
            segment[2] == 'o' && segment[3] == 'b' && segment[4] == 'e')
        {
            frame->adobe = true;
            frame->adobeTransform = segment[11];
        }
        if (marker == 0xDAu) return IMAGE_CORRUPT;   // проход до заголовка кадра

        position += length;
    }
}

ImageStatus JpegInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;

    JpegFrame frame;
    ImageStatus status = ParseFrameHeader((const uint8_t *)bytes, sizeBytes, &frame);
    if (status != IMAGE_OK) return status;

    JpegLayout layout;
    status = ComputeLayout(&frame, &layout);
    if (status != IMAGE_OK) return status;

    uint32_t frameBytes = frame.width * frame.height * 4u;
    outInfo->width = frame.width;
    outInfo->height = frame.height;
    outInfo->frameCount = 1u;
    outInfo->frameMilliseconds[0] = 0u;
    outInfo->frameBytes = frameBytes;
    outInfo->pixelBytes = frameBytes;
    outInfo->scratchBytes = layout.total;
    outInfo->hasAlpha = false;
    return IMAGE_OK;
}

typedef struct JpegScan
{
    uint32_t componentCount;
    uint32_t componentIndex[JPEG_MAX_COMPONENTS];
    uint32_t spectralStart;
    uint32_t spectralEnd;
    uint32_t approximationHigh;
    uint32_t approximationLow;
} JpegScan;

// Коэффициент не выходит за int16 у любого правильного файла. Зажатие
// защищает от испорченного: лучше неверный пиксель, чем переполнение.
static int32_t ClampCoefficient(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

static ImageStatus ParseQuantSegment(JpegDecoder *decoder, const uint8_t *segment, uint32_t length)
{
    uint32_t offset = 0u;
    while (offset < length)
    {
        uint32_t selector = segment[offset];
        offset += 1u;
        uint32_t precision = selector >> 4;
        uint32_t table = selector & 15u;
        if (precision > 1u || table >= JPEG_QUANT_TABLES) return IMAGE_CORRUPT;
        uint32_t needed = precision == 0u ? 64u : 128u;
        if (offset + needed > length) return IMAGE_CORRUPT;

        // В файле таблица идёт зигзагом, а хранится в естественном
        // порядке: так её читает обратное ДКП.
        uint16_t *values = decoder->quant + (size_t)table * 64u;
        for (uint32_t index = 0; index < 64u; ++index)
        {
            values[ZIGZAG[index]] = precision == 0u
                                ? (uint16_t)segment[offset + index]
                                : (uint16_t)ReadU16Be(segment + offset + (size_t)(index * 2u));
        }
        offset += needed;
        decoder->quantPresent[table] = true;
    }
    return IMAGE_OK;
}

static ImageStatus ParseHuffmanSegment(JpegDecoder *decoder, const uint8_t *segment,
                                       uint32_t length)
{
    uint32_t offset = 0u;
    while (offset < length)
    {
        uint32_t selector = segment[offset];
        offset += 1u;
        uint32_t tableClass = selector >> 4;
        uint32_t table = selector & 15u;
        if (tableClass > 1u || table >= 4u) return IMAGE_CORRUPT;
        if (offset + 16u > length) return IMAGE_CORRUPT;

        uint32_t total = 0u;
        for (uint32_t index = 0; index < 16u; ++index) total += segment[offset + index];
        if (total > 256u) return IMAGE_CORRUPT;
        if (offset + 16u + total > length) return IMAGE_CORRUPT;

        ImageStatus status = BuildHuffman(&decoder->huffman[tableClass * 4u + table],
                                          segment + offset, segment + offset + 16u, total);
        if (status != IMAGE_OK) return status;
        offset += 16u + total;
    }
    return IMAGE_OK;
}

static ImageStatus ParseScanSegment(JpegDecoder *decoder, const uint8_t *segment, uint32_t length,
                                    JpegScan *outScan)
{
    if (length < 1u) return IMAGE_CORRUPT;
    uint32_t count = segment[0];
    if (count == 0u || count > decoder->frame.componentCount) return IMAGE_CORRUPT;
    if (length < 1u + count * 2u + 3u) return IMAGE_CORRUPT;

    outScan->componentCount = count;
    for (uint32_t index = 0; index < count; ++index)
    {
        uint32_t id = segment[1u + index * 2u];
        uint32_t tables = segment[2u + index * 2u];
        uint32_t found = JPEG_MAX_COMPONENTS;
        for (uint32_t search = 0; search < decoder->frame.componentCount; ++search)
        {
            if (decoder->frame.components[search].id == id)
            {
                found = search;
                break;
            }
        }
        if (found == JPEG_MAX_COMPONENTS) return IMAGE_CORRUPT;

        JpegComponent *component = &decoder->frame.components[found];
        component->dcTable = (uint8_t)(tables >> 4);
        component->acTable = (uint8_t)(tables & 15u);
        if (component->dcTable >= 4u || component->acTable >= 4u) return IMAGE_CORRUPT;
        // Таблица квантования обязана прийти до прохода: иначе кадр
        // разобрался бы с нулевым делителем и вышел серым.
        if (!decoder->quantPresent[component->quantTable]) return IMAGE_CORRUPT;
        outScan->componentIndex[index] = found;
    }

    const uint8_t *tail = segment + 1u + (size_t)(count * 2u);
    if (decoder->frame.progressive)
    {
        outScan->spectralStart = tail[0];
        outScan->spectralEnd = tail[1];
        outScan->approximationHigh = (uint32_t)(tail[2] >> 4);
        outScan->approximationLow = (uint32_t)(tail[2] & 15u);
        if (outScan->spectralEnd > 63u) return IMAGE_CORRUPT;
        if (outScan->spectralStart > outScan->spectralEnd) return IMAGE_CORRUPT;
        // Проход постоянной составляющей охватывает только её, а проход
        // переменных читает ровно один компонент.
        if (outScan->spectralStart == 0u && outScan->spectralEnd != 0u) return IMAGE_CORRUPT;
        if (outScan->spectralStart != 0u && count != 1u) return IMAGE_CORRUPT;
        if (outScan->approximationLow > 13u) return IMAGE_CORRUPT;
    }
    else
    {
        outScan->spectralStart = 0u;
        outScan->spectralEnd = 63u;
        outScan->approximationHigh = 0u;
        outScan->approximationLow = 0u;
    }
    return IMAGE_OK;
}

static void OutputBlock(const JpegDecoder *decoder, const JpegComponent *component,
                        const int16_t *block, uint32_t blockRow, uint32_t blockColumn)
{
    if (blockRow >= component->blocksPerColumn || blockColumn >= component->blocksPerLine) return;

    const uint16_t *quant = decoder->quant + (size_t)component->quantTable * 64u;
    uint8_t *destination = component->samples +
                           (size_t)blockRow * 8u * component->sampleWidth + (size_t)blockColumn * 8u;
    InverseTransform(block, quant, destination, component->sampleWidth);
}

// Блок только с DC: оба прохода islow сводятся к постоянной
// составляющей, и уровень тот же, что дал бы общий путь.
static void OutputFlatBlock(const JpegDecoder *decoder, const JpegComponent *component, int16_t dc,
                            uint32_t blockRow, uint32_t blockColumn)
{
    if (blockRow >= component->blocksPerColumn || blockColumn >= component->blocksPerLine) return;

    const uint16_t *quant = decoder->quant + (size_t)component->quantTable * 64u;
    const int64_t level = (int64_t)dc * quant[0] * ((int64_t)1 << IDCT_PASS1_BITS);
    const uint8_t value = IdctSample(IdctDescale(level, IDCT_PASS1_BITS + 3u));

    uint8_t *destination = component->samples +
                           (size_t)blockRow * 8u * component->sampleWidth + (size_t)blockColumn * 8u;
    for (uint32_t y = 0; y < 8u; ++y)
    {
        uint8_t *line = destination + (size_t)y * component->sampleWidth;
        for (uint32_t x = 0; x < 8u; ++x) line[x] = value;
    }
}

static ImageStatus DecodeSequentialBlock(JpegDecoder *decoder, JpegComponent *component,
                                         int16_t *block, bool *outAcPresent)
{
    JpegReader *reader = &decoder->reader;
    const JpegHuffman *dcTable = &decoder->huffman[component->dcTable];
    const JpegHuffman *acTable = &decoder->huffman[4u + component->acTable];
    if (!dcTable->present || !acTable->present) return IMAGE_CORRUPT;

    JpegWindow window = LoadWindow(reader);
    RefillWindow(reader, &window);
    uint32_t bits = 0u;
    const int32_t size = TakeCode(dcTable, &window, &bits);
    if (size < 0 || size > 15) return IMAGE_CORRUPT;
    component->dcPrediction =
        ClampCoefficient(component->dcPrediction + Extend(bits, (uint32_t)size));
    block[0] = (int16_t)component->dcPrediction;

    bool acPresent = false;
    uint32_t index = 1u;
    while (index < 64u)
    {
        RefillWindow(reader, &window);
        const int32_t pair = TakeCode(acTable, &window, &bits);
        if (pair < 0) return IMAGE_CORRUPT;
        const uint32_t valueSize = (uint32_t)pair & 15u;
        const uint32_t run = (uint32_t)pair >> 4;
        if (valueSize == 0u)
        {
            if (run != 15u) break;   // конец блока
            index += 16u;
            continue;
        }
        index += run;
        if (index >= 64u) return IMAGE_CORRUPT;
        block[ZIGZAG[index]] = (int16_t)Extend(bits, valueSize);
        acPresent = true;
        index += 1u;
    }
    SaveWindow(reader, window);
    *outAcPresent = acPresent;
    return IMAGE_OK;
}

static ImageStatus DecodeDcFirst(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                 uint32_t approximationLow)
{
    JpegReader *reader = &decoder->reader;
    const JpegHuffman *table = &decoder->huffman[component->dcTable];
    if (!table->present) return IMAGE_CORRUPT;

    JpegWindow window = LoadWindow(reader);
    RefillWindow(reader, &window);
    uint32_t bits = 0u;
    const int32_t size = TakeCode(table, &window, &bits);
    if (size < 0 || size > 15) return IMAGE_CORRUPT;
    SaveWindow(reader, window);
    component->dcPrediction =
        ClampCoefficient(component->dcPrediction + Extend(bits, (uint32_t)size));
    block[0] =
        (int16_t)ClampCoefficient(component->dcPrediction * (int32_t)(1u << approximationLow));
    return IMAGE_OK;
}

static void DecodeDcRefine(JpegDecoder *decoder, int16_t *block, uint32_t approximationLow)
{
    JpegReader *reader = &decoder->reader;
    JpegWindow window = LoadWindow(reader);
    RefillWindow(reader, &window);
    const uint32_t bit = TakeBits(&window, 1u);
    SaveWindow(reader, window);
    if (bit != 0u)
    {
        int32_t value = (int32_t)block[0] | (int32_t)(1u << approximationLow);
        block[0] = (int16_t)ClampCoefficient(value);
    }
}

static ImageStatus DecodeAcFirst(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                 const JpegScan *scan)
{
    if (decoder->eobRun > 0u)
    {
        decoder->eobRun -= 1u;
        return IMAGE_OK;
    }
    JpegReader *reader = &decoder->reader;
    const JpegHuffman *table = &decoder->huffman[4u + component->acTable];
    if (!table->present) return IMAGE_CORRUPT;

    const uint32_t spectralEnd = scan->spectralEnd;
    const int32_t unit = (int32_t)(1u << scan->approximationLow);
    JpegWindow window = LoadWindow(reader);
    uint32_t index = scan->spectralStart;
    while (index <= spectralEnd)
    {
        RefillWindow(reader, &window);
        uint32_t bits = 0u;
        const int32_t pair = TakeCode(table, &window, &bits);
        if (pair < 0) return IMAGE_CORRUPT;
        const uint32_t size = (uint32_t)pair & 15u;
        const uint32_t run = (uint32_t)pair >> 4;
        if (size == 0u)
        {
            if (run < 15u)
            {
                // Конец блока сразу для целой серии блоков.
                decoder->eobRun = (1u << run) - 1u;
                if (run > 0u) decoder->eobRun += TakeBits(&window, run);
                break;
            }
            index += 16u;
            continue;
        }
        index += run;
        if (index > spectralEnd) return IMAGE_CORRUPT;
        block[index] = (int16_t)ClampCoefficient(Extend(bits, size) * unit);
        index += 1u;
    }
    SaveWindow(reader, window);
    return IMAGE_OK;
}

// T.81 G.1.2.3. Уточняющий проход не приносит новых значений целиком:
// он добавляет один бит уже известным коэффициентам и вставляет
// единицы там, где раньше был ноль. Порядок битов в потоке задан
// перебором коэффициентов, поэтому пропустить ни одного нельзя.
static ImageStatus DecodeAcRefine(JpegDecoder *decoder, JpegComponent *component, int16_t *block,
                                  const JpegScan *scan)
{
    JpegReader *reader = &decoder->reader;
    const uint32_t spectralEnd = scan->spectralEnd;
    const int32_t positive = (int32_t)(1u << scan->approximationLow);
    JpegWindow window = LoadWindow(reader);
    uint32_t index = scan->spectralStart;

    if (decoder->eobRun == 0u)
    {
        const JpegHuffman *table = &decoder->huffman[4u + component->acTable];
        if (!table->present) return IMAGE_CORRUPT;
        while (index <= spectralEnd)
        {
            RefillWindow(reader, &window);
            uint32_t bits = 0u;
            const int32_t pair = TakeCode(table, &window, &bits);
            if (pair < 0) return IMAGE_CORRUPT;
            const uint32_t size = (uint32_t)pair & 15u;
            uint32_t run = (uint32_t)pair >> 4;
            int32_t value = 0;

            if (size == 0u)
            {
                if (run < 15u)
                {
                    decoder->eobRun = 1u << run;
                    if (run > 0u) decoder->eobRun += TakeBits(&window, run);
                    break;
                }
            }
            else
            {
                // Уточняющий проход не кодирует величину: только знак.
                if (size != 1u) return IMAGE_CORRUPT;
                value = (bits & 1u) != 0u ? positive : -positive;
            }

            while (index <= spectralEnd)
            {
                int16_t *coefficient = &block[index];
                if (*coefficient != 0)
                {
                    RefillWindow(reader, &window);
                    if (TakeBits(&window, 1u) != 0u && ((int32_t)*coefficient & positive) == 0)
                    {
                        *coefficient = (int16_t)ClampCoefficient(
                            (int32_t)*coefficient + (*coefficient >= 0 ? positive : -positive));
                    }
                }
                else
                {
                    if (run == 0u)
                    {
                        if (value != 0) *coefficient = (int16_t)value;
                        index += 1u;
                        break;
                    }
                    run -= 1u;
                }
                index += 1u;
            }
        }
    }

    if (decoder->eobRun > 0u)
    {
        // Внутри серии конца блока новых коэффициентов нет, но
        // корректирующие биты у ненулевых всё равно лежат в потоке.
        while (index <= spectralEnd)
        {
            int16_t *coefficient = &block[index];
            if (*coefficient != 0)
            {
                RefillWindow(reader, &window);
                if (TakeBits(&window, 1u) != 0u && ((int32_t)*coefficient & positive) == 0)
                {
                    *coefficient = (int16_t)ClampCoefficient(
                        (int32_t)*coefficient + (*coefficient >= 0 ? positive : -positive));
                }
            }
            index += 1u;
        }
        decoder->eobRun -= 1u;
    }
    SaveWindow(reader, window);
    return IMAGE_OK;
}

static ImageStatus DecodeBlock(JpegDecoder *decoder, const JpegScan *scan,
                               JpegComponent *component, uint32_t blockRow, uint32_t blockColumn)
{
    if (decoder->frame.progressive)
    {
        if (blockRow >= component->blocksPerColumn || blockColumn >= component->blocksPerLine)
        {
            return IMAGE_CORRUPT;
        }
        int16_t *block =
            component->coefficients +
            ((size_t)blockRow * component->blocksPerLine + blockColumn) * 64u;
        if (scan->spectralStart == 0u)
        {
            if (scan->approximationHigh == 0u)
            {
                return DecodeDcFirst(decoder, component, block, scan->approximationLow);
            }
            DecodeDcRefine(decoder, block, scan->approximationLow);
            return IMAGE_OK;
        }
        if (scan->approximationHigh == 0u) return DecodeAcFirst(decoder, component, block, scan);
        return DecodeAcRefine(decoder, component, block, scan);
    }

    int16_t block[64];
    for (uint32_t index = 0; index < 64u; ++index) block[index] = 0;
    bool acPresent = false;
    ImageStatus status = DecodeSequentialBlock(decoder, component, block, &acPresent);
    if (status != IMAGE_OK) return status;
    if (!acPresent)
    {
        OutputFlatBlock(decoder, component, block[0], blockRow, blockColumn);
        return IMAGE_OK;
    }
    OutputBlock(decoder, component, block, blockRow, blockColumn);
    return IMAGE_OK;
}

static ImageStatus ReadRestart(JpegDecoder *decoder)
{
    JpegReader *reader = &decoder->reader;
    reader->bitBuffer = 0u;
    reader->bitCount = 0u;
    reader->markerReached = false;
    while (reader->position + 1u < reader->sizeBytes)
    {
        if (reader->bytes[reader->position] != 0xFFu)
        {
            reader->position += 1u;
            continue;
        }
        uint8_t marker = reader->bytes[reader->position + 1u];
        if (marker >= 0xD0u && marker <= 0xD7u)
        {
            reader->position += 2u;
            return IMAGE_OK;
        }
        if (marker == 0xFFu)
        {
            reader->position += 1u;
            continue;
        }
        if (marker == 0x00u)
        {
            // Такой же остаток данных, как обычные байты выше.
            reader->position += 2u;
            continue;
        }
        // Другой маркер там, где обязан стоять рестарт: данные прохода
        // кончились раньше, чем обещал заголовок.
        return IMAGE_TRUNCATED;
    }
    return IMAGE_TRUNCATED;
}

// Блок одиночного прохода или все блоки одного MCU.
static ImageStatus DecodeUnit(JpegDecoder *decoder, const JpegScan *scan, uint32_t row,
                              uint32_t column)
{
    JpegFrame *frame = &decoder->frame;
    if (scan->componentCount == 1u)
    {
        JpegComponent *component = &frame->components[scan->componentIndex[0]];
        return DecodeBlock(decoder, scan, component, row, column);
    }
    for (uint32_t index = 0; index < scan->componentCount; ++index)
    {
        JpegComponent *component = &frame->components[scan->componentIndex[index]];
        for (uint32_t vertical = 0; vertical < component->vertical; ++vertical)
        {
            for (uint32_t horizontal = 0; horizontal < component->horizontal; ++horizontal)
            {
                ImageStatus status =
                    DecodeBlock(decoder, scan, component, row * component->vertical + vertical,
                                column * component->horizontal + horizontal);
                if (status != IMAGE_OK) return status;
            }
        }
    }
    return IMAGE_OK;
}

static ImageStatus DecodeScan(JpegDecoder *decoder, const JpegScan *scan)
{
    JpegFrame *frame = &decoder->frame;
    decoder->eobRun = 0u;
    for (uint32_t index = 0; index < scan->componentCount; ++index)
    {
        frame->components[scan->componentIndex[index]].dcPrediction = 0;
    }

    uint32_t unitsPerLine = 0u;
    uint32_t unitCount = 0u;
    if (scan->componentCount == 1u)
    {
        // Одиночный проход обходит блоки самого компонента, а не MCU:
        // выравнивание до целого MCU в поток не попадает.
        const JpegComponent *component = &frame->components[scan->componentIndex[0]];
        unitsPerLine = component->scanBlocksPerLine;
        unitCount = component->scanBlocksPerLine * component->scanBlocksPerColumn;
    }
    else
    {
        unitsPerLine = frame->mcusPerLine;
        unitCount = frame->mcusPerLine * frame->mcusPerColumn;
    }
    if (unitsPerLine == 0u || unitCount == 0u) return IMAGE_CORRUPT;

    uint32_t interval = decoder->restartInterval != 0u ? decoder->restartInterval : unitCount;
    uint32_t unit = 0u;
    uint32_t row = 0u;
    uint32_t column = 0u;
    while (unit < unitCount)
    {
        uint32_t limit = unit + interval;
        if (limit > unitCount || limit < unit) limit = unitCount;
        for (; unit < limit; ++unit)
        {
            ImageStatus status = DecodeUnit(decoder, scan, row, column);
            if (status != IMAGE_OK) return status;
            column += 1u;
            if (column == unitsPerLine)
            {
                column = 0u;
                row += 1u;
            }
        }
        if (unit < unitCount)
        {
            ImageStatus status = ReadRestart(decoder);
            if (status != IMAGE_OK) return status;
            decoder->eobRun = 0u;
            for (uint32_t index = 0; index < scan->componentCount; ++index)
            {
                frame->components[scan->componentIndex[index]].dcPrediction = 0;
            }
        }
    }
    return IMAGE_OK;
}

// Прогрессивный кадр собирается целиком только после последнего
// прохода: до него коэффициенты неполные, и обратное ДКП дало бы
// промежуточную картинку.
static void FinishProgressive(const JpegDecoder *decoder)
{
    const JpegFrame *frame = &decoder->frame;
    for (uint32_t index = 0; index < frame->componentCount; ++index)
    {
        const JpegComponent *component = &frame->components[index];
        for (uint32_t row = 0; row < component->blocksPerColumn; ++row)
        {
            for (uint32_t column = 0; column < component->blocksPerLine; ++column)
            {
                const int16_t *block =
                    component->coefficients +
                    ((size_t)row * component->blocksPerLine + column) * 64u;
                int16_t natural[64];
                for (uint32_t zigzag = 0; zigzag < 64u; ++zigzag)
                {
                    natural[ZIGZAG[zigzag]] = block[zigzag];
                }
                OutputBlock(decoder, component, natural, row, column);
            }
        }
    }
}

// Перевод цвета тот же, что у libjpeg: коэффициенты BT.601 с 16 битами
// дробной части и одно округление на канал.
#define JPEG_COLOR_ONE 65536
#define JPEG_COLOR_HALF 32768
#define JPEG_CR_TO_RED 91881      // 1.40200
#define JPEG_CB_TO_BLUE 116130    // 1.77200
#define JPEG_CB_TO_GREEN (-22554) // -0.34414
#define JPEG_CR_TO_GREEN (-46802) // -0.71414

static uint8_t ClampByte(int32_t value)
{
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

// Шаг удвоения строки по ширине: у чётного пикселя вес 3 у своего
// значения и 1 у предыдущего, у нечётного то же со следующим.
static JPEG_FORCE_INLINE void StoreDoubled(uint8_t *out, Lanes16 before, Lanes16 current,
                                           Lanes16 after, int16_t evenBias, int16_t oddBias,
                                           int16_t bits)
{
    const Lanes16 centre = Add16(Add16(current, current), current);
    const Lanes16 even = Add16(Add16(centre, before), Repeat16(evenBias));
    const Lanes16 odd = Add16(Add16(centre, after), Repeat16(oddBias));
    StorePairs(out, ShiftDown16(even, bits), ShiftDown16(odd, bits));
}

// Суммы столбцов для половинной высоты: 3 * ближний + следующий.
static JPEG_FORCE_INLINE Lanes16 ColumnSums(const uint8_t *nearRow, const uint8_t *farRow)
{
    const Lanes16 nearest = LoadWidened(nearRow);
    return Add16(Add16(Add16(nearest, nearest), nearest), LoadWidened(farRow));
}

// Ближняя строка компонента, прореженного вдвое по вертикали, и
// следующая за ней: для чётной строки кадра — верхняя соседка, для
// нечётной — нижняя. На краю строка повторяется.
static void HalfHeightRows(const JpegComponent *component, uint32_t y, const uint8_t **outNear,
                           const uint8_t **outFar)
{
    const uint32_t centre = y >> 1;
    uint32_t neighbour = centre;
    if ((y & 1u) != 0u)
    {
        if (centre + 1u < component->plainHeight) neighbour = centre + 1u;
    }
    else if (centre != 0u)
    {
        neighbour = centre - 1u;
    }
    *outNear = component->samples + (size_t)centre * component->sampleWidth;
    *outFar = component->samples + (size_t)neighbour * component->sampleWidth;
}

// Строка y компонента в разрешении кадра; row — место под неё, если
// компонент прорежен. Отсчёт прореженного компонента лежит посередине
// своей пары, поэтому половинное разрешение восстанавливает фильтр
// libjpeg (fancy upsampling): 3/4 ближнего отсчёта и 1/4 следующего по
// каждой прореженной оси, с тем же чередованием округления. Повторение
// соседа расходилось бы с любым другим декодером на десятки единиц по
// краям цвета. Прочие кратности и прореженный компонент шириной в два
// отсчёта и меньше libjpeg повторяет, и здесь так же.
static const uint8_t *UpsampleRow(const JpegFrame *frame, const JpegComponent *component,
                                  uint32_t y, uint8_t *row)
{
    const bool fullWidth = component->horizontal == frame->maxHorizontal;
    const bool fullHeight = component->vertical == frame->maxVertical;
    if (fullWidth && fullHeight) return component->samples + (size_t)y * component->sampleWidth;

    const size_t width = component->plainWidth;
    const bool halfWidth = frame->maxHorizontal == component->horizontal * 2u && width > 2u;
    const bool halfHeight = frame->maxVertical == component->vertical * 2u;
    const uint8_t *nearRow = NULL;
    const uint8_t *farRow = NULL;
    if (fullWidth && halfHeight)
    {
        HalfHeightRows(component, y, &nearRow, &farRow);
        const uint32_t bias = (y & 1u) != 0u ? 2u : 1u;
        for (size_t x = 0; x < frame->width; ++x)
        {
            row[x] = (uint8_t)((3u * nearRow[x] + farRow[x] + bias) >> 2);
        }
        return row;
    }
    if (halfWidth && fullHeight)
    {
        const uint8_t *in = component->samples + (size_t)y * component->sampleWidth;
        row[0] = in[0];
        row[1] = (uint8_t)((3u * in[0] + in[1] + 2u) >> 2);
        size_t index = 1u;
        for (; index + 9u <= width; index += 8u)
        {
            StoreDoubled(row + 2u * index, LoadWidened(in + index - 1u), LoadWidened(in + index),
                         LoadWidened(in + index + 1u), 1, 2, 2);
        }
        for (; index + 1u < width; ++index)
        {
            const uint32_t centre = 3u * in[index];
            row[2u * index] = (uint8_t)((centre + in[index - 1u] + 1u) >> 2);
            row[2u * index + 1u] = (uint8_t)((centre + in[index + 1u] + 2u) >> 2);
        }
        row[2u * index] = (uint8_t)((3u * in[index] + in[index - 1u] + 1u) >> 2);
        if (2u * index + 1u < frame->width) row[2u * index + 1u] = in[index];
        return row;
    }
    if (halfWidth && halfHeight)
    {
        // Сначала по вертикали: сумма столбца — 3 * ближний + следующий.
        HalfHeightRows(component, y, &nearRow, &farRow);
        uint32_t current = 3u * nearRow[0] + farRow[0];
        uint32_t next = 3u * nearRow[1] + farRow[1];
        row[0] = (uint8_t)((4u * current + 8u) >> 4);
        row[1] = (uint8_t)((3u * current + next + 7u) >> 4);
        uint32_t previous = current;
        current = next;
        size_t index = 1u;
        if (index + 16u <= width)
        {
            // Суммы соседей не считаются заново: они уже лежат в регистрах
            // предыдущего и следующего шага.
            Lanes16 sumsBefore = Repeat16((int16_t)previous);
            Lanes16 sums = ColumnSums(nearRow + index, farRow + index);
            for (; index + 16u <= width; index += 8u)
            {
                const Lanes16 sumsAfter = ColumnSums(nearRow + index + 8u, farRow + index + 8u);
                StoreDoubled(row + 2u * index, LanesBefore(sumsBefore, sums), sums,
                             LanesAfter(sums, sumsAfter), 8, 7, 4);
                sumsBefore = sums;
                sums = sumsAfter;
            }
            previous = 3u * nearRow[index - 1u] + farRow[index - 1u];
            current = 3u * nearRow[index] + farRow[index];
        }
        for (; index + 1u < width; ++index)
        {
            next = 3u * nearRow[index + 1u] + farRow[index + 1u];
            row[2u * index] = (uint8_t)((3u * current + previous + 8u) >> 4);
            row[2u * index + 1u] = (uint8_t)((3u * current + next + 7u) >> 4);
            previous = current;
            current = next;
        }
        row[2u * index] = (uint8_t)((3u * current + previous + 8u) >> 4);
        if (2u * index + 1u < frame->width)
        {
            row[2u * index + 1u] = (uint8_t)((4u * current + 7u) >> 4);
        }
        return row;
    }

    // Повторение: отсчёт, в чью область попадает пиксель кадра.
    const uint32_t sourceRow = y * component->vertical / frame->maxVertical;
    const uint8_t *source = component->samples + (size_t)sourceRow * component->sampleWidth;
    for (uint32_t x = 0; x < frame->width; ++x)
    {
        row[x] = source[x * component->horizontal / frame->maxHorizontal];
    }
    return row;
}

static void RgbRowToRgba(const uint8_t *red, const uint8_t *green, const uint8_t *blue, size_t width,
                         uint8_t *out)
{
    size_t x = 0;
    for (; x + 16u <= width; x += 16u)
    {
        StoreRgba(out + 4u * x, LoadBytes(red + x), LoadBytes(green + x), LoadBytes(blue + x));
    }
    for (; x < width; ++x)
    {
        out[4u * x] = red[x];
        out[4u * x + 1u] = green[x];
        out[4u * x + 2u] = blue[x];
        out[4u * x + 3u] = 255u;
    }
}

typedef struct ColorLanes
{
    Lanes16 red;
    Lanes16 green;
    Lanes16 blue;
} ColorLanes;

// Перевод цвета в дорожках. Целая часть коэффициента вынесена в сложение:
// дробь умещается в 16 бит, а округление и насыщение те же, что в строке
// ниже.
static JPEG_FORCE_INLINE ColorLanes ConvertLanes(Lanes16 luma, Lanes16 blueChroma,
                                                 Lanes16 redChroma)
{
    const Lanes16 centre = Repeat16(128);
    const Lanes16 blue = Subtract16(blueChroma, centre);
    const Lanes16 red = Subtract16(redChroma, centre);
    const Lanes32 green = Combine(blue, red, JPEG_CB_TO_GREEN, JPEG_CR_TO_GREEN + JPEG_COLOR_ONE);
    const ColorLanes result = {
        Add16(Add16(luma, red), MultiplyFraction(red, JPEG_CR_TO_RED - JPEG_COLOR_ONE)),
        Add16(Subtract16(luma, red), Narrow(green, 16)),
        Add16(Add16(luma, Add16(blue, blue)),
              MultiplyFraction(blue, JPEG_CB_TO_BLUE - 2 * JPEG_COLOR_ONE)),
    };
    return result;
}

static void YcbcrRowToRgba(const uint8_t *luma, const uint8_t *blueChroma,
                           const uint8_t *redChroma, size_t width, uint8_t *out)
{
    size_t x = 0;
    for (; x + 16u <= width; x += 16u)
    {
        const Lanes8 lumaBytes = LoadBytes(luma + x);
        const Lanes8 blueBytes = LoadBytes(blueChroma + x);
        const Lanes8 redBytes = LoadBytes(redChroma + x);
        const ColorLanes low =
            ConvertLanes(WidenLow(lumaBytes), WidenLow(blueBytes), WidenLow(redBytes));
        const ColorLanes high =
            ConvertLanes(WidenHigh(lumaBytes), WidenHigh(blueBytes), WidenHigh(redBytes));
        StoreRgba(out + 4u * x, NarrowToBytes(low.red, high.red),
                  NarrowToBytes(low.green, high.green), NarrowToBytes(low.blue, high.blue));
    }
    for (; x < width; ++x)
    {
        const int32_t brightness = luma[x];
        const int32_t blueDifference = (int32_t)blueChroma[x] - 128;
        const int32_t redDifference = (int32_t)redChroma[x] - 128;
        // Сдвиг вправо у отрицательного здесь арифметический:
        // это и требуется, деление округляло бы к нулю.
        out[4u * x] =
            ClampByte(brightness + ((JPEG_CR_TO_RED * redDifference + JPEG_COLOR_HALF) >> 16));
        out[4u * x + 1u] = ClampByte(brightness + ((JPEG_CB_TO_GREEN * blueDifference +
                                                    JPEG_CR_TO_GREEN * redDifference +
                                                    JPEG_COLOR_HALF) >> 16));
        out[4u * x + 2u] =
            ClampByte(brightness + ((JPEG_CB_TO_BLUE * blueDifference + JPEG_COLOR_HALF) >> 16));
        out[4u * x + 3u] = 255u;
    }
}

// rows — по строке ширины кадра на компонент: туда восстанавливаются
// прореженные.
static void ConvertToRgba(const JpegFrame *frame, uint8_t *pixels, uint8_t *rows)
{
    // Adobe с transform 0 означает, что три компонента — уже RGB.
    bool convertColor = frame->componentCount == 3u && !(frame->adobe && frame->adobeTransform == 0u);
    const size_t width = frame->width;

    for (uint32_t y = 0; y < frame->height; ++y)
    {
        uint8_t *out = pixels + (size_t)y * width * 4u;
        if (frame->componentCount == 1u)
        {
            const uint8_t *gray = UpsampleRow(frame, &frame->components[0], y, rows);
            RgbRowToRgba(gray, gray, gray, width, out);
            continue;
        }

        const uint8_t *first = UpsampleRow(frame, &frame->components[0], y, rows);
        const uint8_t *second = UpsampleRow(frame, &frame->components[1], y, rows + width);
        const uint8_t *third = UpsampleRow(frame, &frame->components[2], y, rows + 2u * width);
        if (convertColor) YcbcrRowToRgba(first, second, third, width, out);
        else RgbRowToRgba(first, second, third, width, out);
    }
}

ImageStatus JpegDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                       uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL) return IMAGE_INVALID_ARGUMENT;
    if (pixelBytes < info->pixelBytes) return IMAGE_BUFFER_TOO_SMALL;

    const uint8_t *file = (const uint8_t *)bytes;
    JpegDecoder decoder;
    ImageStatus status = ParseFrameHeader(file, sizeBytes, &decoder.frame);
    if (status != IMAGE_OK) return status;
    if (decoder.frame.width != info->width || decoder.frame.height != info->height)
    {
        return IMAGE_CORRUPT;
    }

    JpegLayout layout;
    status = ComputeLayout(&decoder.frame, &layout);
    if (status != IMAGE_OK) return status;
    if (scratch == NULL || scratchBytes < layout.total) return IMAGE_BUFFER_TOO_SMALL;

    uint8_t *base = (uint8_t *)scratch;
    decoder.huffman = (JpegHuffman *)(void *)(base + layout.huffman);
    decoder.quant = (uint16_t *)(void *)(base + layout.quant);
    decoder.restartInterval = 0u;
    decoder.eobRun = 0u;
    for (uint32_t index = 0; index < JPEG_HUFFMAN_TABLES; ++index)
    {
        decoder.huffman[index].present = false;
    }
    for (uint32_t index = 0; index < JPEG_QUANT_TABLES; ++index) decoder.quantPresent[index] = false;

    for (uint32_t index = 0; index < decoder.frame.componentCount; ++index)
    {
        JpegComponent *component = &decoder.frame.components[index];
        component->samples = base + layout.samples[index];
        component->dcPrediction = 0;
        // Нейтральный уровень: у обрезанного файла недошедшие блоки
        // останутся серыми, а не чёрными, и цветность не поедет.
        size_t sampleCount = (size_t)component->sampleWidth * component->blocksPerColumn * 8u;
        for (size_t offset = 0; offset < sampleCount; ++offset) component->samples[offset] = 128u;

        if (decoder.frame.progressive)
        {
            component->coefficients = (int16_t *)(void *)(base + layout.coefficients[index]);
            size_t coefficientCount =
                (size_t)component->blocksPerLine * component->blocksPerColumn * 64u;
            for (size_t offset = 0; offset < coefficientCount; ++offset)
            {
                component->coefficients[offset] = 0;
            }
        }
        else
        {
            component->coefficients = NULL;
        }
    }

    uint32_t position = 2u;
    bool decodedScan = false;
    for (;;)
    {
        uint8_t marker = 0u;
        status = NextMarker(file, sizeBytes, &position, &marker);
        if (status != IMAGE_OK)
        {
            // Конец файла без EOI: проходы уже прочитаны, и требовать
            // маркер ради маркера незачем.
            if (decodedScan) break;
            return status;
        }
        if (marker == 0xD9u) break;
        if (MarkerIsStandalone(marker)) continue;

        if (position + 2u > sizeBytes) return IMAGE_TRUNCATED;
        uint32_t length = ReadU16Be(file + position);
        if (length < 2u) return IMAGE_CORRUPT;
        if (position + length > sizeBytes) return IMAGE_TRUNCATED;
        const uint8_t *segment = file + position + 2u;
        uint32_t payload = length - 2u;

        if (marker == 0xDBu)
        {
            status = ParseQuantSegment(&decoder, segment, payload);
            if (status != IMAGE_OK) return status;
        }
        else if (marker == 0xC4u)
        {
            status = ParseHuffmanSegment(&decoder, segment, payload);
            if (status != IMAGE_OK) return status;
        }
        else if (marker == 0xDDu)
        {
            if (payload < 2u) return IMAGE_CORRUPT;
            decoder.restartInterval = ReadU16Be(segment);
        }
        else if (marker == 0xDAu)
        {
            JpegScan scan;
            status = ParseScanSegment(&decoder, segment, payload, &scan);
            if (status != IMAGE_OK) return status;

            decoder.reader.bytes = file;
            decoder.reader.sizeBytes = sizeBytes;
            decoder.reader.position = position + length;
            decoder.reader.bitBuffer = 0u;
            decoder.reader.bitCount = 0u;
            decoder.reader.markerReached = false;

            status = DecodeScan(&decoder, &scan);
            if (status != IMAGE_OK) return status;
            decodedScan = true;
            position = decoder.reader.position;
            continue;
        }

        position += length;
    }

    if (!decodedScan) return IMAGE_CORRUPT;
    if (decoder.frame.progressive) FinishProgressive(&decoder);
    ConvertToRgba(&decoder.frame, (uint8_t *)pixels, base + layout.rows);
    return IMAGE_OK;
}
