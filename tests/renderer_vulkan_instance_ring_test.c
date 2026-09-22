// Независимый пиксельный oracle для адаптивного Vulkan instance-кольца.
//
// Прежний тест сверял только счётчики draw/drawnQuads и подавал всем вызовам
// один и тот же массив инстансов, поэтому неверное привязывание чанков,
// ошибка dynamic offset или перезапись уже записанных instance bytes
// оставались незамеченными. Этот тест читает последний показанный кадр
// через RendererCaptureFrame и сверяет, в каких слотах кадра оказалась
// геометрия.
//
// Каждый кадр несёт:
//   * несколько видимых probe-вызовов по одному инстансу, разнесённых по
//     слотам и с различимыми поворотами (identity w=1 и пол-оборот вокруг Z);
//   * >16 МиБ offscreen-«балласта» лишними инстансами: он переводит кольцо
//     на новые чанки и нагружает оба кадровых слота, но не даёт overdraw;
//   * probe-вызовы в нескольких чанках, включая два вызова в одном чанке с
//     ненулевым offset.
//
// Oracle не зависит от драйвера: он сверяет не хеш пикселей, а маску
// занятых слотов (геометрия против цвета неба) и суммарную площадь
// геометрии. Пустой слот, «след» прошлого кадра или промах offset ломают
// маску, и тест падает. Без Vulkan-драйвера тест сообщает SKIP (125).

#include "render/chunk_geometry.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKIP_EXIT_CODE 125
#define TEST_WIDTH 64u
#define TEST_HEIGHT 64u
#define MAX_PIXEL_BYTES (TEST_WIDTH * TEST_HEIGHT * 4u)

// Сетка видимых probe-слотов: 3 столбца x 2 строки.
#define SLOT_COLUMNS 3u
#define SLOT_ROWS 2u
#define SLOT_COUNT (SLOT_COLUMNS * SLOT_ROWS)
#define SLOT_SCALE 0.35f

// Балласт: два вызова по 300000 инстансов (18.3 МиБ) или один на 560000
// (17.1 МиБ). Оба варианта выше прежнего лимита 16 МиБ на кадр и заметно
// ниже бюджета 64 МиБ на кадровый слот.
#define BIG_INSTANCES 300000u
#define SINGLE_BIG_INSTANCES 560000u
#define MAX_BALLAST_INSTANCES SINGLE_BIG_INSTANCES

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Vulkan instance ring check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ZeroBytes(void *destination, size_t count)
{
    uint8_t *bytes = (uint8_t *)destination;
    for (size_t index = 0; index < count; ++index)
        bytes[index] = 0u;
}

// Пишет беззнаковое десятичное число: диагностика провала без CRT-формата.
static void WriteUnsigned(uint32_t value)
{
    char reversed[10];
    uint32_t length = 0u;
    if (value == 0u)
    {
        LaiueTestRuntimeWrite("0");
        return;
    }
    while (value != 0u && length < 10u)
    {
        reversed[length++] = (char)('0' + (char)(value % 10u));
        value /= 10u;
    }
    for (uint32_t index = 0u; index < length; ++index)
    {
        char digit[2];
        digit[0] = reversed[length - 1u - index];
        digit[1] = '\0';
        LaiueTestRuntimeWrite(digit);
    }
}

