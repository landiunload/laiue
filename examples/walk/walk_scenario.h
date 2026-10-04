#pragma once

#include <stddef.h>
#include <stdint.h>

#define WALK_SCENARIO_TICK_RATE 128u
#define WALK_SCENARIO_TOTAL_TICKS 1280u
#define WALK_SCENARIO_SAMPLE_CAPACITY 2048u

typedef enum WalkScenarioPhase
{
    WalkScenarioStand,
    WalkScenarioWalk,
    WalkScenarioSprint,
    WalkScenarioJump,
    WalkScenarioTurn,
    WalkScenarioFirstPerson,
    WalkScenarioEdit,
    WalkScenarioRebase,
    WalkScenarioSettle,
    WalkScenarioComplete
} WalkScenarioPhase;

typedef struct WalkScenario
{
    uint32_t tickRate;
    uint32_t totalTicks;
} WalkScenario;

typedef struct WalkScenarioInput
{
    WalkScenarioPhase phase;
    uint32_t phaseTick;
    /* Stable throughout a phase, so a shell can retain a pending capture even
     * when several simulation ticks run before the next rendered frame. */
    uint32_t checkpoint;
    float moveX;
    float moveY;
    float yaw;
    float pitch;
    uint32_t sprint;
    uint32_t jump;
    uint32_t firstPerson;
    uint32_t breakBlock;
    uint32_t placeBlock;
    uint32_t capture;
    uint32_t rebaseCheck;
} WalkScenarioInput;

void WalkScenarioInitialize(WalkScenario *scenario);
/* Input is camera-relative; yaw/pitch are absolute radians. Jump, editing,
 * capture and rebaseCheck are single-tick pulses. Returns 0 at/after completion
 * (with a zeroed Complete output), or for an invalid descriptor/output. */
uint32_t WalkScenarioInputAt(const WalkScenario *scenario, uint32_t tick,
                             WalkScenarioInput *outInput);
const char *WalkScenarioPhaseName(WalkScenarioPhase phase);

typedef struct WalkScenarioMetrics
{
    double samples[WALK_SCENARIO_SAMPLE_CAPACITY];
    double mean;
    uint32_t count;
    uint32_t rejected;
    uint32_t dropped;
} WalkScenarioMetrics;

typedef struct WalkScenarioSummary
{
    uint32_t count;
    uint32_t rejected;
    uint32_t dropped;
    double minimum;
    double mean;
    double p95;
    double p99;
    double maximum;
} WalkScenarioSummary;

void WalkScenarioMetricsInitialize(WalkScenarioMetrics *metrics);
/* Milliseconds must be finite and nonnegative. The first 2048 valid samples are
 * retained; later valid samples increment dropped, invalid ones rejected. */
uint32_t WalkScenarioMetricsAdd(WalkScenarioMetrics *metrics, double milliseconds);
/* Sorts retained samples in place without allocation or a large scratch stack.
 * Call outside the measured frame. Percentiles use the nearest-rank definition.
 * Returns 0 when no valid samples are available; counts are still reported. */
uint32_t WalkScenarioMetricsGetSummary(WalkScenarioMetrics *metrics,
                                       WalkScenarioSummary *outSummary);

typedef struct WalkScenarioImage
{
    uint64_t pixelCount;
    uint64_t blackPixelCount;
    uint64_t whitePixelCount;
    uint64_t differentPixelCount;
    /* Exact count for the 4-bit-per-channel RGB bucket occupying >50% of
     * pixels, or zero when no such majority exists. */
    uint64_t majorityPixelCount;
    uint32_t quantizedColors;
    uint32_t minimumLuminance;
    uint32_t maximumLuminance;
    uint32_t blank;
    uint32_t white;
    uint32_t uniform;
    uint32_t useful;
} WalkScenarioImage;

/* Validates the RGBA8 span (including optional row padding), then examines RGB.
 * Alpha is ignored because a valid swapchain image need not contain opaque
 * alpha. useful excludes uniform, nearly black/white, low-diversity images and
 * images where one quantized RGB bucket occupies at least 99% of pixels.
 * This diagnostic does not prove that textures or character anatomy are right.
 * Returns 0 for an invalid buffer; outImage is cleared on failure. */
uint32_t WalkScenarioAnalyzeRGBA8(const uint8_t *rgba, size_t byteCount, uint32_t width,
                                  uint32_t height, size_t rowStride, WalkScenarioImage *outImage);
