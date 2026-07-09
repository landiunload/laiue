#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Непрозрачный дескриптор состояния ввода: создаётся на конкретное окно,
// глобальных переменных нет.
typedef struct Input Input;

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

LAIUE_INPUT_API Input* InputCreate(void* windowNativeHandle);
LAIUE_INPUT_API void   InputDestroy(Input* input);

// Обрабатывает HRAWINPUT из WM_INPUT; подключается к окну как raw-input callback.
LAIUE_INPUT_API void   InputHandleRawInput(Input* input, void* rawInputHandle);

// Сбрасывает накопленные за кадр данные (нажатия-защёлки и дельту мыши).
// Вызывается в конце каждого кадра.
LAIUE_INPUT_API void   InputEndFrame(Input* input);

// Полный сброс состояния (все клавиши «отпущены», дельты обнулены).
// Вызывается при потере фокуса окном: события отпускания,
// произошедшие вне фокуса, raw input не доставляет.
LAIUE_INPUT_API void   InputResetState(Input* input);

LAIUE_INPUT_API bool   InputIsKeyDown(const Input* input, InputKey key);
LAIUE_INPUT_API bool   InputWasKeyPressed(const Input* input, InputKey key);
LAIUE_INPUT_API bool   InputIsMouseButtonDown(const Input* input, InputMouseButton button);
LAIUE_INPUT_API void   InputGetMouseDelta(const Input* input, int32_t* deltaX, int32_t* deltaY);
