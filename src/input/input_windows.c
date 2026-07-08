#include "input/input.h"

#include <windows.h>
#include <hidusage.h>

struct Input
{
    bool keyDown[INPUT_KEY_COUNT];
    bool keyPressedLatch[INPUT_KEY_COUNT];
    bool mouseButtonDown[INPUT_MOUSE_BUTTON_COUNT];
    int32_t mouseDeltaX;
    int32_t mouseDeltaY;
};

static InputKey MapVirtualKeyToInputKey(uint32_t virtualKey)
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

Input* InputCreate(void* windowNativeHandle)
{
    Input* input = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*input));
    if (input == NULL)
    {
        return NULL;
    }

    RAWINPUTDEVICE devices[] = {
        {
            .usUsagePage = HID_USAGE_PAGE_GENERIC,
            .usUsage     = HID_USAGE_GENERIC_KEYBOARD,
            .hwndTarget  = (HWND)windowNativeHandle,
        },
        {
            .usUsagePage = HID_USAGE_PAGE_GENERIC,
            .usUsage     = HID_USAGE_GENERIC_MOUSE,
            .hwndTarget  = (HWND)windowNativeHandle,
        },
    };

    if (!RegisterRawInputDevices(devices, ARRAYSIZE(devices), sizeof(devices[0])))
    {
        HeapFree(GetProcessHeap(), 0, input);
        return NULL;
    }

    return input;
}

void InputDestroy(Input* input)
{
    if (input != NULL)
    {
        HeapFree(GetProcessHeap(), 0, input);
    }
}

void InputHandleRawInput(Input* input, void* rawInputHandle)
{
    RAWINPUT rawInput;
    UINT rawInputSize = sizeof(rawInput);
    if (GetRawInputData((HRAWINPUT)rawInputHandle, RID_INPUT,
                        &rawInput, &rawInputSize, sizeof(RAWINPUTHEADER)) == (UINT)-1)
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

        bool isKeyRelease = (keyboard->Flags & RI_KEY_BREAK) != 0;
        if (isKeyRelease)
        {
            input->keyDown[key] = false;
        }
        else
        {
            // Защёлка взводится только на переходе «отпущена -> нажата»,
            // авто-повтор клавиатуры её не дребезжит.
            if (!input->keyDown[key])
            {
                input->keyPressedLatch[key] = true;
            }

            input->keyDown[key] = true;
        }
    }
    else if (rawInput.header.dwType == RIM_TYPEMOUSE)
    {
        const RAWMOUSE* mouse = &rawInput.data.mouse;

        input->mouseDeltaX += mouse->lLastX;
        input->mouseDeltaY += mouse->lLastY;

        if (mouse->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)  { input->mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT]  = true; }
        if (mouse->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)    { input->mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT]  = false; }
        if (mouse->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) { input->mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT] = true; }
        if (mouse->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)   { input->mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT] = false; }
    }
}

void InputEndFrame(Input* input)
{
    // Массив крошечный — простой цикл, чтобы компилятор
    // не подставил вызов memset (CRT не линкуется).
    for (int32_t key = 0; key < INPUT_KEY_COUNT; ++key)
    {
        input->keyPressedLatch[key] = false;
    }

    input->mouseDeltaX = 0;
    input->mouseDeltaY = 0;
}

bool InputIsKeyDown(const Input* input, InputKey key)
{
    return (uint32_t)key < INPUT_KEY_COUNT && input->keyDown[key];
}

bool InputWasKeyPressed(const Input* input, InputKey key)
{
    return (uint32_t)key < INPUT_KEY_COUNT && input->keyPressedLatch[key];
}

bool InputIsMouseButtonDown(const Input* input, InputMouseButton button)
{
    return (uint32_t)button < INPUT_MOUSE_BUTTON_COUNT && input->mouseButtonDown[button];
}

void InputGetMouseDelta(const Input* input, int32_t* deltaX, int32_t* deltaY)
{
    *deltaX = input->mouseDeltaX;
    *deltaY = input->mouseDeltaY;
}
