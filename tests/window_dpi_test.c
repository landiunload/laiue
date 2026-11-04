// Окно Windows рисуется настоящими пикселями и только там, где их видно. Экран с
// масштабом здесь не нужен: осведомлённость о DPI видна у самого окна, а сообщение
// о смене монитора тест присылает сам. Свёрнутое окно сообщает размер 0x0, и эти
// сообщения тоже присылает тест: ShowWindow показал бы окно там, где оно создано,
// а тесты не выводят окон на основной монитор.

#include "mod/module_host.h"
#include "platform/system.h"
#include "platform/window_service.h"
#include "test_runtime.h"

#include <windows.h>

#include <dwmapi.h>

#include <stdbool.h>
#include <stdint.h>

#define WINDOW_WIDTH 160
#define WINDOW_HEIGHT 120
// Монитор с масштабом 200 %, на который окно будто бы перешло.
#define MOVED_DPI 192u
// Без сна между кадрами цикл успел бы за это время сотни тысяч кадров.
#define PACE_SECONDS 0.2
#define PACE_MOST_FRAMES 40u
// Цикл, который спит между кадрами, так часто кадры не выдаёт. Счёт идёт по таким
// кадрам, а не по времени: на занятой машине за любой срок их может быть мало.
#define QUICK_FRAME_SECONDS 0.025
#define QUICK_PATIENCE_SECONDS 10.0
// Пусковая программа тестов показывает окно не сразу.
#define SHOW_PATIENCE_SECONDS 3.0

// Этапы идут в одном окне и одном цикле: скрытым пусковая программа запускает
// только первое окно процесса, а второе система показала бы на основном мониторе.
typedef enum PaceStage
{
    PACE_WAIT_FOR_SHOW,
    PACE_WITHOUT_PIXELS,
    PACE_ON_ANOTHER_DESKTOP,
    PACE_OUT_OF_FOCUS,
    PACE_EFFICIENCY,
    PACE_DONE,
} PaceStage;

typedef struct FramePace
{
    const LaiueWindowServiceV1 *windows;
    Window *window;
    HWND handle;
    PaceStage stage;
    double stageEnd;
    double lastFrame;
    bool shown;
    bool cloaked;
    bool foreground;
    bool pixelsWhileMinimized;
    bool resizedWhileMinimized;
    bool resizedByRestore;
    uint32_t frames[PACE_DONE];
    uint32_t quickFrames;
    DWORD priorities[PACE_DONE];
} FramePace;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Window DPI check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Cloak(FramePace *pace, BOOL cloak)
{
    pace->cloaked =
        SUCCEEDED(DwmSetWindowAttribute(pace->handle, DWMWA_CLOAK, &cloak, sizeof(cloak)));
}

static void NextStage(FramePace *pace)
{
    pace->priorities[pace->stage] = GetPriorityClass(GetCurrentProcess());
    pace->foreground = pace->foreground || GetForegroundWindow() == pace->handle;
    pace->stage = (PaceStage)(pace->stage + 1);
    switch (pace->stage)
    {
        case PACE_WITHOUT_PIXELS:
            // Только показанное окно: пусковая программа уже поставила его на место.
            pace->shown = IsWindowVisible(pace->handle) != FALSE;
            if (pace->shown)
            {
                (void)pace->windows->consumeResize(pace->window);
                ShowWindow(pace->handle, SW_SHOWMINNOACTIVE);
                // Так опаздывает сообщение, когда окно показывал другой поток.
                SendMessageW(pace->handle, WM_SIZE, SIZE_RESTORED,
                             MAKELPARAM(WINDOW_WIDTH, WINDOW_HEIGHT));
            }
            break;
        case PACE_ON_ANOTHER_DESKTOP:
            Cloak(pace, TRUE);
            if (pace->shown)
            {
                pace->resizedWhileMinimized = pace->windows->consumeResize(pace->window);
                ShowWindow(pace->handle, SW_SHOWNOACTIVATE);
                pace->resizedByRestore = pace->windows->consumeResize(pace->window);
            }
            break;
        case PACE_OUT_OF_FOCUS:
        {
            const bool cloaked = pace->cloaked;
            Cloak(pace, FALSE);
            pace->cloaked = cloaked;
            break;
        }
        case PACE_EFFICIENCY:
            (void)pace->windows->setPowerPolicy(WINDOW_POWER_EFFICIENCY);
            break;
        default:
            (void)pace->windows->setPowerPolicy(WINDOW_POWER_AUTO);
            pace->windows->requestClose(pace->window);
            break;
    }
    // Срок считается от конца перехода: показ окна сам может занять весь этап.
    pace->stageEnd = PlatformMonotonicSeconds() +
                     (pace->stage == PACE_OUT_OF_FOCUS && pace->shown ? QUICK_PATIENCE_SECONDS
                                                                      : PACE_SECONDS);
}

