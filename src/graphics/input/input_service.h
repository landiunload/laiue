#pragma once

#include "input/input.h"
#include "mod/module_api.h"

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
} LaiueInputServiceV1;

const LaiueModuleApiV1 *LaiueInputGetStaticModuleApiV1(void);
