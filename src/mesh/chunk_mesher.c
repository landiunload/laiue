#include "mesh/chunk_mesher.h"
#include "world/world.h"

#include <windows.h>
#include <intrin.h>

// Бинарный greedy meshing (по мотивам vercidium-patreon/meshing):
// колонна чанка 64 блока = один uint64, отсечение невидимых граней —
// две битовые операции на всю колонну, слияние соседних граней
// в большие прямоугольники — битовые маски по плоскостям.
// CHUNK_SIZE = 64 ложится на uint64 ровно.

// Расширенный регион: чанк плюс слой соседних блоков с каждой стороны,
// чтобы грани на границе чанка отсекались без обращений к соседям.
#define EXTENDED_SIZE (CHUNK_SIZE + 2)

#define BLOCK_INDEX(z, x, y) ((((size_t)(z) * EXTENDED_SIZE) + (size_t)(x)) * EXTENDED_SIZE + (size_t)(y))

// Порядок граней: +X, -X, +Y, -Y, +Z, -Z (совпадает с таблицей в шейдере).
enum
{
    FACE_POSITIVE_X,
    FACE_NEGATIVE_X,
    FACE_POSITIVE_Y,
    FACE_NEGATIVE_Y,
    FACE_POSITIVE_Z,
    FACE_NEGATIVE_Z,
};

// Четыре угла каждой грани (обход по часовой стрелке снаружи —
// под back-face culling D3D12).
static const int8_t g_faceCorners[6][4][3] =
{
    { { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 0 } },  // +X
    { { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 1 } },  // -X
    { { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 0 } },  // +Y
    { { 1, 0, 1 }, { 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0 } },  // -Y
    { { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 } },  // +Z
    { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } },  // -Z
};

// Растущий буфер меша: память растёт через HeapReAlloc по мере надобности.
typedef struct MeshBuffer
{
    ChunkVertex* vertices;
    uint32_t vertexCount;
    uint32_t vertexCapacity;
    uint32_t* indices;
    uint32_t indexCount;
    uint32_t indexCapacity;
} MeshBuffer;

static void* GrowAllocation(void* block, size_t newSize)
{
    return block == NULL
        ? HeapAlloc(GetProcessHeap(), 0, newSize)
        : HeapReAlloc(GetProcessHeap(), 0, block, newSize);
}

static bool MeshBufferReserveQuad(MeshBuffer* mesh)
{
    if (mesh->vertexCount + 4 > mesh->vertexCapacity)
    {
        uint32_t newCapacity = mesh->vertexCapacity == 0 ? 1024 : mesh->vertexCapacity * 2;
        ChunkVertex* newVertices = GrowAllocation(mesh->vertices, (size_t)newCapacity * sizeof(ChunkVertex));
        if (newVertices == NULL)
        {
            return false;
        }
        mesh->vertices = newVertices;
        mesh->vertexCapacity = newCapacity;
    }

    if (mesh->indexCount + 6 > mesh->indexCapacity)
    {
        uint32_t newCapacity = mesh->indexCapacity == 0 ? 1536 : mesh->indexCapacity * 2;
        uint32_t* newIndices = GrowAllocation(mesh->indices, (size_t)newCapacity * sizeof(uint32_t));
        if (newIndices == NULL)
        {
            return false;
        }
        mesh->indices = newIndices;
        mesh->indexCapacity = newCapacity;
    }

    return true;
}

static void MeshBufferDestroy(MeshBuffer* mesh)
{
    if (mesh->vertices != NULL) HeapFree(GetProcessHeap(), 0, mesh->vertices);
    if (mesh->indices != NULL) HeapFree(GetProcessHeap(), 0, mesh->indices);
}

// Вершина: локальная позиция (0..64, по 7 бит на ось) + номер грани (3 бита).
// Цвет и нормаль вершине не нужны — пиксельный шейдер восстанавливает их
// из мировой позиции и номера грани.
static inline uint32_t PackChunkVertex(uint32_t x, uint32_t y, uint32_t z, uint32_t face)
{
    return x | (y << 7) | (z << 14) | (face << 21);
}

// Добавляет прямоугольник greedy-слияния: 4 вершины и 6 индексов.
static bool EmitGreedyQuad(MeshBuffer* mesh, int32_t face,
    uint32_t startX, uint32_t extentX,
    uint32_t startY, uint32_t extentY,
    uint32_t startZ, uint32_t extentZ)
{
    if (!MeshBufferReserveQuad(mesh))
    {
        return false;
    }

    uint32_t baseVertex = mesh->vertexCount;
    const int8_t (*corners)[3] = g_faceCorners[face];

    for (int32_t corner = 0; corner < 4; ++corner)
    {
        uint32_t x = startX + (corners[corner][0] ? extentX : 0);
        uint32_t y = startY + (corners[corner][1] ? extentY : 0);
        uint32_t z = startZ + (corners[corner][2] ? extentZ : 0);
        mesh->vertices[baseVertex + corner].packedData = PackChunkVertex(x, y, z, (uint32_t)face);
    }

    uint32_t* indices = &mesh->indices[mesh->indexCount];
    indices[0] = baseVertex + 0;
    indices[1] = baseVertex + 1;
    indices[2] = baseVertex + 2;
    indices[3] = baseVertex + 0;
    indices[4] = baseVertex + 2;
    indices[5] = baseVertex + 3;

    mesh->vertexCount += 4;
    mesh->indexCount += 6;
    return true;
}

