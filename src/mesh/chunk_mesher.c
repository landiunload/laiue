#include "mesh/chunk_mesher.h"
#include "world/world.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>

// Бинарный greedy meshing (по мотивам vercidium-patreon/meshing):
// колонна чанка 64 блока = один uint64, отсечение невидимых граней —
// две битовые операции на всю колонну, слияние соседних граней
// в большие прямоугольники — битовые маски по плоскостям.
// CHUNK_SIZE = 64 ложится на uint64 ровно.

// Расширенный регион: чанк плюс слой соседних блоков с каждой стороны,
// чтобы грани на границе чанка отсекались без обращений к соседям.
#define EXTENDED_SIZE (CHUNK_SIZE + 2)

#define BLOCK_INDEX(z, x, y) ((((size_t)(z) * EXTENDED_SIZE) + (size_t)(x)) * EXTENDED_SIZE + (size_t)(y))

#define COLUMN_WORDS ((size_t)CHUNK_SIZE * CHUNK_SIZE)

// Порядок граней: +X, -X, +Y, -Y, +Z, -Z (совпадает с chunk_geometry.h и шейдером).
enum
{
    FACE_POSITIVE_X,
    FACE_NEGATIVE_X,
    FACE_POSITIVE_Y,
    FACE_NEGATIVE_Y,
    FACE_POSITIVE_Z,
    FACE_NEGATIVE_Z,
};

// Растущий буфер квадов (переиспользуется между чанками).
typedef struct QuadBuffer
{
    ChunkQuad* quads;
    uint32_t count;
    uint32_t capacity;
} QuadBuffer;

