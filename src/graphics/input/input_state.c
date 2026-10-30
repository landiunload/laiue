#include "input/input_state.h"

#include "math/scalar.h"

#include <stddef.h>
#include <string.h>

#define INPUT_PI 3.14159265358979323846f
// Порог щелчка, если бэкенд не знает плотности экрана.
#define INPUT_DEFAULT_TOUCH_SLOP 8.0f

static int32_t RoundToInt(float value)
{
    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

InputKey InputKeyLetter(uint32_t letter)
{
    static const InputKey letters[26] = {
        INPUT_KEY_A, INPUT_KEY_B, INPUT_KEY_C, INPUT_KEY_D, INPUT_KEY_E, INPUT_KEY_F, INPUT_KEY_G,
        INPUT_KEY_H, INPUT_KEY_I, INPUT_KEY_J, INPUT_KEY_K, INPUT_KEY_L, INPUT_KEY_M, INPUT_KEY_N,
        INPUT_KEY_O, INPUT_KEY_P, INPUT_KEY_Q, INPUT_KEY_R, INPUT_KEY_S, INPUT_KEY_T, INPUT_KEY_U,
        INPUT_KEY_V, INPUT_KEY_W, INPUT_KEY_X, INPUT_KEY_Y, INPUT_KEY_Z,
    };
    return letter < 26u ? letters[letter] : INPUT_KEY_COUNT;
}

InputKey InputKeyDigit(uint32_t digit)
{
    if (digit == 0u)
        return INPUT_KEY_0;
    return digit <= 9u ? (InputKey)(INPUT_KEY_1 + (int)(digit - 1u)) : INPUT_KEY_COUNT;
}

InputKey InputKeyFunction(uint32_t number)
{
    static const InputKey functions[12] = {
        INPUT_KEY_F1, INPUT_KEY_F2, INPUT_KEY_F3, INPUT_KEY_F4,  INPUT_KEY_F5,  INPUT_KEY_F6,
        INPUT_KEY_F7, INPUT_KEY_F8, INPUT_KEY_F9, INPUT_KEY_F10, INPUT_KEY_F11, INPUT_KEY_F12,
    };
    return number >= 1u && number <= 12u ? functions[number - 1u] : INPUT_KEY_COUNT;
}

static void NeutralGesture(InputGesture *gesture)
{
    gesture->panX = 0.0f;
    gesture->panY = 0.0f;
    gesture->scale = 1.0f;
    gesture->rotation = 0.0f;
}

void InputStateInitialize(Input *input, float touchSlop)
{
    if (input == NULL)
        return;
    memset(input, 0, sizeof(*input));
    input->touchSlop = touchSlop > 0.0f ? touchSlop : INPUT_DEFAULT_TOUCH_SLOP;
    NeutralGesture(&input->gesture);
}

void InputStateKey(Input *input, InputKey key, bool down)
{
    if (input == NULL || (uint32_t)key >= INPUT_KEY_COUNT)
        return;
    if (down)
    {
        // Нажатие считается только на переходе «отпущена -> нажата»,
        // поэтому автоповтор клавиатуры очередь не наполняет.
        if (!input->keyDown[key] && input->keyPressCount[key] < UINT8_MAX)
            ++input->keyPressCount[key];
        input->keyDown[key] = true;
    }
    else
        input->keyDown[key] = false;
}

void InputStateButton(Input *input, InputMouseButton button, bool down)
{
    if (input == NULL || (uint32_t)button >= INPUT_MOUSE_BUTTON_COUNT)
        return;
    if (down && !input->buttonDown[button])
        input->buttonPressed[button] = true;
    input->buttonDown[button] = down;
}

void InputStateMove(Input *input, int32_t deltaX, int32_t deltaY)
{
    if (input == NULL)
        return;
    input->deltaX += deltaX;
    input->deltaY += deltaY;
}

void InputStatePointer(Input *input, int32_t x, int32_t y)
{
    if (input == NULL)
        return;
    if (input->hasLastPointer)
    {
        input->deltaX += x - input->lastPointerX;
        input->deltaY += y - input->lastPointerY;
    }
    input->lastPointerX = x;
    input->lastPointerY = y;
    input->hasLastPointer = true;
}

static int32_t TouchIndex(const Input *input, int32_t id)
{
    for (uint32_t index = 0u; index < input->touchCount; ++index)
        if (input->touches[index].id == id)
            return (int32_t)index;
    return -1;
}

// Новая точка отсчёта жеста по двум первым пальцам, без приращения.
static void GestureRebase(Input *input)
{
    if (input->touchCount < 2u)
    {
        input->gestureTracking = false;
        return;
    }
    for (uint32_t finger = 0u; finger < 2u; ++finger)
    {
        input->gestureIds[finger] = input->touches[finger].id;
        input->gestureX[finger] = input->touches[finger].x;
        input->gestureY[finger] = input->touches[finger].y;
    }
    input->gesture.centerX = (input->gestureX[0] + input->gestureX[1]) * 0.5f;
    input->gesture.centerY = (input->gestureY[0] + input->gestureY[1]) * 0.5f;
    input->gestureTracking = true;
}

static void GestureUpdate(Input *input)
{
    if (input->touchCount < 2u)
    {
        input->gestureTracking = false;
        return;
    }
    if (!input->gestureTracking || input->gestureIds[0] != input->touches[0].id ||
        input->gestureIds[1] != input->touches[1].id)
    {
        GestureRebase(input);
        return;
    }
    const InputTouch *first = &input->touches[0];
    const InputTouch *second = &input->touches[1];
    const float oldDx = input->gestureX[1] - input->gestureX[0];
    const float oldDy = input->gestureY[1] - input->gestureY[0];
    const float newDx = second->x - first->x;
    const float newDy = second->y - first->y;
    const float oldCenterX = (input->gestureX[0] + input->gestureX[1]) * 0.5f;
    const float oldCenterY = (input->gestureY[0] + input->gestureY[1]) * 0.5f;
    const float newCenterX = (first->x + second->x) * 0.5f;
    const float newCenterY = (first->y + second->y) * 0.5f;
    input->gesture.panX += newCenterX - oldCenterX;
    input->gesture.panY += newCenterY - oldCenterY;
    const float oldLength = ScalarSqrt(oldDx * oldDx + oldDy * oldDy);
    const float newLength = ScalarSqrt(newDx * newDx + newDy * newDy);
    // Пальцы почти в одной точке не задают ни масштаба, ни направления.
    if (oldLength > 1.0f && newLength > 1.0f)
    {
        input->gesture.scale *= newLength / oldLength;
        float turn = ScalarAtan2(newDy, newDx) - ScalarAtan2(oldDy, oldDx);
        if (turn > INPUT_PI)
            turn -= 2.0f * INPUT_PI;
        else if (turn < -INPUT_PI)
            turn += 2.0f * INPUT_PI;
        input->gesture.rotation += turn;
    }
    input->gestureMoved = true;
    GestureRebase(input);
}

void InputStateTouchDown(Input *input, int32_t id, float x, float y)
{
    if (input == NULL)
        return;
    int32_t index = TouchIndex(input, id);
    if (index < 0)
    {
        // Пальцы сверх ёмкости не учитываются вовсе.
        if (input->touchCount >= INPUT_TOUCH_CAPACITY)
            return;
        index = (int32_t)input->touchCount++;
        input->touches[index].id = id;
    }
    input->touches[index].x = x;
    input->touches[index].y = y;
    if (input->touchCount == 1u)
    {
        if (input->touchMode == INPUT_TOUCH_IDLE)
        {
            input->touchMode = INPUT_TOUCH_PENDING;
            input->touchStartX = x;
            input->touchStartY = y;
            // Указатель переносится к пальцу без дельты.
            input->lastPointerX = RoundToInt(x);
            input->lastPointerY = RoundToInt(y);
            input->hasLastPointer = true;
        }
        return;
    }
    // Второй палец заканчивает эмуляцию мыши: начатое перетаскивание
    // отпускается, несостоявшийся щелчок забывается.
    if (input->touchMode == INPUT_TOUCH_DRAG)
        input->buttonDown[INPUT_MOUSE_BUTTON_LEFT] = false;
    input->touchMode = INPUT_TOUCH_GESTURE;
    GestureRebase(input);
}

void InputStateTouchMove(Input *input, int32_t id, float x, float y)
{
    if (input == NULL)
        return;
    const int32_t index = TouchIndex(input, id);
    if (index < 0)
        return;
    input->touches[index].x = x;
    input->touches[index].y = y;
    if (input->touchMode == INPUT_TOUCH_PENDING)
    {
        const float dx = x - input->touchStartX;
        const float dy = y - input->touchStartY;
        if (dx * dx + dy * dy <= input->touchSlop * input->touchSlop)
            return;
        input->touchMode = INPUT_TOUCH_DRAG;
        InputStateButton(input, INPUT_MOUSE_BUTTON_LEFT, true);
    }
    if (input->touchMode == INPUT_TOUCH_DRAG)
        InputStatePointer(input, RoundToInt(x), RoundToInt(y));
    else if (input->touchMode == INPUT_TOUCH_GESTURE)
        GestureUpdate(input);
}

void InputStateTouchUp(Input *input, int32_t id)
{
    if (input == NULL)
        return;
    const int32_t index = TouchIndex(input, id);
    if (index < 0)
        return;
    for (uint32_t next = (uint32_t)index + 1u; next < input->touchCount; ++next)
        input->touches[next - 1u] = input->touches[next];
    --input->touchCount;
    if (input->touchCount != 0u)
    {
        // Пальцы ещё на экране: жест продолжается с новой парой, если она
        // есть, а одиночный оставшийся палец мышью не становится.
        if (input->touchMode == INPUT_TOUCH_GESTURE)
            GestureRebase(input);
        return;
    }
    if (input->touchMode == INPUT_TOUCH_PENDING)
    {
        InputStateButton(input, INPUT_MOUSE_BUTTON_LEFT, true);
        input->tapReleasePending = true;
    }
    else if (input->touchMode == INPUT_TOUCH_DRAG)
        input->buttonDown[INPUT_MOUSE_BUTTON_LEFT] = false;
    input->touchMode = INPUT_TOUCH_IDLE;
    input->gestureTracking = false;
}

void InputStateTouchCancel(Input *input)
{
    if (input == NULL)
        return;
    if (input->touchMode == INPUT_TOUCH_DRAG)
        input->buttonDown[INPUT_MOUSE_BUTTON_LEFT] = false;
    input->touchCount = 0u;
    input->touchMode = INPUT_TOUCH_IDLE;
    input->gestureTracking = false;
}

void InputEndFrame(Input *input)
{
    if (input == NULL)
        return;
    for (uint32_t button = 0u; button < INPUT_MOUSE_BUTTON_COUNT; ++button)
        input->buttonPressed[button] = false;
    input->deltaX = 0;
    input->deltaY = 0;
    if (input->tapReleasePending)
    {
        input->buttonDown[INPUT_MOUSE_BUTTON_LEFT] = false;
        input->tapReleasePending = false;
    }
    NeutralGesture(&input->gesture);
    input->gestureMoved = false;
}

void InputResetState(Input *input)
{
    if (input == NULL)
        return;
    InputStateInitialize(input, input->touchSlop);
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
           input->buttonDown[button];
}

bool InputWasMouseButtonPressed(const Input *input, InputMouseButton button)
{
    return input != NULL && (uint32_t)button < INPUT_MOUSE_BUTTON_COUNT &&
           input->buttonPressed[button];
}

void InputGetMouseDelta(const Input *input, int32_t *deltaX, int32_t *deltaY)
{
    if (deltaX != NULL)
        *deltaX = input == NULL ? 0 : input->deltaX;
    if (deltaY != NULL)
        *deltaY = input == NULL ? 0 : input->deltaY;
}

uint32_t InputGetTouches(const Input *input, InputTouch *touches, uint32_t capacity)
{
    if (input == NULL || touches == NULL)
        return 0u;
    const uint32_t count = input->touchCount < capacity ? input->touchCount : capacity;
    for (uint32_t index = 0u; index < count; ++index)
        touches[index] = input->touches[index];
    return count;
}

bool InputGetGesture(const Input *input, InputGesture *gesture)
{
    if (gesture == NULL)
        return false;
    NeutralGesture(gesture);
    gesture->centerX = 0.0f;
    gesture->centerY = 0.0f;
    if (input == NULL || (!input->gestureTracking && !input->gestureMoved))
        return false;
    *gesture = input->gesture;
    return true;
}
