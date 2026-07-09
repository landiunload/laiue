#include "render/renderer.h"
#include "world/world.h"

#include <windows.h>
#include <stddef.h>

// 6 face normals
static const int8_t g_faceNormals[6][3] =
{
    {  1,  0,  0 },
    { -1,  0,  0 },
    {  0,  1,  0 },
    {  0, -1,  0 },
    {  0,  0,  1 },
    {  0,  0, -1 },
};

// 4 corners per face (order: CW when viewed from outside, for D3D12 back-face culling)
static const int8_t g_faceCorners[6][4][3] =
{
    { { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 }, { 1, 0, 0 } },  // +X
    { { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 1 } },  // -X
    { { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 0 } },  // +Y
    { { 1, 0, 1 }, { 0, 0, 1 }, { 0, 0, 0 }, { 1, 0, 0 } },  // -Y
    { { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 }, { 1, 0, 1 } },  // +Z
    { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } },  // -Z
};

#define EXT (CHUNK_SIZE + 2) // 66

// Emit a single quad (4 vertices, 2 triangles = 6 indices)
static void EmitQuad(ChunkVertex* verts, uint32_t* tris,
                     uint32_t* vertCount, uint32_t* triCount,
                     float wx, float wy, float wz, int face,
                     uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t base = *vertCount;
    const int8_t (*corners)[3] = g_faceCorners[face];
    for (int i = 0; i < 4; ++i)
    {
        ChunkVertex* v = &verts[base + i];
        v->position[0] = wx + (float)corners[i][0];
        v->position[1] = wy + (float)corners[i][1];
        v->position[2] = wz + (float)corners[i][2];
        v->color[0] = r; v->color[1] = g;
        v->color[2] = b; v->color[3] = 255;
    }
    tris[(*triCount)++] = base + 0;
    tris[(*triCount)++] = base + 1;
    tris[(*triCount)++] = base + 2;
    tris[(*triCount)++] = base + 0;
    tris[(*triCount)++] = base + 2;
    tris[(*triCount)++] = base + 3;
    *vertCount += 4;
}

// Procedural earth color with slight variation
static void BlockColor(int64_t wx, int64_t wy, int64_t wz,
                       uint8_t* r, uint8_t* g, uint8_t* b)
{
    uint32_t h = (uint32_t)(wx * 374761393u + wy * 668265263u + wz * 1274126177u);
    h = (h ^ (h >> 13)) * 1274126177u;
    uint8_t v = (uint8_t)((h >> 16) & 0x1F);
    *r = 101 + (v & 7);
    *g = 67 + ((v >> 3) & 7);
    *b = 33 + ((v >> 6) & 3);
}

