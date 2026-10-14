#include "walk_scenario.h"

#include "walk_math.h"

static void IncrementSaturated(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

void WalkScenarioInitialize(WalkScenario *scenario)
{
    if (scenario == NULL)
        return;
    scenario->tickRate = WALK_SCENARIO_TICK_RATE;
    scenario->totalTicks = WALK_SCENARIO_TOTAL_TICKS;
}

uint32_t WalkScenarioInputAt(const WalkScenario *scenario, uint32_t tick,
                             WalkScenarioInput *outInput)
{
    static const uint32_t ends[] = {128u, 384u, 512u, 640u, 768u, 896u, 1024u, 1152u, 1280u};
    if (outInput == NULL)
        return 0u;
    *outInput = (WalkScenarioInput){.phase = WalkScenarioComplete};
    if (scenario == NULL || scenario->tickRate != WALK_SCENARIO_TICK_RATE ||
        scenario->totalTicks != WALK_SCENARIO_TOTAL_TICKS || tick >= scenario->totalTicks)
        return 0u;

    uint32_t phase = 0u;
    while (tick >= ends[phase])
        ++phase;
    const uint32_t start = phase == 0u ? 0u : ends[phase - 1u];
    const uint32_t localTick = tick - start;
    const uint32_t length = ends[phase] - start;
    outInput->phase = (WalkScenarioPhase)phase;
    outInput->phaseTick = localTick;
    outInput->checkpoint = phase + 1u;
    outInput->capture = localTick == length - 16u;
    if (phase == WalkScenarioWalk || phase == WalkScenarioSprint)
        outInput->moveY = 1.0f;
    if (phase == WalkScenarioSprint)
        outInput->sprint = 1u;
    if (phase == WalkScenarioJump)
        /* Allow the sprint gait to plant both feet before asking for a legal
         * grounded jump. Continuing to move here made the pulse depend on the
         * gait's support phase instead of testing the jump controller. */
        outInput->jump = localTick == WALK_SCENARIO_TICK_RATE / 2u;
    if (phase == WalkScenarioTurn)
    {
        outInput->moveX = 0.5f;
        outInput->yaw = 1.5707963268f * (float)localTick / (float)(length - 1u);
        outInput->pitch = -0.15f;
    }
    else if (phase >= WalkScenarioFirstPerson)
    {
        outInput->yaw = 1.5707963268f;
        outInput->pitch = phase == WalkScenarioEdit ? -1.0f : -0.15f;
    }
    outInput->firstPerson = phase == WalkScenarioFirstPerson || phase == WalkScenarioEdit;
    if (phase == WalkScenarioEdit)
    {
        outInput->breakBlock = localTick == 24u;
        outInput->placeBlock = localTick == 72u;
    }
    if (phase == WalkScenarioRebase)
    {
        outInput->moveX = -0.5f;
        outInput->moveY = 0.5f;
        outInput->rebaseCheck = localTick == 8u;
    }
    return 1u;
}

const char *WalkScenarioPhaseName(WalkScenarioPhase phase)
{
    static const char *const names[] = {"stand",        "walk", "sprint", "jump",   "turn",
                                        "first_person", "edit", "rebase", "settle", "complete"};
    return (uint32_t)phase <= (uint32_t)WalkScenarioComplete ? names[(uint32_t)phase] : "invalid";
}

void WalkScenarioMetricsInitialize(WalkScenarioMetrics *metrics)
{
    if (metrics == NULL)
        return;
    metrics->mean = 0.0;
    metrics->count = 0u;
    metrics->rejected = 0u;
    metrics->dropped = 0u;
}

uint32_t WalkScenarioMetricsAdd(WalkScenarioMetrics *metrics, double milliseconds)
{
    if (metrics == NULL)
        return 0u;
    if (!WalkMathFinite(milliseconds) || milliseconds < 0.0)
    {
        IncrementSaturated(&metrics->rejected);
        return 0u;
    }
    if (metrics->count >= WALK_SCENARIO_SAMPLE_CAPACITY)
    {
        IncrementSaturated(&metrics->dropped);
        return 0u;
    }
    metrics->samples[metrics->count++] = milliseconds;
    metrics->mean += (milliseconds - metrics->mean) / (double)metrics->count;
    return 1u;
}

static void SiftSamples(double *samples, uint32_t root, uint32_t count)
{
    const double value = samples[root];
    while (root < count / 2u)
    {
        uint32_t child = root * 2u + 1u;
        if (child + 1u < count && samples[child] < samples[child + 1u])
            ++child;
        if (value >= samples[child])
            break;
        samples[root] = samples[child];
        root = child;
    }
    samples[root] = value;
}

uint32_t WalkScenarioMetricsGetSummary(WalkScenarioMetrics *metrics,
                                       WalkScenarioSummary *outSummary)
{
    if (outSummary == NULL)
        return 0u;
    *outSummary = (WalkScenarioSummary){0};
    if (metrics == NULL)
        return 0u;
    outSummary->rejected = metrics->rejected;
    outSummary->dropped = metrics->dropped;
    if (metrics->count == 0u || metrics->count > WALK_SCENARIO_SAMPLE_CAPACITY)
        return 0u;
    const uint32_t count = metrics->count;
    for (uint32_t root = count / 2u; root != 0u; --root)
        SiftSamples(metrics->samples, root - 1u, count);
    for (uint32_t end = count - 1u; end != 0u; --end)
    {
        const double maximum = metrics->samples[0];
        metrics->samples[0] = metrics->samples[end];
        metrics->samples[end] = maximum;
        SiftSamples(metrics->samples, 0u, end);
    }
    outSummary->count = count;
    outSummary->minimum = metrics->samples[0];
    outSummary->mean = metrics->mean;
    outSummary->p95 = metrics->samples[(count * 95u + 99u) / 100u - 1u];
    outSummary->p99 = metrics->samples[(count * 99u + 99u) / 100u - 1u];
    outSummary->maximum = metrics->samples[count - 1u];
    return 1u;
}

uint32_t WalkScenarioAnalyzeRGBA8(const uint8_t *rgba, size_t byteCount, uint32_t width,
                                  uint32_t height, size_t rowStride, WalkScenarioImage *outImage)
{
    if (outImage == NULL)
        return 0u;
    *outImage = (WalkScenarioImage){0};
    if (rgba == NULL || width == 0u || height == 0u)
        return 0u;
    const size_t rowBytes = (size_t)width * 4u;
    if (rowBytes / 4u != width || rowStride < rowBytes ||
        (size_t)(height - 1u) > (SIZE_MAX - rowBytes) / rowStride)
        return 0u;
    const size_t required = (size_t)(height - 1u) * rowStride + rowBytes;
    if (byteCount < required)
        return 0u;

    uint64_t colors[64];
    for (uint32_t index = 0u; index < 64u; ++index)
        colors[index] = 0u;
    outImage->pixelCount = (uint64_t)width * height;
    outImage->minimumLuminance = 255u;
    uint32_t majorityBucket = 0u;
    uint64_t majorityVotes = 0u;
    const uint32_t firstColor =
        (uint32_t)rgba[0] | ((uint32_t)rgba[1] << 8u) | ((uint32_t)rgba[2] << 16u);
    for (uint32_t y = 0u; y < height; ++y)
    {
        const uint8_t *row = rgba + (size_t)y * rowStride;
        for (uint32_t x = 0u; x < width; ++x)
        {
            const uint8_t *pixel = row + (size_t)x * 4u;
            const uint32_t r = pixel[0];
            const uint32_t g = pixel[1];
            const uint32_t b = pixel[2];
            const uint32_t bucket = ((r >> 4u) << 8u) | ((g >> 4u) << 4u) | (b >> 4u);
            if (majorityVotes == 0u)
            {
                majorityBucket = bucket;
                majorityVotes = 1u;
            }
            else if (majorityBucket == bucket)
                ++majorityVotes;
            else
                --majorityVotes;
            const uint64_t bit = UINT64_C(1) << (bucket & 63u);
            if ((colors[bucket >> 6u] & bit) == 0u)
            {
                colors[bucket >> 6u] |= bit;
                ++outImage->quantizedColors;
            }
            outImage->blackPixelCount += r <= 3u && g <= 3u && b <= 3u;
            outImage->whitePixelCount += r >= 252u && g >= 252u && b >= 252u;
            const uint32_t color = r | (g << 8u) | (b << 16u);
            outImage->differentPixelCount += color != firstColor;
            const uint32_t luminance = (r * 54u + g * 183u + b * 19u) >> 8u;
            if (luminance < outImage->minimumLuminance)
                outImage->minimumLuminance = luminance;
            if (luminance > outImage->maximumLuminance)
                outImage->maximumLuminance = luminance;
        }
    }
    /* Boyer-Moore identifies the only possible majority without a histogram.
     * Confirm its count in a second pass: remaining votes alone are not its
     * frequency, and the candidate can exist without a true majority. */
    if (majorityVotes != 0u)
    {
        uint64_t count = 0u;
        for (uint32_t y = 0u; y < height; ++y)
        {
            const uint8_t *row = rgba + (size_t)y * rowStride;
            for (uint32_t x = 0u; x < width; ++x)
            {
                const uint8_t *pixel = row + (size_t)x * 4u;
                const uint32_t bucket = ((uint32_t)(pixel[0] >> 4u) << 8u) |
                                        ((uint32_t)(pixel[1] >> 4u) << 4u) | (pixel[2] >> 4u);
                count += bucket == majorityBucket;
            }
        }
        if (count > outImage->pixelCount / 2u)
            outImage->majorityPixelCount = count;
    }
    const uint64_t nearUniform = outImage->pixelCount - outImage->pixelCount / 100u;
    outImage->blank = outImage->blackPixelCount >= nearUniform;
    outImage->white = outImage->whitePixelCount >= nearUniform;
    outImage->uniform = outImage->differentPixelCount == 0u;
    outImage->useful = !outImage->blank && !outImage->white && !outImage->uniform &&
                       outImage->quantizedColors >= 4u &&
                       outImage->maximumLuminance - outImage->minimumLuminance >= 8u &&
                       outImage->majorityPixelCount < nearUniform;
    return 1u;
}
