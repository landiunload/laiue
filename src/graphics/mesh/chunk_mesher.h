#pragma once

#include "api.h"
#include "render/chunk_geometry.h"

#include <stdbool.h>
#include <stdint.h>

// Мешер — адаптер между миром и рендерером: превращает воксельные данные
// в упакованные квады (ChunkQuad). Мир не знает о геометрии,
// рендерер не знает о мире.
typedef struct World World;

// Переиспользуемые рабочие буферы мешинга (~0,5 МБ): создаются один раз
// на поток вместо четырёх пар HeapAlloc/HeapFree на каждый чанк.
// Экземпляр НЕ потокобезопасен — по одному на рабочий поток.
typedef struct ChunkMesherScratch ChunkMesherScratch;

LAIUE_MESHER_API ChunkMesherScratch* ChunkMesherScratchCreate(void);
LAIUE_MESHER_API void ChunkMesherScratchDestroy(ChunkMesherScratch* scratch);

// Строит меш чанка по данным мира (material-aware greedy meshing).
// Массив квадов выделяется через HeapAlloc — вызывающая сторона
// освобождает его HeapFree. Для пустого чанка возвращает true
// с нулевым счётчиком и NULL-массивом.
LAIUE_MESHER_API bool BuildChunkMesh(World* world, ChunkMesherScratch* scratch,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkQuad** outQuads, uint32_t* outQuadCount);
