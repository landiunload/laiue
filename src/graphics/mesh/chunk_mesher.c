#include "mesh/chunk_mesher.h"
#include "platform/system.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#else
#include <emmintrin.h>
#endif
#include <string.h>

// MSVC даёт поиск младшего установленного бита как _BitScanForward64,
// GCC и Clang — как __builtin_ctzll. Обёртка держит вызовы одинаковыми
// и требует ненулевой аргумент, как обе исходные операции.
static inline uint32_t LowestSetBitIndex(uint64_t value)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanForward64(&index, value);
    return (uint32_t)index;
#else
    return (uint32_t)__builtin_ctzll(value);
#endif
}

// GCC/Clang выбирают реализацию по целевому ISA. У MSVC явный POPCNT
// допустим только в профиле AVX2/AVX512: x64/SSE2 сам по себе его не обещает.
// Для SSE2 и MSVC ARM64 остаётся переносимое суммирование групп битов.
static inline uint32_t PopCount64(uint64_t value)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcountll(value);
#elif defined(_MSC_VER) && defined(_M_X64) && (defined(__AVX2__) || defined(__AVX512F__))
    return (uint32_t)__popcnt64(value);
#else
    value = value - ((value >> 1) & 0x5555555555555555ull);
    value = (value & 0x3333333333333333ull) + ((value >> 2) & 0x3333333333333333ull);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (uint32_t)((value * 0x0101010101010101ull) >> 56);
#endif
}

// Расширенный регион: чанк плюс слой соседних блоков с каждой стороны,
// чтобы грани на границе чанка отсекались без обращений к соседям.
#define EXTENDED_SIZE (CHUNK_SIZE + 2)

// Раскладка из WorldFillRegion: ((y * sizeX) + x) * sizeZ + z, z = высота.
#define BLOCK_INDEX(y, x, z) ((((size_t)(y) * EXTENDED_SIZE) + (size_t)(x)) * EXTENDED_SIZE + (size_t)(z))

#define COLUMN_WORDS ((size_t)CHUNK_SIZE * CHUNK_SIZE)

enum
{
    FACE_COUNT = 6,
};

enum
{
    FACE_POSITIVE_X,
    FACE_NEGATIVE_X,
    FACE_POSITIVE_Y,
    FACE_NEGATIVE_Y,
    FACE_POSITIVE_Z,
    FACE_NEGATIVE_Z,
};

typedef struct QuadBuffer
{
    ChunkQuad* quads;
    uint32_t count;
    uint32_t capacity;
} QuadBuffer;

struct ChunkMesherScratch
{
    BlockType* blocks;
    uint64_t* columns;
    // Шесть плоскостей сразу, по одной на направление грани. Раньше их было
    // две и они переиспользовались, потому что greedy-проход запускался сразу
    // после заполнения очередной пары. Теперь сначала считаются все грани, и
    // только потом идёт слияние: иначе не узнать точный размер выдачи до того,
    // как её начали писать.
    uint64_t* planes;
    // Для каждой из шести плоскостей — признак непустоты каждого слова:
    // слово — срез, бит — ряд. Слово плоскости, в котором нет ни одной грани,
    // greedy-проходу делать нечего, а ряды без граней всё равно остаются
    // нулевыми после общей очистки. Маска позволяет перепрыгнуть такие
    // участки вместо того, чтобы перебрать все 64×64 слов каждой грани.
    // Расположение: [face * CHUNK_SIZE + slice], бит — номер ряда.
    uint64_t* sliceRows;
};

ChunkMesherScratch* ChunkMesherScratchCreate(void)
{
    ChunkMesherScratch* scratch = PlatformAllocate(sizeof(*scratch), true);
    if (scratch == NULL)
    {
        return NULL;
    }

    scratch->blocks = PlatformAllocate((size_t)EXTENDED_SIZE * EXTENDED_SIZE * EXTENDED_SIZE, false);
    scratch->columns = PlatformAllocate(COLUMN_WORDS * 3 * sizeof(uint64_t), false);
    scratch->planes = PlatformAllocate(COLUMN_WORDS * FACE_COUNT * sizeof(uint64_t), false);
    scratch->sliceRows = PlatformAllocate(FACE_COUNT * CHUNK_SIZE * sizeof(uint64_t), false);

    if (scratch->blocks == NULL || scratch->columns == NULL || scratch->planes == NULL
        || scratch->sliceRows == NULL)
    {
        ChunkMesherScratchDestroy(scratch);
        return NULL;
    }

    return scratch;
}

