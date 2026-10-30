#pragma once

/* Таблица сервиса laiue.frame_monitor. Модуля нет в релизных сборках:
 * приложение, не нашедшее сервис, просто работает без монитора. */

#include "api.h"
#include "frame_monitor/frame_monitor.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_FRAME_MONITOR_SERVICE_NAME "laiue.frame_monitor"
#define LAIUE_FRAME_MONITOR_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueFrameMonitorServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    bool (*create)(const LaiueFrameMonitorDesc *desc, LaiueFrameMonitor **outMonitor);
    void (*destroy)(LaiueFrameMonitor *monitor);
    uint32_t (*registerZone)(LaiueFrameMonitor *monitor, const char *name);
    const char *(*zoneName)(const LaiueFrameMonitor *monitor, uint32_t zone);
    void (*beginFrame)(LaiueFrameMonitor *monitor);
    void (*endFrame)(LaiueFrameMonitor *monitor);
    void (*beginZone)(LaiueFrameMonitor *monitor, uint32_t zone);
    void (*endZone)(LaiueFrameMonitor *monitor, uint32_t zone);
    void (*submitFrame)(LaiueFrameMonitor *monitor, uint64_t durationNanoseconds,
                        const uint64_t *zoneNanoseconds, uint32_t zoneCount);
    bool (*getSummary)(const LaiueFrameMonitor *monitor, LaiueFrameMonitorSummary *outSummary);
    bool (*getZoneSummary)(const LaiueFrameMonitor *monitor, uint32_t zone,
                           LaiueFrameMonitorSummary *outSummary);
    uint32_t (*spikeCount)(const LaiueFrameMonitor *monitor);
    bool (*getSpike)(const LaiueFrameMonitor *monitor, uint32_t index,
                     LaiueFrameMonitorSpike *outSpike);
    void (*reset)(LaiueFrameMonitor *monitor);
    uintptr_t reserved[8];
} LaiueFrameMonitorServiceV1;

LAIUE_FRAME_MONITOR_API const LaiueFrameMonitorServiceV1 *LaiueFrameMonitorGetStaticServiceV1(void);
LAIUE_FRAME_MONITOR_API const LaiueModuleApiV1 *LaiueFrameMonitorGetStaticModuleApiV1(void);
