#include "input/input_state.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t failureCount;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failureCount;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

static bool Near(float value, float expected)
{
    const float difference = value - expected;
    return difference < 1e-3f && difference > -1e-3f;
}

static void TestKeysAndButtons(void)
{
    Input input;
    InputStateInitialize(&input, 10.0f);
    InputStateKey(&input, INPUT_KEY_Z, true);
    InputStateKey(&input, INPUT_KEY_Z, true);
    Expect(InputIsKeyDown(&input, INPUT_KEY_Z), "a pressed key is down");
    Expect(InputConsumeKeyPress(&input, INPUT_KEY_Z) && !InputConsumeKeyPress(&input, INPUT_KEY_Z),
           "auto-repeat while held queues one press");
    InputStateKey(&input, INPUT_KEY_Z, false);
    InputStateKey(&input, INPUT_KEY_COUNT, true);
    Expect(!InputIsKeyDown(&input, INPUT_KEY_Z), "a released key is up");

    InputStateButton(&input, INPUT_MOUSE_BUTTON_MIDDLE, true);
    Expect(InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_MIDDLE),
           "the middle button latches its press");
    InputEndFrame(&input);
    Expect(!InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_MIDDLE) &&
               InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_MIDDLE),
           "the latch lasts one frame and the button stays down");

    InputStatePointer(&input, 100, 50);
    InputStatePointer(&input, 112, 41);
    InputStateMove(&input, 3, 4);
    int32_t dx = 0;
    int32_t dy = 0;
    InputGetMouseDelta(&input, &dx, &dy);
    Expect(dx == 15 && dy == -5, "absolute and relative motion add up");
    Expect(InputKeyLetter(25u) == INPUT_KEY_Z && InputKeyDigit(0u) == INPUT_KEY_0 &&
               InputKeyDigit(7u) == INPUT_KEY_7 && InputKeyFunction(12u) == INPUT_KEY_F12 &&
               InputKeyFunction(13u) == INPUT_KEY_COUNT && InputKeyLetter(26u) == INPUT_KEY_COUNT,
           "keyboard order lookups map onto the stable key values");
}

static void TestTap(void)
{
    Input input;
    InputStateInitialize(&input, 10.0f);
    InputStateTouchDown(&input, 7, 50.0f, 60.0f);
    InputStateTouchMove(&input, 7, 54.0f, 63.0f);
    Expect(!InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT),
           "a finger inside the slop presses nothing yet");
    InputStateTouchUp(&input, 7);
    Expect(InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_LEFT) &&
               InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT),
           "lifting the finger clicks");
    InputEndFrame(&input);
    Expect(!InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT) &&
               !InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_LEFT),
           "the tap releases after the frame that saw it");
}

static void TestDrag(void)
{
    Input input;
    InputStateInitialize(&input, 10.0f);
    InputStateTouchDown(&input, 1, 100.0f, 100.0f);
    InputStateTouchMove(&input, 1, 130.0f, 90.0f);
    int32_t dx = 0;
    int32_t dy = 0;
    InputGetMouseDelta(&input, &dx, &dy);
    Expect(InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_LEFT) &&
               InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT) && dx == 30 && dy == -10,
           "moving past the slop presses and moves from the touch point");
    InputEndFrame(&input);
    InputStateTouchMove(&input, 1, 131.0f, 95.0f);
    InputGetMouseDelta(&input, &dx, &dy);
    Expect(dx == 1 && dy == 5, "a drag keeps moving the pointer");
    InputStateTouchUp(&input, 1);
    Expect(!InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT), "lifting ends the drag");
}

static void TestGesture(void)
{
    Input input;
    InputStateInitialize(&input, 10.0f);
    InputStateTouchDown(&input, 1, 100.0f, 100.0f);
    InputStateTouchMove(&input, 1, 140.0f, 100.0f);
    Expect(InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT), "one finger drags");
    InputStateTouchDown(&input, 2, 240.0f, 100.0f);
    Expect(!InputIsMouseButtonDown(&input, INPUT_MOUSE_BUTTON_LEFT),
           "a second finger releases the emulated button");
    InputEndFrame(&input);

    InputGesture gesture;
    Expect(InputGetGesture(&input, &gesture) && Near(gesture.scale, 1.0f) &&
               Near(gesture.panX, 0.0f) && Near(gesture.centerX, 190.0f),
           "two fingers make a neutral gesture before moving");
    // Пальцы раздвинуты вдвое вокруг центра и сдвинуты на 10 вниз.
    InputStateTouchMove(&input, 1, 90.0f, 110.0f);
    InputStateTouchMove(&input, 2, 290.0f, 110.0f);
    Expect(InputGetGesture(&input, &gesture) && Near(gesture.scale, 2.0f) &&
               Near(gesture.panX, 0.0f) && Near(gesture.panY, 10.0f) &&
               Near(gesture.rotation, 0.0f),
           "spreading the fingers scales and moving them pans");
    InputEndFrame(&input);
    // Поворот второго пальца на четверть оборота вокруг первого.
    InputStateTouchMove(&input, 2, 90.0f, 310.0f);
    Expect(InputGetGesture(&input, &gesture) && Near(gesture.scale, 1.0f) &&
               Near(gesture.rotation, 1.5707963f),
           "turning the fingers clockwise on screen rotates positively");
    InputStateTouchUp(&input, 2);
    InputStateTouchUp(&input, 1);
    InputEndFrame(&input);
    Expect(!InputGetGesture(&input, &gesture) && Near(gesture.scale, 1.0f) &&
               !InputWasMouseButtonPressed(&input, INPUT_MOUSE_BUTTON_LEFT),
           "lifting both fingers ends the gesture without a click");

    InputTouch touches[INPUT_TOUCH_CAPACITY];
    InputStateTouchDown(&input, 5, 1.0f, 2.0f);
    InputStateTouchDown(&input, 6, 3.0f, 4.0f);
    InputStateTouchCancel(&input);
    Expect(InputGetTouches(&input, touches, INPUT_TOUCH_CAPACITY) == 0u,
           "a cancel forgets every finger");
}

LAIUE_TEST_ENTRY(InputStateTestEntryPoint)
{
    TestKeysAndButtons();
    TestTap();
    TestDrag();
    TestGesture();
    if (failureCount != 0u)
        LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