void ChunkMesherScratchDestroy(ChunkMesherScratch* scratch)
{
    if (scratch == NULL)
    {
        return;
    }

    if (scratch->blocks != NULL) PlatformFree(scratch->blocks);
    if (scratch->columns != NULL) PlatformFree(scratch->columns);
    if (scratch->planes != NULL) PlatformFree(scratch->planes);
    if (scratch->sliceRows != NULL) PlatformFree(scratch->sliceRows);
    PlatformFree(scratch);
}

// Ёмкость буфера — точное число граней, а квадов не бывает больше, чем
// граней: каждый выпущенный квад гасит хотя бы один бит плоскости. Проверка
// всё равно остаётся: переполнить выдачу молча хуже, чем отказать.
static bool QuadBufferAppend(QuadBuffer* buffer, ChunkQuad quad)
{
    if (buffer->count == buffer->capacity)
    {
        return false;
    }

    buffer->quads[buffer->count++] = quad;
    return true;
}

// Индекс блока грани — линейная функция (slice, row, bit): шаги зависят
// только от самой грани, а она постоянна на весь проход. Раньше их считал
// switch внутри самого внутреннего цикла, и на каждый бит уходили переход
// и два умножения; теперь шаги берутся один раз, а срез и ряд сворачиваются
// в указатель до входа в цикл по битам.
typedef struct FaceLayout
{
    const BlockType* base;
    size_t sliceStride;
    size_t rowStride;
    size_t bitStride;
} FaceLayout;

static inline FaceLayout FaceLayoutFor(uint32_t face, const BlockType* blocks)
{
    const size_t plane = (size_t)EXTENDED_SIZE * (size_t)EXTENDED_SIZE;
    FaceLayout layout;
    layout.base = blocks + BLOCK_INDEX(1, 1, 1);
    switch (face)
    {
        case FACE_POSITIVE_X:
        case FACE_NEGATIVE_X:
            // BLOCK_INDEX(row + 1, slice + 1, bit + 1)
            layout.sliceStride = EXTENDED_SIZE;
            layout.rowStride = plane;
            layout.bitStride = 1;
            break;

        case FACE_POSITIVE_Y:
        case FACE_NEGATIVE_Y:
            // BLOCK_INDEX(slice + 1, row + 1, bit + 1)
            layout.sliceStride = plane;
            layout.rowStride = EXTENDED_SIZE;
            layout.bitStride = 1;
            break;

        default:
            // BLOCK_INDEX(row + 1, bit + 1, slice + 1)
            layout.sliceStride = 1;
            layout.rowStride = plane;
            layout.bitStride = EXTENDED_SIZE;
            break;
    }
    return layout;
}

// Переводит прямоугольник плоскости (slice, биты, ряды) в оси чанка.
// Z = высота, Y = вторая горизонталь.
static bool EmitFaceRectangle(QuadBuffer* buffer, uint32_t face, uint32_t slice,
    uint32_t bitStart, uint32_t bitExtent, uint32_t rowStart,
    uint32_t rowExtent, BlockType blockType)
{
    switch (face)
    {
        case FACE_POSITIVE_X:
        case FACE_NEGATIVE_X:
            // Нормаль X; биты = Z (высота), ряды = Y (вторая горизонталь).
            return QuadBufferAppend(buffer,
                PackChunkQuad(slice, rowStart, bitStart, face, blockType,
                    1, rowExtent, bitExtent));

        case FACE_POSITIVE_Y:
        case FACE_NEGATIVE_Y:
            // Нормаль Y; биты = Z (высота), ряды = X.
            return QuadBufferAppend(buffer,
                PackChunkQuad(rowStart, slice, bitStart, face, blockType,
                    rowExtent, 1, bitExtent));

        default:
            // Нормаль Z (высота); биты = X, ряды = Y.
            return QuadBufferAppend(buffer,
                PackChunkQuad(bitStart, rowStart, slice, face, blockType,
                    bitExtent, rowExtent, 1));
    }
}

// Сравнение шестнадцати блоков с одним материалом: бит i результата стоит,
// если блок i совпал. То же, что делает ColumnSolidMask с воздухом, только
// образец задаёт вызывающая сторона. Номер бита — номер байта в памяти на
// обеих архитектурах, порядок байтов внутри слова здесь ни при чём.
#if defined(_M_ARM64) || defined(__aarch64__)
static inline uint32_t EqualMask16(const BlockType* data, BlockType blockType)
{
    static const uint8_t laneBitTable[16] = {
        1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u
    };
    const uint8x16_t laneBits = vld1q_u8(laneBitTable);
    uint8x16_t lanes = vld1q_u8((const uint8_t*)data);
    uint8x16_t equal = vceqq_u8(lanes, vdupq_n_u8((uint8_t)blockType));
    uint8x16_t selected = vandq_u8(equal, laneBits);
    return (uint32_t)vaddv_u8(vget_low_u8(selected))
        | ((uint32_t)vaddv_u8(vget_high_u8(selected)) << 8);
}
#else
static inline uint32_t EqualMask16(const BlockType* data, BlockType blockType)
{
    __m128i lanes = _mm_loadu_si128((const __m128i*)data);
    __m128i wanted = _mm_set1_epi8((char)blockType);
    return (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(lanes, wanted));
}
#endif

