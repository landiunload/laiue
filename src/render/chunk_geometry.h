#pragma once

#include <stdint.h>

// Единый контракт формата геометрии чанков между мешером, рендерером
// и шейдером (shaders/chunk.hlsl — держать в синхроне!).
//
// Меш чанка — массив упакованных квадов по 8 байт, без вершинных
// и индексных буферов: вершинный шейдер разворачивает квад
// по SV_VertexID (vertex pulling), 6 вершин на квад.
//
// positionAndFace: биты 0..6 — startX, 7..13 — startY, 14..20 — startZ
//                  (локальные координаты в чанке, 0..64),
//                  биты 21..23 — номер грани.
// extents:         биты 0..6 — extentX, 7..13 — extentY, 14..20 — extentZ
//                  (размеры квада в блоках; по оси нормали всегда 1).
//
// Порядок граней: +X, -X, +Y, -Y, +Z, -Z.
typedef struct ChunkQuad
{
    uint32_t positionAndFace;
    uint32_t extents;
} ChunkQuad;

static inline ChunkQuad PackChunkQuad(
    uint32_t startX, uint32_t startY, uint32_t startZ, uint32_t face,
    uint32_t extentX, uint32_t extentY, uint32_t extentZ)
{
    ChunkQuad quad = {
        .positionAndFace = startX | (startY << 7) | (startZ << 14) | (face << 21),
        .extents = extentX | (extentY << 7) | (extentZ << 14),
    };
    return quad;
}
