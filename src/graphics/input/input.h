#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Непрозрачный дескриптор состояния ввода: создаётся на конкретное окно,
// глобальных переменных нет.
typedef struct Input Input;

// Значения стабильны: новые клавиши добавляются только перед
// INPUT_KEY_COUNT, поэтому программа, собранная со старым заголовком,
// получает те же номера.
typedef enum InputKey
{
    INPUT_KEY_ESCAPE,
    INPUT_KEY_SPACE,
    INPUT_KEY_W,
    INPUT_KEY_A,
    INPUT_KEY_S,
    INPUT_KEY_D,
    INPUT_KEY_T,
    INPUT_KEY_G,
    INPUT_KEY_V,
    INPUT_KEY_E,
    INPUT_KEY_1,
    INPUT_KEY_2,
    INPUT_KEY_3,
    INPUT_KEY_4,
    INPUT_KEY_5,
    INPUT_KEY_6,
    INPUT_KEY_7,
    INPUT_KEY_8,
    INPUT_KEY_9,
    INPUT_KEY_F3,
    INPUT_KEY_F7,
    INPUT_KEY_SHIFT,
    INPUT_KEY_CONTROL,
    INPUT_KEY_0,
    INPUT_KEY_R,
    INPUT_KEY_F,
    INPUT_KEY_P,
    INPUT_KEY_ENTER,
    INPUT_KEY_BACKSPACE,
    // Остальные буквы, функциональные клавиши и навигация.
    INPUT_KEY_B,
    INPUT_KEY_C,
    INPUT_KEY_H,
    INPUT_KEY_I,
    INPUT_KEY_J,
    INPUT_KEY_K,
    INPUT_KEY_L,
    INPUT_KEY_M,
    INPUT_KEY_N,
    INPUT_KEY_O,
    INPUT_KEY_Q,
    INPUT_KEY_U,
    INPUT_KEY_X,
    INPUT_KEY_Y,
    INPUT_KEY_Z,
    INPUT_KEY_F1,
    INPUT_KEY_F2,
    INPUT_KEY_F4,
    INPUT_KEY_F5,
    INPUT_KEY_F6,
    INPUT_KEY_F8,
    INPUT_KEY_F9,
    INPUT_KEY_F10,
    INPUT_KEY_F11,
    INPUT_KEY_F12,
    INPUT_KEY_LEFT,
    INPUT_KEY_RIGHT,
    INPUT_KEY_UP,
    INPUT_KEY_DOWN,
    INPUT_KEY_HOME,
    INPUT_KEY_END,
    INPUT_KEY_PAGE_UP,
    INPUT_KEY_PAGE_DOWN,
    INPUT_KEY_INSERT,
    INPUT_KEY_DELETE,
    INPUT_KEY_TAB,
    INPUT_KEY_ALT,
    // «-» и «=» основного ряда вместе с теми же знаками цифрового блока.
    INPUT_KEY_MINUS,
    INPUT_KEY_EQUALS,
    INPUT_KEY_COUNT
} InputKey;

typedef enum InputMouseButton
{
    INPUT_MOUSE_BUTTON_LEFT,
    INPUT_MOUSE_BUTTON_RIGHT,
    INPUT_MOUSE_BUTTON_MIDDLE,
    INPUT_MOUSE_BUTTON_COUNT
} InputMouseButton;

LAIUE_INPUT_API Input* InputCreate(void* windowNativeHandle);
LAIUE_INPUT_API void   InputDestroy(Input* input);

// Обрабатывает событие окна: HRAWINPUT из WM_INPUT на Windows, XEvent на
// X11, AInputEvent на Android; подключается к окну как raw-input callback.
LAIUE_INPUT_API void   InputHandleRawInput(Input* input, void* rawInputHandle);

// Сбрасывает только покадровые данные мыши и жеста. Очередь нажатий клавиш
// живёт до явного InputConsumeKeyPress, поэтому не теряется при большом FPS.
LAIUE_INPUT_API void   InputEndFrame(Input* input);

// Полный сброс состояния (все клавиши «отпущены», касаний нет, дельты
// обнулены). Вызывается при потере фокуса окном: события отпускания,
// произошедшие вне фокуса, не доставляются.
LAIUE_INPUT_API void   InputResetState(Input* input);

LAIUE_INPUT_API bool   InputIsKeyDown(const Input* input, InputKey key);
LAIUE_INPUT_API bool   InputWasKeyPressed(const Input* input, InputKey key);
LAIUE_INPUT_API bool   InputConsumeKeyPress(Input* input, InputKey key);
LAIUE_INPUT_API bool   InputIsMouseButtonDown(const Input* input, InputMouseButton button);
LAIUE_INPUT_API bool   InputWasMouseButtonPressed(const Input* input, InputMouseButton button);
LAIUE_INPUT_API void   InputGetMouseDelta(const Input* input, int32_t* deltaX, int32_t* deltaY);

// Касания экрана. Один палец ведёт себя как левая кнопка мыши: короткое
// касание на месте — щелчок при отпускании, а сдвиг дальше порога —
// перетаскивание с нажатием в точке касания. Второй палец отменяет
// эмуляцию мыши, и пальцы дают жест.
#define INPUT_TOUCH_CAPACITY 10u

typedef struct InputTouch
{
    int32_t id;
    // Клиентские пиксели окна.
    float x;
    float y;
} InputTouch;

// Двухпальцевый жест, накопленный с прошлого InputEndFrame: сдвиг центра
// между пальцами в пикселях, отношение расстояний между пальцами (больше
// единицы — раздвинули) и поворот в радианах (положительный — по часовой
// стрелке на экране) вокруг центра centerX, centerY.
typedef struct InputGesture
{
    float panX;
    float panY;
    float scale;
    float rotation;
    float centerX;
    float centerY;
} InputGesture;

// Пишет до capacity текущих касаний в порядке появления и возвращает их число.
LAIUE_INPUT_API uint32_t InputGetTouches(const Input *input, InputTouch *touches,
                                         uint32_t capacity);
// false — двух пальцев на экране нет; gesture тогда нейтрален.
LAIUE_INPUT_API bool InputGetGesture(const Input *input, InputGesture *gesture);
