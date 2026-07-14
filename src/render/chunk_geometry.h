#pragma once

#include <stdint.h>

// Единый контракт формата геометрии чанков между мешером, рендерером
// и шейдером (shaders/chunk.hlsl — держать в синхроне!).
//
// Меш чанка — массив упакованных квадов по 8 байт, без вершинных
// и индексных буферов: вершинный шейдер разворачивает квад
// по SV_VertexID (vertex pulling), 6 вершин на квад.
//
// positionAndFace: биты 0..6 — startX, 7..13 — startZ (высота),
//                  14..20 — startY (вторая горизонталь),
//                  биты 21..23 — номер грани,
//                  биты 24..31 — тип блока (материал).
// extents:         биты 0..6 — extentX, 7..13 — extentZ, 14..20 — extentY.
//
// Порядок граней: +X, -X, +Y, -Y, +Z, -Z (+Z = верх, -Z = низ).
typedef struct ChunkQuad
{
    uint32_t positionAndFace;
    uint32_t extents;
} ChunkQuad;

_Static_assert(sizeof(ChunkQuad) == 8,
    "ChunkQuad is part of the GPU format and must stay compact");

static inline ChunkQuad PackChunkQuad(
    uint32_t startX, uint32_t startY, uint32_t startZ, uint32_t face,
    uint32_t blockType,
    uint32_t extentX, uint32_t extentY, uint32_t extentZ)
{
    ChunkQuad quad = {
        .positionAndFace = startX | (startZ << 7) | (startY << 14)
            | (face << 21) | ((blockType & 0xffu) << 24),
        .extents = extentX | (extentZ << 7) | (extentY << 14),
    };
    return quad;
}
