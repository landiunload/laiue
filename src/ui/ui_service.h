#pragma once

/* Backend-neutral UI service.  The draw list is copied out of the module so
 * a renderer may consume it without importing the UI implementation DLL. */

#include "mod/module_api.h"
#include "graphics/graphics_api.h"

#include <stdint.h>

#define LAIUE_UI_SERVICE_ABI_VERSION_1 1u
#define LAIUE_UI_SERVICE_NAME "laiue.ui"

typedef LaiueGraphicsUiQuadV1 LaiueUiQuadV1;

typedef uint32_t(LAIUE_MODULE_CALL *LaiueUiContextCreateFn)(void **outContext);
/* Context-aware creation keeps module state instance-owned.  The original
 * contextCreate remains a compatibility entry point for clients that do not
 * need a host allocator. */
typedef uint32_t(LAIUE_MODULE_CALL *LaiueUiContextCreateWithContextFn)(
    void *moduleContext, void **outContext);
typedef void(LAIUE_MODULE_CALL *LaiueUiContextDestroyFn)(void *context);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueUiBeginFn)(
    void *context, int32_t width, int32_t height, float mouseX, float mouseY,
    uint32_t mouseDown, uint32_t mousePressed, float wheelSteps, float deltaSeconds);
typedef void(LAIUE_MODULE_CALL *LaiueUiSetClipFn)(
    void *context, float x, float y, float width, float height);
typedef void(LAIUE_MODULE_CALL *LaiueUiClearClipFn)(void *context);
typedef void(LAIUE_MODULE_CALL *LaiueUiRectFn)(
    void *context, float x, float y, float width, float height,
    float cornerRadius, uint32_t color);
typedef void(LAIUE_MODULE_CALL *LaiueUiImageFn)(
    void *context, float x, float y, float width, float height,
    float u0, float v0, float u1, float v1, uint32_t color);
typedef void(LAIUE_MODULE_CALL *LaiueUiTextUtf8Fn)(
    void *context, float x, float lineTopY, uint32_t color, const char *text);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueUiCopyDrawListFn)(
    const void *context, LaiueUiQuadV1 *destination, uint32_t capacity,
    uint32_t *outCount);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueUiGetFontAtlasFn)(
    const void *context, const uint8_t **outPixels, uint32_t *outWidth,
    uint32_t *outHeight);
typedef float(LAIUE_MODULE_CALL *LaiueUiTextWidthUtf8Fn)(
    const void *context, const char *text);

typedef struct LaiueUiServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueUiContextCreateFn contextCreate;
    LaiueUiContextDestroyFn contextDestroy;
    LaiueUiBeginFn begin;
    LaiueUiSetClipFn setClip;
    LaiueUiClearClipFn clearClip;
    LaiueUiRectFn rect;
    LaiueUiImageFn image;
    LaiueUiTextUtf8Fn textUtf8;
    LaiueUiCopyDrawListFn copyDrawList;
    LaiueUiGetFontAtlasFn getFontAtlas;
    LaiueUiTextWidthUtf8Fn textWidthUtf8;
    uintptr_t reserved[8];
    LaiueUiContextCreateWithContextFn contextCreateWithContext;
    void *context;
} LaiueUiServiceV1;

LAIUE_UI_API const LaiueModuleApiV1 *LaiueUiGetStaticModuleApiV1(void);
