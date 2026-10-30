#include "input/input_state.h"

#define Window X11Window
#include <X11/Xlib.h>
#include <X11/keysym.h>
#undef Window

#include "platform/system.h"

static InputKey MapKey(KeySym key)
{
    if (key >= XK_a && key <= XK_z)
        return InputKeyLetter((uint32_t)(key - XK_a));
    if (key >= XK_A && key <= XK_Z)
        return InputKeyLetter((uint32_t)(key - XK_A));
    if (key >= XK_0 && key <= XK_9)
        return InputKeyDigit((uint32_t)(key - XK_0));
    if (key >= XK_F1 && key <= XK_F12)
        return InputKeyFunction((uint32_t)(key - XK_F1) + 1u);
    switch (key)
    {
    case XK_Escape:
        return INPUT_KEY_ESCAPE;
    case XK_space:
        return INPUT_KEY_SPACE;
    case XK_Return:
    case XK_KP_Enter:
        return INPUT_KEY_ENTER;
    case XK_BackSpace:
        return INPUT_KEY_BACKSPACE;
    case XK_Tab:
        return INPUT_KEY_TAB;
    case XK_Shift_L:
    case XK_Shift_R:
        return INPUT_KEY_SHIFT;
    case XK_Control_L:
    case XK_Control_R:
        return INPUT_KEY_CONTROL;
    case XK_Alt_L:
    case XK_Alt_R:
        return INPUT_KEY_ALT;
    case XK_Left:
        return INPUT_KEY_LEFT;
    case XK_Right:
        return INPUT_KEY_RIGHT;
    case XK_Up:
        return INPUT_KEY_UP;
    case XK_Down:
        return INPUT_KEY_DOWN;
    case XK_Home:
        return INPUT_KEY_HOME;
    case XK_End:
        return INPUT_KEY_END;
    case XK_Prior:
        return INPUT_KEY_PAGE_UP;
    case XK_Next:
        return INPUT_KEY_PAGE_DOWN;
    case XK_Insert:
        return INPUT_KEY_INSERT;
    case XK_Delete:
        return INPUT_KEY_DELETE;
    case XK_minus:
    case XK_KP_Subtract:
        return INPUT_KEY_MINUS;
    case XK_equal:
    case XK_KP_Add:
        return INPUT_KEY_EQUALS;
    default:
        return INPUT_KEY_COUNT;
    }
}

static bool MapButton(unsigned int button, InputMouseButton *outButton)
{
    switch (button)
    {
    case Button1:
        *outButton = INPUT_MOUSE_BUTTON_LEFT;
        return true;
    case Button2:
        *outButton = INPUT_MOUSE_BUTTON_MIDDLE;
        return true;
    case Button3:
        *outButton = INPUT_MOUSE_BUTTON_RIGHT;
        return true;
    default:
        return false;
    }
}

Input *InputCreate(void *windowNativeHandle)
{
    (void)windowNativeHandle;
    Input *input = (Input *)PlatformAllocate(sizeof(Input), false);
    InputStateInitialize(input, 0.0f);
    return input;
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
        const KeySym keySym = XLookupKeysym((XKeyEvent *)&event->xkey, 0);
        InputStateKey(input, MapKey(keySym), event->type == KeyPress);
    }
    else if (event->type == ButtonPress || event->type == ButtonRelease)
    {
        InputMouseButton button = INPUT_MOUSE_BUTTON_LEFT;
        if (MapButton(event->xbutton.button, &button))
            InputStateButton(input, button, event->type == ButtonPress);
    }
    else if (event->type == MotionNotify)
        InputStatePointer(input, event->xmotion.x, event->xmotion.y);
}
