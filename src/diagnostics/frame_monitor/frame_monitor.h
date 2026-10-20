#pragma once

/* laiue.frame_monitor — время кадра и его частей для отладки и профилировки.
 *
 * Монитора нет в релизе: модуль ставится в SDK только для конфигураций Debug
 * и RelWithDebInfo, а макросы LAIUE_FRAME_* ниже раскрываются в пустоту,
 * если приложение собирается без LAIUE_DIAGNOSTICS. Цель laiue::frame_monitor
 * сама задаёт LAIUE_DIAGNOSTICS=1 потребителям во всех конфигурациях, кроме
 * Release и MinSizeRel. Приложение, загружающее модули по имени, в релизе
 * просто не найдёт его — как и любой отсутствующий модуль.
 *
 * Как работает: каждый кадр обрамляется BeginFrame/EndFrame, внутри —
 * зоны (симуляция, рендер, UI…). Монитор хранит длительности последних
 * historyFrames кадров и время каждой зоны в них. Перцентили считаются по
 * запросу, поэтому кадр стоит лишь несколько чтений часов и записей в
 * массив. Кадр дольше бюджета или дольше спайк-множителя медианы попадает
 * в журнал спайков вместе с тремя самыми долгими зонами.
 *
 * Один монитор — один поток (поток кадра). Вложенность зон до 16 уровней;
 * время вложенной зоны входит и во внешнюю. */

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_FRAME_MONITOR_MAXIMUM_ZONES 64u
#define LAIUE_FRAME_MONITOR_MAXIMUM_HISTORY 65536u
#define LAIUE_FRAME_MONITOR_SPIKE_ZONES 3u
#define LAIUE_FRAME_MONITOR_INVALID_ZONE UINT32_MAX

typedef struct LaiueFrameMonitor LaiueFrameMonitor;

typedef struct LaiueFrameMonitorDesc
{
    uint32_t structSize;
    /* Сколько последних кадров хранить, 16–65536. */
    uint32_t historyFrames;
    /* Бюджет кадра, например 16 666 667 нс для 60 Гц; 0 — без бюджета. */
    uint64_t budgetNanoseconds;
    /* Кадр дольше медианы × percent / 100 — спайк; 0 — не сравнивать с
     * медианой. Медиана обновляется раз в historyFrames / 4 кадров. */
    uint32_t spikeMedianPercent;
    /* Сколько последних спайков помнить, 1–4096. */
    uint32_t spikeCapacity;
} LaiueFrameMonitorDesc;

typedef struct LaiueFrameMonitorSummary
{
    uint32_t structSize;
    /* Кадров в окне истории и всего с создания. */
    uint32_t frames;
    uint64_t totalFrames;
    uint64_t totalSpikes;
    uint64_t meanNanoseconds;
    uint64_t p50Nanoseconds;
    uint64_t p90Nanoseconds;
    uint64_t p99Nanoseconds;
    uint64_t p999Nanoseconds;
    uint64_t maximumNanoseconds;
    /* Средние кадры в секунду по окну, в тысячных долях. */
    uint64_t framesPerSecondMilli;
} LaiueFrameMonitorSummary;

typedef struct LaiueFrameMonitorSpike
{
    uint32_t structSize;
    uint64_t frameIndex;
    uint64_t durationNanoseconds;
    uint64_t thresholdNanoseconds;
    uint32_t zone[LAIUE_FRAME_MONITOR_SPIKE_ZONES];
    uint64_t zoneNanoseconds[LAIUE_FRAME_MONITOR_SPIKE_ZONES];
} LaiueFrameMonitorSpike;

LAIUE_FRAME_MONITOR_API bool LaiueFrameMonitorCreate(const LaiueFrameMonitorDesc *desc,
                                                     LaiueFrameMonitor **outMonitor);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorDestroy(LaiueFrameMonitor *monitor);
/* Имя копируется (до 31 байта). Повторное имя возвращает тот же номер. */
LAIUE_FRAME_MONITOR_API uint32_t LaiueFrameMonitorRegisterZone(LaiueFrameMonitor *monitor,
                                                               const char *name);
LAIUE_FRAME_MONITOR_API const char *LaiueFrameMonitorZoneName(const LaiueFrameMonitor *monitor,
                                                              uint32_t zone);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorBeginFrame(LaiueFrameMonitor *monitor);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorEndFrame(LaiueFrameMonitor *monitor);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorBeginZone(LaiueFrameMonitor *monitor, uint32_t zone);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorEndZone(LaiueFrameMonitor *monitor, uint32_t zone);
/* Кадр известной длительности без часов: для детерминированных тестов и
 * для времени, измеренного где-то ещё (например, GPU). */
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorSubmitFrame(LaiueFrameMonitor *monitor,
                                                          uint64_t durationNanoseconds,
                                                          const uint64_t *zoneNanoseconds,
                                                          uint32_t zoneCount);
LAIUE_FRAME_MONITOR_API bool LaiueFrameMonitorGetSummary(const LaiueFrameMonitor *monitor,
                                                         LaiueFrameMonitorSummary *outSummary);
/* Те же перцентили по времени одной зоны в кадрах окна. */
LAIUE_FRAME_MONITOR_API bool LaiueFrameMonitorGetZoneSummary(const LaiueFrameMonitor *monitor,
                                                             uint32_t zone,
                                                             LaiueFrameMonitorSummary *outSummary);
/* Спайки от новых к старым: index 0 — последний. */
LAIUE_FRAME_MONITOR_API uint32_t LaiueFrameMonitorSpikeCount(const LaiueFrameMonitor *monitor);
LAIUE_FRAME_MONITOR_API bool LaiueFrameMonitorGetSpike(const LaiueFrameMonitor *monitor,
                                                       uint32_t index,
                                                       LaiueFrameMonitorSpike *outSpike);
LAIUE_FRAME_MONITOR_API void LaiueFrameMonitorReset(LaiueFrameMonitor *monitor);

/* Инструментирование, которое исчезает из релиза целиком: аргументы даже
 * не вычисляются. */
#if defined(LAIUE_DIAGNOSTICS) && LAIUE_DIAGNOSTICS
#define LAIUE_FRAME_BEGIN(monitor) LaiueFrameMonitorBeginFrame(monitor)
#define LAIUE_FRAME_END(monitor) LaiueFrameMonitorEndFrame(monitor)
#define LAIUE_FRAME_ZONE_BEGIN(monitor, zone) LaiueFrameMonitorBeginZone((monitor), (zone))
#define LAIUE_FRAME_ZONE_END(monitor, zone) LaiueFrameMonitorEndZone((monitor), (zone))
#else
#define LAIUE_FRAME_BEGIN(monitor) ((void)0)
#define LAIUE_FRAME_END(monitor) ((void)0)
#define LAIUE_FRAME_ZONE_BEGIN(monitor, zone) ((void)0)
#define LAIUE_FRAME_ZONE_END(monitor, zone) ((void)0)
#endif
