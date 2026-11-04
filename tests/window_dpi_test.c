// Окно Windows рисуется настоящими пикселями. Экран с масштабом здесь не нужен:
// осведомлённость о DPI видна у самого окна, а сообщение о смене монитора тест
// присылает сам.

#include "mod/module_host.h"
#include "platform/system.h"
#include "platform/window_service.h"
#include "test_runtime.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#define WINDOW_WIDTH 160
#define WINDOW_HEIGHT 120
// Монитор с масштабом 200 %, на который окно будто бы перешло.
#define MOVED_DPI 192u

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Window DPI check failed: ");
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

LAIUE_TEST_ENTRY(WindowDpiTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) &&
               Join(modulePath, directory, L"laiue_window.dll"),
           "the module path is known");
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "the module host creates");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueWindowServiceV1 *windows = (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
        LAIUE_WINDOW_SERVICE_V1_SCALE_SIZE, &version, &size);
    Expect(windows != NULL && windows->getContentScale != NULL,
           "the window service reports the content scale");

    const WindowConfiguration configuration = {
        .title = L"laiue window dpi test", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
    Window *window = windows->create(&configuration);
    Expect(window != NULL, "a window creates");
    const HWND handle = (HWND)windows->getNativeHandle(window);
    Expect(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(handle),
                                        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
           "the window follows the DPI of its monitor");
    const UINT dpi = GetDpiForWindow(handle);
    Expect(windows->getContentScale(window) == (float)dpi / (float)USER_DEFAULT_SCREEN_DPI,
           "the content scale is the window's DPI");
    int32_t width = 0;
    int32_t height = 0;
    windows->getClientSize(window, &width, &height);
    Expect(width == MulDiv(WINDOW_WIDTH, (int)dpi, USER_DEFAULT_SCREEN_DPI) &&
               height == MulDiv(WINDOW_HEIGHT, (int)dpi, USER_DEFAULT_SCREEN_DPI),
           "the requested size counts in points");

    RECT before;
    Expect(GetWindowRect(handle, &before), "the window has a rectangle");
    const RECT suggested = {before.left + 7, before.top + 5,
                            before.left + 7 + 2 * (before.right - before.left),
                            before.top + 5 + 2 * (before.bottom - before.top)};
    SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(MOVED_DPI, MOVED_DPI), (LPARAM)&suggested);
    RECT after;
    Expect(GetWindowRect(handle, &after) && EqualRect(&after, &suggested),
           "a window on another monitor takes the suggested rectangle");
    RECT client;
    Expect(GetClientRect(handle, &client), "the window has a client area");
    windows->getClientSize(window, &width, &height);
    Expect(width == client.right && height == client.bottom,
           "the new client size reaches the program");

    windows->destroy(window);
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
