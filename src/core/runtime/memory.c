/* Резервные memset, memcpy, memmove и memcmp для сборки без CRT
 * (/NODEFAULTLIB).
 *
 * Оптимизатор любой версии MSVC или clang вправе синтезировать вызовы этих
 * функций (инициализаторы структур, копирования, циклы), поэтому каждый
 * модуль носит свои определения — иначе линковка сломается при смене версии
 * компилятора. Вызовы с длиной, известной только во время работы, и слитые
 * компилятором сравнения приходят сюда, поэтому функции быстрые: короткие
 * длины — без циклов, средние — циклы 16-байтных векторов SSE2 и NEON,
 * длинные на x64 — rep stosb и rep movsb.
 *
 * Внутри них нельзя писать цикл, который оптимизатор распознает как ту же
 * функцию: получится рекурсия. clang такие замены запрещает атрибут
 * no_builtin. MSVC распознаёт заполнение и копирование, поэтому его циклы
 * здесь идут только через интринсики. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <intrin.h>

#if defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#define RUNTIME_NEON 1
#else
#include <emmintrin.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/* Иначе cl считает функции интринсиками и не даёт их определить. */
#pragma function(memset)
#pragma function(memcpy)
#pragma function(memcmp)
#pragma function(memmove)
#endif

#if defined(__clang__)
#define RUNTIME_NO_BUILTIN __attribute__((no_builtin))
#else
#define RUNTIME_NO_BUILTIN
#endif

/* Без CRT компилятор при использовании float ищет _fltused. Внешнее
 * определение остаётся в обычном объектном файле без LTO, поэтому доступно и
 * для ссылок, которые LTO создаёт только во время кодогенерации. Само
 * значение не меняется и не должно занимать writable-секцию. */
const int _fltused = 0;

/* ---- Невыровненные слова и 16-байтные порции -------------------------------- */

#if defined(__clang__)

/* __builtin_memcpy постоянной длины — одна загрузка или запись и при
 * no_builtin. */
static uint32_t Load32(const unsigned char *bytes)
{
    uint32_t value;
    __builtin_memcpy(&value, bytes, sizeof(value));
    return value;
}

static void Store32(unsigned char *bytes, uint32_t value)
{
    __builtin_memcpy(bytes, &value, sizeof(value));
}

static uint64_t Load64(const unsigned char *bytes)
{
    uint64_t value;
    __builtin_memcpy(&value, bytes, sizeof(value));
    return value;
}

static void Store64(unsigned char *bytes, uint64_t value)
{
    __builtin_memcpy(bytes, &value, sizeof(value));
}

#else

/* MSVC не выводит ничего из правил псевдонимов, а __unaligned разрешает
 * любой адрес. */
static uint32_t Load32(const unsigned char *bytes)
{
    return *(const uint32_t __unaligned *)bytes;
}

static void Store32(unsigned char *bytes, uint32_t value)
{
    *(uint32_t __unaligned *)bytes = value;
}

static uint64_t Load64(const unsigned char *bytes)
{
    return *(const uint64_t __unaligned *)bytes;
}

static void Store64(unsigned char *bytes, uint64_t value)
{
    *(uint64_t __unaligned *)bytes = value;
}

#endif

#if defined(RUNTIME_NEON)

typedef uint8x16_t Chunk;

static Chunk ChunkLoad(const unsigned char *bytes)
{
    return vld1q_u8(bytes);
}

static void ChunkStore(unsigned char *bytes, Chunk chunk)
{
    vst1q_u8(bytes, chunk);
}

static Chunk ChunkFill(unsigned char value)
{
    return vdupq_n_u8(value);
}

static int ChunkDiffers(Chunk first, Chunk second)
{
    const uint64x2_t difference = vreinterpretq_u64_u8(veorq_u8(first, second));
    return (vgetq_lane_u64(difference, 0) | vgetq_lane_u64(difference, 1)) != 0u;
}

#else

typedef __m128i Chunk;

static Chunk ChunkLoad(const unsigned char *bytes)
{
    return _mm_loadu_si128((const __m128i *)(const void *)bytes);
}

static void ChunkStore(unsigned char *bytes, Chunk chunk)
{
    _mm_storeu_si128((__m128i *)(void *)bytes, chunk);
}

