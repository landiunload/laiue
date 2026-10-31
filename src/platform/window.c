#include "platform/window.h"
#include "platform/efficiency_windows.h"

#include <windows.h>

#define WINDOW_CLASS_NAME L"LaiueWindowClass"

#define WINDOW_TEXT_CAPACITY 256u

struct Window
{
    HWND handle;
    int32_t clientWidth;
    int32_t clientHeight;
    bool resizePending;
    bool focusLossPending;
    bool mouseLookEnabled;
    bool cursorHidden;
    bool focused;
    float wheelSteps;             // накопленные щелчки колеса до Consume
    // Набранный текст до Consume: кольцо кодовых точек и старшая половина
    // суррогатной пары, ждущая младшую.
    uint32_t text[WINDOW_TEXT_CAPACITY];
    uint32_t textHead;
    uint32_t textCount;
    uint32_t highSurrogate;
    bool fullscreen;
    WINDOWPLACEMENT windowedPlacement;   // положение окна до фуллскрина
    LONG windowedStyle;                  // стиль окна до фуллскрина
    RawInputCallback rawInputCallback;
    void* rawInputUserData;
};

// Политика энергопотребления процесса (WINDOW_POWER_*) и окна модуля, ещё не
// получившие WM_DESTROY: без окон программе нечему быть в фоне. Политику
// применяют под замком, чтобы WM_ACTIVATEAPP и смена политики из другого
// потока не оставили процесс в режиме прежней.
static SRWLOCK g_powerLock = SRWLOCK_INIT;
static uint32_t g_powerPolicy = WINDOW_POWER_AUTO;
static uint32_t g_windowCount;

static bool ProcessIsForeground(void)
{
    DWORD processId = 0;
    const HWND foreground = GetForegroundWindow();
    return foreground != NULL && GetWindowThreadProcessId(foreground, &processId) != 0
        && processId == GetCurrentProcessId();
}

// Вызывается под g_powerLock.
static bool ApplyPowerPolicyLocked(bool foreground)
{
    PlatformPowerMode mode = PLATFORM_POWER_DEFAULT;
    if (g_powerPolicy == WINDOW_POWER_PERFORMANCE)
    {
        mode = PLATFORM_POWER_PERFORMANCE;
    }
    else if (g_powerPolicy == WINDOW_POWER_EFFICIENCY || (g_windowCount != 0u && !foreground))
    {
        mode = PLATFORM_POWER_EFFICIENCY;
    }
    return PlatformPowerModeSet(mode);
}

// Ограничивает курсор клиентской областью окна (в экранных координатах).
static void ApplyCursorClip(const Window* window)
{
    RECT clientRect;
    if (!GetClientRect(window->handle, &clientRect))
    {
        return;
    }

    POINT topLeft     = { clientRect.left,  clientRect.top };
    POINT bottomRight = { clientRect.right, clientRect.bottom };
    ClientToScreen(window->handle, &topLeft);
    ClientToScreen(window->handle, &bottomRight);

    RECT screenRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    ClipCursor(&screenRect);
}

// Приводит видимость и захват курсора в соответствие с режимом mouse look.
// Вызывается каждую итерацию цикла — реагирует и на потерю фокуса.
static void UpdateMouseLookState(Window* window)
{
    bool shouldHideCursor = window->mouseLookEnabled && window->focused;
    if (shouldHideCursor == window->cursorHidden)
    {
        return;
    }

    window->cursorHidden = shouldHideCursor;

    if (shouldHideCursor)
    {
        while (ShowCursor(FALSE) >= 0) {}
        ApplyCursorClip(window);
    }
    else
    {
        while (ShowCursor(TRUE) < 0) {}
        ClipCursor(NULL);
    }
}

