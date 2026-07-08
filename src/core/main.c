#include "api.h"
#include "platform/window.h"
#include "input/input.h"

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

static void* g_window;

static void OnFrame(void* userData)
{
    (void)userData;

    WindowUpdateMouseLook(g_window);

    if (InputWasKeyPressed(INPUT_KEY_ESCAPE))
    {
        WindowRequestClose(g_window);
        return;
    }

    if (InputWasKeyPressed(INPUT_KEY_F7))
    {
        WindowSetMouseLook(g_window, !InputIsKeyDown(INPUT_KEY_SHIFT));
    }

    if (WindowConsumeResize(g_window))
    {
        Int32 width, height;
        WindowGetClientSize(g_window, &width, &height);
    }

    InputEndFrame();
}

LAIUE_CORE_API void __stdcall Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title  = L"laiue",
        .width  = 1280,
        .height = 720
    };

    g_window = WindowCreate(windowConfiguration);
    if (g_window == NULL) return;

    InputInitialize(WindowGetHandle(g_window));
    WindowSetRawInputCallback(g_window, InputHandleRawInput);

    WindowRunLoop(g_window, OnFrame, NULL);
}
