#pragma once

#include <stdint.h>

// Асинхронная подгрузка чанков (внутренний компонент ядра):
// пул рабочих потоков строит меши через laiue_mesher, главный поток
// забирает готовые с бюджетом на кадр, хранит их в кеше по координате
// чанка и рисует спереди-назад с отсечением по пирамиде видимости.
// Гистерезис: меши строятся в радиусе обзора, а выбрасываются на чанк
// дальше — осцилляция на границе не вызывает перестроений.
typedef struct ChunkStreaming ChunkStreaming;

typedef struct World World;
typedef struct Renderer Renderer;

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks);
void ChunkStreamingDestroy(ChunkStreaming* streaming);

// Смена центра обзора (в координатах чанков): заказывает недостающие
// меши от ближних к дальним и выбрасывает вышедшие из радиуса+1.
void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ);

// Пометить чанки, содержащие блок (включая соседей при границе),
// устаревшими — они будут перестроены рабочими потоками.
void ChunkStreamingInvalidateBlock(ChunkStreaming* streaming, int64_t blockX, int64_t blockY, int64_t blockZ);

// Забирает готовые меши из рабочих потоков и загружает их на GPU
// (не больше бюджета на кадр). Вызывается каждый кадр до начала кадра.
void ChunkStreamingPump(ChunkStreaming* streaming);

// Рисует видимые меши спереди-назад (между Begin/EndFrame).
// cameraBlockPosition — блок камеры: origin rebasing, все координаты
// отрисовки считаются относительно него.
void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t cameraBlockPosition[3]);
