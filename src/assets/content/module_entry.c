#include "content/content_service.h"

typedef struct LaiueContentModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueContentModuleState;

static LaiueContentModuleState moduleState;

static uint32_t GetRoot(
    LaiueContentCatalog *catalog, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogGetRoot(catalog, destination, capacity) ? 1u : 0u;
}

static uint32_t SetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name)
{
    return LaiueContentCatalogSetActivePack(catalog, (LaiueContentType)type, name) ? 1u : 0u;
}

static uint32_t GetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogGetActivePack(
               catalog, (LaiueContentType)type, destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t BuildPath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *childName, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogBuildPath(
               catalog, (LaiueContentType)type, name, childName, destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t BuildResourcePath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *resourcePath, const wchar_t *extension,
    wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogBuildResourcePath(catalog, (LaiueContentType)type, name,
                                                resourcePath, extension, destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t Enumerate(
    LaiueContentCatalog *catalog, uint32_t type, LaiueContentList *outList)
{
    return LaiueContentCatalogEnumerate(catalog, (LaiueContentType)type, outList) ? 1u : 0u;
}

static void ReleaseList(LaiueContentList *list)
{
    LaiueContentListRelease(list);
}

static uint32_t OrderFormats(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *const *defaults,
    uint32_t defaultCount, const wchar_t **outOrder, uint32_t capacity)
{
    return LaiueContentCatalogOrderFormats(catalog, (LaiueContentType)type, defaults,
                                            defaultCount, outOrder, capacity);
}

static uint32_t NameIsSafe(const wchar_t *name)
{
    return LaiueContentNameIsSafe(name) ? 1u : 0u;
}

static uint32_t PathIsSafe(const wchar_t *path)
{
    return LaiueContentPathIsSafe(path) ? 1u : 0u;
}

static const LaiueContentServiceV1 service = {
    .structSize = sizeof(LaiueContentServiceV1),
    .abiVersion = LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
    .createCatalog = LaiueContentCatalogCreate,
    .destroyCatalog = LaiueContentCatalogDestroy,
    .getRoot = GetRoot,
    .setActivePack = SetActivePack,
    .getActivePack = GetActivePack,
    .buildPath = BuildPath,
    .buildResourcePath = BuildResourcePath,
    .enumerate = Enumerate,
    .releaseList = ReleaseList,
    .orderFormats = OrderFormats,
    .nameIsSafe = NameIsSafe,
    .pathIsSafe = PathIsSafe,
    .defaultCatalog = LaiueContentCatalogDefault,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL)
        return 0u;
    moduleState.host = host;
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueContentModuleState *state = (LaiueContentModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_CONTENT_SERVICE_NAME,
        .version = LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    LaiueContentModuleState *state = (LaiueContentModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_CONTENT_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
}

static const char *const provides[] = {LAIUE_CONTENT_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.content",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueContentGetStaticModuleApiV1(void)
{
    return &api;
}

const LaiueContentServiceV1 *LaiueContentGetStaticServiceV1(void)
{
    return &service;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
