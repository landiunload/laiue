#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Непрозрачный дескриптор окна: всё состояние живёт внутри реализации,
// поэтому окон может быть сколько угодно и глобальных переменных нет.
typedef struct Window Window;

typedef struct WindowConfiguration
{
    const wchar_t* title;
    int32_t width;
    int32_t height;
} WindowConfiguration;

typedef void (*FrameCallback)(void* userData);
typedef void (*RawInputCallback)(void* userData, void* rawInputHandle);

LAIUE_WINDOW_API Window* WindowCreate(const WindowConfiguration* configuration);
LAIUE_WINDOW_API void    WindowDestroy(Window* window);
LAIUE_WINDOW_API void*   WindowGetNativeHandle(const Window* window);
LAIUE_WINDOW_API void    WindowSetRawInputCallback(Window* window, RawInputCallback callback, void* userData);
LAIUE_WINDOW_API void    WindowGetClientSize(const Window* window, int32_t* width, int32_t* height);
LAIUE_WINDOW_API bool    WindowConsumeResize(Window* window);
LAIUE_WINDOW_API bool    WindowConsumeFocusLoss(Window* window);
LAIUE_WINDOW_API void    WindowRunLoop(Window* window, FrameCallback onFrame, void* userData);
LAIUE_WINDOW_API void    WindowSetMouseLook(Window* window, bool enabled);
LAIUE_WINDOW_API bool    WindowIsMouseLookEnabled(const Window* window);
// Позиция курсора в клиентских координатах окна (для интерфейса);
// вне окна значения могут выходить за его пределы.
LAIUE_WINDOW_API void    WindowGetCursorClientPosition(const Window* window,
    int32_t* x, int32_t* y);
LAIUE_WINDOW_API void    WindowSetOverlayText(Window* window, const wchar_t* text);
LAIUE_WINDOW_API void    WindowRequestClose(Window* window);