static void FailMask(const char *scenario, uint32_t expected, uint32_t actual, uint32_t measured,
                     uint32_t expectedArea)
{
    LaiueTestRuntimeWrite("Vulkan instance ring check failed: ");
    LaiueTestRuntimeWrite(scenario);
    LaiueTestRuntimeWrite(" slot mask expected ");
    WriteUnsigned(expected);
    LaiueTestRuntimeWrite(" got ");
    WriteUnsigned(actual);
    LaiueTestRuntimeWrite(" geometry pixels ");
    WriteUnsigned(measured);
    LaiueTestRuntimeWrite(" expected area ");
    WriteUnsigned(expectedArea);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void FailHalfCovered(const char *scenario, uint32_t slot)
{
    LaiueTestRuntimeWrite("Vulkan instance ring check failed: ");
    LaiueTestRuntimeWrite(scenario);
    LaiueTestRuntimeWrite(" slot ");
    WriteUnsigned(slot);
    LaiueTestRuntimeWrite(" is neither clearly covered nor clearly empty\n");
    LaiueTestRuntimeExit(1);
}

static void SetIdentity(float matrix[16])
{
    ZeroBytes(matrix, 16u * sizeof(float));
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static void BuildSetup(RendererFrameSetup *setup, uint32_t width, uint32_t height)
{
    ZeroBytes(setup, sizeof(*setup));
    setup->gamma = 1.0f;
    // Небо — чистый красный: геометрия материала 1 заведомо не красная, а
    // значит «не небо» детектируется независимо от точного цвета материала.
    setup->skyColor[0] = 1.0f;
    setup->sunDirection[2] = -1.0f;
    setup->sunColor[0] = 1.0f;
    setup->sunColor[1] = 1.0f;
    setup->sunColor[2] = 1.0f;
    setup->ambientColor[0] = 1.0f;
    setup->ambientColor[1] = 1.0f;
    setup->ambientColor[2] = 1.0f;
    setup->passCount = 1u;
    SetIdentity(setup->passes[0].viewProjection);
    setup->passes[0].rectMaxX = width;
    setup->passes[0].rectMaxY = height;
}

// Слот — прямоугольник в NDC. Столбцы центрированы в -0.66 / 0 / +0.66,
// строки — в +0.45 / -0.45; при SLOT_SCALE 0.35 слоты не пересекаются и не
// касаются краёв кадра.
static void SlotRect(uint32_t slot, float *outX0, float *outY0, float *outX1, float *outY1)
{
    uint32_t column = slot % SLOT_COLUMNS;
    uint32_t row = slot / SLOT_COLUMNS;
    float centerX = -0.66f + (float)column * 0.66f;
    float centerY = 0.45f - (float)row * 0.90f;
    *outX0 = centerX - SLOT_SCALE * 0.5f;
    *outX1 = centerX + SLOT_SCALE * 0.5f;
    *outY0 = centerY - SLOT_SCALE * 0.5f;
    *outY1 = centerY + SLOT_SCALE * 0.5f;
}

// Меш растёт от локального нуля в плюс, поэтому:
//   identity (w = 1)   кладёт квадрат в [origin, origin + scale];
//   пол-оборот вокруг Z кладёт его в [origin - scale, origin].
// Для повёрнутого probe origin выбирается в правом верхнем углу слота —
// тогда на экране он оказывается ровно в слоте, а если поворот потерян,
// квадрат уходит за слот, и маска это ловит.
static void BuildProbeInstance(uint32_t slot, bool rotated, RendererMeshInstance *outInstance)
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    SlotRect(slot, &x0, &y0, &x1, &y1);

    outInstance->scale = SLOT_SCALE;
    outInstance->originRelative[2] = 0.5f - SLOT_SCALE;
    if (rotated)
    {
        outInstance->originRelative[0] = x1;
        outInstance->originRelative[1] = y1;
        outInstance->rotation[0] = 0.0f;
        outInstance->rotation[1] = 0.0f;
        outInstance->rotation[2] = 1.0f;
        outInstance->rotation[3] = 0.0f;
    }
    else
    {
        outInstance->originRelative[0] = x0;
        outInstance->originRelative[1] = y0;
        outInstance->rotation[0] = 0.0f;
        outInstance->rotation[1] = 0.0f;
        outInstance->rotation[2] = 0.0f;
        outInstance->rotation[3] = 1.0f;
    }
}

// Пиксель неба — почти чистый красный. Всё прочее считаем геометрией.
static bool PixelIsGeometry(const uint8_t *pixel)
{
    return !(pixel[0] > 200u && pixel[1] < 70u && pixel[2] < 70u);
}

// Ядро слота — центральные 50% его прямоугольника: края растеризации не
// участвуют, поэтому «занят/пуст» однозначно.
static void SlotCorePixels(uint32_t slot, uint32_t width, uint32_t height, uint32_t *outX0,
                           uint32_t *outY0, uint32_t *outX1, uint32_t *outY1)
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    SlotRect(slot, &x0, &y0, &x1, &y1);
    float margin = SLOT_SCALE * 0.25f;
    int32_t px0 = (int32_t)((x0 + margin + 1.0f) * 0.5f * (float)width);
    int32_t px1 = (int32_t)((x1 - margin + 1.0f) * 0.5f * (float)width);
    int32_t py0 = (int32_t)((1.0f - (y1 - margin)) * 0.5f * (float)height);
    int32_t py1 = (int32_t)((1.0f - (y0 + margin)) * 0.5f * (float)height);
    if (px0 < 0)
        px0 = 0;
    if (py0 < 0)
        py0 = 0;
    if (px1 > (int32_t)width)
        px1 = (int32_t)width;
    if (py1 > (int32_t)height)
        py1 = (int32_t)height;
    if (px1 < px0)
        px1 = px0;
    if (py1 < py0)
        py1 = py0;
    *outX0 = (uint32_t)px0;
    *outY0 = (uint32_t)py0;
    *outX1 = (uint32_t)px1;
    *outY1 = (uint32_t)py1;
}

