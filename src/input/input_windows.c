#include "input/input.h"

#include <windows.h>

#pragma intrinsic(memset)

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

static Bool  g_keyDown[INPUT_KEY_COUNT];
static Bool  g_keyPressedLatch[INPUT_KEY_COUNT];
static Bool  g_mouseButtonDown[INPUT_MOUSE_BUTTON_COUNT];
static Int32 g_mouseDeltaX;
static Int32 g_mouseDeltaY;

static InputKey MapVirtualKeyToInputKey(UInt32 virtualKey)
{
    switch (virtualKey)
    {
        case VK_ESCAPE:   return INPUT_KEY_ESCAPE;
        case VK_SPACE:    return INPUT_KEY_SPACE;
        case 'W':         return INPUT_KEY_W;
        case 'A':         return INPUT_KEY_A;
        case 'S':         return INPUT_KEY_S;
        case 'D':         return INPUT_KEY_D;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:   return INPUT_KEY_SHIFT;
        case VK_F7:       return INPUT_KEY_F7;
        default:          return INPUT_KEY_COUNT;
    }
}

void InputInitialize(void* windowHandle)
{
    HWND nativeWindow = (HWND)windowHandle;

    g_mouseDeltaX = 0;
    g_mouseDeltaY = 0;

    RAWINPUTDEVICE devices[2] = { 0 };

    devices[0].usUsagePage = 0x01;
    devices[0].usUsage     = 0x06;
    devices[0].dwFlags     = 0;
    devices[0].hwndTarget  = nativeWindow;

    devices[1].usUsagePage = 0x01;
    devices[1].usUsage     = 0x02;
    devices[1].dwFlags     = 0;
    devices[1].hwndTarget  = nativeWindow;

    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
}

void InputHandleRawInput(void* rawInputHandle)
{
    HRAWINPUT handle = (HRAWINPUT)rawInputHandle;

    RAWINPUT rawInput;
    UINT size = sizeof(rawInput);
    if (GetRawInputData(handle, RID_INPUT, &rawInput, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
    {
        return;
    }

    if (rawInput.header.dwType == RIM_TYPEKEYBOARD)
    {
        const RAWKEYBOARD* keyboard = &rawInput.data.keyboard;
        InputKey key = MapVirtualKeyToInputKey(keyboard->VKey);
        if (key == INPUT_KEY_COUNT)
        {
            return;
        }

        Bool isKeyUp = (keyboard->Flags & RI_KEY_BREAK) != 0;
        if (isKeyUp)
        {
            g_keyDown[key] = BOOL_FALSE;
        }
        else
        {
            if (!g_keyDown[key])
            {
                g_keyPressedLatch[key] = BOOL_TRUE;
            }

            g_keyDown[key] = BOOL_TRUE;
        }
    }
    else if (rawInput.header.dwType == RIM_TYPEMOUSE)
    {
        const RAWMOUSE* mouse = &rawInput.data.mouse;
        g_mouseDeltaX += mouse->lLastX;
        g_mouseDeltaY += mouse->lLastY;

        if (mouse->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   g_mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT]   = BOOL_TRUE;
        if (mouse->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)     g_mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT]   = BOOL_FALSE;
        if (mouse->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  g_mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT]  = BOOL_TRUE;
        if (mouse->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)    g_mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT]  = BOOL_FALSE;
    }
}

void InputEndFrame(void)
{
    memset(g_keyPressedLatch, 0, sizeof(g_keyPressedLatch));
    g_mouseDeltaX = 0;
    g_mouseDeltaY = 0;
}

Bool InputIsKeyDown(InputKey key)
{
    return g_keyDown[key];
}

Bool InputWasKeyPressed(InputKey key)
{
    return g_keyPressedLatch[key];
}

Bool InputIsMouseButtonDown(InputMouseButton button)
{
    return g_mouseButtonDown[button];
}

void InputGetMouseDelta(Int32* deltaX, Int32* deltaY)
{
    *deltaX = g_mouseDeltaX;
    *deltaY = g_mouseDeltaY;
}
