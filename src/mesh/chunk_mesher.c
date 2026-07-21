#include "mesh/chunk_mesher.h"
#include "world/world.h"

#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <string.h>

// Расширенный регион: чанк плюс слой соседних блоков с каждой стороны,
// чтобы грани на границе чанка отсекались без обращений к соседям.
#define EXTENDED_SIZE (CHUNK_SIZE + 2)

// Раскладка из WorldFillRegion: ((y * sizeX) + x) * sizeZ + z, z = высота.
#define BLOCK_INDEX(y, x, z) ((((size_t)(y) * EXTENDED_SIZE) + (size_t)(x)) * EXTENDED_SIZE + (size_t)(z))

#define COLUMN_WORDS ((size_t)CHUNK_SIZE * CHUNK_SIZE)

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
    float* heights;
    uint64_t* columns;
    uint64_t* planesPositive;
    uint64_t* planesNegative;
    QuadBuffer quadBuffer;
};

ChunkMesherScratch* ChunkMesherScratchCreate(void)
{
    ChunkMesherScratch* scratch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*scratch));
    if (scratch == NULL)
    {
        return NULL;
    }

    scratch->blocks = HeapAlloc(GetProcessHeap(), 0, (size_t)EXTENDED_SIZE * EXTENDED_SIZE * EXTENDED_SIZE);
    scratch->heights = HeapAlloc(GetProcessHeap(), 0, (size_t)EXTENDED_SIZE * EXTENDED_SIZE * sizeof(float));
    scratch->columns = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * 3 * sizeof(uint64_t));
    scratch->planesPositive = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * sizeof(uint64_t));
    scratch->planesNegative = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * sizeof(uint64_t));

    if (scratch->blocks == NULL || scratch->heights == NULL
        || scratch->columns == NULL ||
        scratch->planesPositive == NULL || scratch->planesNegative == NULL)
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

    if (scratch->blocks != NULL) HeapFree(GetProcessHeap(), 0, scratch->blocks);
    if (scratch->heights != NULL) HeapFree(GetProcessHeap(), 0, scratch->heights);
    if (scratch->columns != NULL) HeapFree(GetProcessHeap(), 0, scratch->columns);
    if (scratch->planesPositive != NULL) HeapFree(GetProcessHeap(), 0, scratch->planesPositive);
    if (scratch->planesNegative != NULL) HeapFree(GetProcessHeap(), 0, scratch->planesNegative);
    if (scratch->quadBuffer.quads != NULL) HeapFree(GetProcessHeap(), 0, scratch->quadBuffer.quads);
    HeapFree(GetProcessHeap(), 0, scratch);
}

static bool QuadBufferAppend(QuadBuffer* buffer, ChunkQuad quad)
{
    if (buffer->count == buffer->capacity)
    {
        uint32_t newCapacity = buffer->capacity == 0 ? 1024 : buffer->capacity * 2;
        ChunkQuad* newQuads = buffer->quads == NULL
            ? HeapAlloc(GetProcessHeap(), 0, (size_t)newCapacity * sizeof(ChunkQuad))
            : HeapReAlloc(GetProcessHeap(), 0, buffer->quads, (size_t)newCapacity * sizeof(ChunkQuad));
        if (newQuads == NULL)
        {
            return false;
        }
        buffer->quads = newQuads;
        buffer->capacity = newCapacity;
    }

    buffer->quads[buffer->count++] = quad;
    return true;
}