static Chunk ChunkFill(unsigned char value)
{
    return _mm_set1_epi8((char)value);
}

static int ChunkDiffers(Chunk first, Chunk second)
{
    return _mm_movemask_epi8(_mm_cmpeq_epi8(first, second)) != 0xFFFF;
}

#endif

/* ---- Короткие длины --------------------------------------------------------
 *
 * До 32 байт — без циклов: две перекрывающиеся порции нужной ширины, и все
 * загрузки раньше записей. Поэтому копия верна при любом перекрытии, а
 * запуск rep movsb и rep stosb на таких длинах дороже самой работы. */

#define SMALL_LIMIT 32u

RUNTIME_NO_BUILTIN
static void CopySmall(unsigned char *output, const unsigned char *input, size_t count)
{
    if (count >= 16u)
    {
        const Chunk head = ChunkLoad(input);
        const Chunk tail = ChunkLoad(input + count - 16u);
        ChunkStore(output, head);
        ChunkStore(output + count - 16u, tail);
    }
    else if (count >= 8u)
    {
        const uint64_t head = Load64(input);
        const uint64_t tail = Load64(input + count - 8u);
        Store64(output, head);
        Store64(output + count - 8u, tail);
    }
    else if (count >= 4u)
    {
        const uint32_t head = Load32(input);
        const uint32_t tail = Load32(input + count - 4u);
        Store32(output, head);
        Store32(output + count - 4u, tail);
    }
    else if (count != 0u)
    {
        const unsigned char first = input[0];
        const unsigned char middle = input[count / 2u];
        const unsigned char last = input[count - 1u];
        output[0] = first;
        output[count / 2u] = middle;
        output[count - 1u] = last;
    }
}

RUNTIME_NO_BUILTIN
static void FillSmall(unsigned char *output, unsigned char value, size_t count)
{
    if (count >= 16u)
    {
        const Chunk fill = ChunkFill(value);
        ChunkStore(output, fill);
        ChunkStore(output + count - 16u, fill);
    }
    else if (count >= 8u)
    {
        const uint64_t word = value * UINT64_C(0x0101010101010101);
        Store64(output, word);
        Store64(output + count - 8u, word);
    }
    else if (count >= 4u)
    {
        const uint32_t word = value * UINT32_C(0x01010101);
        Store32(output, word);
        Store32(output + count - 4u, word);
    }
    else if (count != 0u)
    {
        output[0] = value;
        output[count / 2u] = value;
        output[count - 1u] = value;
    }
}

/* ---- memset и memcpy -------------------------------------------------------- */

/* Длиннее — rep stosb и rep movsb на x64: на современных CPU (ERMSB) это
 * аппаратная скорость, но их запуск стоит как цикл 16-байтных порций на
 * нескольких сотнях байт. У ARM64 таких инструкций нет, там всегда цикл. */
#define LOOP_LIMIT 512u

/* Порции по 16 байт, последняя перекрывает предыдущую. */
RUNTIME_NO_BUILTIN
static void Fill(unsigned char *output, unsigned char value, size_t count)
{
    if (count <= SMALL_LIMIT)
    {
        FillSmall(output, value, count);
        return;
    }
#if !defined(RUNTIME_NEON)
    if (count > LOOP_LIMIT)
    {
#if defined(__clang__)
        /* __stosb под clang понижается до @llvm.memset, а затем может стать
         * вызовом memset. Прямой GNU inline asm не проходит через LLVM memory
         * intrinsics: в объекте гарантированно остаётся rep stosb. */
        __asm__ volatile("rep stosb" : "+D"(output), "+c"(count) : "a"(value) : "memory");
#else
        __stosb(output, value, count);
#endif
        return;
    }
#endif
    const Chunk fill = ChunkFill(value);
    for (size_t index = 0u; index < count - 16u; index += 16u)
        ChunkStore(output + index, fill);
    ChunkStore(output + count - 16u, fill);
}

/* Порции идут по возрастанию адресов, каждая читается до записи, а
 * последнюю цикл перекрыл бы раньше, чем дошёл до неё, поэтому она читается
 * заранее. rep movsb идёт вперёд побайтно по определению инструкции. Так
 * копия верна и тогда, когда приёмник перекрывает источник левее него. */
