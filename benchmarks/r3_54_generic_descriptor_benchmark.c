// Vulkan-only CPU benchmark for per-draw generic descriptor allocation.
// Each sample records 50 frames of 256 draws while cycling through 1, 16 or
// 256 descriptor keys. GPU work and frame waits are outside the timed loop.

#include "platform/system.h"
#include "render/renderer.h"
#include "render/renderer_offscreen.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BENCH_WIDTH 320u
#define BENCH_HEIGHT 180u
#define PIXEL_BYTES (BENCH_WIDTH * BENCH_HEIGHT * 4u)
#define KEY_COUNT 256u
#define DRAW_COUNT 256u
#define FRAME_COUNT 50u
#define SAMPLE_COUNT 5u
#define SKIP_EXIT_CODE 125

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);

    char text[22];
    for (uint32_t index = 0u; index < count; ++index)
        text[index] = digits[count - index - 1u];
    text[count] = '\0';
    WriteText(text);
}

static uint64_t ChecksumPixels(const uint8_t *pixels)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t index = 0u; index < PIXEL_BYTES; ++index)
        hash = (hash ^ pixels[index]) * 1099511628211ull;
    return hash;
}

static void BuildFrameSetup(RendererFrameSetup *setup)
{
    setup->gamma = 1.0f;
    setup->skyColor[2] = 1.0f;
    setup->sunDirection[2] = -1.0f;
    setup->sunColor[0] = setup->sunColor[1] = setup->sunColor[2] = 1.0f;
    setup->ambientColor[0] = setup->ambientColor[1] = setup->ambientColor[2] = 1.0f;
    setup->passCount = 1u;
    for (uint32_t index = 0u; index < 16u; ++index)
        setup->passes[0].viewProjection[index] = 0.0f;
    setup->passes[0].viewProjection[0] = 1.0f;
    setup->passes[0].viewProjection[5] = 1.0f;
    setup->passes[0].viewProjection[10] = 1.0f;
    setup->passes[0].viewProjection[15] = 1.0f;
    setup->passes[0].rectMaxX = BENCH_WIDTH;
    setup->passes[0].rectMaxY = BENCH_HEIGHT;
}

static uint64_t NowMicroseconds(void)
{
    return (uint64_t)(PlatformMonotonicSeconds() * 1000000.0 + 0.5);
}

static bool RunSample(Renderer *renderer, RendererFrameSetup *setup,
    RendererMesh *const *meshes, uint32_t keyCount, uint64_t *outMicroseconds)
{
    uint64_t elapsed = 0u;
    for (uint32_t frame = 0u; frame < FRAME_COUNT; ++frame)
    {
        if (!RendererBeginFrame(renderer, setup)) return false;
        RendererBeginScenePass(renderer, 0u);
        uint64_t begin = NowMicroseconds();
        for (uint32_t draw = 0u; draw < DRAW_COUNT; ++draw)
            RendererDrawGenericMeshRange(renderer, meshes[draw % keyCount],
                NULL, 1.0f, 0u, 3u);
        elapsed += NowMicroseconds() - begin;
        if (!RendererEndFrame(renderer)) return false;

        RendererStats stats;
        RendererGetStats(renderer, &stats);
        if (stats.drawCalls != DRAW_COUNT) return false;
    }
    *outMicroseconds = elapsed;
    return true;
}

static bool RunScenario(Renderer *renderer, RendererFrameSetup *setup,
    RendererMesh *const *meshes, uint32_t keyCount, uint64_t *outChecksum)
{
    uint8_t *pixels = (uint8_t *)PlatformAllocate(PIXEL_BYTES, false);
    if (pixels == NULL) return false;

    const char *name = keyCount == 1u ? "generic_k1" :
                       keyCount == 16u ? "generic_k16" : "generic_k256";
    bool ok = true;
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT && ok; ++sample)
    {
        uint64_t elapsed = 0u;
        ok = RunSample(renderer, setup, meshes, keyCount, &elapsed);
        if (ok)
        {
            WriteText("stage=");
            WriteText(name);
            WriteText(" sample=");
            WriteUnsigned(sample);
            WriteText(" us=");
            WriteUnsigned(elapsed);
            WriteText(" draws=");
            WriteUnsigned((uint64_t)DRAW_COUNT * FRAME_COUNT);
            WriteText("\n");
        }
    }

    uint32_t width = 0u;
    uint32_t height = 0u;
    if (ok)
        ok = RendererCaptureFrame(renderer, pixels, PIXEL_BYTES, &width, &height) &&
            width == BENCH_WIDTH && height == BENCH_HEIGHT;
    if (ok) *outChecksum = ChecksumPixels(pixels);
    PlatformFree(pixels);
    return ok;
}