static LRESULT CALLBACK WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
{
    Window* window = (Window*)GetWindowLongPtrW(handle, GWLP_USERDATA);

    switch (message)
    {
        case WM_NCCREATE:
        {
            // CreateWindowExW передаёт сюда указатель на Window —
            // привязываем его к HWND, дальше достаём через GWLP_USERDATA.
            const CREATESTRUCTW* createStruct = (const CREATESTRUCTW*)lParam;
            SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)createStruct->lpCreateParams);
            return DefWindowProcW(handle, message, wParam, lParam);
        }

        case WM_SIZE:
            // Сворачивание даёт клиентскую область 0x0 — это не resize.
            if (window != NULL && wParam != SIZE_MINIMIZED)
            {
                window->clientWidth   = (int32_t)LOWORD(lParam);
                window->clientHeight  = (int32_t)HIWORD(lParam);
                window->resizePending = true;

                if (window->cursorHidden)
                {
                    ApplyCursorClip(window);
                }
            }
            return 0;

        case WM_CHAR:
            if (window != NULL)
            {
                const uint32_t unit = (uint32_t)wParam;
                uint32_t codepoint = unit;
                if (unit >= 0xD800u && unit < 0xDC00u)
                {
                    window->highSurrogate = unit;
                    return 0;
                }
                if (unit >= 0xDC00u && unit < 0xE000u)
                {
                    codepoint = window->highSurrogate != 0u
                        ? 0x10000u + ((window->highSurrogate - 0xD800u) << 10u)
                              + (unit - 0xDC00u)
                        : 0u;
                }
                window->highSurrogate = 0u;
                // Управляющие символы приходят клавишами, не текстом.
                if (codepoint >= 0x20u && codepoint != 0x7Fu
                    && window->textCount < WINDOW_TEXT_CAPACITY)
                {
                    const uint32_t slot =
                        (window->textHead + window->textCount) % WINDOW_TEXT_CAPACITY;
                    window->text[slot] = codepoint;
                    window->textCount++;
                }
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (window != NULL)
            {
                window->wheelSteps +=
                    (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
            }
            return 0;

        case WM_MOVE:
            // Область захвата курсора задана в экранных координатах —
            // при перемещении окна её нужно пересчитать.
            if (window != NULL && window->cursorHidden)
            {
                ApplyCursorClip(window);
            }
            return 0;

        case WM_INPUT:
            if (window != NULL && window->rawInputCallback != NULL)
            {
                window->rawInputCallback(window->rawInputUserData, (void*)lParam);
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_ACTIVATEAPP:
            // Программа ушла в фон или вернулась: при WINDOW_POWER_AUTO
            // процесс входит в режим эффективности, как по переключателю
            // диспетчера задач, и выходит из него вместе с фокусом.
            AcquireSRWLockExclusive(&g_powerLock);
            ApplyPowerPolicyLocked(wParam != FALSE);
            ReleaseSRWLockExclusive(&g_powerLock);
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_ACTIVATE:
            if (window != NULL)
            {
                bool nowFocused = LOWORD(wParam) != WA_INACTIVE;
                if (window->focused && !nowFocused)
                {
                    // Пока окно не в фокусе, raw input не приходит —
                    // потребитель по этой защёлке сбрасывает состояние ввода.
                    window->focusLossPending = true;
                }
                window->focused = nowFocused;
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_DESTROY:
            // WM_QUIT не постится: цикл каждого окна следит только
            // за своим окном, уничтожение одного не завершает остальные.
            // handle == NULL: CreateWindowExW ещё не вернулся и окно не
            // было посчитано.
            if (window != NULL && window->handle != NULL)
            {
                window->handle = NULL;
                // Без окон процесс, который продолжит работу, получает
                // обратно прежние QoS и приоритет.
                AcquireSRWLockExclusive(&g_powerLock);
                --g_windowCount;
                ApplyPowerPolicyLocked(ProcessIsForeground());
                ReleaseSRWLockExclusive(&g_powerLock);
            }
            return 0;

        default:
            return DefWindowProcW(handle, message, wParam, lParam);
    }
}

Window* WindowCreate(const WindowConfiguration* configuration)
{
    if (configuration == NULL)
    {
        return NULL;
    }

    HINSTANCE instance = GetModuleHandleW(NULL);

    WNDCLASSEXW windowClass = {
        .cbSize        = sizeof(windowClass),
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = WindowProcedure,
        .hInstance     = instance,
        .hIcon         = LoadIconW(NULL, IDI_APPLICATION),
        .hCursor       = LoadCursorW(NULL, IDC_ARROW),
        .lpszClassName = WINDOW_CLASS_NAME,
    };

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return NULL;
    }

    Window* window = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*window));
    if (window == NULL)
    {
        return NULL;
    }

    window->clientWidth  = configuration->width;
    window->clientHeight = configuration->height;

    // Размер окна подбирается так, чтобы клиентская область
    // совпала с запрошенными width x height.
    RECT windowRect = { 0, 0, configuration->width, configuration->height };
    const DWORD windowStyle         = WS_OVERLAPPEDWINDOW;
    const DWORD windowStyleExtended = WS_EX_APPWINDOW;
    AdjustWindowRectEx(&windowRect, windowStyle, FALSE, windowStyleExtended);

    window->handle = CreateWindowExW(
        windowStyleExtended,
        WINDOW_CLASS_NAME,
        configuration->title,
        windowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL,
        NULL,
        instance,
        window
    );

    if (window->handle == NULL)
    {
        HeapFree(GetProcessHeap(), 0, window);
        return NULL;
    }

    AcquireSRWLockExclusive(&g_powerLock);
    ++g_windowCount;
    ReleaseSRWLockExclusive(&g_powerLock);

    ShowWindow(window->handle, SW_SHOWDEFAULT);
    UpdateWindow(window->handle);

    // Окно, показанное без фокуса, не получит WM_ACTIVATEAPP, пока его не
    // выберут: политику применяет само создание.
    AcquireSRWLockExclusive(&g_powerLock);
    ApplyPowerPolicyLocked(ProcessIsForeground());
    ReleaseSRWLockExclusive(&g_powerLock);

    return window;
}