// Переводит прямоугольник плоскости (slice, биты, ряды) в оси чанка.
static bool EmitFaceRectangle(MeshBuffer* mesh, int32_t face, uint32_t slice,
    uint32_t bitStart, uint32_t bitExtent, uint32_t rowStart, uint32_t rowExtent)
{
    switch (face)
    {
        case FACE_POSITIVE_X:
        case FACE_NEGATIVE_X:
            // Нормаль X; биты плоскости — Y, ряды — Z.
            return EmitGreedyQuad(mesh, face, slice, 1, bitStart, bitExtent, rowStart, rowExtent);

        case FACE_POSITIVE_Y:
        case FACE_NEGATIVE_Y:
            // Нормаль Y; биты — X, ряды — Z.
            return EmitGreedyQuad(mesh, face, bitStart, bitExtent, slice, 1, rowStart, rowExtent);

        default:
            // Нормаль Z; биты — Y, ряды — X.
            return EmitGreedyQuad(mesh, face, rowStart, rowExtent, bitStart, bitExtent, slice, 1);
    }
}

// Greedy-слияние по битовым плоскостям: в ряду находится серия
// подряд идущих граней, затем серия расширяется на соседние ряды,
// пока они содержат ту же маску.
static bool GreedyMeshPlanes(MeshBuffer* mesh, int32_t face, uint64_t* planes)
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

                if (!EmitFaceRectangle(mesh, face, slice, (uint32_t)runStart, runLength, row, rowExtent))
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

bool BuildChunkMesh(World* world,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkVertex** outVertices, uint32_t* outVertexCount,
    uint32_t** outIndices, uint32_t* outIndexCount)
{
    *outVertices = NULL;
    *outVertexCount = 0;
    *outIndices = NULL;
    *outIndexCount = 0;

    int64_t baseX = chunkX * CHUNK_SIZE;
    int64_t baseY = chunkY * CHUNK_SIZE;
    int64_t baseZ = chunkZ * CHUNK_SIZE;

    BlockType* blocks = HeapAlloc(GetProcessHeap(), 0,
        (size_t)EXTENDED_SIZE * EXTENDED_SIZE * EXTENDED_SIZE);
    if (blocks == NULL)
    {
        return false;
    }

    WorldRegionContents contents = WorldFillRegion(world,
        baseX - 1, baseY - 1, baseZ - 1,
        EXTENDED_SIZE, EXTENDED_SIZE, EXTENDED_SIZE,
        blocks);

    // Однородный регион (весь воздух или весь камень) видимых граней
    // не содержит — меш пуст.
    if (contents != WORLD_REGION_MIXED)
    {
        HeapFree(GetProcessHeap(), 0, blocks);
        return true;
    }

    // Битовые колонны твёрдости по трём осям + две плоскости граней.
    size_t columnWords = (size_t)CHUNK_SIZE * CHUNK_SIZE;
    uint64_t* scratch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, columnWords * 3 * sizeof(uint64_t));
    uint64_t* planesPositive = HeapAlloc(GetProcessHeap(), 0, columnWords * sizeof(uint64_t));
    uint64_t* planesNegative = HeapAlloc(GetProcessHeap(), 0, columnWords * sizeof(uint64_t));

    if (scratch == NULL || planesPositive == NULL || planesNegative == NULL)
    {
        if (scratch != NULL) HeapFree(GetProcessHeap(), 0, scratch);
        if (planesPositive != NULL) HeapFree(GetProcessHeap(), 0, planesPositive);
        if (planesNegative != NULL) HeapFree(GetProcessHeap(), 0, planesNegative);
        HeapFree(GetProcessHeap(), 0, blocks);
        return false;
    }

    uint64_t* columnsY = scratch;                    // [z*64+x], биты вдоль Y
    uint64_t* columnsX = scratch + columnWords;      // [z*64+y], биты вдоль X
    uint64_t* columnsZ = scratch + columnWords * 2;  // [x*64+y], биты вдоль Z

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

    MeshBuffer mesh = { 0 };
    bool succeeded = true;
    size_t planeBytes = columnWords * sizeof(uint64_t);

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
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_POSITIVE_Y, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_NEGATIVE_Y, planesNegative);

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
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_POSITIVE_X, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_NEGATIVE_X, planesNegative);

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
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_POSITIVE_Z, planesPositive);
    succeeded = succeeded && GreedyMeshPlanes(&mesh, FACE_NEGATIVE_Z, planesNegative);

    HeapFree(GetProcessHeap(), 0, scratch);
    HeapFree(GetProcessHeap(), 0, planesPositive);
    HeapFree(GetProcessHeap(), 0, planesNegative);
    HeapFree(GetProcessHeap(), 0, blocks);

    if (!succeeded)
    {
        MeshBufferDestroy(&mesh);
        return false;
    }

    // Ужатие до фактического размера (HeapReAlloc обычно ужимает на месте).
    if (mesh.vertexCount > 0)
    {
        ChunkVertex* shrunkVertices = HeapReAlloc(GetProcessHeap(), 0,
            mesh.vertices, (size_t)mesh.vertexCount * sizeof(ChunkVertex));
        if (shrunkVertices != NULL)
        {
            mesh.vertices = shrunkVertices;
        }

        uint32_t* shrunkIndices = HeapReAlloc(GetProcessHeap(), 0,
            mesh.indices, (size_t)mesh.indexCount * sizeof(uint32_t));
        if (shrunkIndices != NULL)
        {
            mesh.indices = shrunkIndices;
        }

        *outVertices = mesh.vertices;
        *outVertexCount = mesh.vertexCount;
        *outIndices = mesh.indices;
        *outIndexCount = mesh.indexCount;
    }
    else
    {
        MeshBufferDestroy(&mesh);
    }

    return true;
}