typedef enum StepKind
{
    STEP_PROBE = 0,
    STEP_BALLAST = 1,
} StepKind;

typedef struct Step
{
    StepKind kind;
    uint32_t slot;  // для STEP_PROBE
    bool rotated;   // для STEP_PROBE
    uint32_t count; // для STEP_BALLAST
} Step;

typedef struct Scenario
{
    const char *name;
    const Step *steps;
    uint32_t stepCount;
} Scenario;

// Вызовы probe нечётных слотов верхней/нижней середины повёрнуты на 180
// градусов вокруг Z: поворот хранится в том же 32-байтовом инстансе, что и
// позиция, поэтому промах offset искажает и поворот.
static const Step kProbesThenSplitSteps[] = {
    {STEP_PROBE, 0u, false, 0u},
    {STEP_PROBE, 1u, true, 0u},
    {STEP_PROBE, 2u, false, 0u},
    {STEP_BALLAST, 0u, false, BIG_INSTANCES},
    {STEP_BALLAST, 0u, false, BIG_INSTANCES},
    {STEP_PROBE, 3u, false, 0u},
    {STEP_PROBE, 4u, true, 0u},
    {STEP_PROBE, 5u, false, 0u},
};

static const Step kBallastThenSplitSteps[] = {
    {STEP_BALLAST, 0u, false, BIG_INSTANCES},
    {STEP_PROBE, 0u, false, 0u},
    {STEP_PROBE, 1u, true, 0u},
    {STEP_BALLAST, 0u, false, BIG_INSTANCES},
    {STEP_PROBE, 2u, false, 0u},
    {STEP_PROBE, 3u, false, 0u},
    {STEP_PROBE, 4u, true, 0u},
    {STEP_PROBE, 5u, false, 0u},
};

// Подмножество слотов: пустые слоты обязаны остаться небом. Это и есть
// проверка отсутствия «следов» соседних кадров и лишних probe.
static const Step kSingleBigSubsetSteps[] = {
    {STEP_PROBE, 0u, false, 0u},
    {STEP_PROBE, 2u, false, 0u},
    {STEP_BALLAST, 0u, false, SINGLE_BIG_INSTANCES},
    {STEP_PROBE, 4u, true, 0u},
};

static const Step kReverseSubsetSteps[] = {
    {STEP_PROBE, 5u, false, 0u},
    {STEP_PROBE, 4u, true, 0u},
    {STEP_BALLAST, 0u, false, SINGLE_BIG_INSTANCES},
    {STEP_PROBE, 0u, false, 0u},
    {STEP_PROBE, 1u, true, 0u},
};

static const Scenario kScenarios[] = {
    {"probes-then-split", kProbesThenSplitSteps,
     (uint32_t)(sizeof(kProbesThenSplitSteps) / sizeof(kProbesThenSplitSteps[0]))},
    {"ballast-then-split", kBallastThenSplitSteps,
     (uint32_t)(sizeof(kBallastThenSplitSteps) / sizeof(kBallastThenSplitSteps[0]))},
    {"single-big-subset", kSingleBigSubsetSteps,
     (uint32_t)(sizeof(kSingleBigSubsetSteps) / sizeof(kSingleBigSubsetSteps[0]))},
    {"reverse-subset", kReverseSubsetSteps,
     (uint32_t)(sizeof(kReverseSubsetSteps) / sizeof(kReverseSubsetSteps[0]))},
};

