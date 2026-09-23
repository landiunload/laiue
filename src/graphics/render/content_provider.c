#include "render/content_provider.h"

#include <stddef.h>

static const LaiueContentServiceV1 *g_contentService;

static bool HasField(const LaiueContentServiceV1 *service, size_t offset,
                     size_t size)
{
    return service != NULL && service->structSize >= offset + size;
}

static const LaiueContentServiceV1 *ValidService(void)
{
    const LaiueContentServiceV1 *service = g_contentService;
    if (service == NULL || service->abiVersion != LAIUE_CONTENT_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueContentServiceV1, createCatalog) +
                                  sizeof(service->createCatalog))
        return NULL;
    return service;
}

void RendererSetContentService(const LaiueContentServiceV1 *service)
{
    if (service == NULL || service->abiVersion != LAIUE_CONTENT_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueContentServiceV1, createCatalog) +
                                  sizeof(service->createCatalog))
        g_contentService = NULL;
    else
        g_contentService = service;
}

const LaiueContentServiceV1 *RendererGetContentService(void)
{
    return ValidService();
}

LaiueContentCatalog *RendererContentDefaultCatalog(void)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
                   HasField(service, offsetof(LaiueContentServiceV1, defaultCatalog),
                            sizeof(service->defaultCatalog)) &&
                   service->defaultCatalog != NULL
               ? service->defaultCatalog()
               : NULL;
}

bool RendererContentCatalogEnumerate(
    LaiueContentCatalog *catalog, uint32_t type, LaiueContentList *outList)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, enumerate),
                    sizeof(service->enumerate)) &&
           service->enumerate != NULL && catalog != NULL && outList != NULL &&
           service->enumerate(catalog, type, outList) != 0u;
}

void RendererContentListRelease(LaiueContentList *list)
{
    const LaiueContentServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueContentServiceV1, releaseList),
                 sizeof(service->releaseList)) &&
        service->releaseList != NULL && list != NULL)
        service->releaseList(list);
}

bool RendererContentCatalogSetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, setActivePack),
                    sizeof(service->setActivePack)) &&
           service->setActivePack != NULL && catalog != NULL &&
           service->setActivePack(catalog, type, name) != 0u;
}

bool RendererContentCatalogGetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, wchar_t *destination,
    uint32_t capacity)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, getActivePack),
                    sizeof(service->getActivePack)) &&
           service->getActivePack != NULL && catalog != NULL &&
           destination != NULL && capacity != 0u &&
           service->getActivePack(catalog, type, destination, capacity) != 0u;
}

bool RendererContentCatalogBuildPath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *childName, wchar_t *destination, uint32_t capacity)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, buildPath),
                    sizeof(service->buildPath)) &&
           service->buildPath != NULL && catalog != NULL && destination != NULL &&
           capacity != 0u &&
           service->buildPath(catalog, type, name, childName, destination, capacity) != 0u;
}

bool RendererContentCatalogBuildResourcePath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *resourcePath, const wchar_t *extension,
    wchar_t *destination, uint32_t capacity)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, buildResourcePath),
                    sizeof(service->buildResourcePath)) &&
           service->buildResourcePath != NULL && catalog != NULL && destination != NULL &&
           capacity != 0u &&
           service->buildResourcePath(catalog, type, name, resourcePath, extension,
                                      destination, capacity) != 0u;
}

uint32_t RendererContentCatalogOrderFormats(
    LaiueContentCatalog *catalog, uint32_t type,
    const wchar_t *const *defaults, uint32_t defaultCount,
    const wchar_t **outOrder, uint32_t capacity)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
                   HasField(service, offsetof(LaiueContentServiceV1, orderFormats),
                            sizeof(service->orderFormats)) &&
                   service->orderFormats != NULL && catalog != NULL &&
                   defaults != NULL && outOrder != NULL && capacity != 0u
               ? service->orderFormats(catalog, type, defaults, defaultCount,
                                       outOrder, capacity)
               : 0u;
}

bool RendererContentPathIsSafe(const wchar_t *path)
{
    const LaiueContentServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueContentServiceV1, pathIsSafe),
                    sizeof(service->pathIsSafe)) &&
           service->pathIsSafe != NULL && path != NULL &&
           service->pathIsSafe(path) != 0u;
}