static void CountFrame(void *context)
{
    FramePace *pace = (FramePace *)context;
    const double now = PlatformMonotonicSeconds();
    if (pace->stage != PACE_DONE &&
        (now >= pace->stageEnd ||
         (pace->stage == PACE_WAIT_FOR_SHOW && IsWindowVisible(pace->handle)) ||
         (pace->stage == PACE_OUT_OF_FOCUS && pace->quickFrames > PACE_MOST_FRAMES)))
        NextStage(pace);
    if (pace->stage == PACE_DONE)
        return;
    // Активным окно считается только свёрнутым: обычному DefWindowProc отдал бы
    // фокус, а тесты его не забирают.
    const bool active = pace->stage == PACE_WITHOUT_PIXELS && pace->shown;
    if (active)
    {
        int32_t width = 0;
        int32_t height = 0;
        pace->windows->getClientSize(pace->window, &width, &height);
        pace->pixelsWhileMinimized = pace->pixelsWhileMinimized || width != 0 || height != 0;
    }
    SendMessageW(pace->handle, WM_ACTIVATE, MAKEWPARAM(active ? WA_ACTIVE : WA_INACTIVE, active),
                 0);
    ++pace->frames[pace->stage];
    if (pace->stage == PACE_OUT_OF_FOCUS && now - pace->lastFrame < QUICK_FRAME_SECONDS)
        ++pace->quickFrames;
    pace->lastFrame = now;
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
        LAIUE_WINDOW_SERVICE_V1_POWER_SIZE, &version, &size);
    Expect(windows != NULL && windows->getContentScale != NULL && windows->setPowerPolicy != NULL,
           "the window service reports the content scale");

    const DWORD priority = GetPriorityClass(GetCurrentProcess());
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

    SendMessageW(handle, WM_SIZE, SIZE_MINIMIZED, 0);
    windows->getClientSize(window, &width, &height);
    Expect(width == client.right && height == client.bottom,
           "a late size message does not take the pixels of a window");

    // Этапы опускают приоритет процесса, и занятая машина держала бы тест десятки
    // секунд: поток окна сохраняет свой.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    FramePace pace = {.windows = windows,
                      .window = window,
                      .handle = handle,
                      .stageEnd = PlatformMonotonicSeconds() + SHOW_PATIENCE_SECONDS};
    windows->runLoop(window, CountFrame, &pace);
    Expect(pace.frames[PACE_WITHOUT_PIXELS] != 0u &&
               pace.frames[PACE_WITHOUT_PIXELS] <= PACE_MOST_FRAMES,
           "an active window without pixels sleeps between frames");
    Expect(!pace.pixelsWhileMinimized, "a minimized window has nothing to draw on");
    Expect(!pace.resizedWhileMinimized, "minimizing is not a resize");
    Expect(!pace.shown || pace.resizedByRestore, "the program hears of the restored size");
    Expect(!pace.shown || pace.quickFrames > PACE_MOST_FRAMES,
           "a visible window out of focus keeps its pace");
    Expect(!pace.shown || pace.priorities[PACE_OUT_OF_FOCUS] == priority,
           "a program that can be seen keeps its priority out of focus");
    Expect(!pace.cloaked || pace.frames[PACE_ON_ANOTHER_DESKTOP] <= PACE_MOST_FRAMES,
           "a window on another desktop sleeps between frames");
    Expect(!pace.cloaked || pace.foreground ||
               pace.priorities[PACE_ON_ANOTHER_DESKTOP] == IDLE_PRIORITY_CLASS,
           "a program nobody can see goes into efficiency mode");
    Expect(pace.frames[PACE_EFFICIENCY] <= PACE_MOST_FRAMES,
           "efficiency mode spares frames out of focus");

    windows->destroy(window);
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