static void VerifyOccupancy(const Scenario *scenario, const uint8_t *pixels, uint32_t width,
                            uint32_t height)
{
    uint32_t expectedMask = 0u;
    uint32_t probeCount = 0u;
    for (uint32_t index = 0u; index < scenario->stepCount; ++index)
    {
        if (scenario->steps[index].kind != STEP_PROBE)
            continue;
        expectedMask |= 1u << scenario->steps[index].slot;
        ++probeCount;
    }

    uint32_t measured = 0u;
    for (uint32_t y = 0u; y < height; ++y)
    {
        for (uint32_t x = 0u; x < width; ++x)
        {
            const uint8_t *pixel = pixels + ((size_t)y * width + x) * 4u;
            if (PixelIsGeometry(pixel))
                ++measured;
        }
    }

    uint32_t actualMask = 0u;
    for (uint32_t slot = 0u; slot < SLOT_COUNT; ++slot)
    {
        uint32_t x0 = 0u;
        uint32_t y0 = 0u;
        uint32_t x1 = 0u;
        uint32_t y1 = 0u;
        SlotCorePixels(slot, width, height, &x0, &y0, &x1, &y1);
        uint32_t corePixels = (x1 - x0) * (y1 - y0);
        if (corePixels == 0u)
            continue;
        uint32_t covered = 0u;
        for (uint32_t y = y0; y < y1; ++y)
        {
            for (uint32_t x = x0; x < x1; ++x)
            {
                const uint8_t *pixel = pixels + ((size_t)y * width + x) * 4u;
                if (PixelIsGeometry(pixel))
                    ++covered;
            }
        }
        double ratio = (double)covered / (double)corePixels;
        if (ratio >= 0.4)
            actualMask |= 1u << slot;
        else if (ratio > 0.1)
            FailHalfCovered(scenario->name, slot);
    }

    // Площадь геометрии ловит лишние/пропавшие probe, даже если маска слотов
    // случайно совпала. Границы широкие: считаем не пиксель-в-пиксель, а
    // порядок площади; растеризация краёв драйвер-независима «на глаз».
    uint32_t slotWidthPixels = (uint32_t)(SLOT_SCALE * 0.5f * (float)width);
    uint32_t slotHeightPixels = (uint32_t)(SLOT_SCALE * 0.5f * (float)height);
    uint32_t expectedArea = probeCount * slotWidthPixels * slotHeightPixels;
    bool areaOk = measured > expectedArea / 2u && measured < expectedArea * 2u;
    if (actualMask != expectedMask || !areaOk)
        FailMask(scenario->name, expectedMask, actualMask, measured, expectedArea);
}

static void RunScenario(Renderer *renderer, RendererMesh *mesh, const RendererMeshInstance *ballast,
                        const Scenario *scenario, uint8_t *pixels, uint32_t width, uint32_t height)
{
    RendererFrameSetup setup;
    BuildSetup(&setup, width, height);

    Expect(RendererBeginFrame(renderer, &setup), "a probe frame could not begin");
    RendererBeginScenePass(renderer, 0u);

    uint64_t expectedQuads = 0u;
    uint64_t ballastBytes = 0u;
    for (uint32_t index = 0u; index < scenario->stepCount; ++index)
    {
        const Step *step = &scenario->steps[index];
        if (step->kind == STEP_PROBE)
        {
            RendererMeshInstance instance;
            BuildProbeInstance(step->slot, step->rotated, &instance);
            RendererDrawMeshInstances(renderer, mesh, &instance, 1u);
            expectedQuads += 1u;
        }
        else
        {
            RendererDrawMeshInstances(renderer, mesh, ballast, step->count);
            expectedQuads += step->count;
            ballastBytes += (uint64_t)step->count * (uint64_t)sizeof(RendererMeshInstance);
        }
    }
    Expect(RendererEndFrame(renderer), "a probe frame could not end");

    // Каждый сценарий обязан перевалить прежний лимит 16 МиБ: иначе это не
    // регрессия адаптивного кольца.
    Expect(ballastBytes > 16u * 1024u * 1024u,
           "the scenario load must exceed the former 16 MiB budget");

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    Expect(stats.drawCalls == scenario->stepCount, "a probe frame dropped or added a draw call");
    Expect(stats.drawnQuads == expectedQuads, "a probe frame lost or duplicated instances");

    uint32_t capturedWidth = 0u;
    uint32_t capturedHeight = 0u;
    Expect(RendererCaptureFrame(renderer, pixels, MAX_PIXEL_BYTES, &capturedWidth, &capturedHeight),
           "a probe frame could not be captured");
    Expect(capturedWidth == width && capturedHeight == height,
           "the captured probe frame must follow the requested size");

    VerifyOccupancy(scenario, pixels, width, height);
}

