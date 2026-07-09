#pragma once

#include <stdint.h>

// Асинхронная подгрузка чанков (внутренний компонент ядра):
// рабочий поток строит меши через laiue_mesher, главный поток забирает
// готовые, хранит их в кеше по координате чанка и рисует с отсечением
// по пирамиде видимости. При смене центра строятся только новые чанки,
// вышедшие из радиуса — освобождаются.
typedef struct ChunkStreaming ChunkStreaming;

typedef struct World World;
typedef struct Renderer Renderer;

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks);
void ChunkStreamingDestroy(ChunkStreaming* streaming);

// Смена центра обзора (в координатах чанков): заказывает недостающие
// меши от ближних к дальним и выбрасывает вышедшие из радиуса.
void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ);

// Забирает готовые меши из рабочего потока и загружает их на GPU.
// Вызывается каждый кадр.
void ChunkStreamingPump(ChunkStreaming* streaming);

// Рисует видимые меши (между RendererBeginFrame и RendererEndFrame).
void ChunkStreamingDraw(const ChunkStreaming* streaming, const float viewProjection[16]);
