#include "input/input_state.h"

#include "platform/system.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>

// Доля меньшей стороны окна, за которой касание становится
// перетаскиванием: на телефоне это около миллиметра, как и 8 dp Android.
#define ANDROID_TOUCH_SLOP_FRACTION 0.02f
#define ANDROID_TOUCH_SLOP_MINIMUM 8.0f

static InputKey MapKey(int32_t code)
{
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
        return InputKeyLetter((uint32_t)(code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
        return InputKeyDigit((uint32_t)(code - AKEYCODE_0));
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
        return InputKeyFunction((uint32_t)(code - AKEYCODE_F1) + 1u);
    switch (code)
    {
    // «Назад» — тот же выход из режима, что Escape на клавиатуре.
    case AKEYCODE_BACK:
    case AKEYCODE_ESCAPE:
        return INPUT_KEY_ESCAPE;
    case AKEYCODE_SPACE:
        return INPUT_KEY_SPACE;
    case AKEYCODE_ENTER:
    case AKEYCODE_NUMPAD_ENTER:
        return INPUT_KEY_ENTER;
    case AKEYCODE_DEL:
        return INPUT_KEY_BACKSPACE;
    case AKEYCODE_FORWARD_DEL:
        return INPUT_KEY_DELETE;
    case AKEYCODE_TAB:
        return INPUT_KEY_TAB;
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT:
        return INPUT_KEY_SHIFT;
    case AKEYCODE_CTRL_LEFT:
    case AKEYCODE_CTRL_RIGHT:
        return INPUT_KEY_CONTROL;
    case AKEYCODE_ALT_LEFT:
    case AKEYCODE_ALT_RIGHT:
        return INPUT_KEY_ALT;
    case AKEYCODE_DPAD_LEFT:
        return INPUT_KEY_LEFT;
    case AKEYCODE_DPAD_RIGHT:
        return INPUT_KEY_RIGHT;
    case AKEYCODE_DPAD_UP:
        return INPUT_KEY_UP;
    case AKEYCODE_DPAD_DOWN:
        return INPUT_KEY_DOWN;
    case AKEYCODE_MOVE_HOME:
        return INPUT_KEY_HOME;
    case AKEYCODE_MOVE_END:
        return INPUT_KEY_END;
    case AKEYCODE_PAGE_UP:
        return INPUT_KEY_PAGE_UP;
    case AKEYCODE_PAGE_DOWN:
        return INPUT_KEY_PAGE_DOWN;
    case AKEYCODE_INSERT:
        return INPUT_KEY_INSERT;
    case AKEYCODE_MINUS:
    case AKEYCODE_NUMPAD_SUBTRACT:
        return INPUT_KEY_MINUS;
    case AKEYCODE_EQUALS:
    case AKEYCODE_PLUS:
    case AKEYCODE_NUMPAD_ADD:
        return INPUT_KEY_EQUALS;
    default:
        return INPUT_KEY_COUNT;
    }
}

Input *InputCreate(void *windowNativeHandle)
{
    Input *input = (Input *)PlatformAllocate(sizeof(Input), false);
    if (input == NULL)
        return NULL;
    float slop = ANDROID_TOUCH_SLOP_MINIMUM;
    ANativeWindow *window = (ANativeWindow *)windowNativeHandle;
    if (window != NULL)
    {
        const int32_t width = ANativeWindow_getWidth(window);
        const int32_t height = ANativeWindow_getHeight(window);
        const int32_t shorter = width < height ? width : height;
        if ((float)shorter * ANDROID_TOUCH_SLOP_FRACTION > slop)
            slop = (float)shorter * ANDROID_TOUCH_SLOP_FRACTION;
    }
    InputStateInitialize(input, slop);
    return input;
}

void InputDestroy(Input *input)
{
    PlatformFree(input);
}

static void HandleTouch(Input *input, const AInputEvent *event)
{
    const int32_t raw = AMotionEvent_getAction(event);
    const int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
    const size_t actionIndex = (size_t)(raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                               AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const size_t count = AMotionEvent_getPointerCount(event);
    switch (action)
    {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        if (actionIndex < count)
            InputStateTouchDown(input, AMotionEvent_getPointerId(event, actionIndex),
                                AMotionEvent_getX(event, actionIndex),
                                AMotionEvent_getY(event, actionIndex));
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t index = 0u; index < count; ++index)
            InputStateTouchMove(input, AMotionEvent_getPointerId(event, index),
                                AMotionEvent_getX(event, index), AMotionEvent_getY(event, index));
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        if (actionIndex < count)
        {
            // Последняя позиция пальца перед отпусканием тоже важна:
            // щелчок приходится туда, где палец поднят.
            const int32_t id = AMotionEvent_getPointerId(event, actionIndex);
            InputStateTouchMove(input, id, AMotionEvent_getX(event, actionIndex),
                                AMotionEvent_getY(event, actionIndex));
            InputStateTouchUp(input, id);
        }
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        InputStateTouchCancel(input);
        break;
    default:
        break;
    }
}

static void HandleMouse(Input *input, const AInputEvent *event)
{
    const int32_t buttons = AMotionEvent_getButtonState(event);
    InputStateButton(input, INPUT_MOUSE_BUTTON_LEFT, (buttons & AMOTION_EVENT_BUTTON_PRIMARY) != 0);
    InputStateButton(input, INPUT_MOUSE_BUTTON_RIGHT,
                     (buttons & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
    InputStateButton(input, INPUT_MOUSE_BUTTON_MIDDLE,
                     (buttons & AMOTION_EVENT_BUTTON_TERTIARY) != 0);
    const float x = AMotionEvent_getX(event, 0u);
    const float y = AMotionEvent_getY(event, 0u);
    InputStatePointer(input, (int32_t)(x + 0.5f), (int32_t)(y + 0.5f));
}

void InputHandleRawInput(Input *input, void *rawInputHandle)
{
    if (input == NULL || rawInputHandle == NULL)
        return;
    const AInputEvent *event = (const AInputEvent *)rawInputHandle;
    const int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
    {
        const int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP)
            InputStateKey(input, MapKey(AKeyEvent_getKeyCode(event)),
                          action == AKEY_EVENT_ACTION_DOWN);
        return;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION || AMotionEvent_getPointerCount(event) == 0u)
        return;
    const int32_t source = AInputEvent_getSource(event);
    if ((source & AINPUT_SOURCE_CLASS_MASK) != AINPUT_SOURCE_CLASS_POINTER)
        return;
    if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE)
        HandleMouse(input, event);
    else
        HandleTouch(input, event);
}
