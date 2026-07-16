#pragma once

#include "core/ui_font.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Immediate-mode интерфейс (внутренний компонент ядра): виджеты каждый
// кадр заново собирают список квадов для RendererUiQueue. Состояние
// между кадрами — только анимации наведения и захват мыши слайдером.

#define UI_MAX_DRAW_QUADS RENDERER_UI_MAX_QUADS
#define UI_MAX_ANIMATIONS 48

typedef struct UiAnimation
{
    uint32_t id;
    float value;
} UiAnimation;

typedef struct UiContext
{
    UiFont font;
    int32_t bakedPixelSize;
    bool fontDirty;      // атлас изменился — передать рендереру
    float scale;         // масштаб интерфейса от высоты окна

    float mouseX;
    float mouseY;
    bool mouseDown;
    bool mousePressed;   // нажатие в этом кадре
    float wheelSteps;    // щелчки колеса за кадр (положительные — от себя)
    float deltaSeconds;

    // Прямоугольник отсечения: квады обрезаются геометрически (и по UV),
    // поэтому прокручиваемый контент не вылезает за панель.
    bool clipEnabled;
    float clipRect[4];   // x0, y0, x1, y1

    uint32_t activeId;   // виджет, захвативший мышь (слайдер)

    UiAnimation animations[UI_MAX_ANIMATIONS];
    uint32_t animationCount;

    RendererUiQuad quads[UI_MAX_DRAW_QUADS];
    uint32_t quadCount;
} UiContext;

static inline uint32_t UiColor(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Единая палитра HUD, меню и виджетов.
#define UI_COLOR_TEXT        UiColor(232, 236, 244, 255)
#define UI_COLOR_TEXT_DIM    UiColor(150, 158, 172, 255)
#define UI_COLOR_BUTTON      UiColor(42, 48, 62, 255)
#define UI_COLOR_BUTTON_HOT  UiColor(58, 68, 88, 255)
#define UI_COLOR_BUTTON_HELD UiColor(34, 40, 52, 255)
#define UI_COLOR_ACCENT      UiColor(108, 148, 255, 255)
#define UI_COLOR_TRACK       UiColor(30, 34, 44, 255)
#define UI_COLOR_KNOB        UiColor(232, 236, 244, 255)
#define UI_COLOR_PANEL       UiColor(22, 26, 34, 244)

// Начало кадра интерфейса: масштаб, ввод, при необходимости перепекает
// шрифт (после вызова проверить fontDirty и отдать атлас рендереру).
// false — шрифт недоступен, интерфейс рисовать нельзя.
bool UiBegin(UiContext* ui, int32_t width, int32_t height,
    float mouseX, float mouseY, bool mouseDown, bool mousePressed,
    float wheelSteps, float deltaSeconds);

// Отсечение виджетов прямоугольником (например, прокручиваемая панель).
// Пока клип активен, виджеты вне прямоугольника не рисуются и не ловят мышь.
void UiSetClip(UiContext* ui, float x, float y, float width, float height);
void UiClearClip(UiContext* ui);

void UiRelease(UiContext* ui);

// Примитивы (координаты в пикселях окна).
void UiRect(UiContext* ui, float x, float y, float width, float height,
    float cornerRadius, uint32_t color);
void UiText(UiContext* ui, float x, float lineTopY, uint32_t color,
    const wchar_t* text);
void UiTextCentered(UiContext* ui, float centerX, float lineTopY,
    uint32_t color, const wchar_t* text);
float UiTextWidth(const UiContext* ui, const wchar_t* text);

// Общие композиционные элементы. HUD и меню используют один стиль,
// поэтому тени, панели и строки значений не дублируются по модулям.
void UiPanel(UiContext* ui, float x, float y, float width, float height);
float UiLabelValueRow(UiContext* ui, float x, float width, float y,
    const wchar_t* label, const wchar_t* value, float bottomGap);

// Анимация значения к цели (для наведения и переключателей).
float UiAnimate(UiContext* ui, uint32_t id, bool towardOne);

// Виджеты: true — действие в этом кадре (нажатие/изменение значения).
bool UiButton(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* label);
bool UiSliderInt(UiContext* ui, uint32_t id, float x, float y,
    float width, int32_t minimum, int32_t maximum, int32_t* value);
bool UiToggle(UiContext* ui, uint32_t id, float x, float y, bool* value);
bool UiRadioRow(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* label, bool selected);

// Сегментные вкладки: равные сегменты по всей ширине; true — активная
// вкладка сменилась (индекс пишется в activeIndex).
bool UiSegmented(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* const* labels,
    int32_t count, int32_t* activeIndex);

// Размеры виджетов в масштабированных пикселях.
static inline float UiScaled(const UiContext* ui, float value)
{
    return value * ui->scale;
}
