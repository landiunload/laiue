#include "test_runtime.h"
#include "walk_scenario.h"

#include <float.h>

/* Large retained sample arrays belong to persistent app state, not the stack. */
static WalkScenarioMetrics g_metrics;
static uint8_t g_texturePixels[32u * 32u * 4u];

static void Expect(uint32_t condition, const char *message)
{
    if (condition == 0u)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static void TestScenario(void)
{
    WalkScenario scenario;
    WalkScenarioInput input;
    WalkScenarioInitialize(&scenario);
    Expect(scenario.tickRate == 128u && scenario.totalTicks == 1280u,
           "scenario uses ten seconds of the real 128 Hz physics step");
    uint32_t seen = 0u;
    uint32_t captures = 0u;
    uint32_t jumps = 0u;
    uint32_t breaks = 0u;
    uint32_t places = 0u;
    uint32_t rebases = 0u;
    uint32_t breakTick = UINT32_MAX;
    uint32_t placeTick = UINT32_MAX;
    uint32_t lastCheckpoint = 0u;
    float firstTurnYaw = 0.0f;
    float lastTurnYaw = 0.0f;
    for (uint32_t tick = 0u; tick < scenario.totalTicks; ++tick)
    {
        Expect(WalkScenarioInputAt(&scenario, tick, &input), "every scheduled tick has input");
        Expect(input.phase < WalkScenarioComplete, "scheduled input has an active phase");
        seen |= 1u << (uint32_t)input.phase;
        Expect(input.moveX >= -1.0f && input.moveX <= 1.0f && input.moveY >= -1.0f &&
                   input.moveY <= 1.0f,
               "scenario movement is normalized camera-relative input");
        Expect(input.checkpoint >= lastCheckpoint,
               "capture identifiers remain ordered through catch-up ticks");
        if (input.phaseTick != 0u)
            Expect(input.checkpoint == lastCheckpoint, "checkpoint stays stable within a phase");
        lastCheckpoint = input.checkpoint;
        if (input.sprint)
            Expect(input.moveY > 0.0f && input.phase == WalkScenarioSprint,
                   "sprint exercises forward movement only in its phase");
        if (input.phase == WalkScenarioJump)
            Expect(input.moveX == 0.0f && input.moveY == 0.0f && input.sprint == 0u,
                   "jump phase lets the sprint gait settle into a planted support");
        if (input.jump)
            Expect(input.phase == WalkScenarioJump &&
                       input.phaseTick >= WALK_SCENARIO_TICK_RATE / 2u,
                   "grounded jump waits half a second for the preceding gait to settle");
        if (input.phase == WalkScenarioTurn)
        {
            if (input.phaseTick == 0u)
                firstTurnYaw = input.yaw;
            Expect(input.yaw >= lastTurnYaw, "camera turn progresses smoothly");
            lastTurnYaw = input.yaw;
        }
        if (input.phase == WalkScenarioFirstPerson || input.phase == WalkScenarioEdit)
            Expect(input.firstPerson, "both view and editing exercise first-person mode");
        if (input.breakBlock)
        {
            breakTick = tick;
            Expect(input.phase == WalkScenarioEdit && input.firstPerson && input.pitch < -0.8f,
                   "block removal targets the generated ground from first person");
        }
        if (input.placeBlock)
            placeTick = tick;
        if (input.capture)
            Expect(input.phaseTick >= 16u, "capture gives each phase time to settle");
        captures += input.capture;
        jumps += input.jump;
        breaks += input.breakBlock;
        places += input.placeBlock;
        rebases += input.rebaseCheck;
    }
    Expect(seen == 0x1ffu && captures == 9u, "all nine phases offer distinct capture checkpoints");
    Expect(jumps == 1u && breaks == 1u && places == 1u && rebases == 1u,
           "single-use actions never repeat in a held input");
    Expect(placeTick > breakTick + 16u, "placement follows removal after multiple physics ticks");
    Expect(lastTurnYaw - firstTurnYaw > 1.5f,
           "turn changes the view by approximately ninety degrees");
    Expect(WalkScenarioInputAt(&scenario, scenario.totalTicks, &input) == 0u &&
               input.phase == WalkScenarioComplete && input.jump == 0u && input.capture == 0u,
           "terminal input is complete and cannot repeat an action");
    Expect(WalkScenarioInputAt(&scenario, UINT32_MAX, &input) == 0u &&
               input.phase == WalkScenarioComplete,
           "extreme ticks do not overflow the scenario");
    Expect(WalkScenarioInputAt(NULL, 0u, &input) == 0u && input.phase == WalkScenarioComplete &&
               WalkScenarioInputAt(&scenario, 0u, NULL) == 0u,
           "missing descriptors and output are rejected");
    scenario.tickRate = 60u;
    Expect(WalkScenarioInputAt(&scenario, 0u, &input) == 0u,
           "different tick rates cannot silently change the scenario timing");
}

static void TestMetrics(void)
{
    WalkScenarioSummary summary;
    WalkScenarioMetricsInitialize(&g_metrics);
    Expect(WalkScenarioMetricsGetSummary(&g_metrics, &summary) == 0u && summary.count == 0u &&
               summary.maximum == 0.0,
           "an empty collector does not manufacture percentile values");
    for (uint32_t sample = 100u; sample != 0u; --sample)
        Expect(WalkScenarioMetricsAdd(&g_metrics, (double)sample),
               "valid CPU samples are retained");
    Expect(WalkScenarioMetricsGetSummary(&g_metrics, &summary) && summary.count == 100u &&
               summary.minimum == 1.0 && summary.mean == 50.5 && summary.p95 == 95.0 &&
               summary.p99 == 99.0 && summary.maximum == 100.0,
           "descending samples produce nearest-rank tail latency and exact mean");
    Expect(WalkScenarioMetricsAdd(&g_metrics, 0.0), "a zero-resolution measurement is valid");
    Expect(WalkScenarioMetricsGetSummary(&g_metrics, &summary) && summary.count == 101u &&
               summary.minimum == 0.0 && summary.p95 == 95.0 && summary.p99 == 99.0,
           "collection can continue after an earlier report sorted the buffer");
    union
    {
        uint64_t bits;
        double number;
    } invalid = {UINT64_C(0x7ff8000000000000)};
    Expect(!WalkScenarioMetricsAdd(&g_metrics, invalid.number), "NaN measurements are rejected");
    invalid.bits = UINT64_C(0x7ff0000000000000);
    Expect(!WalkScenarioMetricsAdd(&g_metrics, invalid.number),
           "infinite measurements are rejected");
    Expect(!WalkScenarioMetricsAdd(&g_metrics, -0.5), "negative time is rejected");
    Expect(WalkScenarioMetricsGetSummary(&g_metrics, &summary) && summary.rejected == 3u &&
               summary.count == 101u,
           "invalid samples do not contaminate retained statistics");

    WalkScenarioMetricsInitialize(&g_metrics);
    for (uint32_t sample = 0u; sample < WALK_SCENARIO_SAMPLE_CAPACITY; ++sample)
        Expect(WalkScenarioMetricsAdd(&g_metrics, 2.0), "bounded collector retains its capacity");
    Expect(!WalkScenarioMetricsAdd(&g_metrics, 1000.0), "samples beyond capacity are dropped");
    Expect(WalkScenarioMetricsGetSummary(&g_metrics, &summary) &&
               summary.count == WALK_SCENARIO_SAMPLE_CAPACITY && summary.dropped == 1u &&
               summary.mean == 2.0 && summary.p99 == 2.0 && summary.maximum == 2.0,
           "dropped tail samples are explicit and cannot silently affect the bounded report");

    WalkScenarioMetricsInitialize(&g_metrics);
    Expect(WalkScenarioMetricsAdd(&g_metrics, DBL_MAX) &&
               WalkScenarioMetricsAdd(&g_metrics, DBL_MAX) &&
               WalkScenarioMetricsGetSummary(&g_metrics, &summary) && summary.mean == DBL_MAX,
           "finite extreme samples do not overflow a running sum");
    Expect(WalkScenarioMetricsGetSummary(NULL, &summary) == 0u &&
               WalkScenarioMetricsGetSummary(&g_metrics, NULL) == 0u &&
               WalkScenarioMetricsAdd(NULL, 1.0) == 0u,
           "metric operations safely reject missing state");
}

static void FillImage(uint8_t *pixels, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t pixel = 0u; pixel < 100u; ++pixel)
    {
        pixels[pixel * 4u] = r;
        pixels[pixel * 4u + 1u] = g;
        pixels[pixel * 4u + 2u] = b;
        pixels[pixel * 4u + 3u] = (uint8_t)pixel;
    }
}