LAIUE_TEST_ENTRY(RendererVulkanInstanceRingTestEntryPoint)
{
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        LaiueTestRuntimeWrite("No Vulkan backend available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    Renderer *renderer = RendererCreateWithBackend(NULL, (int32_t)TEST_WIDTH, (int32_t)TEST_HEIGHT,
                                                   RENDERER_BACKEND_VULKAN);
    if (renderer == NULL)
    {
        LaiueTestRuntimeWrite("No Vulkan driver available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    Expect(RendererPrepareWorld(renderer), "the Vulkan world could not be prepared");

    ChunkQuad quad = PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
    RendererMesh *mesh = RendererCreateMesh(renderer, &quad, 1u);
    Expect(mesh != NULL, "the Vulkan test mesh could not be created");

    uint8_t *pixels = (uint8_t *)PlatformAllocate(MAX_PIXEL_BYTES, true);
    Expect(pixels != NULL, "the frame readback buffer could not be allocated");

    // Балласт один раз: все экземпляры далеко за пределами clip space, поэтому
    // они нагружают кольцо и вершинный шейдер, но не дают ни одного фрагмента.
    RendererMeshInstance *ballast = (RendererMeshInstance *)PlatformAllocate(
        (size_t)MAX_BALLAST_INSTANCES * sizeof(RendererMeshInstance), false);
    Expect(ballast != NULL, "the offscreen ballast array could not be allocated");
    for (uint32_t index = 0u; index < MAX_BALLAST_INSTANCES; ++index)
    {
        ballast[index].originRelative[0] = 1000.0f;
        ballast[index].originRelative[1] = 1000.0f;
        ballast[index].originRelative[2] = 0.0f;
        ballast[index].scale = 1.0f;
        ballast[index].rotation[0] = 0.0f;
        ballast[index].rotation[1] = 0.0f;
        ballast[index].rotation[2] = 0.0f;
        ballast[index].rotation[3] = 1.0f;
    }

    // Порядок чередует сценарии и тем самым оба кадровых слота (кольцо и
    // чанки у каждого слота свои), разную раскладку probe по чанкам, разную
    // величину нагрузки и оба режима «probe до/после балласта».
    RunScenario(renderer, mesh, ballast, &kScenarios[0], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[1], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[2], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[3], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[0], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[1], pixels, TEST_WIDTH, TEST_HEIGHT);

    // Resize: слоты в NDC те же, но цель и depth пересоздаются. Оба размера
    // проверяются пиксельно, чтобы «успешный» кадр после resize не сошёл за
    // рабочий.
    RendererResize(renderer, 32, 32);
    RunScenario(renderer, mesh, ballast, &kScenarios[2], pixels, 32u, 32u);
    RunScenario(renderer, mesh, ballast, &kScenarios[0], pixels, 32u, 32u);
    RunScenario(renderer, mesh, ballast, &kScenarios[3], pixels, 32u, 32u);

    RendererResize(renderer, (int32_t)TEST_WIDTH, (int32_t)TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[1], pixels, TEST_WIDTH, TEST_HEIGHT);
    RunScenario(renderer, mesh, ballast, &kScenarios[2], pixels, TEST_WIDTH, TEST_HEIGHT);

    PlatformFree(ballast);
    PlatformFree(pixels);
    RendererDestroyMesh(renderer, mesh);
    RendererDestroy(renderer);
    LaiueTestRuntimeWrite("Vulkan instance ring pixel checks passed\n");
    LAIUE_TEST_SUCCESS();
}
