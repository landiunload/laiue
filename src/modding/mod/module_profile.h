#pragma once

#include "mod/module_host.h"

#include <stdint.h>

#define LAIUE_MODULE_PROFILE_TEXT_CAPACITY 8192u
#define LAIUE_MODULE_PROFILE_PATH_CAPACITY 1024u

/* Caller-owned storage for the small text profile used by examples and
 * packaging tools.  Parsing is metadata-only: it does not open a library or
 * execute a module. */
typedef struct LaiueModuleProfileStorageV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    wchar_t paths[LAIUE_MODULE_HOST_MAX_MODULES][LAIUE_MODULE_PROFILE_PATH_CAPACITY];
    LaiueModuleBinaryV1 binaries[LAIUE_MODULE_HOST_MAX_MODULES];
    char providerServices[LAIUE_MODULE_HOST_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    char providerModules[LAIUE_MODULE_HOST_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    LaiueModuleProviderSelectionV1 selections[LAIUE_MODULE_HOST_MAX_SERVICES];
    uint32_t binaryCount;
    uint32_t selectionCount;
    LaiueModuleProfileV1 profile;
} LaiueModuleProfileStorageV1;

/* Grammar:
 *   LAIUE PROFILE 1
 *   flags = partial
 *   module = path/to/module.dll [optional]
 *   provider = service.name:module.id
 *
 * `flags = strict` is also accepted. Paths are UTF-8 and are converted to
 * the platform wide-character path held by the storage object. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleProfileParseTextV1(
    const char *text, uint32_t textSize, LaiueModuleProfileStorageV1 *storage,
    LaiueModuleDiagnostic *diagnostic);

