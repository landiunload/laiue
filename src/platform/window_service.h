#pragma once

#include "platform/window.h"
#include "mod/module_api.h"

#include <stddef.h>
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
    /* Optional tail: check LAIUE_WINDOW_SERVICE_V1_TEXT_SIZE. Text typed
     * since the last call, as WindowConsumeText. */
    uint32_t (*consumeText)(Window *window, uint32_t *codepoints, uint32_t capacity);
    /* Optional tail: check LAIUE_WINDOW_SERVICE_V1_SURFACE_SIZE. Surface
     * loss and return, as WindowSetSurfaceCallback. */
    void (*setSurfaceCallback)(Window *window, SurfaceCallback callback, void *userData);
    /* Optional tail: check LAIUE_WINDOW_SERVICE_V1_SCALE_SIZE. Window
     * pixels per interface point, as WindowGetContentScale. */
    float (*getContentScale)(const Window *window);
} LaiueWindowServiceV1;

/* The service before the text tail, with it, with the surface tail and with
 * the content scale. */
#define LAIUE_WINDOW_SERVICE_V1_LEGACY_SIZE                                                  \
    ((uint32_t)offsetof(LaiueWindowServiceV1, consumeText))
#define LAIUE_WINDOW_SERVICE_V1_TEXT_SIZE                                                          \
    ((uint32_t)offsetof(LaiueWindowServiceV1, setSurfaceCallback))
#define LAIUE_WINDOW_SERVICE_V1_SURFACE_SIZE                                                       \
    ((uint32_t)offsetof(LaiueWindowServiceV1, getContentScale))
#define LAIUE_WINDOW_SERVICE_V1_SCALE_SIZE ((uint32_t)sizeof(LaiueWindowServiceV1))

const LaiueModuleApiV1 *LaiueWindowGetStaticModuleApiV1(void);
