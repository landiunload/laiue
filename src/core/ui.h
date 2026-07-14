#pragma once

#include "core/ui_font.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Immediate-mode интерфейс (внутренний компонент ядра): виджеты каждый
// кадр заново собирают список квадов для RendererUiQueue. Состояние
// между кадрами — только анимации наведения и захват мыши слайдером.

#define UI_MAX_DRAW_QUADS 1024
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
    float deltaSeconds;

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

// Начало кадра интерфейса: масштаб, ввод, при необходимости перепекает
// шрифт (после вызова проверить fontDirty и отдать атлас рендереру).
// false — шрифт недоступен, интерфейс рисовать нельзя.
bool UiBegin(UiContext* ui, int32_t width, int32_t height,
    float mouseX, float mouseY, bool mouseDown, bool mousePressed,
    float deltaSeconds);

void UiRelease(UiContext* ui);

// Примитивы (координаты в пикселях окна).
void UiRect(UiContext* ui, float x, float y, float width, float height,
    float cornerRadius, uint32_t color);
void UiText(UiContext* ui, float x, float lineTopY, uint32_t color,
    const wchar_t* text);
void UiTextCentered(UiContext* ui, float centerX, float lineTopY,
    uint32_t color, const wchar_t* text);
float UiTextWidth(const UiContext* ui, const wchar_t* text);

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

// Размеры виджетов в масштабированных пикселях.
static inline float UiScaled(const UiContext* ui, float value)
{
    return value * ui->scale;
}
