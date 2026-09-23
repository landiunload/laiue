#pragma once

#include "api.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_MODULE_MANIFEST_ABI_VERSION_1 1u
#define LAIUE_MODULE_MANIFEST_MAX_TEXT 128u
#define LAIUE_MODULE_MANIFEST_MAX_SERVICES 128u

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
