#include "ui/ui.h"
#include "ui/ui_service.h"
#include "graphics/graphics_device_service.h"
#include "platform/system.h"

#include <string.h>

_Static_assert(sizeof(LaiueUiQuadV1) == sizeof(LaiueGraphicsUiQuadV1),
               "UI service quad layout must match graphics upload layout");

typedef struct UiModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueGraphicsDeviceServiceV1 *graphics;
    LaiueUiServiceV1 service;
} UiModuleState;

/* The public UI context remains layout-compatible with UiContext.  The
 * ownership trailer lets both compatibility and context-aware creation free
 * the same opaque pointer without a DLL-global allocator or host pointer. */
typedef struct UiOwnedContext
{
    UiContext ui;
    LaiueModuleFreeFn freeFn;
    void *freeContext;
} UiOwnedContext;

static uint32_t ContextCreate(void **outContext)
{
    if (outContext == NULL)
        return 0u;
    UiOwnedContext *owned =
        (UiOwnedContext *)PlatformAllocate(sizeof(*owned), true);
    if (owned == NULL)
        return 0u;
    *outContext = &owned->ui;
    return 1u;
}

static uint32_t ContextCreateWithContext(void *moduleContext, void **outContext)
{
    UiModuleState *state = (UiModuleState *)moduleContext;
    if (state == NULL || state->host == NULL || state->host->allocate == NULL ||
        state->host->free == NULL || outContext == NULL)
        return 0u;
    UiOwnedContext *owned = (UiOwnedContext *)state->host->allocate(
        state->host->context, sizeof(*owned));
    if (owned == NULL)
        return 0u;
    memset(owned, 0, sizeof(*owned));
    owned->freeFn = state->host->free;
    owned->freeContext = state->host->context;
    *outContext = &owned->ui;
    return 1u;
}

static void ContextDestroy(void *context)
{
    if (context == NULL)
        return;
    UiOwnedContext *owned = (UiOwnedContext *)context;
    UiRelease(&owned->ui);
    if (owned->freeFn != NULL)
        owned->freeFn(owned->freeContext, owned);
    else
        PlatformFree(owned);
}

static uint32_t Begin(void *context, int32_t width, int32_t height, float mouseX, float mouseY,
                      uint32_t mouseDown, uint32_t mousePressed, float wheelSteps,
                      float deltaSeconds)
{
    return UiBegin((UiContext *)context, width, height, mouseX, mouseY, mouseDown != 0u,
                   mousePressed != 0u, wheelSteps, deltaSeconds)
               ? 1u
               : 0u;
}

static void SetClip(void *context, float x, float y, float width, float height)
{
    UiSetClip((UiContext *)context, x, y, width, height);
}

static void ClearClip(void *context)
{
    UiClearClip((UiContext *)context);
}

static void Rect(void *context, float x, float y, float width, float height, float cornerRadius,
                 uint32_t color)
{
    UiRect((UiContext *)context, x, y, width, height, cornerRadius, color);
}

static void Image(void *context, float x, float y, float width, float height, float u0, float v0,
                  float u1, float v1, uint32_t color)
{
    UiImage((UiContext *)context, x, y, width, height, u0, v0, u1, v1, color);
}

static uint32_t DecodeUtf8(const char *text, wchar_t *destination, uint32_t capacity)
{
    if (destination == NULL || capacity == 0u)
        return 0u;
    uint32_t write = 0u;
    while (text != NULL && *text != '\0' && write + 1u < capacity)
    {
        uint32_t codepoint = 0xfffdu;
        uint8_t first = (uint8_t)*text++;
        uint32_t needed = 0u;
        if (first < 0x80u)
        {
            codepoint = first;
        }
        else if ((first & 0xe0u) == 0xc0u)
        {
            codepoint = first & 0x1fu;
            needed = 1u;
        }
        else if ((first & 0xf0u) == 0xe0u)
        {
            codepoint = first & 0x0fu;
            needed = 2u;
        }
        else if ((first & 0xf8u) == 0xf0u)
        {
            codepoint = first & 0x07u;
            needed = 3u;
        }
        else
        {
            needed = UINT32_MAX;
        }
        if (needed != UINT32_MAX)
        {
            for (uint32_t index = 0u; index < needed; ++index)
            {
                uint8_t continuation = (uint8_t)text[index];
                if ((continuation & 0xc0u) != 0x80u)
                {
                    needed = UINT32_MAX;
                    break;
                }
                codepoint = (codepoint << 6u) | (continuation & 0x3fu);
            }
            if (needed != UINT32_MAX)
                text += needed;
        }
        if (needed == UINT32_MAX || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu))
            codepoint = 0xfffdu;
#if WCHAR_MAX >= 0x10ffff
        destination[write++] = (wchar_t)codepoint;
#else
        if (codepoint > 0xffffu)
        {
            if (write + 2u >= capacity)
                break;
            codepoint -= 0x10000u;
            destination[write++] = (wchar_t)(0xd800u + (codepoint >> 10u));
            destination[write++] = (wchar_t)(0xdc00u + (codepoint & 0x3ffu));
        }
        else
        {
            destination[write++] = (wchar_t)codepoint;
        }
#endif
    }
    destination[write] = L'\0';
    return write;
}

