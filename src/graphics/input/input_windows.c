#include "input/input_state.h"

#include <windows.h>
#include <hidusage.h>

static InputKey MapVirtualKeyToInputKey(uint32_t virtualKey)
{
    if (virtualKey >= 'A' && virtualKey <= 'Z')
        return InputKeyLetter(virtualKey - 'A');
    if (virtualKey >= '0' && virtualKey <= '9')
        return InputKeyDigit(virtualKey - '0');
    if (virtualKey >= VK_F1 && virtualKey <= VK_F12)
        return InputKeyFunction(virtualKey - VK_F1 + 1u);
    switch (virtualKey)
    {
    case VK_ESCAPE:
        return INPUT_KEY_ESCAPE;
    case VK_SPACE:
        return INPUT_KEY_SPACE;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return INPUT_KEY_SHIFT;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return INPUT_KEY_CONTROL;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return INPUT_KEY_ALT;
    case VK_RETURN:
        return INPUT_KEY_ENTER;
    case VK_BACK:
        return INPUT_KEY_BACKSPACE;
    case VK_TAB:
        return INPUT_KEY_TAB;
    case VK_LEFT:
        return INPUT_KEY_LEFT;
    case VK_RIGHT:
        return INPUT_KEY_RIGHT;
    case VK_UP:
        return INPUT_KEY_UP;
    case VK_DOWN:
        return INPUT_KEY_DOWN;
    case VK_HOME:
        return INPUT_KEY_HOME;
    case VK_END:
        return INPUT_KEY_END;
    case VK_PRIOR:
        return INPUT_KEY_PAGE_UP;
    case VK_NEXT:
        return INPUT_KEY_PAGE_DOWN;
    case VK_INSERT:
        return INPUT_KEY_INSERT;
    case VK_DELETE:
        return INPUT_KEY_DELETE;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        return INPUT_KEY_MINUS;
    case VK_OEM_PLUS:
    case VK_ADD:
        return INPUT_KEY_EQUALS;
    default:
        return INPUT_KEY_COUNT;
    }
}

Input* InputCreate(void* windowNativeHandle)
{
    Input *input = HeapAlloc(GetProcessHeap(), 0, sizeof(*input));
    if (input == NULL)
    {
        return NULL;
    }
    // Порог щелчка касанием: Windows сама превращает касания в мышь.
    InputStateInitialize(input, 0.0f);

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

static void MouseButton(Input *input, USHORT flags, USHORT downFlag, USHORT upFlag,
                        InputMouseButton button)
{
    if ((flags & downFlag) != 0)
    {
        InputStateButton(input, button, true);
    }
    if ((flags & upFlag) != 0)
    {
        InputStateButton(input, button, false);
    }
}

void InputHandleRawInput(Input* input, void* rawInputHandle)
{
    if (input == NULL)
    {
        return;
    }
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
        InputStateKey(input, MapVirtualKeyToInputKey(keyboard->VKey),
                      (keyboard->Flags & RI_KEY_BREAK) == 0);
    }
    else if (rawInput.header.dwType == RIM_TYPEMOUSE)
    {
        const RAWMOUSE* mouse = &rawInput.data.mouse;
        if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            // Абсолютные координаты (нормированные 0..65535: удалённый
            // рабочий стол, планшет) — дельта восстанавливается по
            // предыдущей позиции.
            InputStatePointer(input, mouse->lLastX, mouse->lLastY);
        }
        else
        {
            InputStateMove(input, mouse->lLastX, mouse->lLastY);
        }
        const USHORT flags = mouse->usButtonFlags;
        MouseButton(input, flags, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP,
                    INPUT_MOUSE_BUTTON_LEFT);
        MouseButton(input, flags, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP,
                    INPUT_MOUSE_BUTTON_RIGHT);
        MouseButton(input, flags, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP,
                    INPUT_MOUSE_BUTTON_MIDDLE);
    }
}