void WindowDestroy(Window* window)
{
    if (window == NULL)
    {
        return;
    }

    if (window->handle != NULL)
    {
        DestroyWindow(window->handle);
    }

    // Пока живы другие окна класса, вызов не пройдёт (ERROR_CLASS_HAS_WINDOWS)
    // и это нормально; последний уничтоживший — разрегистрирует.
    // Иначе после FreeLibrary в процессе остался бы класс
    // с lpfnWndProc, указывающим в выгруженную DLL.
    UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandleW(NULL));

    HeapFree(GetProcessHeap(), 0, window);
}

void* WindowGetNativeHandle(const Window* window)
{
    return window->handle;
}

void WindowSetRawInputCallback(Window* window, RawInputCallback callback, void* userData)
{
    window->rawInputCallback = callback;
    window->rawInputUserData = userData;
}

void WindowGetClientSize(const Window* window, int32_t* width, int32_t* height)
{
    *width  = window->clientWidth;
    *height = window->clientHeight;
}

bool WindowConsumeResize(Window* window)
{
    if (window->resizePending)
    {
        window->resizePending = false;
        return true;
    }

    return false;
}

bool WindowConsumeFocusLoss(Window* window)
{
    if (window->focusLossPending)
    {
        window->focusLossPending = false;
        return true;
    }

    return false;
}

void WindowRunLoop(Window* window, FrameCallback onFrame, void* userData)
{
    while (window->handle != NULL)
    {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);

            // WM_DESTROY этого окна мог прийти во время DispatchMessage.
            if (window->handle == NULL)
            {
                return;
            }
        }

        UpdateMouseLookState(window);

        if (onFrame != NULL)
        {
            onFrame(userData);
        }

        // В фокусе ожидание кадра при необходимости происходит в Present:
        // дополнительный сон мешал бы vsync и ограничивал режим без него.
        // Вне фокуса — экономный режим: ~20 кадров в секунду.
        if (!window->focused)
        {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        }
    }
}

