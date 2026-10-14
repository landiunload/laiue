#pragma once

#include "platform/window.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_WINDOW_SERVICE_NAME "laiue.window"
#define LAIUE_WINDOW_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueWindowServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    Window *(*create)(const WindowConfiguration *configuration);
    void (*destroy)(Window *window);
    void *(*getNativeHandle)(const Window *window);
    void (*setRawInputCallback)(Window *window, RawInputCallback callback,
                                void *userData);
    void (*getClientSize)(const Window *window, int32_t *width, int32_t *height);
    bool (*consumeResize)(Window *window);
    bool (*consumeFocusLoss)(Window *window);
    void (*runLoop)(Window *window, FrameCallback onFrame, void *userData);
    void (*setMouseLook)(Window *window, bool enabled);
    bool (*isMouseLookEnabled)(const Window *window);
    void (*getCursorClientPosition)(const Window *window, int32_t *x, int32_t *y);
    void (*requestClose)(Window *window);
    float (*consumeMouseWheelSteps)(Window *window);
    void (*setFullscreen)(Window *window, bool enabled);
    bool (*isFullscreen)(const Window *window);
} LaiueWindowServiceV1;

const LaiueModuleApiV1 *LaiueWindowGetStaticModuleApiV1(void);