LAIUE_TEST_ENTRY(R3GenericDescriptorBenchmarkEntryPoint)
{
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN))
    {
        WriteText("Vulkan backend is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    Renderer *renderer = RendererCreateWithBackend(NULL, (int32_t)BENCH_WIDTH,
        (int32_t)BENCH_HEIGHT, RENDERER_BACKEND_VULKAN);
    if (renderer == NULL)
    {
        WriteText("No Vulkan driver available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    if (!RendererPrepareWorld(renderer))
    {
        WriteText("Vulkan world preparation failed\n");
        LaiueTestRuntimeExit(1);
    }

    RendererFrameSetup setup = { 0 };
    BuildFrameSetup(&setup);
    uint32_t vertexCount = KEY_COUNT * (KEY_COUNT + 5u) / 2u;
    RendererGenericVertex *vertices = (RendererGenericVertex *)PlatformAllocate(
        (size_t)vertexCount * sizeof(*vertices), false);
    RendererMesh **meshes = (RendererMesh **)PlatformAllocate(
        KEY_COUNT * sizeof(*meshes), true);
    if (vertices == NULL || meshes == NULL)
    {
        WriteText("benchmark memory allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    const RendererGenericVertex triangle[3] = {
        { { -0.75f, -0.75f, 0.0f }, { 0.0f, 0.0f }, 0xFFFFFFFFu },
        { {  0.75f, -0.75f, 0.0f }, { 1.0f, 0.0f }, 0xFFFFFFFFu },
        { {  0.00f,  0.75f, 0.0f }, { 0.5f, 1.0f }, 0xFFFFFFFFu },
    };
    uint32_t vertexOffset = 0u;
    for (uint32_t key = 0u; key < KEY_COUNT; ++key)
    {
        uint32_t count = key + 3u;
        for (uint32_t vertex = 0u; vertex < count; ++vertex)
            vertices[vertexOffset + vertex] = triangle[vertex % 3u];
        meshes[key] = RendererCreateGenericMesh(renderer,
            &vertices[vertexOffset], count);
        if (meshes[key] == NULL)
        {
            WriteText("generic mesh creation failed\n");
            LaiueTestRuntimeExit(1);
        }
        vertexOffset += count;

        // The renderer queues at most 64 uploads before they must be recorded.
        if ((key + 1u) % 64u == 0u)
        {
            if (!RendererBeginFrame(renderer, &setup))
            {
                WriteText("generic mesh upload frame could not begin\n");
                LaiueTestRuntimeExit(1);
            }
            RendererBeginScenePass(renderer, 0u);
            if (!RendererEndFrame(renderer))
            {
                WriteText("generic mesh upload frame could not end\n");
                LaiueTestRuntimeExit(1);
            }
        }
    }

    uint64_t checksums[3] = { 0u, 0u, 0u };
    const uint32_t keyCounts[3] = { 1u, 16u, KEY_COUNT };
    bool ok = true;
    for (uint32_t scenario = 0u; scenario < 3u && ok; ++scenario)
        ok = RunScenario(renderer, &setup, meshes, keyCounts[scenario],
            &checksums[scenario]);
    ok = ok && checksums[0] == checksums[1] && checksums[0] == checksums[2];

    RendererDestroy(renderer);
    PlatformFree(meshes);
    PlatformFree(vertices);
    if (!ok)
    {
        WriteText("generic descriptor benchmark verification failed\n");
        LaiueTestRuntimeExit(1);
    }
    WriteText("generic descriptor benchmark checksum=");
    WriteUnsigned(checksums[0]);
    WriteText(" verify=ok\n");
    LAIUE_TEST_SUCCESS();
}
