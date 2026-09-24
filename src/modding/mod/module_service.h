#pragma once

/* Small header-only helper for module entrypoints. Dependency resolution is
 * performed once during start; callers keep the returned table for the
 * module lifetime and never perform string lookup in a hot path. */

#include "mod/module_api.h"

#include <stdbool.h>

typedef struct LaiueModuleServiceViewV1
{
    const void *table;
    uint32_t version;
    uint32_t tableSize;
} LaiueModuleServiceViewV1;

static inline bool LaiueModuleQueryServiceView(
    const LaiueModuleHostV1 *host, const char *name,
    uint32_t minimumVersion, uint32_t minimumSize,
    LaiueModuleServiceViewV1 *outView)
{
    if (outView != NULL)
        *outView = (LaiueModuleServiceViewV1){0};
    if (host == NULL || host->queryService == NULL || name == NULL ||
        minimumVersion == 0u || minimumSize == 0u || outView == NULL)
        return false;
    outView->table = host->queryService(host->context, name, minimumVersion,
                                        minimumSize, &outView->version,
                                        &outView->tableSize);
    return outView->table != NULL && outView->version >= minimumVersion &&
           outView->tableSize >= minimumSize;
}

static inline const void *LaiueModuleQueryRequiredService(
    const LaiueModuleHostV1 *host, const char *name,
    uint32_t minimumVersion, uint32_t minimumSize)
{
    LaiueModuleServiceViewV1 view;
    if (!LaiueModuleQueryServiceView(host, name, minimumVersion, minimumSize, &view))
        return NULL;
    return view.table;
}

/* Optional services deliberately have the same validation as required ones,
 * but absence is a supported result. The host may defer a consumer when a
 * selected provider is still pending; once the graph starts, this helper is
 * a single lookup retained for the module lifetime. */
static inline const void *LaiueModuleQueryOptionalService(
    const LaiueModuleHostV1 *host, const char *name,
    uint32_t minimumVersion, uint32_t minimumSize)
{
    LaiueModuleServiceViewV1 view;
    if (!LaiueModuleQueryServiceView(host, name, minimumVersion, minimumSize, &view))
        return NULL;
    return view.table;
}