void WindowSetMouseLook(Window* window, bool enabled)
{
    window->mouseLookEnabled = enabled;
}

bool WindowIsMouseLookEnabled(const Window* window)
{
    return window->mouseLookEnabled;
}

void WindowGetCursorClientPosition(const Window* window, int32_t* x, int32_t* y)
{
    *x = -1;
    *y = -1;

    POINT point;
    if (window->handle == NULL || !GetCursorPos(&point)
        || !ScreenToClient(window->handle, &point))
    {
        return;
    }
    *x = (int32_t)point.x;
    *y = (int32_t)point.y;
}

float WindowConsumeMouseWheelSteps(Window* window)
{
    float steps = window->wheelSteps;
    window->wheelSteps = 0.0f;
    return steps;
}

uint32_t WindowConsumeText(Window* window, uint32_t* codepoints, uint32_t capacity)
{
    if (window == NULL || codepoints == NULL)
    {
        return 0u;
    }
    uint32_t written = 0u;
    while (written < capacity && window->textCount != 0u)
    {
        codepoints[written++] = window->text[window->textHead];
        window->textHead = (window->textHead + 1u) % WINDOW_TEXT_CAPACITY;
        window->textCount--;
    }
    return written;
}

void WindowSetFullscreen(Window* window, bool enabled)
{
    if (window == NULL || window->fullscreen == enabled)
    {
        return;
    }

    if (enabled)
    {
        window->windowedStyle = GetWindowLongW(window->handle, GWL_STYLE);
        window->windowedPlacement.length = sizeof(window->windowedPlacement);
        GetWindowPlacement(window->handle, &window->windowedPlacement);

        MONITORINFO monitorInfo = { .cbSize = sizeof(monitorInfo) };
        HMONITOR monitor = MonitorFromWindow(window->handle,
            MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            return;
        }

        SetWindowLongW(window->handle, GWL_STYLE,
            (LONG)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(window->handle, HWND_TOP,
            monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        window->fullscreen = true;
    }
    else
    {
        SetWindowLongW(window->handle, GWL_STYLE, window->windowedStyle);
        SetWindowPlacement(window->handle, &window->windowedPlacement);
        SetWindowPos(window->handle, NULL, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
            | SWP_NOZORDER | SWP_NOOWNERZORDER);
        window->fullscreen = false;
    }

    if (window->cursorHidden)
    {
        ApplyCursorClip(window);
    }
}

bool WindowIsFullscreen(const Window* window)
{
    return window->fullscreen;
}

void WindowSetSurfaceCallback(Window *window, SurfaceCallback callback, void *userData)
{
    // Поверхность окна Win32 живёт вместе с окном и не меняется.
    (void)window;
    (void)callback;
    (void)userData;
}

float WindowGetContentScale(const Window *window)
{
    // Процесс без объявленной осведомлённости о DPI получает 96, а окно
    // целиком растягивает система: тогда пиксель окна и есть точка.
    const UINT dpi = window != NULL ? GetDpiForWindow(window->handle) : 0u;
    return dpi != 0u ? (float)dpi / (float)USER_DEFAULT_SCREEN_DPI : 1.0f;
}

void WindowRequestClose(Window* window)
{
    PostMessageW(window->handle, WM_CLOSE, 0, 0);
}

bool WindowSetPowerPolicy(uint32_t policy)
{
    if (policy > WINDOW_POWER_PERFORMANCE)
    {
        return false;
    }

    AcquireSRWLockExclusive(&g_powerLock);
    g_powerPolicy = policy;
    const bool applied = ApplyPowerPolicyLocked(ProcessIsForeground());
    ReleaseSRWLockExclusive(&g_powerLock);
    return applied;
}

uint32_t WindowGetPowerPolicy(void)
{
    AcquireSRWLockShared(&g_powerLock);
    const uint32_t policy = g_powerPolicy;
    ReleaseSRWLockShared(&g_powerLock);
    return policy;
}
