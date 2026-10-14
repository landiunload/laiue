// Контракт шейдерпаков 2 для D3D12: замена стадии принимается только как
// DXIL. Набор из встроенного DXIL обязан загрузиться целиком, а DXBC и
// повреждённый контейнер — получить отказ, после которого прежний набор
// остаётся рабочим. D3D12 нужен настоящий HWND, поэтому окно создаётся
// голым Win32 и не показывается.

#include "render/content_provider.h"
#include "render/renderer.h"
#include "render/shader_pack.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>

#include "render/generated/d3d12/chunk_ps.h"
#include "render/generated/d3d12/chunk_vs.h"
#include "render/generated/d3d12/panorama_ps.h"
#include "render/generated/d3d12/panorama_vs.h"
#include "render/generated/d3d12/ui_ps.h"
#include "render/generated/d3d12/ui_vs.h"
#endif

#define CONTRACT_WIDTH 160
#define CONTRACT_HEIGHT 90
// Драйвера может не быть: тогда тест сообщает о пропуске, а не о провале.
#define SKIP_EXIT_CODE 125

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("D3D12 shader contract check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

#if defined(_WIN32)
static HWND CreateHiddenWindow(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"LaiueShaderContractTestWindow";
    WNDCLASSEXW windowClass = {
        .cbSize = sizeof(windowClass),
        .lpfnWndProc = DefWindowProcW,
        .hInstance = instance,
        .lpszClassName = className,
    };
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExW(0, className, L"laiue shader contract", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, CONTRACT_WIDTH, CONTRACT_HEIGHT, NULL,
                           NULL, instance, NULL);
}

static bool ReloadSingle(Renderer *renderer, LaiueShaderSlot slot, const void *bytes,
                         uint32_t sizeBytes)
{
    LaiueShaderSet shaderSet;
    LaiueShaderSetInitialize(&shaderSet);
    Expect(LaiueShaderSetSetOverride(&shaderSet, slot, bytes, sizeBytes),
           "the override could not be recorded");
    return RendererReloadShaderSet(renderer, &shaderSet);
}
#endif

LAIUE_TEST_ENTRY(RendererD3D12ShaderContractTestEntryPoint)
{
    RendererSetContentService(LaiueContentGetStaticServiceV1());
#if !defined(_WIN32)
    LaiueTestRuntimeWrite("The D3D12 shader contract test needs Win32; skipping\n");
    LaiueTestRuntimeExit(SKIP_EXIT_CODE);
#else
    if (!RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        LaiueTestRuntimeWrite("D3D12 is not linked; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    HWND window = CreateHiddenWindow();
    Expect(window != NULL, "the hidden Win32 window could not be created");
    Renderer *renderer = RendererCreateWithBackend(window, CONTRACT_WIDTH, CONTRACT_HEIGHT,
                                                   RENDERER_BACKEND_D3D12);
    if (renderer == NULL)
    {
        DestroyWindow(window);
        LaiueTestRuntimeWrite("No D3D12 adapter with Shader Model 6.0; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    // Мир готов: перезагрузка пересоздаёт и конвейер чанков.
    Expect(RendererPrepareWorld(renderer), "the D3D12 world could not be prepared");

    // Пак contract 2 со всеми стадиями. Встроенный DXIL — заведомо
    // корректный байткод того же контракта.
    LaiueShaderSet fullSet;
    LaiueShaderSetInitialize(&fullSet);
    const struct
    {
        LaiueShaderSlot slot;
        const uint32_t *words;
        uint32_t sizeBytes;
    } stages[] = {
        {LAIUE_SHADER_CHUNK_VERTEX, g_chunk_vs, (uint32_t)sizeof(g_chunk_vs)},
        {LAIUE_SHADER_CHUNK_PIXEL, g_chunk_ps, (uint32_t)sizeof(g_chunk_ps)},
        {LAIUE_SHADER_PANORAMA_VERTEX, g_panorama_vs, (uint32_t)sizeof(g_panorama_vs)},
        {LAIUE_SHADER_PANORAMA_PIXEL, g_panorama_ps, (uint32_t)sizeof(g_panorama_ps)},
        {LAIUE_SHADER_UI_VERTEX, g_ui_vs, (uint32_t)sizeof(g_ui_vs)},
        {LAIUE_SHADER_UI_PIXEL, g_ui_ps, (uint32_t)sizeof(g_ui_ps)},
    };
    for (size_t index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index)
        Expect(LaiueShaderSetSetOverride(&fullSet, stages[index].slot, stages[index].words,
                                         stages[index].sizeBytes),
               "a DXIL stage could not be recorded");
    Expect(RendererReloadShaderSet(renderer, &fullSet),
           "a complete DXIL shader set must load");

    // DXBC Shader Model 5: валидный контейнер, но программа в части SHEX.
    static const uint32_t dxbc[12] = {
        0x43425844u, 0u, 0u, 0u, 0u, 0x00000001u, 48u, 1u, 36u, 0x58454853u, 4u, 0u,
    };
    Expect(!ReloadSingle(renderer, LAIUE_SHADER_UI_VERTEX, dxbc, (uint32_t)sizeof(dxbc)),
           "D3D12 must reject DXBC under contract 2");

    // Обрезанный DXIL: размер в заголовке контейнера больше переданного.
    Expect(!ReloadSingle(renderer, LAIUE_SHADER_UI_PIXEL, g_ui_ps,
                         (uint32_t)sizeof(g_ui_ps) - 4u),
           "D3D12 must reject a truncated DXIL container");

    // SPIR-V — байткод другого бэкенда.
    static const uint32_t spirv[8] = {0x07230203u, 0x00010300u, 0u, 8u, 0u, 0u, 0u, 0u};
    Expect(!ReloadSingle(renderer, LAIUE_SHADER_UI_VERTEX, spirv, (uint32_t)sizeof(spirv)),
           "D3D12 must reject SPIR-V");

    Expect(RendererReloadShaderSet(renderer, NULL),
           "the embedded set must stay loadable after rejected overrides");

    RendererDestroy(renderer);
    DestroyWindow(window);
    LaiueTestRuntimeWrite("D3D12 shader contract checks passed\n");
    LAIUE_TEST_SUCCESS();
#endif
}
