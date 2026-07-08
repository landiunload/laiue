#ifndef INPUT_H
#define INPUT_H

#include "core/types.h"
#include "api.h"

typedef enum InputKey
{
    INPUT_KEY_ESCAPE,
    INPUT_KEY_SPACE,
    INPUT_KEY_W,
    INPUT_KEY_A,
    INPUT_KEY_S,
    INPUT_KEY_D,
    INPUT_KEY_F7,
    INPUT_KEY_SHIFT,
    INPUT_KEY_COUNT
} InputKey;

typedef enum InputMouseButton
{
    INPUT_MOUSE_BUTTON_LEFT,
    INPUT_MOUSE_BUTTON_RIGHT,
    INPUT_MOUSE_BUTTON_COUNT
} InputMouseButton;

LAIUE_INPUT_API void InputInitialize(void* windowHandle);
LAIUE_INPUT_API void InputHandleRawInput(void* rawInputData);
LAIUE_INPUT_API void InputEndFrame(void);

LAIUE_INPUT_API Bool InputIsKeyDown(InputKey key);
LAIUE_INPUT_API Bool InputWasKeyPressed(InputKey key);
LAIUE_INPUT_API Bool InputIsMouseButtonDown(InputMouseButton button);
LAIUE_INPUT_API void InputGetMouseDelta(Int32* deltaX, Int32* deltaY);

#endif