// Сколько подряд идущих блоков ряда, начиная со start, имеют материал
// blockType; больше limit не считает.
//
// У четырёх направлений из шести ряд грани лежит в памяти сплошняком: у ±X и
// ±Y бит — это z, а z в регионе идёт подряд. Тогда шестнадцать блоков
// сравниваются одной командой вместо шестнадцати чтений. Окно сдвигается так,
// чтобы не выходить за сам ряд, поэтому лишних байтов не читается никогда.
static inline uint32_t MaterialRunContiguous(const BlockType* row, uint32_t start,
    uint32_t limit, BlockType blockType)
{
    uint32_t length = 0;
    for (;;)
    {
        uint32_t offset = start + length;
        uint32_t group = offset + 16u <= (uint32_t)CHUNK_SIZE
            ? offset
            : (uint32_t)CHUNK_SIZE - 16u;
        uint32_t shift = offset - group;

        // В дополнении маски разряды выше окна стоят, поэтому нулевого
        // аргумента у поиска младшего бита не бывает, а ответ не уходит за окно.
        uint32_t step =
            LowestSetBitIndex(~(uint64_t)(EqualMask16(row + group, blockType) >> shift));
        length += step;
        if (length >= limit)
        {
            return limit;
        }
        if (step < 16u - shift)
        {
            return length;
        }
    }
}

// Тот же счёт для ±Z, где соседние блоки ряда отстоят на целую строку региона
// и группами их не взять.
static inline uint32_t MaterialRunStrided(const BlockType* row, uint32_t start,
    uint32_t length, uint32_t limit, BlockType blockType, size_t bitStride)
{
    while (length < limit && row[(size_t)(start + length) * bitStride] == blockType)
    {
        ++length;
    }
    return length;
}

// Продолжает полосу, у которой первые length блоков уже совпали.
static inline uint32_t MaterialRun(const BlockType* row, uint32_t start, uint32_t length,
    uint32_t limit, BlockType blockType, size_t bitStride)
{
    if (length >= limit)
    {
        return limit;
    }
    if (bitStride == 1)
    {
        return MaterialRunContiguous(row, start, limit, blockType);
    }
    return MaterialRunStrided(row, start, length, limit, blockType, bitStride);
}

