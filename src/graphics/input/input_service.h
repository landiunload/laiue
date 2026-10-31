#pragma once

#include "input/input.h"
#include "mod/module_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_INPUT_SERVICE_NAME "laiue.input"
#define LAIUE_INPUT_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueInputServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    Input *(*create)(void *windowNativeHandle);
    void (*destroy)(Input *input);
    void (*handleRawInput)(Input *input, void *rawInputHandle);
    void (*endFrame)(Input *input);
    void (*resetState)(Input *input);
    bool (*isKeyDown)(const Input *input, InputKey key);
    bool (*wasKeyPressed)(const Input *input, InputKey key);
    bool (*consumeKeyPress)(Input *input, InputKey key);
    bool (*isMouseButtonDown)(const Input *input, InputMouseButton button);
    bool (*wasMouseButtonPressed)(const Input *input, InputMouseButton button);
    void (*getMouseDelta)(const Input *input, int32_t *deltaX, int32_t *deltaY);
    /* Optional tail: check LAIUE_INPUT_SERVICE_V1_TOUCH_SIZE. Touches and the
     * two-finger gesture, as InputGetTouches and InputGetGesture. */
    uint32_t (*getTouches)(const Input *input, InputTouch *touches, uint32_t capacity);
    bool (*getGesture)(const Input *input, InputGesture *gesture);
} LaiueInputServiceV1;

/* The service before the touch tail, and with it. */
#define LAIUE_INPUT_SERVICE_V1_LEGACY_SIZE ((uint32_t)offsetof(LaiueInputServiceV1, getTouches))
#define LAIUE_INPUT_SERVICE_V1_TOUCH_SIZE ((uint32_t)sizeof(LaiueInputServiceV1))

const LaiueModuleApiV1 *LaiueInputGetStaticModuleApiV1(void);
