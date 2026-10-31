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
    // Кадр программы, пока идёт WindowRunLoop: в модальных циклах Windows
    // его вызывают часы кадров.
    FrameCallback frameCallback;
    void* frameUserData;
    bool frameRunning;
    uint32_t modalDepth;                 // вложенность модальных циклов
    // Часы кадров: поток, который будит оконную процедуру, пока модальный
    // цикл держит поток окна. Создаются при первом модальном цикле.
    HANDLE frameClock;
    HANDLE frameClockWake;               // взведено, пока идёт модальный цикл
    HANDLE frameDone;                    // кадр модального цикла закончен
    HWND frameClockTarget;
    volatile LONG frameClockStop;
    volatile LONG frameTickPending;      // сообщение кадра ещё не разобрано
};

#define WINDOW_FRAME_MESSAGE (WM_APP + 1u)
#define WINDOW_MODAL_FRAME_TIMER 1u

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

// Кадр программы; поверх незаконченного кадра новый не начинается.
static void RunFrame(Window* window)
{
    if (window->frameCallback == NULL || window->frameRunning)
    {
        return;
    }
    window->frameRunning = true;
    window->frameCallback(window->frameUserData);
    window->frameRunning = false;
}

// Часы кадров для нажатия на заголовок и его кнопки: пока DefWindowProc
// ждёт мышь, они будят оконную процедуру к следующему кадру, как только
// закончился предыдущий. Таймер туда не доходит: цикл кнопок заголовка
// берёт из очереди только сообщения мыши, а до первого движения мыши после
// нажатия на заголовок DefWindowProc не разбирает ничего. Присланное
// сообщение (SendNotifyMessage не ждёт получателя) доходит до оконной
// процедуры из любого ожидания.
static DWORD WINAPI FrameClockMain(void* parameter)
{
    Window* window = (Window*)parameter;
    // Миллисекундной паузе нужен таймер высокого разрешения: обычное
    // ожидание округляется до системного тика, часто 15,6 мс.
    HANDLE pause = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    for (;;)
    {
        WaitForSingleObject(window->frameClockWake, INFINITE);
        if (InterlockedCompareExchange(&window->frameClockStop, 0, 0) != 0)
        {
            break;
        }
        if (InterlockedExchange(&window->frameTickPending, 1) == 0
            && !SendNotifyMessageW(window->frameClockTarget, WINDOW_FRAME_MESSAGE, 0, 0))
        {
            InterlockedExchange(&window->frameTickPending, 0);
        }
        // Следующее сообщение — когда кадр закончится; если кадр не начался,
        // повтор через период обновления обычного экрана.
        WaitForSingleObject(window->frameDone, 16);
        // Присланные сообщения Windows отдаёт потоку окна раньше ввода: без
        // паузы следующий кадр всегда опережал бы мышь, и цикл кнопки
        // заголовка не получал бы её, пока мышь движется.
        const LARGE_INTEGER pauseTime = { .QuadPart = -10000 };   // 1 мс
        if (pause == NULL || !SetWaitableTimer(pause, &pauseTime, 0, NULL, NULL, FALSE)
            || WaitForSingleObject(pause, INFINITE) != WAIT_OBJECT_0)
        {
            Sleep(1);
        }
    }
    if (pause != NULL)
    {
        CloseHandle(pause);
    }
    return 0;
}

