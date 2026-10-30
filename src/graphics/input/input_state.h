#pragma once

// Состояние ввода, общее для всех платформ. Бэкенд (Win32 raw input, X11,
// Android) только переводит события окна в вызовы InputState*, а очередь
// нажатий, защёлки кнопок, дельты, касания и жест живут здесь в одном
// экземпляре: поведение одинаково везде и проверяется одним тестом.

#include "input/input.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum InputTouchMode
{
    // Пальцев нет.
    INPUT_TOUCH_IDLE,
    // Один палец ещё не сдвинулся дальше порога: это может быть щелчок.
    INPUT_TOUCH_PENDING,
    // Один палец тащит: левая кнопка нажата.
    INPUT_TOUCH_DRAG,
    // Пальцев два и больше: жест, мышь не эмулируется до отпускания всех.
    INPUT_TOUCH_GESTURE,
} InputTouchMode;

struct Input
{
    bool keyDown[INPUT_KEY_COUNT];
    uint8_t keyPressCount[INPUT_KEY_COUNT];
    bool buttonDown[INPUT_MOUSE_BUTTON_COUNT];
    bool buttonPressed[INPUT_MOUSE_BUTTON_COUNT];
    int32_t deltaX;
    int32_t deltaY;
    // Абсолютный указатель (планшет, удалённый стол, X11): дельта считается
    // от прошлой позиции.
    int32_t lastPointerX;
    int32_t lastPointerY;
    bool hasLastPointer;

    InputTouch touches[INPUT_TOUCH_CAPACITY];
    uint32_t touchCount;
    InputTouchMode touchMode;
    // Порог, за которым касание становится перетаскиванием, в пикселях.
    float touchSlop;
    float touchStartX;
    float touchStartY;
    // Щелчок касанием: кнопка нажата на один кадр и отпускается в конце
    // кадра, когда программа уже увидела нажатие.
    bool tapReleasePending;

    // Жест по двум первым пальцам: их прежние позиции и накопленное.
    bool gestureTracking;
    int32_t gestureIds[2];
    float gestureX[2];
    float gestureY[2];
    InputGesture gesture;
    bool gestureMoved;
};

// Клавиши по порядку на клавиатуре; INPUT_KEY_COUNT вне диапазона.
// letter 0..25 — A..Z, digit 0..9, function 1..12 — F1..F12.
InputKey InputKeyLetter(uint32_t letter);
InputKey InputKeyDigit(uint32_t digit);
InputKey InputKeyFunction(uint32_t number);

// Обнуляет всё; touchSlop — порог щелчка в пикселях этого экрана.
void InputStateInitialize(Input *input, float touchSlop);

void InputStateKey(Input *input, InputKey key, bool down);
void InputStateButton(Input *input, InputMouseButton button, bool down);
// Относительное движение мыши.
void InputStateMove(Input *input, int32_t deltaX, int32_t deltaY);
// Абсолютная позиция указателя; дельта считается от прошлой.
void InputStatePointer(Input *input, int32_t x, int32_t y);

void InputStateTouchDown(Input *input, int32_t id, float x, float y);
void InputStateTouchMove(Input *input, int32_t id, float x, float y);
void InputStateTouchUp(Input *input, int32_t id);
// Система забрала касания (жест системы, диалог): ни щелчка, ни нажатия.
void InputStateTouchCancel(Input *input);
