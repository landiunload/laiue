#include "ui/ui.h"
#include "ui/ui_service.h"
#include "render/graphics_service.h"

#include <string.h>

_Static_assert(sizeof(LaiueUiQuadV1) == sizeof(RendererUiQuad),
               "UI service quad layout must match renderer upload layout");

static const LaiueModuleHostV1 *moduleHost;
static const LaiueGraphicsServiceV1 *graphicsService;

static uint32_t ContextCreate(void **outContext)
{
    if (moduleHost == NULL || moduleHost->allocate == NULL || outContext == NULL)
        return 0u;
    UiContext *ui = (UiContext *)moduleHost->allocate(moduleHost->context, sizeof(*ui));
    if (ui == NULL)
        return 0u;
    memset(ui, 0, sizeof(*ui));
    *outContext = ui;
    return 1u;
}

static void ContextDestroy(void *context)
{
    if (context == NULL)
        return;
    UiRelease((UiContext *)context);
    if (moduleHost != NULL && moduleHost->free != NULL)
        moduleHost->free(moduleHost->context, context);
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

static const LaiueUiServiceV1 service = {
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
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    moduleHost = host;
    graphicsService = NULL;
    *outContext = (void *)&moduleHost;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    (void)context;
    if (moduleHost == NULL || moduleHost->queryService == NULL)
        return 0u;
    graphicsService = NULL;
    uint32_t version = 0u;
    uint32_t size = 0u;
    graphicsService = (const LaiueGraphicsServiceV1 *)moduleHost->queryService(
        moduleHost->context, LAIUE_GRAPHICS_SERVICE_NAME,
        LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1, sizeof(LaiueGraphicsServiceV1),
        &version, &size);
    if (graphicsService == NULL || version < LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1 ||
        size < sizeof(*graphicsService))
    {
        graphicsService = NULL;
        return 0u;
    }
    LaiueModuleServiceV1 published = {
        .name = LAIUE_UI_SERVICE_NAME,
        .version = LAIUE_UI_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (moduleHost->publishService(moduleHost->context, &published) != LAIUE_MODULE_OK)
    {
        graphicsService = NULL;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    (void)context;
    if (moduleHost != NULL && moduleHost->unpublishService != NULL)
        (void)moduleHost->unpublishService(moduleHost->context, LAIUE_UI_SERVICE_NAME);
    graphicsService = NULL;
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleHost = NULL;
    graphicsService = NULL;
}

static const char *const provides[] = {LAIUE_UI_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_GRAPHICS_SERVICE_NAME, LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1},
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
