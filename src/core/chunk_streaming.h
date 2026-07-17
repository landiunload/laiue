#pragma once

#include <stdbool.h>
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

typedef struct ChunkStreamingStats
{
    uint64_t queuedRequests;
    uint64_t completedBuilds;
    uint64_t cancelledBuilds;
    uint64_t discardedBuilds;
    uint64_t uploadedMeshes;
    uint32_t pendingRequests;
    uint32_t pendingResults;
    uint32_t peakUnfinishedWork;
    double averageBuildMilliseconds;
} ChunkStreamingStats;

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks);
void ChunkStreamingDestroy(ChunkStreaming* streaming);
bool ChunkStreamingPause(ChunkStreaming* streaming);

// Вызывается после смены origin мира. Сохраняет только уже готовые GPU-меши,
// абсолютные чанки которых попадают в новую зону обзора.
bool ChunkStreamingResumeAfterOriginChange(ChunkStreaming* streaming,
    bool originDeltaFits,
    int64_t chunkOriginDeltaX, int64_t chunkOriginDeltaY, int64_t chunkOriginDeltaZ,
    int64_t newCenterX, int64_t newCenterY, int64_t newCenterZ);

// Смена центра обзора (в координатах чанков): заказывает недостающие
// меши от ближних к дальним и выбрасывает вышедшие из радиуса+1.
void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ);

// Пометить чанки, содержащие блок (включая соседей при границе),
// устаревшими — они будут перестроены рабочими потоками.
void ChunkStreamingInvalidateBlock(ChunkStreaming* streaming, int64_t blockX, int64_t blockY, int64_t blockZ);

// Забирает готовые меши из рабочих потоков и загружает их на GPU
// (не больше бюджета на кадр). Вызывается каждый кадр до начала кадра.
void ChunkStreamingPump(ChunkStreaming* streaming);

// Снимок накопительных счётчиков для HUD/профилировщика. Вызов дешёвый и
// не останавливает рабочие потоки надолго.
void ChunkStreamingGetStats(ChunkStreaming* streaming,
    ChunkStreamingStats* outStats);

// Рисует видимые меши спереди-назад (между Begin/EndFrame).
// cameraBlockPosition — блок камеры: origin rebasing, все координаты
// отрисовки считаются относительно него.
void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t cameraBlockPosition[3]);
