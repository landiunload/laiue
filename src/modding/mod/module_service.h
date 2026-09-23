#pragma once

/* Small header-only helper for module entrypoints. Dependency resolution is
 * performed once during start; callers keep the returned table for the
 * module lifetime and never perform string lookup in a hot path. */

#include "mod/module_api.h"

static inline const void *LaiueModuleQueryRequiredService(
    const LaiueModuleHostV1 *host, const char *name,
    uint32_t minimumVersion, uint32_t minimumSize)
{
    if (host == NULL || host->queryService == NULL || name == NULL ||
        minimumVersion == 0u || minimumSize == 0u)
        return NULL;

    uint32_t version = 0u;
    uint32_t size = 0u;
    const void *table = host->queryService(
        host->context, name, minimumVersion, minimumSize, &version, &size);
    return table != NULL && version >= minimumVersion && size >= minimumSize
               ? table
               : NULL;
}

/* Optional services deliberately have the same validation as required ones,
 * but absence is a supported result. The host may defer a consumer when a
 * selected provider is still pending; once the graph starts, this helper is
 * a single lookup retained for the module lifetime. */
static inline const void *LaiueModuleQueryOptionalService(
    const LaiueModuleHostV1 *host, const char *name,
    uint32_t minimumVersion, uint32_t minimumSize)
{
    if (host == NULL || host->queryService == NULL || name == NULL ||
        minimumVersion == 0u || minimumSize == 0u)
        return NULL;

    uint32_t version = 0u;
    uint32_t size = 0u;
    const void *table = host->queryService(
        host->context, name, minimumVersion, minimumSize, &version, &size);
    return table != NULL && version >= minimumVersion && size >= minimumSize
               ? table
               : NULL;
}
