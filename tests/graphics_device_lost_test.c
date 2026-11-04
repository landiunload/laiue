// Потеря устройства на D3D12 через службу графики. ID3D12Device5::RemoveDevice
// снимает устройство только в этом процессе, как сброс драйвера. Устройство на
// адаптер одно, поэтому D3D12CreateDevice отдаёт то же, на котором работает
// модуль. Новое устройство заработает, только если старое отпущено целиком:
// иначе вернётся снятое. Окно голое Win32 и не показывается.

#include "graphics/graphics_device_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FRAME_WIDTH 64u
#define FRAME_HEIGHT 64u
// Снятие доходит до устройства не сразу: столько кадров ему даётся.
#define FRAMES_TO_LOSS 8u
// Драйвера может не быть: тогда тест сообщает о пропуске, а не о провале.
#define SKIP_EXIT_CODE 125

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Graphics device loss check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t *output, const wchar_t *directory, const wchar_t *name)
{
    uint32_t length = 0u;
    while (*directory != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[length++] = *directory++;
    if (*directory != L'\0')
        return false;
    if (length != 0u && output[length - 1u] != L'/' && output[length - 1u] != L'\\')
        output[length++] = L'/';
    while (*name != L'\0' && length + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[length++] = *name++;
    if (*name != L'\0')
        return false;
    output[length] = L'\0';
    return true;
}

static HWND CreateHiddenWindow(void)
{
    const wchar_t *name = L"LaiueGraphicsDeviceLostTestWindow";
    HINSTANCE module = GetModuleHandleW(NULL);
    WNDCLASSEXW windowClass = {
        .cbSize = sizeof(windowClass),
        .lpfnWndProc = DefWindowProcW,
        .hInstance = module,
        .lpszClassName = name,
    };
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExW(0u, name, L"laiue device lost", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, (int)FRAME_WIDTH, (int)FRAME_HEIGHT, NULL, NULL, module,
                           NULL);
}

// Адаптер выбирается как в рендерере: по умолчанию, при неудаче программный WARP.
static ID3D12Device5 *ModuleDevice(void)
{
    ID3D12Device5 *device = NULL;
    if (SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device5,
                                    (void **)&device)))
        return device;
    IDXGIFactory4 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0u, &IID_IDXGIFactory4, (void **)&factory)))
        return NULL;
    IDXGIAdapter *warp = NULL;
    if (SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void **)&warp)))
    {
        if (FAILED(D3D12CreateDevice((IUnknown *)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device5,
                                     (void **)&device)))
            device = NULL;
        IDXGIAdapter_Release(warp);
    }
    IDXGIFactory4_Release(factory);
    return device;
}

static bool Lost(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsDiagnosticsV2 diagnostics = {.structSize = sizeof(diagnostics)};
    Expect(device->getDiagnostics(device, &diagnostics) != 0u, "diagnostics are available");
    return (diagnostics.flags & LAIUE_GRAPHICS_DIAGNOSTICS_DEVICE_LOST) != 0u;
}

static bool DrawFrame(LaiueGraphicsDeviceV2 *device)
{
    LaiueGraphicsCameraV2 camera = {.structSize = sizeof(camera)};
    camera.viewProjection[0] = camera.viewProjection[5] = 1.0f;
    camera.viewProjection[10] = camera.viewProjection[15] = 1.0f;
    return device->setCamera(device, &camera) != 0u &&
           device->beginFrame(device, FRAME_WIDTH, FRAME_HEIGHT) != 0u &&
           device->endFrame(device) != 0u;
}

static bool DrawsOn(LaiueGraphicsDeviceV2 *device)
{
    for (uint32_t frame = 0u; frame < FRAMES_TO_LOSS; ++frame)
        if (!DrawFrame(device))
            return false;
    return true;
}

LAIUE_TEST_ENTRY(GraphicsDeviceLostTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) &&
               Join(modulePath, directory, L"laiue_render.dll"),
           "renderer module path is available");
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic moduleDiagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &moduleDiagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &moduleDiagnostic) == LAIUE_MODULE_OK,
           moduleDiagnostic.message);
    const LaiueGraphicsDeviceServiceV2 *service =
        (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_2,
            LAIUE_GRAPHICS_DEVICE_SERVICE_V2_CONTEXT_SIZE, NULL, NULL);
    Expect(service != NULL && service->createDeviceWithContext != NULL,
           "V2 device service is available");
    const HWND window = CreateHiddenWindow();
    Expect(window != NULL, "a hidden window creates");

    LaiueGraphicsDeviceV2 *device = NULL;
    if (service->createDeviceWithContext(service->context, window, FRAME_WIDTH, FRAME_HEIGHT,
                                         LAIUE_GRAPHICS_BACKEND_D3D12, &device) == 0u)
    {
        LaiueTestRuntimeWrite("No D3D12 adapter with Shader Model 6.0; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    Expect(device != NULL && device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_DIAGNOSTICS_SIZE &&
               device->getDiagnostics != NULL,
           "device publishes diagnostics");
    Expect(DrawFrame(device) && !Lost(device), "a new device draws and is not lost");

    ID3D12Device5 *native = ModuleDevice();
    if (native == NULL)
    {
        LaiueTestRuntimeWrite("This system cannot remove a device; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    ID3D12Device5_RemoveDevice(native);
    ID3D12Device5_Release(native);
    if (!Lost(device) && DrawsOn(device))
    {
        LaiueTestRuntimeWrite("The module runs on another adapter; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    Expect(Lost(device), "a removed device reports the loss");
    Expect(!DrawFrame(device) && Lost(device), "a lost device stays lost and draws nothing");
    service->destroyDevice(device);

    device = NULL;
    Expect(service->createDeviceWithContext(service->context, window, FRAME_WIDTH, FRAME_HEIGHT,
                                            LAIUE_GRAPHICS_BACKEND_D3D12, &device) != 0u &&
               device != NULL,
           "a new device replaces the lost one on the same window");
    Expect(DrawsOn(device) && !Lost(device), "the replacement draws and is not lost");
    service->destroyDevice(device);
    DestroyWindow(window);
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
