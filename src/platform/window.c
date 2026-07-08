#include "platform/window.h"

#include <windows.h>

void _RTC_InitBase(void) {}
void _RTC_Shutdown(void) {}
int _RTC_CheckStackVars(void* frame, void* descriptors) { (void)frame; (void)descriptors; return 0; }

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)fdwReason;
    (void)lpvReserved;
    return TRUE;
}

static HWND  g_windowHandle = NULL;
static Int32 g_clientWidth  = 0;
static Int32 g_clientHeight = 0;
static Bool  g_resizePending = BOOL_FALSE;
static RawInputCallback g_rawInputCallback = NULL;
static Bool  g_mouseLookEnabled = BOOL_FALSE;
static Bool  g_cursorHidden     = BOOL_FALSE;
static Bool  g_windowFocused    = BOOL_FALSE;

#define WINDOW_CLASS_NAME L"LaiueWindowClass"

static LRESULT CALLBACK WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_SIZE:
            g_clientWidth   = (Int32)LOWORD(lParam);
            g_clientHeight  = (Int32)HIWORD(lParam);
            g_resizePending = BOOL_TRUE;
            return 0;

        case WM_INPUT:
            if (g_rawInputCallback != NULL)
            {
                g_rawInputCallback((void*)lParam);
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_ACTIVATE:
            g_windowFocused = (LOWORD(wParam) != WA_INACTIVE) ? BOOL_TRUE : BOOL_FALSE;
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(handle, message, wParam, lParam);
    }
}

void* WindowCreate(WindowConfiguration configuration)
{
    HINSTANCE instance = GetModuleHandleW(NULL);

    WNDCLASSEXW windowClass = { 0 };
    windowClass.cbSize        = sizeof(windowClass);
    windowClass.style         = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc   = WindowProcedure;
    windowClass.hInstance     = instance;
    windowClass.hIcon         = LoadIconW(instance, (LPCWSTR)IDI_APPLICATION);
    windowClass.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    windowClass.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&windowClass))
    {
        return NULL;
    }

    RECT clientRect;
    clientRect.left   = 0;
    clientRect.top    = 0;
    clientRect.right  = configuration.width;
    clientRect.bottom = configuration.height;

    DWORD windowStyle   = WS_OVERLAPPEDWINDOW;
    DWORD windowStyleEx = WS_EX_APPWINDOW;

    AdjustWindowRectEx(&clientRect, windowStyle, FALSE, windowStyleEx);

    Int32 windowWidth  = clientRect.right - clientRect.left;
    Int32 windowHeight = clientRect.bottom - clientRect.top;

    HWND handle = CreateWindowExW(
        windowStyleEx,
        WINDOW_CLASS_NAME,
        configuration.title,
        windowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        instance,
        NULL
    );

    if (handle == NULL)
    {
        return NULL;
    }

    g_windowHandle  = handle;
    g_clientWidth   = configuration.width;
    g_clientHeight  = configuration.height;
    g_resizePending = BOOL_FALSE;

    ShowWindow(handle, SW_SHOWDEFAULT);
    UpdateWindow(handle);

    return (void*)handle;
}

void* WindowGetHandle(void* window)
{
    return window;
}

void WindowSetRawInputCallback(void* window, RawInputCallback callback)
{
    (void)window;
    g_rawInputCallback = callback;
}

void WindowGetClientSize(void* window, Int32* width, Int32* height)
{
    (void)window;
    *width  = g_clientWidth;
    *height = g_clientHeight;
}

Bool WindowConsumeResize(void* window)
{
    (void)window;

    if (g_resizePending)
    {
        g_resizePending = BOOL_FALSE;
        return BOOL_TRUE;
    }

    return BOOL_FALSE;
}

void WindowRunLoop(void* window, FrameCallback onFrame, void* userData)
{
    (void)window;
    MSG message = { 0 };

    while (message.message != WM_QUIT)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (onFrame != NULL)
        {
            onFrame(userData);
        }

        // ~60 FPS когда нет сообщений, ~0% CPU в простое
        MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
    }
}

void WindowSetMouseLook(void* window, Bool enabled)
{
    (void)window;
    g_mouseLookEnabled = enabled;
}

void WindowUpdateMouseLook(void* window)
{
    (void)window;

    Bool shouldHide = g_mouseLookEnabled && g_windowFocused;

    if (shouldHide == g_cursorHidden)
    {
        return;
    }

    if (shouldHide)
    {
        while (ShowCursor(FALSE) >= 0);
        g_cursorHidden = BOOL_TRUE;

        RECT clientRect;
        GetClientRect(g_windowHandle, &clientRect);
        POINT clientTopLeft = { clientRect.left, clientRect.top };
        POINT clientBottomRight = { clientRect.right, clientRect.bottom };
        ClientToScreen(g_windowHandle, &clientTopLeft);
        ClientToScreen(g_windowHandle, &clientBottomRight);

        clientRect.left   = clientTopLeft.x;
        clientRect.top    = clientTopLeft.y;
        clientRect.right  = clientBottomRight.x;
        clientRect.bottom = clientBottomRight.y;
        ClipCursor(&clientRect);
    }
    else
    {
        while (ShowCursor(TRUE) < 0);
        g_cursorHidden = BOOL_FALSE;
        ClipCursor(NULL);
    }
}

void WindowRequestClose(void* window)
{
    (void)window;
    PostMessageW(g_windowHandle, WM_CLOSE, 0, 0);
}
