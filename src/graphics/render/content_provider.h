#pragma once

/* Runtime bridge from the graphics module to the asset/content provider.
 *
 * The renderer keeps only this service table.  It never imports the content
 * module, so a graphics DLL can be shipped without an asset implementation
 * and report an unavailable content path at the call site.  The module host
 * installs the table for the duration of the graphics module lifetime; direct
 * embedders may do the same before using pack helpers.
 */

#include "content/content_service.h"

#include <stdbool.h>
#include <stdint.h>

LAIUE_RENDER_API void RendererSetContentService(
    const LaiueContentServiceV1 *service);
LAIUE_RENDER_API const LaiueContentServiceV1 *RendererGetContentService(void);

/* The old renderer helpers use a process-default bridge.  Dynamic provider
 * instances acquire it with their host state as owner so a second host cannot
 * clear or replace the first host's content service during rollback. */
bool RendererTryAcquireContentService(const void *owner,
                                      const LaiueContentServiceV1 *service);
void RendererReleaseContentService(const void *owner);

LaiueContentCatalog *RendererContentDefaultCatalog(void);
bool RendererContentCatalogEnumerate(
    LaiueContentCatalog *catalog, uint32_t type, LaiueContentList *outList);
void RendererContentListRelease(LaiueContentList *list);
bool RendererContentCatalogSetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name);
bool RendererContentCatalogGetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, wchar_t *destination,
    uint32_t capacity);
bool RendererContentCatalogBuildPath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *childName, wchar_t *destination, uint32_t capacity);
bool RendererContentCatalogBuildResourcePath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *resourcePath, const wchar_t *extension,
    wchar_t *destination, uint32_t capacity);
uint32_t RendererContentCatalogOrderFormats(
    LaiueContentCatalog *catalog, uint32_t type,
    const wchar_t *const *defaults, uint32_t defaultCount,
    const wchar_t **outOrder, uint32_t capacity);
bool RendererContentPathIsSafe(const wchar_t *path);
