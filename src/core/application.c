#include "api.h"
#include "platform/window.h"
#include "input/input.h"

#include <stddef.h>

typedef struct ApplicationState
{
    Window* window;
    Input* input;
} ApplicationState;

// Адаптер сигнатуры: окно передаёт userData + HRAWINPUT,
// модуль ввода принимает типизированный Input*.
static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static void OnFrame(void* userData)
{
    ApplicationState* application = userData;

    if (InputWasKeyPressed(application->input, INPUT_KEY_ESCAPE))
    {
        WindowRequestClose(application->window);
        return;
    }

    // F7 — включить mouse look, Shift+F7 — выключить.
    if (InputWasKeyPressed(application->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(application->window, !InputIsKeyDown(application->input, INPUT_KEY_SHIFT));
    }

    if (WindowConsumeResize(application->window))
    {
        int32_t clientWidth;
        int32_t clientHeight;
        WindowGetClientSize(application->window, &clientWidth, &clientHeight);
        // Здесь будет пересоздание render-таргетов под новый размер.
        (void)clientWidth;
        (void)clientHeight;
    }

    InputEndFrame(application->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title  = L"laiue",
        .width  = 1280,
        .height = 720,
    };

    Window* window = WindowCreate(&windowConfiguration);
    if (window == NULL)
    {
        return;
    }

    Input* input = InputCreate(WindowGetNativeHandle(window));
    if (input == NULL)
    {
        WindowDestroy(window);
        return;
    }

    ApplicationState application = {
        .window = window,
        .input  = input,
    };

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, &application);

    InputDestroy(input);
    WindowDestroy(window);
}
