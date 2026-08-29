#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Асинхронная подгрузка чанков:
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

LAIUE_SCENE_API ChunkStreaming* ChunkStreamingCreate(
    World* world, Renderer* renderer, int32_t viewRadiusChunks);
LAIUE_SCENE_API void ChunkStreamingDestroy(ChunkStreaming* streaming);
LAIUE_SCENE_API bool ChunkStreamingPause(ChunkStreaming* streaming);

// Вызывается после смены origin мира. Сохраняет только уже готовые GPU-меши,
// абсолютные чанки которых попадают в новую зону обзора.
LAIUE_SCENE_API bool ChunkStreamingResumeAfterOriginChange(
    ChunkStreaming* streaming,
    bool originDeltaFits,
    int64_t chunkOriginDeltaX, int64_t chunkOriginDeltaY, int64_t chunkOriginDeltaZ,
    int64_t newCenterX, int64_t newCenterY, int64_t newCenterZ);

// Смена центра обзора (в координатах чанков): заказывает недостающие
// меши от ближних к дальним и выбрасывает вышедшие из радиуса+1.
LAIUE_SCENE_API void ChunkStreamingSetCenter(
    ChunkStreaming* streaming,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ);

// Пометить чанки, содержащие блок (включая соседей при границе),
// устаревшими — они будут перестроены рабочими потоками.
LAIUE_SCENE_API void ChunkStreamingInvalidateBlock(
    ChunkStreaming* streaming,
    int64_t blockX, int64_t blockY, int64_t blockZ);

// Забирает готовые меши из рабочих потоков и загружает их на GPU
// (не больше бюджета на кадр). Вызывается каждый кадр до начала кадра.
LAIUE_SCENE_API void ChunkStreamingPump(ChunkStreaming* streaming);

// Снимок накопительных счётчиков для диагностики/профилировщика. Вызов дешёвый и
// не останавливает рабочие потоки надолго.
LAIUE_SCENE_API void ChunkStreamingGetStats(ChunkStreaming* streaming,
    ChunkStreamingStats* outStats);

// Рисует видимые меши спереди-назад (между Begin/EndFrame).
// renderOriginBlock — локальный блок, который GPU считает (0,0,0).
// Благодаря этому абсолютная дальность мира не попадает во float-рендер.
LAIUE_SCENE_API void ChunkStreamingDraw(
    ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t renderOriginBlock[3]);