static void TestImages(void)
{
    uint8_t pixels[400];
    WalkScenarioImage image;
    FillImage(pixels, 255u, 255u, 255u);
    Expect(WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 40u, &image) && image.white &&
               image.uniform && !image.useful,
           "white-screen graphics failures are rejected regardless of alpha");
    FillImage(pixels, 0u, 0u, 0u);
    Expect(WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 40u, &image) && image.blank &&
               image.uniform && !image.useful,
           "an empty black framebuffer is rejected");
    FillImage(pixels, 30u, 130u, 70u);
    Expect(WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 40u, &image) &&
               !image.blank && !image.white && image.uniform && !image.useful,
           "a colored clear image alone cannot pass a rendered-scene check");
    for (uint32_t pixel = 0u; pixel < 1024u; ++pixel)
    {
        g_texturePixels[pixel * 4u] = pixel < 5u ? 255u : 48u;
        g_texturePixels[pixel * 4u + 1u] = pixel < 5u ? (uint8_t)(pixel * 48u) : 96u;
        g_texturePixels[pixel * 4u + 2u] = pixel < 5u ? 0u : 160u;
        g_texturePixels[pixel * 4u + 3u] = 255u;
    }
    Expect(WalkScenarioAnalyzeRGBA8(g_texturePixels, sizeof(g_texturePixels), 32u, 32u, 128u,
                                    &image) &&
               image.quantizedColors >= 6u && !image.uniform && !image.blank && !image.white &&
               image.maximumLuminance - image.minimumLuminance >= 8u &&
               image.majorityPixelCount == 1019u && !image.useful,
           "a tiny multicolor antialiased crosshair cannot disguise a colored clear framebuffer");
    for (uint32_t y = 0u; y < 32u; ++y)
    {
        for (uint32_t x = 0u; x < 32u; ++x)
        {
            const uint32_t pixel = (y * 32u + x) * 4u;
            g_texturePixels[pixel] = (uint8_t)(x * 8u);
            g_texturePixels[pixel + 1u] = (uint8_t)(y * 8u);
            g_texturePixels[pixel + 2u] = (uint8_t)(((x + y) & 31u) * 8u);
        }
    }
    Expect(WalkScenarioAnalyzeRGBA8(g_texturePixels, sizeof(g_texturePixels), 32u, 32u, 128u,
                                    &image) &&
               image.majorityPixelCount == 0u && image.useful,
           "an ordinary varied 32 by 32 texture passes without a dominant background");
    for (uint32_t pixel = 0u; pixel < 100u; ++pixel)
    {
        pixels[pixel * 4u] = (uint8_t)(pixel * 2u);
        pixels[pixel * 4u + 1u] = (uint8_t)(255u - pixel * 2u);
        pixels[pixel * 4u + 2u] = (uint8_t)(pixel % 3u * 80u);
    }
    Expect(WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 40u, &image) &&
               image.pixelCount == 100u && image.quantizedColors >= 4u &&
               image.maximumLuminance > image.minimumLuminance && image.useful,
           "a varied visible image passes only the basic nonblank diagnostic");
    Expect(WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 4u, 4u, 40u, &image) &&
               image.pixelCount == 16u,
           "readback rows may contain padding that is not analyzed as pixels");
    Expect(!WalkScenarioAnalyzeRGBA8(pixels, 135u, 4u, 4u, 40u, &image) && image.pixelCount == 0u,
           "a truncated final row is rejected before pixels are read");
    Expect(!WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 39u, &image),
           "a stride shorter than one pixel row is rejected");
    Expect(!WalkScenarioAnalyzeRGBA8(pixels, SIZE_MAX, 1u, 2u, SIZE_MAX, &image),
           "last-row offset overflow is rejected");
    Expect(!WalkScenarioAnalyzeRGBA8(pixels, SIZE_MAX, UINT32_MAX, UINT32_MAX, SIZE_MAX, &image),
           "extreme image dimensions cannot wrap the required buffer span");
    Expect(!WalkScenarioAnalyzeRGBA8(NULL, sizeof(pixels), 10u, 10u, 40u, &image) &&
               !WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 0u, 10u, 40u, &image) &&
               !WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 0u, 40u, &image) &&
               !WalkScenarioAnalyzeRGBA8(pixels, sizeof(pixels), 10u, 10u, 40u, NULL),
           "missing and empty images fail without reading memory");
}

LAIUE_TEST_ENTRY(WalkScenarioTestEntryPoint)
{
    TestScenario();
    TestMetrics();
    TestImages();
    LaiueTestRuntimeWrite("walk scenario, bounded metrics and RGBA8 diagnostics passed\n");
    LAIUE_TEST_SUCCESS();
}
