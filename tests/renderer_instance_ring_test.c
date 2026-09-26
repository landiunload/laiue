// Инстансное кольцо D3D12: адаптивный чанковый пул обязан не терять
// инстансные вызовы, переживать смену кадрового слота и resize.
//
// Проверка идёт по счётчикам RendererStats, а не по пикселям: старый
// фиксированный бюджет 16 МиБ на кадр молча отбрасывал бы вызовы сверх
// лимита (drawCalls меньше фактического числа вызовов), а чанковый пул
// обязан отрисовать все. D3D12 нужен настоящий HWND, поэтому окно создаётся
// голым Win32 и не показывается.

#include "render/chunk_geometry.h"
#include "render/content_provider.h"
#include "render/renderer.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define RING_WIDTH 320
#define RING_HEIGHT 180
// Драйвера может не быть: тогда тест сообщает о пропуске, а не о провале.
#define SKIP_EXIT_CODE 125

#define SMALL_INSTANCES 2048u
#define BIG_INSTANCES_PER_DRAW 200000u
#define BIG_DRAWS_PER_FRAME 4u
#define BIG_FRAMES 6u
// 4 x 200000 x 32 Б = 25.6 МиБ за кадр — заметно выше прежнего лимита 16 МиБ.
#define BIG_FRAME_BYTES                                                                            \
    ((uint64_t)BIG_DRAWS_PER_FRAME * BIG_INSTANCES_PER_DRAW *                                      \
     (uint64_t)sizeof(RendererMeshInstance))

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Instance ring check failed: ");
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

static void SetIdentity(float matrix[16])
{
    for (uint32_t index = 0; index < 16u; ++index)
        matrix[index] = 0.0f;
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static void BuildFrameSetup(RendererFrameSetup *setup)
{
    ZeroBytes(setup, sizeof(*setup));
    setup->gamma = 1.0f;
    setup->skyColor[0] = 0.0f;
    setup->skyColor[1] = 0.0f;
    setup->skyColor[2] = 0.0f;
    setup->sunDirection[0] = 0.0f;
    setup->sunDirection[1] = 0.0f;
    setup->sunDirection[2] = -1.0f;
    setup->sunColor[0] = 1.0f;
    setup->sunColor[1] = 1.0f;
    setup->sunColor[2] = 1.0f;
    setup->ambientColor[0] = 1.0f;
    setup->ambientColor[1] = 1.0f;
    setup->ambientColor[2] = 1.0f;
    setup->passCount = 1u;
    SetIdentity(setup->passes[0].viewProjection);
    setup->passes[0].rectMaxX = RING_WIDTH;
    setup->passes[0].rectMaxY = RING_HEIGHT;
}

static ChunkQuad MakeQuad(void)
{
    return PackChunkQuad(0u, 0u, 0u, 4u, 1u, 1u, 1u, 1u);
}

#if defined(_WIN32)
static HWND CreateHiddenWindow(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"LaiueInstanceRingTestWindow";

    WNDCLASSEXW windowClass;
    ZeroBytes(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return NULL;
    }

    return CreateWindowExW(0, className, L"laiue instance ring", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, RING_WIDTH, RING_HEIGHT, NULL, NULL, instance, NULL);
}

static void FillInstances(RendererMeshInstance *instances, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        instances[index].originRelative[0] = (float)(index % 32u) * 0.01f;
        instances[index].originRelative[1] = (float)(index / 32u) * 0.01f;
        instances[index].originRelative[2] = 0.0f;
        instances[index].scale = 0.001f;
        instances[index].rotation[0] = 0.0f;
        instances[index].rotation[1] = 0.0f;
        instances[index].rotation[2] = 0.0f;
        instances[index].rotation[3] = 0.0f;
    }
}

// Возвращает true, если счётчики кадра совпали с ожиданием.
static bool RunFrame(Renderer *renderer, const RendererMesh *mesh,
                     const RendererMeshInstance *instances, const RendererFrameSetup *setup,
                     uint32_t draws, uint32_t instancesPerDraw, uint64_t expectedQuads,
                     const char *reason)
{
    Expect(RendererBeginFrame(renderer, setup), reason);
    RendererBeginScenePass(renderer, 0u);
    for (uint32_t draw = 0; draw < draws; ++draw)
    {
        RendererDrawMeshInstances(renderer, mesh, instances, instancesPerDraw);
    }
    Expect(RendererEndFrame(renderer), "the instance ring frame could not end");

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    return stats.drawCalls == draws && stats.drawnQuads == expectedQuads;
}

