#pragma once

#include "api.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Мешер — адаптер между миром и рендерером: превращает воксельные данные
// в геометрию формата ChunkVertex. Мир не знает о геометрии,
// рендерер не знает о мире.
typedef struct World World;

// Строит меш чанка по данным мира. Массивы вершин и индексов выделяются
// через HeapAlloc — вызывающая сторона освобождает их HeapFree.
// Для пустого чанка возвращает true с нулевыми счётчиками и NULL-массивами.
LAIUE_MESHER_API bool BuildChunkMesh(World* world,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkVertex** outVertices, uint32_t* outVertexCount,
    uint32_t** outIndices, uint32_t* outIndexCount);