static bool StartFrameClock(Window* window, HWND handle)
{
    if (window->frameClock != NULL)
    {
        return true;
    }
    window->frameClockTarget = handle;
    window->frameClockWake = CreateEventW(NULL, TRUE, FALSE, NULL);
    window->frameDone = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (window->frameClockWake != NULL && window->frameDone != NULL)
    {
        window->frameClock = CreateThread(NULL, (SIZE_T)64u * 1024u, FrameClockMain, window,
            STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    if (window->frameClock != NULL)
    {
        return true;
    }
    // Без часов кадры в модальных циклах просто стоят, как раньше.
    if (window->frameClockWake != NULL)
    {
        CloseHandle(window->frameClockWake);
        window->frameClockWake = NULL;
    }
    if (window->frameDone != NULL)
    {
        CloseHandle(window->frameDone);
        window->frameDone = NULL;
    }
    return false;
}

static void StopFrameClock(Window* window)
{
    if (window->frameClock != NULL)
    {
        InterlockedExchange(&window->frameClockStop, 1);
        SetEvent(window->frameClockWake);
        SetEvent(window->frameDone);
        WaitForSingleObject(window->frameClock, INFINITE);
        CloseHandle(window->frameClock);
        CloseHandle(window->frameClockWake);
        CloseHandle(window->frameDone);
        window->frameClock = NULL;
        window->frameClockWake = NULL;
        window->frameDone = NULL;
    }
}

// Пока кнопка мыши держит заголовок, рамку или кнопку заголовка окна или
// открыто меню окна, Windows крутит свой цикл сообщений внутри
// DefWindowProc, и WindowRunLoop не получает управления. Кадры в это время
// запускают часы кадров или таймер. Мышь и клавиатура принадлежат системе:
// ввод программы сбрасывается, как при потере фокуса, и raw input ей не
// передаётся, иначе нажатие на заголовок стало бы щелчком в программе.
static void EnterModalLoop(Window* window)
{
    window->modalDepth++;
    if (window->modalDepth == 1u)
    {
        window->focusLossPending = true;
    }
}

static void LeaveModalLoop(Window* window)
{
    if (window->modalDepth != 0u)
    {
        window->modalDepth--;
    }
}

static void ResumeFrameClock(Window* window, HWND handle)
{
    if (window->frameCallback != NULL && StartFrameClock(window, handle))
    {
        SetEvent(window->frameClockWake);
    }
}

static void PauseFrameClock(Window* window)
{
    if (window->frameClockWake != NULL)
    {
        ResetEvent(window->frameClockWake);
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
            if (window != NULL && window->rawInputCallback != NULL && window->modalDepth == 0u)
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

        case WM_SYSKEYDOWN:
            // Alt+Enter переключает полноэкранный режим, как принято в
            // Windows, одинаково для любой графики; автоповтор удерживаемой
            // клавиши его не повторяет.
            if (window != NULL && wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN) != 0
                && (HIWORD(lParam) & KF_REPEAT) == 0)
            {
                WindowSetFullscreen(window, !window->fullscreen);
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_NCLBUTTONDOWN:
        {
            // Заголовок, рамка и кнопки заголовка ведут мышь в своём цикле:
            // DefWindowProc возвращается, когда кнопку отпустят.
            if (window == NULL)
            {
                return DefWindowProcW(handle, message, wParam, lParam);
            }
            EnterModalLoop(window);
            ResumeFrameClock(window, handle);
            const LRESULT result = DefWindowProcW(handle, message, wParam, lParam);
            PauseFrameClock(window);
            LeaveModalLoop(window);
            return result;
        }

        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            // Циклы перемещения, изменения размера и меню разбирают все
            // сообщения, и кадры в них идут по таймеру: WM_TIMER приходит,
            // только когда очередь пуста, и не отнимает у цикла мышь. Часы
            // кадров присылали бы кадр за кадром раньше мыши, и окно
            // двигалось бы рывками.
            if (window != NULL)
            {
                EnterModalLoop(window);
                PauseFrameClock(window);
                if (window->frameCallback != NULL)
                {
                    SetTimer(handle, WINDOW_MODAL_FRAME_TIMER, USER_TIMER_MINIMUM, NULL);
                }
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            if (window != NULL)
            {
                KillTimer(handle, WINDOW_MODAL_FRAME_TIMER);
                LeaveModalLoop(window);
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_TIMER:
            if (window != NULL && wParam == WINDOW_MODAL_FRAME_TIMER)
            {
                if (window->modalDepth != 0u)
                {
                    RunFrame(window);
                }
                return 0;
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WINDOW_FRAME_MESSAGE:
            // Кадр от часов кадров. Опоздавшее после конца цикла сообщение
            // кадра не запускает: кадры снова ведёт WindowRunLoop.
            if (window != NULL)
            {
                InterlockedExchange(&window->frameTickPending, 0);
                if (window->modalDepth != 0u && !window->frameRunning)
                {
                    RunFrame(window);
                    SetEvent(window->frameDone);
                }
            }
            return 0;

        case WM_SYSCHAR:
            // Enter с Alt уже обработан: DefWindowProc искал бы его среди
            // пунктов меню и подавал звуковой сигнал.
            if (wParam == VK_RETURN)
            {
                return 0;
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_SYSCOMMAND:
            // Alt или F10 без другой клавиши включают режим меню, и его
            // модальный цикл внутри DefWindowProc останавливает кадры до
            // следующего нажатия. Alt+Space по-прежнему открывает системное
            // меню: у него lParam — код пробела.
            if ((wParam & 0xFFF0u) == SC_KEYMENU && lParam == 0)
            {
                return 0;
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
                // Окно закрыли изнутри модального цикла: часам больше
                // некого будить.
                window->modalDepth = 0u;
                PauseFrameClock(window);
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
    StopFrameClock(window);

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

// Разбирает очередь сообщений; false — окно уничтожено.
static bool PumpMessages(Window* window)
{
    MSG message;
    // WM_DESTROY этого окна мог прийти во время DispatchMessage.
    while (window->handle != NULL && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return window->handle != NULL;
}

void WindowRunLoop(Window* window, FrameCallback onFrame, void* userData)
{
    window->frameCallback = onFrame;
    window->frameUserData = userData;
    while (PumpMessages(window))
    {
        UpdateMouseLookState(window);
        RunFrame(window);

        // В фокусе ожидание кадра при необходимости происходит в Present:
        // дополнительный сон мешал бы vsync и ограничивал режим без него.
        // Вне фокуса — экономный режим: ~20 кадров в секунду.
        if (!window->focused)
        {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        }
    }
    window->frameCallback = NULL;
    window->frameUserData = NULL;
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
