#pragma once

/* Asset/content catalog provider. Pack formats remain an implementation
 * detail; applications receive only an opaque catalog and bounded path API. */

#include "api.h"
#include "content/content_catalog.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_CONTENT_SERVICE_ABI_VERSION_1 1u
#define LAIUE_CONTENT_SERVICE_NAME "laiue.assets"

typedef LaiueContentCatalog *(*LaiueContentCreateCatalogFn)(const wchar_t *rootDirectory);
typedef void (*LaiueContentDestroyCatalogFn)(LaiueContentCatalog *catalog);
typedef uint32_t (*LaiueContentGetRootFn)(
    LaiueContentCatalog *catalog, wchar_t *destination, uint32_t capacity);
typedef uint32_t (*LaiueContentSetActivePackFn)(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name);
typedef uint32_t (*LaiueContentGetActivePackFn)(
    LaiueContentCatalog *catalog, uint32_t type, wchar_t *destination, uint32_t capacity);
typedef uint32_t (*LaiueContentBuildPathFn)(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *childName, wchar_t *destination, uint32_t capacity);
typedef uint32_t (*LaiueContentBuildResourcePathFn)(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *resourcePath, const wchar_t *extension,
    wchar_t *destination, uint32_t capacity);

typedef struct LaiueContentServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueContentCreateCatalogFn createCatalog;
    LaiueContentDestroyCatalogFn destroyCatalog;
    LaiueContentGetRootFn getRoot;
    LaiueContentSetActivePackFn setActivePack;
    LaiueContentGetActivePackFn getActivePack;
    LaiueContentBuildPathFn buildPath;
    LaiueContentBuildResourcePathFn buildResourcePath;
    uintptr_t reserved[8];
} LaiueContentServiceV1;

LAIUE_CONTENT_API const LaiueModuleApiV1 *LaiueContentGetStaticModuleApiV1(void);
