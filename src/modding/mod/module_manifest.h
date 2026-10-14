#pragma once

#include "api.h"
#include "mod/module_api.h"

#include <stdint.h>
#include <wchar.h>

#define LAIUE_MODULE_MANIFEST_ABI_VERSION_1 1u
#define LAIUE_MODULE_MANIFEST_MAX_TEXT 128u
#define LAIUE_MODULE_MANIFEST_MAX_SERVICES 128u
#define LAIUE_MODULE_MANIFEST_TEXT_CAPACITY 8192u

typedef struct LaiueModuleManifestDiagnostic
{
    LaiueModuleStatus status;
    char message[LAIUE_MODULE_MANIFEST_MAX_TEXT];
} LaiueModuleManifestDiagnostic;

typedef struct LaiueModuleManifestV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    const char *id;
    const char *author;
    const char *version;
    const char *platform;
    /* A manifest names one leaf artifact. Directory traversal is rejected. */
    const char *binary;
    const char *const *provides;
    uint32_t providesCount;
    const LaiueModuleRequirementV1 *requiresServices;
    uint32_t requiresCount;
    uint32_t flags;
    uintptr_t reserved[4];
} LaiueModuleManifestV1;

/* Caller-owned scratch for the text form distributed as `module.laiue`.
 * Parsing never opens or loads the named binary.  Keeping all strings and
 * arrays in this object makes the resulting manifest independent of a parser
 * heap and lets a profile retain it until explicit artifact validation. */
typedef struct LaiueModuleManifestStorageV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    char id[LAIUE_MODULE_MAX_NAME];
    char author[LAIUE_MODULE_MAX_NAME];
    char version[LAIUE_MODULE_MAX_NAME];
    char platform[LAIUE_MODULE_MAX_NAME];
    char binary[LAIUE_MODULE_MAX_NAME];
    char providedNames[LAIUE_MODULE_MANIFEST_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    char requiredNames[LAIUE_MODULE_MANIFEST_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    const char *provides[LAIUE_MODULE_MANIFEST_MAX_SERVICES];
    LaiueModuleRequirementV1 requiresServices[LAIUE_MODULE_MANIFEST_MAX_SERVICES];
    uint32_t providedVersions[LAIUE_MODULE_MANIFEST_MAX_SERVICES];
    uint32_t providesCount;
    uint32_t requiresCount;
    LaiueModuleManifestV1 manifest;
} LaiueModuleManifestStorageV1;

#define LAIUE_MODULE_MANIFEST_OPTIONAL UINT32_C(1) << 0

/* Validates metadata before the application turns its explicitly selected
 * artifact into a LaiueModuleBinaryV1. This function never opens a file. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleManifestValidate(
    const LaiueModuleManifestV1 *manifest, LaiueModuleManifestDiagnostic *diagnostic);

/* Checks that the loaded binary declares the same identity and services as
 * the selected manifest. It does not execute lifecycle callbacks. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleManifestValidateApi(
    const LaiueModuleManifestV1 *manifest, const LaiueModuleApiV1 *api,
    LaiueModuleManifestDiagnostic *diagnostic);

/* Parses the bounded UTF-8 text representation used by module.laiue into
 * caller-owned storage, then runs the same structural validator.  The input
 * is not required to be NUL-terminated.  No library is opened or executed. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleManifestParseTextV1(
    const char *text, uint32_t textSize, LaiueModuleManifestStorageV1 *storage,
    LaiueModuleManifestDiagnostic *diagnostic);

/* Convenience boundary for an explicitly selected manifest file.  Only the
 * bounded text is read; the `binary` field is never opened by this function. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleManifestParseFileV1(
    const wchar_t *path, uint64_t maximumBytes,
    LaiueModuleManifestStorageV1 *storage, LaiueModuleManifestDiagnostic *diagnostic);