RUNTIME_NO_BUILTIN
static void CopyForward(unsigned char *output, const unsigned char *input, size_t count)
{
    if (count <= SMALL_LIMIT)
    {
        CopySmall(output, input, count);
        return;
    }
#if !defined(RUNTIME_NEON)
    if (count > LOOP_LIMIT)
    {
        /* __movsb под clang остаётся инструкцией rep movsb (проверено
         * дизассемблером), а не вызовом memcpy. */
        __movsb(output, input, count);
        return;
    }
#endif
    const Chunk last = ChunkLoad(input + count - 16u);
    for (size_t index = 0u; index < count - 16u; index += 16u)
        ChunkStore(output + index, ChunkLoad(input + index));
    ChunkStore(output + count - 16u, last);
}

RUNTIME_NO_BUILTIN
void *memset(void *destination, int value, size_t count)
{
    Fill((unsigned char *)destination, (unsigned char)value, count);
    return destination;
}

RUNTIME_NO_BUILTIN
void *memcpy(void *destination, const void *source, size_t count)
{
    CopyForward((unsigned char *)destination, (const unsigned char *)source, count);
    return destination;
}

/* ---- memmove ---------------------------------------------------------------- */

/* Оптимизатор вызывает memmove для перекрывающихся сдвигов, например при
 * удалении элемента из середины массива. */
RUNTIME_NO_BUILTIN
void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    /* Приёмник левее источника или области не пересекаются: тогда разность
     * адресов, взятая без знака, не меньше длины, и копия вперёд верна.
     * Короткие длины копия вперёд читает целиком до записи. */
    if (count <= SMALL_LIMIT || (uintptr_t)output - (uintptr_t)input >= count)
    {
        CopyForward(output, input, count);
        return destination;
    }
    /* Приёмник перекрывает источник правее него: порции с конца, каждая
     * читается до записи; первую цикл перекрыл бы раньше, поэтому она
     * читается заранее. */
    const Chunk first = ChunkLoad(input);
    for (size_t index = count; index > 16u; index -= 16u)
        ChunkStore(output + index - 16u, ChunkLoad(input + index - 16u));
    ChunkStore(output, first);
    return destination;
}

/* ---- memcmp ----------------------------------------------------------------- */

/* Порядок двух 8-байтных слов, прочитанных из памяти: решает байт с
 * младшим адресом, то есть младший различающийся байт слова (x64 и ARM64 —
 * little-endian). Байты сравниваются как unsigned char. */
static int CompareWords(uint64_t left, uint64_t right)
{
    const uint64_t difference = left ^ right;
    if (difference == 0u)
        return 0;
#if defined(__clang__)
    const unsigned bit = (unsigned)__builtin_ctzll(difference);
#else
    unsigned long bit = 0u;
    (void)_BitScanForward64(&bit, difference);
#endif
    const unsigned shift = (unsigned)bit & ~7u;
    return (int)((left >> shift) & 0xFFu) - (int)((right >> shift) & 0xFFu);
}

/* Совпадающие порции пропускаются целиком; в несовпавшей различие ищется
 * по её двум словам. Хвост короче порции — два перекрывающихся слова: байты
 * в их общей части уже совпали и порядок не меняют. */
RUNTIME_NO_BUILTIN
int memcmp(const void *first, const void *second, size_t count)
{
    const unsigned char *left = (const unsigned char *)first;
    const unsigned char *right = (const unsigned char *)second;
    size_t index = 0u;
    for (; count - index >= 16u; index += 16u)
    {
        if (ChunkDiffers(ChunkLoad(left + index), ChunkLoad(right + index)))
        {
            const int order = CompareWords(Load64(left + index), Load64(right + index));
            return order != 0 ? order
                              : CompareWords(Load64(left + index + 8u), Load64(right + index + 8u));
        }
    }
    if (count - index >= 8u)
    {
        const int order = CompareWords(Load64(left + index), Load64(right + index));
        return order != 0 ? order
                          : CompareWords(Load64(left + count - 8u), Load64(right + count - 8u));
    }
    for (; index < count; ++index)
    {
        if (left[index] != right[index])
            return (int)left[index] - (int)right[index];
    }
    return 0;
}