struct ChunkMesherScratch
{
    BlockType* blocks;          // расширенный регион 66^3
    uint64_t* columns;          // битовые колонны трёх осей, 3 x 64x64
    uint64_t* planesPositive;   // плоскости граней, 64x64
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
    scratch->columns = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * 3 * sizeof(uint64_t));
    scratch->planesPositive = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * sizeof(uint64_t));
    scratch->planesNegative = HeapAlloc(GetProcessHeap(), 0, COLUMN_WORDS * sizeof(uint64_t));

    if (scratch->blocks == NULL || scratch->columns == NULL ||
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

// Переводит прямоугольник плоскости (slice, биты, ряды) в оси чанка.
static bool EmitFaceRectangle(QuadBuffer* buffer, uint32_t face, uint32_t slice,
    uint32_t bitStart, uint32_t bitExtent, uint32_t rowStart, uint32_t rowExtent)
{
    switch (face)
    {
        case FACE_POSITIVE_X:
        case FACE_NEGATIVE_X:
            // Нормаль X; биты плоскости — Y, ряды — Z.
            return QuadBufferAppend(buffer,
                PackChunkQuad(slice, bitStart, rowStart, face, 1, bitExtent, rowExtent));

        case FACE_POSITIVE_Y:
        case FACE_NEGATIVE_Y:
            // Нормаль Y; биты — X, ряды — Z.
            return QuadBufferAppend(buffer,
                PackChunkQuad(bitStart, slice, rowStart, face, bitExtent, 1, rowExtent));

        default:
            // Нормаль Z; биты — Y, ряды — X.
            return QuadBufferAppend(buffer,
                PackChunkQuad(rowStart, bitStart, slice, face, rowExtent, bitExtent, 1));
    }
}

// Greedy-слияние по битовым плоскостям: в ряду находится серия
// подряд идущих граней, затем серия расширяется на соседние ряды,
// пока они содержат ту же маску.
static bool GreedyMeshPlanes(QuadBuffer* buffer, uint32_t face, uint64_t* planes)
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

                uint64_t shifted = bits >> runStart;
                uint64_t inverted = ~shifted;
                uint32_t runLength;
                if (inverted == 0)
                {
                    runLength = 64 - (uint32_t)runStart;
                }
                else
                {
                    unsigned long firstZero;
                    _BitScanForward64(&firstZero, inverted);
                    runLength = (uint32_t)firstZero;
                }

                uint64_t runMask = runLength == 64
                    ? ~0ull
                    : (((1ull << runLength) - 1) << runStart);

                uint32_t rowExtent = 1;
                while (row + rowExtent < CHUNK_SIZE &&
                       (rows[row + rowExtent] & runMask) == runMask)
                {
                    rows[row + rowExtent] &= ~runMask;
                    ++rowExtent;
                }

                bits &= ~runMask;
                rows[row] &= ~runMask;

                if (!EmitFaceRectangle(buffer, face, slice, (uint32_t)runStart, runLength, row, rowExtent))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

// Рассеивает биты видимых граней колонны в плоскости соответствующих срезов.
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
    int64_t baseY = chunkY * CHUNK_SIZE;
    int64_t baseZ = chunkZ * CHUNK_SIZE;

    BlockType* blocks = scratch->blocks;

    WorldRegionContents contents = WorldFillRegion(world,
        baseX - 1, baseY - 1, baseZ - 1,
        EXTENDED_SIZE, EXTENDED_SIZE, EXTENDED_SIZE,
        blocks);

    // Однородный регион (весь воздух или весь камень) видимых граней
    // не содержит — меш пуст.
    if (contents != WORLD_REGION_MIXED)
    {
        return true;
    }

    uint64_t* columnsY = scratch->columns;                    // [z*64+x], биты вдоль Y
    uint64_t* columnsX = scratch->columns + COLUMN_WORDS;     // [z*64+y], биты вдоль X
    uint64_t* columnsZ = scratch->columns + COLUMN_WORDS * 2; // [x*64+y], биты вдоль Z
    uint64_t* planesPositive = scratch->planesPositive;
    uint64_t* planesNegative = scratch->planesNegative;
    size_t planeBytes = COLUMN_WORDS * sizeof(uint64_t);

    memset(scratch->columns, 0, planeBytes * 3);

    // Один проход по блокам заполняет колонны всех трёх осей.
    for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
    {
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            const BlockType* column = &blocks[BLOCK_INDEX(z + 1, x + 1, 1)];
            for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
            {
                if (column[y] != BLOCK_AIR)
                {
                    columnsY[z * CHUNK_SIZE + x] |= 1ull << y;
                    columnsX[z * CHUNK_SIZE + y] |= 1ull << x;
                    columnsZ[x * CHUNK_SIZE + y] |= 1ull << z;
                }
            }
        }
    }

    QuadBuffer* quadBuffer = &scratch->quadBuffer;
    quadBuffer->count = 0;
    bool succeeded = true;

    // === Грани ±Y ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
    {
        for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        {
            uint64_t column = columnsY[z * CHUNK_SIZE + x];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(z + 1, x + 1, EXTENDED_SIZE - 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(z + 1, x + 1, 0)] != BLOCK_AIR);
            // Грань видна там, где блок твёрдый, а сосед по направлению — воздух.
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, z, 1ull << x);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, z, 1ull << x);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_POSITIVE_Y, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_NEGATIVE_Y, planesNegative);

    // === Грани ±X ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t z = 0; z < CHUNK_SIZE && succeeded; ++z)
    {
        for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
        {
            uint64_t column = columnsX[z * CHUNK_SIZE + y];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(z + 1, EXTENDED_SIZE - 1, y + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(z + 1, 0, y + 1)] != BLOCK_AIR);
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, z, 1ull << y);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, z, 1ull << y);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_POSITIVE_X, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_NEGATIVE_X, planesNegative);

    // === Грани ±Z ===
    memset(planesPositive, 0, planeBytes);
    memset(planesNegative, 0, planeBytes);
    for (uint32_t x = 0; x < CHUNK_SIZE && succeeded; ++x)
    {
        for (uint32_t y = 0; y < CHUNK_SIZE; ++y)
        {
            uint64_t column = columnsZ[x * CHUNK_SIZE + y];
            if (column == 0)
            {
                continue;
            }
            uint64_t neighborAbove = (uint64_t)(blocks[BLOCK_INDEX(EXTENDED_SIZE - 1, x + 1, y + 1)] != BLOCK_AIR);
            uint64_t neighborBelow = (uint64_t)(blocks[BLOCK_INDEX(0, x + 1, y + 1)] != BLOCK_AIR);
            ScatterFaceBits(column & ~((column >> 1) | (neighborAbove << 63)), planesPositive, x, 1ull << y);
            ScatterFaceBits(column & ~((column << 1) | neighborBelow), planesNegative, x, 1ull << y);
        }
    }
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_POSITIVE_Z, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(quadBuffer, FACE_NEGATIVE_Z, planesNegative);

    if (!succeeded)
    {
        return false;
    }

    // Копия точного размера — она уходит другому потоку, scratch остаётся.
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
