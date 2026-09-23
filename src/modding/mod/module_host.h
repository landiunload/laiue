#pragma once

#include "api.h"
#include "mod/module_api.h"

#include <stdint.h>
#include <wchar.h>

#define LAIUE_MODULE_HOST_MAX_MODULES 64u
#define LAIUE_MODULE_HOST_MAX_SERVICES 128u
#define LAIUE_MODULE_DIAGNOSTIC_CAPACITY 256u
#define LAIUE_MODULE_PROFILE_ALLOW_PARTIAL UINT32_C(1) << 0
#define LAIUE_MODULE_PROFILE_ENTRY_SKIPPED UINT32_C(1) << 0
#define LAIUE_MODULE_PROFILE_ENTRY_LOADED UINT32_C(1) << 1
#define LAIUE_MODULE_PROFILE_ENTRY_DISABLED UINT32_C(1) << 2

typedef struct LaiueModuleDiagnostic
{
    LaiueModuleStatus status;
    char message[LAIUE_MODULE_DIAGNOSTIC_CAPACITY];
} LaiueModuleDiagnostic;

/* Startup-only, caller-owned diagnostics for a profile load. The report is
 * deliberately a flat C table so a UI, logger, or platform adapter can use it
 * without retaining loader internals. */
typedef struct LaiueModuleLoadReportEntryV1
{
    uint32_t structSize;
    uint32_t status;
    uint32_t flags;
    char id[LAIUE_MODULE_MAX_NAME];
    char message[LAIUE_MODULE_DIAGNOSTIC_CAPACITY];
} LaiueModuleLoadReportEntryV1;

typedef struct LaiueModuleLoadReportV1
{
    uint32_t structSize;
    uint32_t flags;
    uint32_t capacity;
    uint32_t count;
    uint32_t loadedCount;
    uint32_t skippedCount;
    LaiueModuleLoadReportEntryV1 *entries;
} LaiueModuleLoadReportV1;

typedef struct LaiueModuleBinaryV1
{
    const wchar_t *path;
    uint32_t flags;
    /* Non-NULL only for LAIUE_MODULE_BINARY_STATIC entries. The pointer is
     * owned by the application and remains valid until unload completes. */
    const LaiueModuleApiV1 *staticApi;
} LaiueModuleBinaryV1;

/* A missing optional artifact is skipped. Invalid present artifacts still
 * fail the transaction, so a typo cannot silently disable a technology. */
#define LAIUE_MODULE_BINARY_OPTIONAL UINT32_C(1) << 0
/* Use a statically registered API instead of opening path. This is the same
 * ABI for consoles/mobile and external platform adapters. */
#define LAIUE_MODULE_BINARY_STATIC UINT32_C(1) << 1

/* A profile may pin the provider of a service explicitly.  Provider IDs are
 * module descriptor IDs, not file names, so the same profile remains valid
 * when the artifact is replaced for another platform. */
typedef struct LaiueModuleProviderSelectionV1
{
    uint32_t structSize;
    const char *serviceName;
    const char *moduleId;
    uintptr_t reserved[2];
} LaiueModuleProviderSelectionV1;

typedef struct LaiueModuleProfileV1
{
    uint32_t structSize;
    uint32_t flags;
    const LaiueModuleBinaryV1 *binaries;
    uint32_t binaryCount;
    const LaiueModuleProviderSelectionV1 *providerSelections;
    uint32_t providerSelectionCount;
    uintptr_t reserved[4];
} LaiueModuleProfileV1;

typedef void (*LaiueModuleHostLogCallback)(void *context, LaiueModuleLogLevel level,
                                           const char *moduleId, const char *message);

typedef struct LaiueModuleHostConfigV1
{
    uint32_t structSize;
    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    void *logContext;
    LaiueModuleHostLogCallback log;
} LaiueModuleHostConfigV1;

typedef struct LaiueModuleHost LaiueModuleHost;

LAIUE_MOD_API const char *LaiueModuleStatusString(LaiueModuleStatus status);
LAIUE_MOD_API void LaiueModuleHostConfigInitialize(LaiueModuleHostConfigV1 *config);
LAIUE_MOD_API LaiueModuleHost *LaiueModuleHostCreate(const LaiueModuleHostConfigV1 *config,
                                                       LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModuleHostDestroy(LaiueModuleHost *host);

/* Host-owned services (window, graphics, input) are registered before load. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostRegisterService(
    LaiueModuleHost *host, const LaiueModuleServiceV1 *service,
    LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostUnregisterService(
    LaiueModuleHost *host, const char *name, LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API const void *LaiueModuleHostQueryService(
    const LaiueModuleHost *host, const char *name, uint32_t minimumVersion,
    uint32_t minimumSize, uint32_t *outVersion, uint32_t *outSize);

/* Loads explicit paths; no directory scan or automatic execution is performed. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostLoad(
    LaiueModuleHost *host, const LaiueModuleBinaryV1 *binaries, uint32_t count,
    LaiueModuleDiagnostic *diagnostic);
/* Profile loading is opt-in. With ALLOW_PARTIAL, invalid/missing optional
 * artifacts and disconnected dependency components are reported per entry;
 * independent providers still start. Without the flag this is equivalent to
 * the strict transactional loader above. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostLoadProfile(
    LaiueModuleHost *host, const LaiueModuleBinaryV1 *binaries, uint32_t count,
    uint32_t profileFlags, LaiueModuleLoadReportV1 *report,
    LaiueModuleDiagnostic *diagnostic);
/* Extended profile form with explicit service-provider choices.  The
 * compatibility overload above remains the shorthand for a profile without
 * selections.  When selections are present, the selected provider is loaded
 * even in strict mode; an unselected competing provider is intentionally
 * reported as disabled rather than treated as a random load-order choice. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostLoadProfileV1(
    LaiueModuleHost *host, const LaiueModuleProfileV1 *profile,
    LaiueModuleLoadReportV1 *report, LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModuleLoadReportInitialize(
    LaiueModuleLoadReportV1 *report,
    LaiueModuleLoadReportEntryV1 *entries, uint32_t capacity);
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostLoadStatic(
    LaiueModuleHost *host, const LaiueModuleApiV1 *const *apis, uint32_t count,
    LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModuleHostUnloadAll(LaiueModuleHost *host);
LAIUE_MOD_API uint32_t LaiueModuleHostLoadedCount(const LaiueModuleHost *host);
/* Fixed-width C ABI result: non-zero when the selected module is loaded. */
LAIUE_MOD_API uint32_t LaiueModuleHostIsLoaded(const LaiueModuleHost *host, const char *id);
