#include "platform/window.h"

#include <windows.h>

#define WINDOW_CLASS_NAME L"LaiueWindowClass"

struct Window
{
    HWND handle;
    int32_t clientWidth;
    int32_t clientHeight;
    bool resizePending;
    bool mouseLookEnabled;
    bool cursorHidden;
    bool focused;
    RawInputCallback rawInputCallback;
    void* rawInputUserData;
};

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

        case WM_ACTIVATE:
            if (window != NULL)
            {
                window->focused = LOWORD(wParam) != WA_INACTIVE;
            }
            return DefWindowProcW(handle, message, wParam, lParam);

        case WM_DESTROY:
            if (window != NULL)
            {
                window->handle = NULL;
            }
            PostQuitMessage(0);
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

    ShowWindow(window->handle, SW_SHOWDEFAULT);
    UpdateWindow(window->handle);

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

void WindowRunLoop(Window* window, FrameCallback onFrame, void* userData)
{
    for (;;)
    {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                return;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        UpdateMouseLookState(window);

        if (onFrame != NULL)
        {
            onFrame(userData);
        }

        // ~60 кадров в секунду при простое, ~0% CPU без сообщений.
        MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
    }
}

void WindowSetMouseLook(Window* window, bool enabled)
{
    window->mouseLookEnabled = enabled;
}

void WindowRequestClose(Window* window)
{
    PostMessageW(window->handle, WM_CLOSE, 0, 0);
}
