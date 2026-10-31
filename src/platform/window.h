#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Непрозрачный дескриптор окна: всё состояние живёт внутри реализации,
// поэтому окон может быть сколько угодно и глобальных переменных нет.
typedef struct Window Window;

/* Native surface payload used by non-Win32 presentation providers.  On
 * Windows WindowGetNativeHandle returns the HWND directly; on X11 it returns
 * a pointer to this pair so a Vulkan provider can create an Xlib surface
 * without importing the window implementation DLL. */
typedef struct LaiueWindowNativeHandleV1
{
    void *display;
    uintptr_t window;
} LaiueWindowNativeHandleV1;

typedef struct WindowConfiguration
{
    const wchar_t* title;
    int32_t width;
    int32_t height;
} WindowConfiguration;

typedef void (*FrameCallback)(void* userData);
typedef void (*RawInputCallback)(void* userData, void* rawInputHandle);
// available == false: поверхность окна уходит, и всё созданное из
// WindowGetNativeHandle (Vulkan surface, swapchain) нужно отдать до
// возврата из callback-а. available == true: появилась новая поверхность,
// WindowGetNativeHandle уже возвращает её.
typedef void (*SurfaceCallback)(void *userData, bool available);

LAIUE_WINDOW_API Window* WindowCreate(const WindowConfiguration* configuration);
LAIUE_WINDOW_API void    WindowDestroy(Window* window);
LAIUE_WINDOW_API void*   WindowGetNativeHandle(const Window* window);
// Passing NULL as callback unregisters the current callback and user data.
LAIUE_WINDOW_API void    WindowSetRawInputCallback(Window* window, RawInputCallback callback, void* userData);
LAIUE_WINDOW_API void    WindowGetClientSize(const Window* window, int32_t* width, int32_t* height);
LAIUE_WINDOW_API bool    WindowConsumeResize(Window* window);
// true один раз после того, как окно лишилось ввода: ушло из фокуса или
// отдало мышь и клавиатуру системе (на Windows — пока окно перетаскивают,
// меняют его размер или открыто его меню). Отпускания клавиш за это время
// не приходят, поэтому потребитель сбрасывает состояние ввода.
LAIUE_WINDOW_API bool    WindowConsumeFocusLoss(Window* window);
// Вызывает onFrame, пока окно не закроют. На Windows кадры идут и пока окно
// перетаскивают или меняют его размер: тогда onFrame вызывается из оконной
// процедуры по таймеру.
LAIUE_WINDOW_API void    WindowRunLoop(Window* window, FrameCallback onFrame, void* userData);
LAIUE_WINDOW_API void    WindowSetMouseLook(Window* window, bool enabled);
LAIUE_WINDOW_API bool    WindowIsMouseLookEnabled(const Window* window);
// Позиция курсора в клиентских координатах окна (для интерфейса);
// вне окна значения могут выходить за его пределы.
LAIUE_WINDOW_API void    WindowGetCursorClientPosition(const Window* window,
    int32_t* x, int32_t* y);
LAIUE_WINDOW_API void    WindowRequestClose(Window* window);

// Накопленные с прошлого вызова щелчки колеса мыши (положительные — от себя).
LAIUE_WINDOW_API float   WindowConsumeMouseWheelSteps(Window* window);

// Набранный с прошлого вызова текст: до capacity кодовых точек Unicode в
// порядке набора, управляющие символы (Enter, Backspace, Tab) не входят.
// Возвращает число записанных; лишнее остаётся до следующего вызова.
LAIUE_WINDOW_API uint32_t WindowConsumeText(Window* window, uint32_t* codepoints,
    uint32_t capacity);

// Полноэкранный режим без рамки на текущем мониторе. Обычный оконный
// режим восстанавливает прежние стиль и положение окна. На Windows и X11
// его переключает и Alt+Enter.
LAIUE_WINDOW_API void    WindowSetFullscreen(Window* window, bool enabled);
LAIUE_WINDOW_API bool    WindowIsFullscreen(const Window* window);

// Поверхность, которая приходит и уходит, пока программа работает: на
// Android её отнимает пауза активности. Вызывается изнутри WindowRunLoop и
// никогда — на настольных системах, где поверхность живёт вместе с окном.
// NULL снимает callback.
LAIUE_WINDOW_API void WindowSetSurfaceCallback(Window *window, SurfaceCallback callback,
                                               void *userData);

// Пикселей окна на точку интерфейса: размеры, заданные в точках (кнопка в
// 44 точки), умножаются на это число. 1 — обычный экран в 96 точек на дюйм
// (160 на Android); на плотном экране телефона — около 3. Меняется вместе с
// экраном: читать каждый кадр.
LAIUE_WINDOW_API float WindowGetContentScale(const Window *window);

// Как программа расходует энергию, общее для процесса.
// AUTO: пока программа в фоне, процесс в режиме эффективности Windows (как у
// переключателя в диспетчере задач: EcoQoS и низкий приоритет), а на переднем
// плане QoS выбирает система. EFFICIENCY: режим эффективности всегда.
// PERFORMANCE: никогда, и система не замедляет процесс даже в фоне.
#define WINDOW_POWER_AUTO 0u
#define WINDOW_POWER_EFFICIENCY 1u
#define WINDOW_POWER_PERFORMANCE 2u

// true — система применяет политику. Такой механизм есть только у Windows;
// на других системах политика запоминается, но ничего не меняет.
// Неизвестное значение даёт false и оставляет прежнюю политику.
LAIUE_WINDOW_API bool WindowSetPowerPolicy(uint32_t policy);
LAIUE_WINDOW_API uint32_t WindowGetPowerPolicy(void);
