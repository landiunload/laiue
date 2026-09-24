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
struct LaiueSceneMathServiceV1;
struct LaiueWorldServiceV1;
struct LaiueMesherServiceV1;
struct LaiueGraphicsServiceV1;

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

// Resolves the matrix provider once when voxel_render starts. The streaming
// implementation keeps no link-time dependency on scene_math.
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetSceneMathService(
    const struct LaiueSceneMathServiceV1* service);
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetWorldService(
    const struct LaiueWorldServiceV1* service);
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetMesherService(
    const struct LaiueMesherServiceV1* service);
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetGraphicsService(
    const struct LaiueGraphicsServiceV1* service);

LAIUE_VOXEL_RENDER_API ChunkStreaming* ChunkStreamingCreate(
    World* world, Renderer* renderer, int32_t viewRadiusChunks);

/* Instance-owned service bindings.  The legacy create/setter path remains
 * available for older callers, but new modules must use this constructor so
 * two streaming instances never race through process-global service slots. */
LAIUE_VOXEL_RENDER_API ChunkStreaming* ChunkStreamingCreateWithServices(
    World* world, Renderer* renderer, int32_t viewRadiusChunks,
    const struct LaiueSceneMathServiceV1* sceneMath,
    const struct LaiueWorldServiceV1* worldService,
    const struct LaiueMesherServiceV1* mesher,
    const struct LaiueGraphicsServiceV1* graphics);
LAIUE_VOXEL_RENDER_API void ChunkStreamingDestroy(ChunkStreaming* streaming);
LAIUE_VOXEL_RENDER_API bool ChunkStreamingPause(ChunkStreaming* streaming);

// Вызывается после смены origin мира. Сохраняет только уже готовые GPU-меши,
// абсолютные чанки которых попадают в новую зону обзора.
LAIUE_VOXEL_RENDER_API bool ChunkStreamingResumeAfterOriginChange(
    ChunkStreaming* streaming,
    bool originDeltaFits,
    int64_t chunkOriginDeltaX, int64_t chunkOriginDeltaY, int64_t chunkOriginDeltaZ,
    int64_t newCenterX, int64_t newCenterY, int64_t newCenterZ);

// Смена центра обзора (в координатах чанков): заказывает недостающие
// меши от ближних к дальним и выбрасывает вышедшие из радиуса+1.
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetCenter(
    ChunkStreaming* streaming,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ);

// Пометить чанки, содержащие блок (включая соседей при границе),
// устаревшими — они будут перестроены рабочими потоками.
LAIUE_VOXEL_RENDER_API void ChunkStreamingInvalidateBlock(
    ChunkStreaming* streaming,
    int64_t blockX, int64_t blockY, int64_t blockZ);

// Забирает готовые меши из рабочих потоков и загружает их на GPU
// (не больше бюджета на кадр). Вызывается каждый кадр до начала кадра.
LAIUE_VOXEL_RENDER_API void ChunkStreamingPump(ChunkStreaming* streaming);

// Снимок накопительных счётчиков для диагностики/профилировщика. Вызов дешёвый и
// не останавливает рабочие потоки надолго.
LAIUE_VOXEL_RENDER_API void ChunkStreamingGetStats(ChunkStreaming* streaming,
    ChunkStreamingStats* outStats);

// Рисует видимые меши спереди-назад (между Begin/EndFrame).
// renderOriginBlock — локальный блок, который GPU считает (0,0,0).
// Благодаря этому абсолютная дальность мира не попадает во float-рендер.
LAIUE_VOXEL_RENDER_API void ChunkStreamingDraw(
    ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t renderOriginBlock[3]);
