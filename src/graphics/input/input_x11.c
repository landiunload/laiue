#include "input/input.h"

#define Window X11Window
#include <X11/Xlib.h>
#include <X11/keysym.h>
#undef Window

#include "platform/system.h"

struct Input
{
    bool keyDown[INPUT_KEY_COUNT];
    uint8_t keyPressCount[INPUT_KEY_COUNT];
    bool mouseButtonDown[INPUT_MOUSE_BUTTON_COUNT];
    bool mouseButtonPressedLatch[INPUT_MOUSE_BUTTON_COUNT];
    int32_t mouseDeltaX;
    int32_t mouseDeltaY;
    int32_t lastPointerX;
    int32_t lastPointerY;
    bool hasLastPointer;
};

static InputKey MapKey(KeySym key)
{
    switch (key)
    {
        case XK_Escape: return INPUT_KEY_ESCAPE;
        case XK_space: return INPUT_KEY_SPACE;
        case XK_w: case XK_W: return INPUT_KEY_W;
        case XK_a: case XK_A: return INPUT_KEY_A;
        case XK_s: case XK_S: return INPUT_KEY_S;
        case XK_d: case XK_D: return INPUT_KEY_D;
        case XK_t: case XK_T: return INPUT_KEY_T;
        case XK_g: case XK_G: return INPUT_KEY_G;
        case XK_v: case XK_V: return INPUT_KEY_V;
        case XK_e: case XK_E: return INPUT_KEY_E;
        case XK_1: return INPUT_KEY_1;
        case XK_2: return INPUT_KEY_2;
        case XK_3: return INPUT_KEY_3;
        case XK_4: return INPUT_KEY_4;
        case XK_5: return INPUT_KEY_5;
        case XK_6: return INPUT_KEY_6;
        case XK_7: return INPUT_KEY_7;
        case XK_8: return INPUT_KEY_8;
        case XK_9: return INPUT_KEY_9;
        case XK_F3: return INPUT_KEY_F3;
        case XK_F7: return INPUT_KEY_F7;
        case XK_Shift_L: case XK_Shift_R: return INPUT_KEY_SHIFT;
        case XK_Control_L: case XK_Control_R: return INPUT_KEY_CONTROL;
        default: return INPUT_KEY_COUNT;
    }
}

Input *InputCreate(void *windowNativeHandle)
{
    (void)windowNativeHandle;
    return (Input *)PlatformAllocate(sizeof(Input), true);
}

void InputDestroy(Input *input)
{
    PlatformFree(input);
}

void InputHandleRawInput(Input *input, void *rawInputHandle)
{
    if (input == NULL || rawInputHandle == NULL)
        return;
    const XEvent *event = (const XEvent *)rawInputHandle;
    if (event->type == KeyPress || event->type == KeyRelease)
    {
        KeySym keySym = XLookupKeysym((XKeyEvent *)&event->xkey, 0);
        const InputKey key = MapKey(keySym);
        if (key == INPUT_KEY_COUNT)
            return;
        if (event->type == KeyRelease)
            input->keyDown[key] = false;
        else
        {
            if (!input->keyDown[key] && input->keyPressCount[key] < UINT8_MAX)
                ++input->keyPressCount[key];
            input->keyDown[key] = true;
        }
    }
    else if (event->type == ButtonPress || event->type == ButtonRelease)
    {
        const unsigned int button = event->xbutton.button;
        if (button == Button1)
        {
            if (event->type == ButtonPress)
            {
                input->mouseButtonPressedLatch[INPUT_MOUSE_BUTTON_LEFT] =
                    !input->mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT];
                input->mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT] = true;
            }
            else
                input->mouseButtonDown[INPUT_MOUSE_BUTTON_LEFT] = false;
        }
        else if (button == Button3)
        {
            if (event->type == ButtonPress)
            {
                input->mouseButtonPressedLatch[INPUT_MOUSE_BUTTON_RIGHT] =
                    !input->mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT];
                input->mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT] = true;
            }
            else
                input->mouseButtonDown[INPUT_MOUSE_BUTTON_RIGHT] = false;
        }
    }
    else if (event->type == MotionNotify)
    {
        if (input->hasLastPointer)
        {
            input->mouseDeltaX += event->xmotion.x - input->lastPointerX;
            input->mouseDeltaY += event->xmotion.y - input->lastPointerY;
        }
        input->lastPointerX = event->xmotion.x;
        input->lastPointerY = event->xmotion.y;
        input->hasLastPointer = true;
    }
}

void InputEndFrame(Input *input)
{
    if (input == NULL)
        return;
    for (uint32_t button = 0u; button < INPUT_MOUSE_BUTTON_COUNT; ++button)
        input->mouseButtonPressedLatch[button] = false;
    input->mouseDeltaX = 0;
    input->mouseDeltaY = 0;
}

void InputResetState(Input *input)
{
    if (input == NULL)
        return;
    for (uint32_t key = 0u; key < INPUT_KEY_COUNT; ++key)
    {
        input->keyDown[key] = false;
        input->keyPressCount[key] = 0u;
    }
    for (uint32_t button = 0u; button < INPUT_MOUSE_BUTTON_COUNT; ++button)
    {
        input->mouseButtonDown[button] = false;
        input->mouseButtonPressedLatch[button] = false;
    }
    input->mouseDeltaX = 0;
    input->mouseDeltaY = 0;
    input->hasLastPointer = false;
}

bool InputIsKeyDown(const Input *input, InputKey key)
{
    return input != NULL && (uint32_t)key < INPUT_KEY_COUNT && input->keyDown[key];
}

bool InputWasKeyPressed(const Input *input, InputKey key)
{
    return input != NULL && (uint32_t)key < INPUT_KEY_COUNT && input->keyPressCount[key] != 0u;
}

bool InputConsumeKeyPress(Input *input, InputKey key)
{
    if (input == NULL || (uint32_t)key >= INPUT_KEY_COUNT || input->keyPressCount[key] == 0u)
        return false;
    --input->keyPressCount[key];
    return true;
}

bool InputIsMouseButtonDown(const Input *input, InputMouseButton button)
{
    return input != NULL && (uint32_t)button < INPUT_MOUSE_BUTTON_COUNT &&
           input->mouseButtonDown[button];
}

bool InputWasMouseButtonPressed(const Input *input, InputMouseButton button)
{
    return input != NULL && (uint32_t)button < INPUT_MOUSE_BUTTON_COUNT &&
           input->mouseButtonPressedLatch[button];
}

void InputGetMouseDelta(const Input *input, int32_t *deltaX, int32_t *deltaY)
{
    if (deltaX != NULL)
        *deltaX = input == NULL ? 0 : input->mouseDeltaX;
    if (deltaY != NULL)
        *deltaY = input == NULL ? 0 : input->mouseDeltaY;
}