// Чередование обычного и инстансного вызова в одном кадре: смещение
// инстанса — общее состояние кадра, обычный вызов обязан его перезаписать,
// а следующий инстансный — восстановить. Проверяются счётчики кадра:
// 1 + 3 + 1 + 2 = 7 квадов за четыре вызова.
static bool RunMixedFrame(Renderer *renderer, const RendererMesh *mesh,
                          const RendererMeshInstance *instances,
                          const RendererFrameSetup *setup)
{
    Expect(RendererBeginFrame(renderer, setup), "the mixed draw frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    RendererDrawMesh(renderer, mesh, origin);
    RendererDrawMeshInstances(renderer, mesh, instances, 3u);
    RendererDrawMesh(renderer, mesh, origin);
    RendererDrawMeshInstances(renderer, mesh, instances, 2u);
    Expect(RendererEndFrame(renderer), "the mixed draw frame could not end");

    RendererStats stats;
    RendererGetStats(renderer, &stats);
    return stats.drawCalls == 4u && stats.drawnQuads == 7u;
}
#endif

LAIUE_TEST_ENTRY(RendererInstanceRingTestEntryPoint)
{
    RendererSetContentService(LaiueContentGetStaticServiceV1());
#if !defined(_WIN32)
    LaiueTestRuntimeWrite("The instance ring test needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        LaiueTestRuntimeWrite("D3D12 is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    HWND window = CreateHiddenWindow();
    Expect(window != NULL, "the hidden Win32 window could not be created");

    Renderer *renderer =
        RendererCreateWithBackend(window, RING_WIDTH, RING_HEIGHT, RENDERER_BACKEND_D3D12);
    if (renderer == NULL)
    {
        LaiueTestRuntimeWrite("No D3D12 adapter available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    RendererSetVerticalSync(renderer, false);
    Expect(RendererPrepareWorld(renderer), "the D3D12 world could not be prepared");

    const ChunkQuad quad = MakeQuad();
    RendererMesh *mesh = RendererCreateMesh(renderer, &quad, 1u);
    Expect(mesh != NULL, "the instanced mesh could not be created");

    // The backend must also execute the backend-neutral generic vertex path;
    // this deliberately bypasses ChunkQuad and only checks GPU submission
    // counters because the hidden D3D12 window has no readback contract.
    const RendererGenericVertex genericVertices[3] = {
        { { -0.25f, -0.25f, 0.0f }, { 0.0f, 0.0f }, 0xFFFFFFFFu },
        { {  0.25f, -0.25f, 0.0f }, { 1.0f, 0.0f }, 0xFFFFFFFFu },
        { {  0.00f,  0.25f, 0.0f }, { 0.5f, 1.0f }, 0xFFFFFFFFu },
    };
    RendererMesh *genericMesh = RendererCreateGenericMesh(renderer,
                                                           genericVertices, 3u);
    Expect(genericMesh != NULL, "the generic D3D12 mesh could not be created");
    RendererFrameSetup genericSetup;
    BuildFrameSetup(&genericSetup);
    Expect(RendererBeginFrame(renderer, &genericSetup),
           "the generic D3D12 frame could not begin");
    RendererBeginScenePass(renderer, 0u);
    RendererDrawGenericMeshRange(renderer, genericMesh, NULL, 1.0f, 0u, 3u);
    Expect(RendererEndFrame(renderer), "the generic D3D12 frame could not end");
    RendererStats genericStats;
    RendererGetStats(renderer, &genericStats);
    Expect(genericStats.drawCalls == 1u && genericStats.drawnQuads == 1u,
           "the generic D3D12 mesh must reach the GPU draw path");
    RendererDestroyMesh(renderer, genericMesh);

    const uint32_t capacity = BIG_INSTANCES_PER_DRAW;
    RendererMeshInstance *instances = (RendererMeshInstance *)PlatformAllocate(
        (size_t)capacity * sizeof(RendererMeshInstance), true);
    Expect(instances != NULL, "the instance array could not be allocated");
    FillInstances(instances, capacity);

    RendererFrameSetup setup;
    BuildFrameSetup(&setup);

    // 1. Малая нагрузка: один вызов рисуется целиком.
    Expect(RunFrame(renderer, mesh, instances, &setup, 1u, SMALL_INSTANCES,
                    (uint64_t)SMALL_INSTANCES, "the small instanced frame could not begin"),
           "the small frame lost its only instanced draw");

    // 2. Смена кадрового слота. Шесть больших кадров подряд: каждый обязан
    //    отрисовать все вызовы, а курсор чанков — корректно сбрасываться
    //    при переходе между слотами.
    Expect(BIG_FRAME_BYTES > 16u * 1024u * 1024u,
           "the big frame must exceed the former 16 MiB per-frame budget");
    for (uint32_t frame = 0; frame < BIG_FRAMES; ++frame)
    {
        Expect(RunFrame(renderer, mesh, instances, &setup, BIG_DRAWS_PER_FRAME,
                        BIG_INSTANCES_PER_DRAW,
                        (uint64_t)BIG_DRAWS_PER_FRAME * BIG_INSTANCES_PER_DRAW,
                        "a big instanced frame could not begin"),
               "a big instanced frame dropped a caller draw");
    }

    // 3. Resize не должен ломать пул: за ним снова идёт большой кадр.
    RendererResize(renderer, RING_WIDTH / 2, RING_HEIGHT / 2);
    Expect(RunFrame(renderer, mesh, instances, &setup, BIG_DRAWS_PER_FRAME, BIG_INSTANCES_PER_DRAW,
                    (uint64_t)BIG_DRAWS_PER_FRAME * BIG_INSTANCES_PER_DRAW,
                    "an instanced frame after resize could not begin"),
           "an instanced frame after resize dropped a caller draw");

    RendererResize(renderer, RING_WIDTH, RING_HEIGHT);
    Expect(RunFrame(renderer, mesh, instances, &setup, 1u, SMALL_INSTANCES,
                    (uint64_t)SMALL_INSTANCES,
                    "an instanced frame after resize back could not begin"),
           "the small frame after resize back lost its draw");

    // 4. Смешение обычных и инстансных вызовов в одном кадре.
    Expect(RunMixedFrame(renderer, mesh, instances, &setup),
           "the mixed plain/instanced frame lost a call");

    // 5. Release: меш и рендерер (со всеми чанками) обязаны освободиться
    //    без утечек и падений.
    RendererDestroyMesh(renderer, mesh);
    RendererDestroy(renderer);
    PlatformFree(instances);
    DestroyWindow(window);
    LaiueTestRuntimeWrite("Instance ring checks passed\n");
    LAIUE_TEST_SUCCESS();
#endif
}