static bool GreedyMeshPlanes(QuadBuffer* buffer, uint32_t face,
    uint64_t* planes, const uint64_t* sliceRows, const BlockType* blocks)
{
    const FaceLayout layout = FaceLayoutFor(face, blocks);

    for (uint32_t slice = 0; slice < CHUNK_SIZE; ++slice)
    {
        // Срез без единой грани пропускается целиком: раньше пустой ряд
        // всё равно стоил чтения слова из плоскости и проверки на ноль.
        if (sliceRows[slice] == 0)
        {
            continue;
        }

        uint64_t* rows = &planes[(size_t)slice * CHUNK_SIZE];
        const BlockType* sliceBase = layout.base + (size_t)slice * layout.sliceStride;

        for (uint32_t row = 0; row < CHUNK_SIZE; ++row)
        {
            uint64_t bits = rows[row];
            const BlockType* rowBase = sliceBase + (size_t)row * layout.rowStride;

            while (bits != 0)
            {
                uint32_t runStart = LowestSetBitIndex(bits);

                BlockType blockType = rowBase[(size_t)runStart * layout.bitStride];

                // Второй блок проверяется поштучно, как и раньше: на
                // несливаемой сцене полоса кончается ровно здесь, и всё
                // остальное было бы чистым проигрышем. Дальше, то есть на
                // длинных полосах, работает другой счёт: сплошной участок
                // установленных битов ограничивает полосу сверху без единого
                // обращения к блокам, а материал сверяется группами.
                uint64_t tail = bits >> runStart;
                uint32_t runLength = 1;
                if ((tail & 2u) != 0
                    && rowBase[(size_t)(runStart + 1) * layout.bitStride] == blockType)
                {
                    uint64_t gaps = ~tail;
                    uint32_t bitRun = gaps != 0
                        ? LowestSetBitIndex(gaps)
                        : (uint32_t)CHUNK_SIZE - runStart;
                    runLength = MaterialRun(rowBase, runStart, 2u, bitRun, blockType,
                        layout.bitStride);
                }

                uint64_t runMask = runLength == 64
                    ? ~0ull
                    : (((1ull << runLength) - 1) << runStart);

                uint32_t rowExtent = 1;
                while (row + rowExtent < CHUNK_SIZE)
                {
                    uint32_t nextRow = row + rowExtent;
                    if ((rows[nextRow] & runMask) != runMask)
                    {
                        break;
                    }

                    const BlockType* nextBase = sliceBase + (size_t)nextRow * layout.rowStride;
                    if (nextBase[(size_t)runStart * layout.bitStride] != blockType)
                    {
                        break;
                    }
                    if (MaterialRun(nextBase, runStart, 1u, runLength, blockType,
                            layout.bitStride) != runLength)
                    {
                        break;
                    }

                    rows[row + rowExtent] &= ~runMask;
                    ++rowExtent;
                }

                bits &= ~runMask;

                if (!EmitFaceRectangle(buffer, face, slice,
                        (uint32_t)runStart, runLength, row, rowExtent,
                        blockType))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Маска непустых вокселей колонны из 64 блоков: четыре сравнения по 16 байт
// вместо 64 отдельных. BlockType — байт, BLOCK_AIR — ноль, сравнение с нулём
// и даёт искомые биты. Чтение неровное по выравниванию — так и задумано.
// SSE2 входит в базовый набор команд x64, NEON — в базовый набор ARMv8,
// поэтому проверки CPU не нужны ни на одной из архитектур.
#if defined(_M_ARM64) || defined(__aarch64__)
static inline uint64_t ColumnSolidMask(const BlockType* column)
{
    // У NEON нет movemask: каждая ненулевая полоса умножается на свой бит,
    // а горизонтальная сумма половин собирает два байта результата.
    static const uint8_t laneBitTable[16] = {
        1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u
    };
    const uint8x16_t laneBits = vld1q_u8(laneBitTable);
    uint64_t solidMask = 0;

    for (uint32_t offset = 0; offset < CHUNK_SIZE; offset += 16)
    {
        uint8x16_t voxels = vld1q_u8((const uint8_t*)(column + offset));
        uint8x16_t solidLanes = vmvnq_u8(vceqq_u8(voxels, vdupq_n_u8(0)));
        uint8x16_t selected = vandq_u8(solidLanes, laneBits);
        uint32_t solidBits = (uint32_t)vaddv_u8(vget_low_u8(selected))
            | ((uint32_t)vaddv_u8(vget_high_u8(selected)) << 8);
        solidMask |= (uint64_t)solidBits << offset;
    }

    return solidMask;
}
#else
static inline uint64_t ColumnSolidMask(const BlockType* column)
{
    const __m128i airVector = _mm_setzero_si128();
    uint64_t solidMask = 0;

    for (uint32_t offset = 0; offset < CHUNK_SIZE; offset += 16)
    {
        __m128i voxels = _mm_loadu_si128((const __m128i*)(column + offset));
        uint32_t airBits = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(voxels, airVector));
        solidMask |= (uint64_t)(~airBits & 0xFFFFu) << offset;
    }

    return solidMask;
}
#endif

// Транспонирование битовой матрицы 64x64 на месте: после вызова бит i слова
// s равен биту s слова i до вызова. Шесть раундов обмена половинами, четвертями
// и так далее.
//
// Обмен зеркален привычной записи из литературы: там нулевым столбцом считают
// старший бит, а здесь номер бита и есть номер столбца.
//
// Все сдвиги в обмене — внутри одного 64-битного слова, поэтому два соседних
// слова обрабатываются одной 128-битной командой: раунд со страйдом S читает
// по паре слов, и обе половины параллельно обмениваются со своими парами.
// Так проходят страйды 32, 16, 8, 4 и 2. Последний раунд со страйдом 1
// сцепляет соседние слова, и 128-битное чтение легло бы на границу массива,
// поэтому он остаётся скалярным — тридцать два обмена над регистрами.
#if defined(_M_ARM64) || defined(__aarch64__)
#define TRANSPOSE_ROUND_NEON(S, MASK)                                                    \
    do                                                                                   \
    {                                                                                    \
        const uint32_t step_ = (S)*2u;                                                   \
        const uint64x2_t mask_ = vdupq_n_u64((uint64_t)(MASK));                          \
        for (uint32_t base_ = 0u; base_ < CHUNK_SIZE; base_ += step_)                    \
        {                                                                                \
            for (uint32_t index_ = base_; index_ < base_ + (S); index_ += 2u)            \
            {                                                                            \
                uint64x2_t low_ = vld1q_u64(&matrix[index_]);                            \
                uint64x2_t high_ = vld1q_u64(&matrix[index_ + (S)]);                     \
                uint64x2_t swap_ = vandq_u64(veorq_u64(vshrq_n_u64(low_, (S)), high_),   \
                    mask_);                                                              \
                low_ = veorq_u64(low_, vshlq_n_u64(swap_, (S)));                         \
                high_ = veorq_u64(high_, swap_);                                         \
                vst1q_u64(&matrix[index_], low_);                                        \
                vst1q_u64(&matrix[index_ + (S)], high_);                                 \
            }                                                                            \
        }                                                                                \
    } while (0)

static void TransposeBits64(uint64_t matrix[CHUNK_SIZE])
{
    TRANSPOSE_ROUND_NEON(32u, 0x00000000FFFFFFFFull);
    TRANSPOSE_ROUND_NEON(16u, 0x0000FFFF0000FFFFull);
    TRANSPOSE_ROUND_NEON(8u, 0x00FF00FF00FF00FFull);
    TRANSPOSE_ROUND_NEON(4u, 0x0F0F0F0F0F0F0F0Full);
    TRANSPOSE_ROUND_NEON(2u, 0x3333333333333333ull);
    {
        const uint64_t mask = 0x5555555555555555ull;
        for (uint32_t index = 0u; index < CHUNK_SIZE; index += 2u)
        {
            uint64_t swap = ((matrix[index] >> 1u) ^ matrix[index + 1u]) & mask;
            matrix[index] ^= swap << 1u;
            matrix[index + 1u] ^= swap;
        }
    }
}
#else
#define TRANSPOSE_ROUND_SSE2(S, MASK)                                                    \
    do                                                                                   \
    {                                                                                    \
        const uint32_t step_ = (S)*2u;                                                   \
        const __m128i mask_ = _mm_set1_epi64x((long long)(MASK));                        \
        for (uint32_t base_ = 0u; base_ < CHUNK_SIZE; base_ += step_)                    \
        {                                                                                \
            for (uint32_t index_ = base_; index_ < base_ + (S); index_ += 2u)            \
            {                                                                            \
                __m128i low_ = _mm_loadu_si128((const __m128i *)(const void *)&matrix[index_]); \
                __m128i high_ = _mm_loadu_si128(                                         \
                    (const __m128i *)(const void *)&matrix[index_ + (S)]);               \
                __m128i swap_ = _mm_and_si128(_mm_xor_si128(_mm_srli_epi64(low_, (S)),   \
                    high_), mask_);                                                      \
                low_ = _mm_xor_si128(low_, _mm_slli_epi64(swap_, (S)));                  \
                high_ = _mm_xor_si128(high_, swap_);                                     \
                _mm_storeu_si128((__m128i *)(void *)&matrix[index_], low_);              \
                _mm_storeu_si128((__m128i *)(void *)&matrix[index_ + (S)], high_);       \
            }                                                                            \
        }                                                                                \
    } while (0)

static void TransposeBits64(uint64_t matrix[CHUNK_SIZE])
{
    TRANSPOSE_ROUND_SSE2(32u, 0x00000000FFFFFFFFull);
    TRANSPOSE_ROUND_SSE2(16u, 0x0000FFFF0000FFFFull);
    TRANSPOSE_ROUND_SSE2(8u, 0x00FF00FF00FF00FFull);
    TRANSPOSE_ROUND_SSE2(4u, 0x0F0F0F0F0F0F0F0Full);
    TRANSPOSE_ROUND_SSE2(2u, 0x3333333333333333ull);
    {
        const uint64_t mask = 0x5555555555555555ull;
        for (uint32_t index = 0u; index < CHUNK_SIZE; index += 2u)
        {
            uint64_t swap = ((matrix[index] >> 1u) ^ matrix[index + 1u]) & mask;
            matrix[index] ^= swap << 1u;
            matrix[index + 1u] ^= swap;
        }
    }
}
#endif

// В отличие от плоскостей граней, обе раскладки колонн пишутся подряд.
// Редкая матрица дешевле раскладывается по битам, плотная — транспонируется.
// Порог проверен A/B; он влияет только на способ построения тех же масок.
#define MESH_TRANSPOSE_COLUMN_SOLIDS 256u
#define MESH_SPARSE_CHUNK_SOLIDS 4096u

// source и destination лежат в разных массивах колонн, поэтому sparse-путь
// может обнулить destination и повторно прочитать исходные слова.
static void BuildTransverseColumns(uint64_t *destination, const uint64_t *source, size_t stride)
{
    uint32_t solidCount = 0;
    for (uint32_t index = 0; index < CHUNK_SIZE; ++index)
    {
        uint64_t column = source[(size_t)index * stride];
        destination[index] = column;
        solidCount += PopCount64(column);
    }
    if (solidCount == 0)
    {
        return;
    }
    if (solidCount >= MESH_TRANSPOSE_COLUMN_SOLIDS)
    {
        TransposeBits64(destination);
        return;
    }

    memset(destination, 0, CHUNK_SIZE * sizeof(uint64_t));
    for (uint32_t index = 0; index < CHUNK_SIZE; ++index)
    {
        uint64_t remaining = source[(size_t)index * stride];
        uint64_t bit = 1ull << index;
        while (remaining != 0)
        {
            uint32_t offset = LowestSetBitIndex(remaining);
            remaining &= remaining - 1;
            destination[offset] |= bit;
        }
    }
}

// Складывает шестьдесят четыре маски одного ряда в плоскость.
//
// Маска номер i несёт биты по срезам, а плоскости нужен срез с битами по i —
// это ровно транспонирование. Раньше то же самое делалось поразрядно: на
// плотной сцене до четырёх тысяч разбросанных чтений-записей на ряд, каждая в
// свою строку кэша, потому что соседние срезы отстоят на полкилобайта.
//
// Запись, а не наложение: слово plane[slice * CHUNK_SIZE + row] принадлежит
// одному ряду, и другой ряд в него не пишет. Ряды без единой грани
// пропускаются целиком и остаются нулевыми после общей очистки плоскостей.
static void StorePlaneRow(uint64_t* plane, uint32_t row, uint64_t masks[CHUNK_SIZE])
{
    TransposeBits64(masks);
    for (uint32_t slice = 0u; slice < CHUNK_SIZE; ++slice)
    {
        plane[(size_t)slice * CHUNK_SIZE + row] = masks[slice];
    }
}

// Поразрядная раскладка того же ряда. Транспонирование стоит одинаково при
// любом числе граней, а этот путь — по одной записи на грань, поэтому на
// редких рядах он дешевле.
static void ScatterPlaneRow(uint64_t* plane, uint32_t row, const uint64_t masks[CHUNK_SIZE])
{
    for (uint32_t index = 0u; index < CHUNK_SIZE; ++index)
    {
        uint64_t faceMask = masks[index];
        uint64_t planeBit = 1ull << index;
        while (faceMask != 0)
        {
            uint32_t slice = LowestSetBitIndex(faceMask);
            faceMask &= faceMask - 1;
            plane[(size_t)slice * CHUNK_SIZE + row] |= planeBit;
        }
    }
}

// Порог, за которым транспонирование окупается. Оно обходится примерно в
// тысячу операций над регистрами независимо от плотности, а поразрядная
// раскладка — в одну разбросанную запись на грань, и каждая попадает в свою
// строку кэша: соседние срезы отстоят на полкилобайта. Значение выбрано
// замером, а не рассуждением; оба пути дают одну и ту же плоскость.
#define MESH_TRANSPOSE_FACES 256u

static void EmitPlaneRow(uint64_t* plane, uint32_t row, uint64_t masks[CHUNK_SIZE], uint32_t faces)
{
    if (faces == 0u)
    {
        return;
    }
    if (faces >= MESH_TRANSPOSE_FACES)
    {
        StorePlaneRow(plane, row, masks);
    }
    else
    {
        ScatterPlaneRow(plane, row, masks);
    }
}

bool BuildChunkMesh(const ChunkMesherWorldSource* source, ChunkMesherScratch* scratch,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkQuad** outQuads, uint32_t* outQuadCount)
{
    if (outQuads == NULL || outQuadCount == NULL)
    {
        return false;
    }
    *outQuads = NULL;
    *outQuadCount = 0;

    if (source == NULL || source->fillRegion == NULL || scratch == NULL ||
        scratch->blocks == NULL || scratch->columns == NULL || scratch->planes == NULL)
    {
        return false;
    }

    int64_t baseX = chunkX * CHUNK_SIZE;
    int64_t baseY = chunkY * CHUNK_SIZE;  // Y = вторая горизонталь
    int64_t baseZ = chunkZ * CHUNK_SIZE;  // Z = высота

    BlockType* blocks = scratch->blocks;

    WorldRegionContents contents = source->fillRegion(source->context,
        baseX - 1, baseY - 1, baseZ - 1,
        EXTENDED_SIZE, EXTENDED_SIZE, EXTENDED_SIZE,
        blocks);

    if (contents != WORLD_REGION_MIXED)
    {
        return true;
    }

    // columnsZ[y*64+x] — биты вдоль Z (высота)
    // columnsY[x*64+z] — биты вдоль Y (вторая горизонталь)
    // columnsX[y*64+z] — биты вдоль X
    uint64_t* columnsZ = scratch->columns;                      // [y*64+x]
    uint64_t* columnsY = scratch->columns + COLUMN_WORDS;       // [x*64+z]
    uint64_t* columnsX = scratch->columns + COLUMN_WORDS * 2;   // [y*64+z]
    uint64_t* planes[FACE_COUNT];
    for (uint32_t face = 0; face < FACE_COUNT; ++face)
    {
        planes[face] = scratch->planes + (size_t)face * COLUMN_WORDS;
    }
    size_t planeBytes = COLUMN_WORDS * sizeof(uint64_t);

    // Сначала непрерывные Z-колонны. Две поперечные раскладки — это те же
    // битовые матрицы, транспонированные независимо при фиксированных Y и X.
    // В dense-пути каждый выходной элемент записывается целиком, поэтому
    // предварительная очистка колонн не нужна.
    uint32_t solidCount = 0;
    for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
    {
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            uint64_t column = ColumnSolidMask(&blocks[BLOCK_INDEX(y + 1, x + 1, 1)]);
            columnsZ[y * CHUNK_SIZE + x] = column;
            solidCount += PopCount64(column);
        }
    }

    if (solidCount == 0)
    {
        return true;
    }
    // Для почти пустого чанка общий scatter дешевле подготовки 128 матриц.
    // Даже при полном halo пустое ядро выше выходит без старых масок scratch.
    if (solidCount < MESH_SPARSE_CHUNK_SOLIDS)
    {
        memset(columnsY, 0, planeBytes * 2);
        for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
        {
            uint64_t rowBit = 1ull << y;
            for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
            {
                uint64_t remaining = columnsZ[y * CHUNK_SIZE + x];
                uint64_t columnBit = 1ull << x;
                while (remaining != 0)
                {
                    uint32_t z = LowestSetBitIndex(remaining);
                    remaining &= remaining - 1;
                    columnsY[x * CHUNK_SIZE + z] |= rowBit;
                    columnsX[y * CHUNK_SIZE + z] |= columnBit;
                }
            }
        }
    }
    else
    {
        for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
        {
            BuildTransverseColumns(&columnsX[y * CHUNK_SIZE], &columnsZ[y * CHUNK_SIZE], 1);
        }
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            BuildTransverseColumns(&columnsY[x * CHUNK_SIZE], &columnsZ[x], CHUNK_SIZE);
        }
    }

    uint32_t faceCount = 0;
    memset(scratch->planes, 0, planeBytes * FACE_COUNT);

    // Один ряд масок на оба знака грани; переиспользуется всеми тремя парами
    // осей, поэтому кадр стека остаётся в килобайте.
    uint64_t positive[CHUNK_SIZE];
    uint64_t negative[CHUNK_SIZE];

    // === Грани ±Z (высота, нормаль = Z) ===
    for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
    {
        uint32_t anyPositive = 0;
        uint32_t anyNegative = 0;
        uint64_t spanPositive = 0;
        uint64_t spanNegative = 0;
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            uint64_t column = columnsZ[y * CHUNK_SIZE + x];
            if (column == 0)
            {
                positive[x] = 0;
                negative[x] = 0;
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(y + 1, x + 1, EXTENDED_SIZE - 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(y + 1, x + 1, 0)] != BLOCK_AIR);
            positive[x] = column & ~((column >> 1) | (neighborAbove << 63));
            negative[x] = column & ~((column << 1) | neighborBelow);
            anyPositive += PopCount64(positive[x]);
            anyNegative += PopCount64(negative[x]);
            spanPositive |= positive[x];
            spanNegative |= negative[x];
        }
        // Бит среза в этом ряду для маски непустых слов плоскости.
        scratch->sliceRows[(size_t)FACE_POSITIVE_Z * CHUNK_SIZE + y] = spanPositive;
        scratch->sliceRows[(size_t)FACE_NEGATIVE_Z * CHUNK_SIZE + y] = spanNegative;
        faceCount += anyPositive + anyNegative;
        EmitPlaneRow(planes[FACE_POSITIVE_Z], y, positive, anyPositive);
        EmitPlaneRow(planes[FACE_NEGATIVE_Z], y, negative, anyNegative);
    }
    TransposeBits64(&scratch->sliceRows[FACE_POSITIVE_Z * CHUNK_SIZE]);
    TransposeBits64(&scratch->sliceRows[FACE_NEGATIVE_Z * CHUNK_SIZE]);

    // === Грани ±X ===
    for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
    {
        uint32_t anyPositive = 0;
        uint32_t anyNegative = 0;
        uint64_t spanPositive = 0;
        uint64_t spanNegative = 0;
        for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        {
            uint64_t column = columnsX[y * CHUNK_SIZE + z];
            if (column == 0)
            {
                positive[z] = 0;
                negative[z] = 0;
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(y + 1, EXTENDED_SIZE - 1, z + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(y + 1, 0, z + 1)] != BLOCK_AIR);
            positive[z] = column & ~((column >> 1) | (neighborAbove << 63));
            negative[z] = column & ~((column << 1) | neighborBelow);
            anyPositive += PopCount64(positive[z]);
            anyNegative += PopCount64(negative[z]);
            spanPositive |= positive[z];
            spanNegative |= negative[z];
        }
        scratch->sliceRows[(size_t)FACE_POSITIVE_X * CHUNK_SIZE + y] = spanPositive;
        scratch->sliceRows[(size_t)FACE_NEGATIVE_X * CHUNK_SIZE + y] = spanNegative;
        faceCount += anyPositive + anyNegative;
        EmitPlaneRow(planes[FACE_POSITIVE_X], y, positive, anyPositive);
        EmitPlaneRow(planes[FACE_NEGATIVE_X], y, negative, anyNegative);
    }
    TransposeBits64(&scratch->sliceRows[FACE_POSITIVE_X * CHUNK_SIZE]);
    TransposeBits64(&scratch->sliceRows[FACE_NEGATIVE_X * CHUNK_SIZE]);

    // === Грани ±Y (вторая горизонталь, нормаль = Y) ===
    for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
    {
        uint32_t anyPositive = 0;
        uint32_t anyNegative = 0;
        uint64_t spanPositive = 0;
        uint64_t spanNegative = 0;
        for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        {
            uint64_t column = columnsY[x * CHUNK_SIZE + z];
            if (column == 0)
            {
                positive[z] = 0;
                negative[z] = 0;
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(EXTENDED_SIZE - 1, x + 1, z + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(0, x + 1, z + 1)] != BLOCK_AIR);
            positive[z] = column & ~((column >> 1) | (neighborAbove << 63));
            negative[z] = column & ~((column << 1) | neighborBelow);
            anyPositive += PopCount64(positive[z]);
            anyNegative += PopCount64(negative[z]);
            spanPositive |= positive[z];
            spanNegative |= negative[z];
        }
        scratch->sliceRows[(size_t)FACE_POSITIVE_Y * CHUNK_SIZE + x] = spanPositive;
        scratch->sliceRows[(size_t)FACE_NEGATIVE_Y * CHUNK_SIZE + x] = spanNegative;
        faceCount += anyPositive + anyNegative;
        EmitPlaneRow(planes[FACE_POSITIVE_Y], x, positive, anyPositive);
        EmitPlaneRow(planes[FACE_NEGATIVE_Y], x, negative, anyNegative);
    }
    TransposeBits64(&scratch->sliceRows[FACE_POSITIVE_Y * CHUNK_SIZE]);
    TransposeBits64(&scratch->sliceRows[FACE_NEGATIVE_Y * CHUNK_SIZE]);

    if (faceCount == 0)
    {
        return true;
    }

    // Выдача выделяется один раз и ровно на число граней: слить их можно
    // только в меньшее число квадов, больше не станет никогда. Greedy-проход
    // пишет прямо сюда, поэтому копии в конце нет вовсе.
    QuadBuffer quadBuffer;
    quadBuffer.quads = PlatformAllocate((size_t)faceCount * sizeof(ChunkQuad), false);
    quadBuffer.count = 0;
    quadBuffer.capacity = faceCount;
    if (quadBuffer.quads == NULL)
    {
        return false;
    }

    // Порядок направлений — часть формата геометрии и менять его нельзя.
    static const uint32_t order[FACE_COUNT] = {
        FACE_POSITIVE_Z, FACE_NEGATIVE_Z, FACE_POSITIVE_X,
        FACE_NEGATIVE_X, FACE_POSITIVE_Y, FACE_NEGATIVE_Y,
    };
    for (uint32_t index = 0; index < FACE_COUNT; ++index)
    {
        if (!GreedyMeshPlanes(&quadBuffer, order[index], planes[order[index]],
                &scratch->sliceRows[(size_t)order[index] * CHUNK_SIZE], blocks))
        {
            PlatformFree(quadBuffer.quads);
            return false;
        }
    }

    // Квадов вышло меньше, чем граней: лишний хвост возвращается аллокатору.
    // Уменьшение блока обычно происходит на месте; если аллокатор всё же
    // откажет, прежний блок остаётся годным, просто с запасом.
    size_t bytes = (size_t)quadBuffer.count * sizeof(ChunkQuad);
    ChunkQuad* shrunk = PlatformReallocate(quadBuffer.quads, bytes, false);
    *outQuads = shrunk != NULL ? shrunk : quadBuffer.quads;
    *outQuadCount = quadBuffer.count;
    return true;
}
