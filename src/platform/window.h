#ifndef WINDOW_H
#define WINDOW_H

#include "core/types.h"
#include "api.h"
#include <stddef.h>

typedef struct WindowConfiguration
{
    wchar_t* title;
    Int32 width;
    Int32 height;
} WindowConfiguration;

typedef void (*FrameCallback)(void* userData);
typedef void (*RawInputCallback)(void* rawInputData);

LAIUE_WINDOW_API void* WindowCreate(WindowConfiguration configuration);
LAIUE_WINDOW_API void* WindowGetHandle(void* window);
LAIUE_WINDOW_API void  WindowSetRawInputCallback(void* window, RawInputCallback callback);
LAIUE_WINDOW_API void  WindowGetClientSize(void* window, Int32* width, Int32* height);
LAIUE_WINDOW_API Bool  WindowConsumeResize(void* window);
LAIUE_WINDOW_API void  WindowRunLoop(void* window, FrameCallback onFrame, void* userData);
LAIUE_WINDOW_API void  WindowSetMouseLook(void* window, Bool enabled);
LAIUE_WINDOW_API void  WindowUpdateMouseLook(void* window);
LAIUE_WINDOW_API void  WindowRequestClose(void* window);

#endif