// Материал блока, которому принадлежит грань. Координаты соответствуют
// раскладке плоскостей greedy-мешера, а +1 переводит их в halo-регион 66^3.
static inline BlockType FaceBlockType(const BlockType* blocks,
    uint32_t face, uint32_t slice, uint32_t row, uint32_t bit)
{
    switch (face)
    {
        case FACE_POSITIVE_X:
        case FACE_NEGATIVE_X:
            return blocks[BLOCK_INDEX(row + 1, slice + 1, bit + 1)];

        case FACE_POSITIVE_Y:
        case FACE_NEGATIVE_Y:
            return blocks[BLOCK_INDEX(slice + 1, row + 1, bit + 1)];

        default:
            return blocks[BLOCK_INDEX(row + 1, bit + 1, slice + 1)];
    }
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

static bool GreedyMeshPlanes(QuadBuffer* buffer, uint32_t face,
    uint64_t* planes, const BlockType* blocks)
{
    for (uint32_t slice = 0; slice < CHUNK_SIZE; ++slice)
    {
        uint64_t* rows = &planes[(size_t)slice * CHUNK_SIZE];

        for (uint32_t row = 0; row < CHUNK_SIZE; ++row)
        {
            uint64_t bits = rows[row];

            while (bits != 0)
            {
                unsigned long runStart;
                _BitScanForward64(&runStart, bits);

                BlockType blockType = FaceBlockType(
                    blocks, face, slice, row, (uint32_t)runStart);
                uint32_t runLength = 1;
                while ((uint32_t)runStart + runLength < CHUNK_SIZE)
                {
                    uint32_t bit = (uint32_t)runStart + runLength;
                    if ((bits & (1ull << bit)) == 0
                        || FaceBlockType(blocks, face, slice, row, bit)
                            != blockType)
                    {
                        break;
                    }
                    ++runLength;
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

                    bool sameMaterial = true;
                    for (uint32_t offset = 0; offset < runLength; ++offset)
                    {
                        if (FaceBlockType(blocks, face, slice, nextRow,
                                (uint32_t)runStart + offset) != blockType)
                        {
                            sameMaterial = false;
                            break;
                        }
                    }
                    if (!sameMaterial)
                    {
                        break;
                    }

                    rows[row + rowExtent] &= ~runMask;
                    ++rowExtent;
                }

                bits &= ~runMask;
                rows[row] &= ~runMask;

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

// Маска непустых вокселей колонны из 64 блоков. SSE2 входит в базовый набор
// команд x64, поэтому проверки CPU не нужны: четыре сравнения по 16 байт
// вместо 64 отдельных. BlockType — байт, BLOCK_AIR — ноль, сравнение с нулём
// и даёт искомые биты. Чтение неровное по выравниванию — так и задумано.
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

static inline void ScatterFaceBits(uint64_t faceMask, uint64_t* planes, uint32_t row, uint64_t planeBit)
{
    while (faceMask != 0)
    {
        unsigned long slice;
        _BitScanForward64(&slice, faceMask);
        faceMask &= faceMask - 1;
        planes[(size_t)slice * CHUNK_SIZE + row] |= planeBit;
    }
}

bool BuildChunkMesh(World* world, ChunkMesherScratch* scratch,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkQuad** outQuads, uint32_t* outQuadCount)
{
    *outQuads = NULL;
    *outQuadCount = 0;

    int64_t baseX = chunkX * CHUNK_SIZE;
    int64_t baseY = chunkY * CHUNK_SIZE;  // Y = вторая горизонталь
    int64_t baseZ = chunkZ * CHUNK_SIZE;  // Z = высота

    BlockType* blocks = scratch->blocks;

    const size_t heightScratchCount = (size_t)EXTENDED_SIZE * EXTENDED_SIZE;
    WorldRegionContents contents = WorldFillRegion(world,
        baseX - 1, baseY - 1, baseZ - 1,
        EXTENDED_SIZE, EXTENDED_SIZE, EXTENDED_SIZE,
        blocks, scratch->heights, heightScratchCount);

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
    uint64_t* planesPositive = scratch->planesPositive;
    uint64_t* planesNegative = scratch->planesNegative;
    size_t planeBytes = COLUMN_WORDS * sizeof(uint64_t);

    memset(scratch->columns, 0, planeBytes * 3);

    // Один проход по блокам заполняет колонны всех трёх осей. Колонна вдоль Z
    // читается целиком одной маской, поперечные оси получают биты обходом
    // установленных разрядов — пустые колонны (а над поверхностью их
    // большинство) пропускаются целиком.
    for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
    {
        const uint64_t rowBit = 1ull << y;

        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            const uint64_t solidMask = ColumnSolidMask(&blocks[BLOCK_INDEX(y + 1, x + 1, 1)]);
            if (solidMask == 0)
            {
                continue;
            }

            columnsZ[y * CHUNK_SIZE + x] = solidMask;

            const uint64_t columnBit = 1ull << x;
            uint64_t remaining = solidMask;
            while (remaining != 0)
            {
                unsigned long z;
                _BitScanForward64(&z, remaining);
                remaining &= remaining - 1;
                columnsY[x * CHUNK_SIZE + (uint32_t)z] |= rowBit;
                columnsX[y * CHUNK_SIZE + (uint32_t)z] |= columnBit;
            }
        }
    }

    QuadBuffer* quadBuffer = &scratch->quadBuffer;
    quadBuffer->count = 0;
    bool succeeded = true;

    // === Грани ±Z (высота, нормаль = Z) ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
    {
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            uint64_t column = columnsZ[y * CHUNK_SIZE + x];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(y + 1, x + 1, EXTENDED_SIZE - 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(y + 1, x + 1, 0)] != BLOCK_AIR);
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, y, 1ull << x);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, y, 1ull << x);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_POSITIVE_Z, planesPositive, blocks);
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_NEGATIVE_Z, planesNegative, blocks);

    // === Грани ±X ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t y = 0; y < CHUNK_SIZE && succeeded; ++y)
    {
        for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        {
            uint64_t column = columnsX[y * CHUNK_SIZE + z];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(y + 1, EXTENDED_SIZE - 1, z + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(y + 1, 0, z + 1)] != BLOCK_AIR);
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, y, 1ull << z);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, y, 1ull << z);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_POSITIVE_X, planesPositive, blocks);
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_NEGATIVE_X, planesNegative, blocks);

    // === Грани ±Y (вторая горизонталь, нормаль = Y) ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t x = 0; x < CHUNK_SIZE && succeeded; ++x)
    {
        for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        {
            uint64_t column = columnsY[x * CHUNK_SIZE + z];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(EXTENDED_SIZE - 1, x + 1, z + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(0, x + 1, z + 1)] != BLOCK_AIR);
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, x, 1ull << z);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, x, 1ull << z);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_POSITIVE_Y, planesPositive, blocks);
    succeeded = succeeded && GreedyMeshPlanes(
        quadBuffer, FACE_NEGATIVE_Y, planesNegative, blocks);

    if (!succeeded)
    {
        return false;
    }

    if (quadBuffer->count > 0)
    {
        size_t bytes = (size_t)quadBuffer->count * sizeof(ChunkQuad);
        ChunkQuad* quads = HeapAlloc(GetProcessHeap(), 0, bytes);
        if (quads == NULL)
        {
            return false;
        }
        memcpy(quads, quadBuffer->quads, bytes);
        *outQuads = quads;
        *outQuadCount = quadBuffer->count;
    }

    return true;
}