bool BuildChunkMesh(World* world, int64_t cx, int64_t cy, int64_t cz,
    ChunkVertex** outVertices, uint32_t* outVertexCount,
    uint32_t** outIndices, uint32_t* outIndexCount)
{
    int64_t baseX = cx * CHUNK_SIZE;
    int64_t baseY = cy * CHUNK_SIZE;
    int64_t baseZ = cz * CHUNK_SIZE;

    // ---- Precompute terrain heights for each column (66x66) ----
    // Allocate on heap to avoid __chkstk (no CRT).
    int16_t (*heights)[EXT] = HeapAlloc(GetProcessHeap(), 0, EXT * EXT * sizeof(int16_t));
    if (!heights) return false;
    for (int lz = -1; lz <= CHUNK_SIZE; ++lz)
    {
        int iz = lz + 1;
        int64_t wz = baseZ + lz;
        for (int lx = -1; lx <= CHUNK_SIZE; ++lx)
        {
            int ix = lx + 1;
            int64_t wx = baseX + lx;
            heights[iz][ix] = (int16_t)WorldGetTerrainHeight(world, wx, wz);
        }
    }

    // ---- Fill dense 66×66×66 block array ----
    BlockType* blocks = HeapAlloc(GetProcessHeap(), 0, EXT * EXT * EXT);
    if (!blocks) { HeapFree(GetProcessHeap(), 0, heights); return false; }

    for (int lz = -1; lz <= CHUNK_SIZE; ++lz)
    {
        int iz = lz + 1;
        int64_t wz = baseZ + lz;
        for (int lx = -1; lx <= CHUNK_SIZE; ++lx)
        {
            int ix = lx + 1;
            int64_t wx = baseX + lx;
            int16_t h = heights[iz][ix];
            for (int ly = -1; ly <= CHUNK_SIZE; ++ly)
            {
                int64_t wy = baseY + ly;
                int iy = ly + 1;
                BlockType b;
                if (wy >= h - 2 && wy <= h + 1)
                    b = WorldGetBlock(world, wx, wy, wz);
                else
                    b = (wy < h) ? BLOCK_EARTH : BLOCK_AIR;
                blocks[((iz * EXT) + ix) * EXT + iy] = b;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, heights);

    // ---- Allocate output buffers (worst case: every block face visible) ----
    // Max 6 faces × 4 verts = 24 verts, 6 faces × 6 indices = 36 indices per block
    uint32_t maxVerts = (uint32_t)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 24;
    uint32_t maxTris  = (uint32_t)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 36;

    ChunkVertex* verts = HeapAlloc(GetProcessHeap(), 0, maxVerts * sizeof(ChunkVertex));
    uint32_t*    tris  = HeapAlloc(GetProcessHeap(), 0, maxTris * sizeof(uint32_t));
    if (!verts || !tris)
    {
        HeapFree(GetProcessHeap(), 0, blocks);
        if (verts) HeapFree(GetProcessHeap(), 0, verts);
        if (tris)  HeapFree(GetProcessHeap(), 0, tris);
        return false;
    }

    uint32_t vertCount = 0;
    uint32_t triCount  = 0;

    // ---- Build mesh from dense array ----
    // Iterate z (slow), x (medium), y (fast) to match memory layout blocks[z][x][y]
    for (int lz = 0; lz < CHUNK_SIZE; ++lz)
    {
        int iz = lz + 1;            // index in dense array (0..65)
        int iz_m1 = lz;             // index for -z neighbor
        int iz_p1 = lz + 2;         // index for +z neighbor
        int64_t wz = baseZ + lz;

        for (int lx = 0; lx < CHUNK_SIZE; ++lx)
        {
            int ix = lx + 1;
            int ix_m1 = lx;
            int ix_p1 = lx + 2;
            int64_t wx = baseX + lx;

            int row_iz   = iz * EXT;
            int row_iz_m1 = iz_m1 * EXT;
            int row_iz_p1 = iz_p1 * EXT;

            for (int ly = 0; ly < CHUNK_SIZE; ++ly)
            {
                int iy = ly + 1;
                int iy_m1 = ly;
                int iy_p1 = ly + 2;
                int64_t wy = baseY + ly;

                BlockType block = blocks[(row_iz + ix) * EXT + iy];
                if (block == BLOCK_AIR)
                    continue;

                uint8_t r, g, b;
                BlockColor(wx, wy, wz, &r, &g, &b);

                // +X face
                if (blocks[(row_iz + ix_p1) * EXT + iy] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 0, r, g, b);

                // -X face (reuse +X offset of neighbor, so check ix_m1)
                if (blocks[(row_iz + ix_m1) * EXT + iy] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 1, r, g, b);

                // +Y face
                if (blocks[(row_iz + ix) * EXT + iy_p1] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 2, r, g, b);

                // -Y face
                if (blocks[(row_iz + ix) * EXT + iy_m1] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 3, r, g, b);

                // +Z face
                if (blocks[(row_iz_p1 + ix) * EXT + iy] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 4, r, g, b);

                // -Z face
                if (blocks[(row_iz_m1 + ix) * EXT + iy] == BLOCK_AIR)
                    EmitQuad(verts, tris, &vertCount, &triCount,
                             (float)wx, (float)wy, (float)wz, 5, r, g, b);
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, blocks);

    // ---- Shrink to fit ----
    if (vertCount > 0)
    {
        ChunkVertex* finalV = HeapAlloc(GetProcessHeap(), 0, vertCount * sizeof(ChunkVertex));
        if (!finalV) { HeapFree(GetProcessHeap(), 0, verts); HeapFree(GetProcessHeap(), 0, tris); return false; }
        for (uint32_t i = 0; i < vertCount; ++i) finalV[i] = verts[i];
        HeapFree(GetProcessHeap(), 0, verts);
        *outVertices = finalV;
    }
    else
    {
        *outVertices = NULL;
        HeapFree(GetProcessHeap(), 0, verts);
    }

    if (triCount > 0)
    {
        uint32_t* finalI = HeapAlloc(GetProcessHeap(), 0, triCount * sizeof(uint32_t));
        if (!finalI) { HeapFree(GetProcessHeap(), 0, *outVertices); HeapFree(GetProcessHeap(), 0, tris); return false; }
        for (uint32_t i = 0; i < triCount; ++i) finalI[i] = tris[i];
        HeapFree(GetProcessHeap(), 0, tris);
        *outIndices = finalI;
    }
    else
    {
        *outIndices = NULL;
        HeapFree(GetProcessHeap(), 0, tris);
    }

    *outVertexCount = vertCount;
    *outIndexCount  = triCount / 3;
    return true;
}
