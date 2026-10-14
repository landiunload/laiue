#pragma once

#include "api.h"
#include "world/world.h"
#include "render/chunk_geometry.h"

#include <stdbool.h>
#include <stdint.h>

// Мешер — адаптер между источником блоков и рендерером: превращает
// воксельные данные в упакованные квады (ChunkQuad). Он не знает о
// реализации World и поэтому не импортирует world DLL.

typedef WorldRegionContents (*ChunkMesherFillRegion)(void* context,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks);

typedef struct ChunkMesherWorldSource
{
    void* context;
    ChunkMesherFillRegion fillRegion;
} ChunkMesherWorldSource;

// Переиспользуемые рабочие буферы мешинга (~0,5 МБ): создаются один раз
// на поток вместо четырёх пар HeapAlloc/HeapFree на каждый чанк.
// Экземпляр НЕ потокобезопасен — по одному на рабочий поток.
typedef struct ChunkMesherScratch ChunkMesherScratch;

LAIUE_MESHER_API ChunkMesherScratch* ChunkMesherScratchCreate(void);
LAIUE_MESHER_API void ChunkMesherScratchDestroy(ChunkMesherScratch* scratch);

// Строит меш чанка по данным источника блоков (material-aware greedy
// meshing). Источник и его context принадлежат вызывающей стороне и должны
// оставаться живыми до завершения вызова.
// Массив квадов выделяется через HeapAlloc — вызывающая сторона
// освобождает его HeapFree. Для пустого чанка возвращает true
// с нулевым счётчиком и NULL-массивом.
LAIUE_MESHER_API bool BuildChunkMesh(const ChunkMesherWorldSource* source,
    ChunkMesherScratch* scratch,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkQuad** outQuads, uint32_t* outQuadCount);