static void TextUtf8(void *context, float x, float lineTopY, uint32_t color, const char *text)
{
    wchar_t converted[512];
    DecodeUtf8(text, converted, sizeof(converted) / sizeof(converted[0]));
    UiText((UiContext *)context, x, lineTopY, color, converted);
}

static uint32_t CopyDrawList(const void *context, LaiueUiQuadV1 *destination, uint32_t capacity,
                             uint32_t *outCount)
{
    const UiContext *ui = (const UiContext *)context;
    if (outCount == NULL)
        return 0u;
    *outCount = 0u;
    if (ui == NULL || (capacity != 0u && destination == NULL))
        return 0u;
    uint32_t count = ui->quadCount < capacity ? ui->quadCount : capacity;
    if (count != 0u)
        memcpy(destination, ui->quads, (size_t)count * sizeof(*destination));
    *outCount = count;
    return count == ui->quadCount ? 1u : 0u;
}

static uint32_t GetFontAtlas(const void *context, const uint8_t **outPixels, uint32_t *outWidth,
                             uint32_t *outHeight)
{
    const UiContext *ui = (const UiContext *)context;
    if (outPixels == NULL || outWidth == NULL || outHeight == NULL || ui == NULL)
        return 0u;
    *outPixels = ui->font.atlas;
    *outWidth = ui->font.atlasWidth;
    *outHeight = ui->font.atlasHeight;
    return ui->font.atlas != NULL ? 1u : 0u;
}

static float TextWidthUtf8(const void *context, const char *text)
{
    wchar_t converted[512];
    DecodeUtf8(text, converted, sizeof(converted) / sizeof(converted[0]));
    return UiTextWidth((const UiContext *)context, converted);
}

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    UiModuleState *state = (UiModuleState *)host->allocate(host->context,
                                                            sizeof(*state));
    if (state == NULL)
        return 0u;
    memset(state, 0, sizeof(*state));
    state->host = host;
    state->service = (LaiueUiServiceV1){
        .structSize = sizeof(LaiueUiServiceV1),
        .abiVersion = LAIUE_UI_SERVICE_ABI_VERSION_1,
        .contextCreate = ContextCreate,
        .contextDestroy = ContextDestroy,
        .begin = Begin,
        .setClip = SetClip,
        .clearClip = ClearClip,
        .rect = Rect,
        .image = Image,
        .textUtf8 = TextUtf8,
        .copyDrawList = CopyDrawList,
        .getFontAtlas = GetFontAtlas,
        .textWidthUtf8 = TextWidthUtf8,
        .contextCreateWithContext = ContextCreateWithContext,
        .context = state,
    };
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    UiModuleState *state = (UiModuleState *)context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return 0u;
    state->graphics = NULL;
    uint32_t version = 0u;
    uint32_t size = 0u;
    state->graphics = (const LaiueGraphicsDeviceServiceV1 *)state->host->queryService(
        state->host->context, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
        LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V1_LEGACY_SIZE,
        &version, &size);
    if (state->graphics == NULL ||
        version < LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1 ||
        size < LAIUE_GRAPHICS_DEVICE_SERVICE_V1_LEGACY_SIZE)
    {
        state->graphics = NULL;
        return 0u;
    }
    LaiueModuleServiceV1 published = {
        .name = LAIUE_UI_SERVICE_NAME,
        .version = LAIUE_UI_SERVICE_ABI_VERSION_1,
        .table = &state->service,
        .tableSize = sizeof(state->service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->graphics = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    UiModuleState *state = (UiModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_UI_SERVICE_NAME);
    if (state != NULL)
        state->graphics = NULL;
}

static void ModuleDestroy(void *context)
{
    UiModuleState *state = (UiModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        state->graphics = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
        else
            PlatformFree(state);
    }
}

static const char *const provides[] = {LAIUE_UI_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
     LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.ui",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = sizeof(requiresServices) / sizeof(requiresServices[0]),
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueUiGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
