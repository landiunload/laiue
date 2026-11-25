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

/* Caller-owned diagnostics for a profile load. The report is deliberately a
 * flat C table so a UI, logger, or platform adapter can use it without
 * retaining loader internals. loadedCount counts the modules this load
 * started, not the ones that were already running. */
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

/* Host-owned services (window, graphics, input). Register and unregister work
 * at any time outside a lifecycle operation. Unregister returns
 * LAIUE_MODULE_IN_USE while a loaded module uses the service, so no module is
 * left holding a removed table. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostRegisterService(
    LaiueModuleHost *host, const LaiueModuleServiceV1 *service,
    LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostUnregisterService(
    LaiueModuleHost *host, const char *name, LaiueModuleDiagnostic *diagnostic);
LAIUE_MOD_API const void *LaiueModuleHostQueryService(
    const LaiueModuleHost *host, const char *name, uint32_t minimumVersion,
    uint32_t minimumSize, uint32_t *outVersion, uint32_t *outSize);

/* Loads explicit paths; no directory scan or automatic execution is performed.
 * Loading adds to the modules already running and needs no restart: new
 * modules may use the services of running ones, and a module id or service
 * that is already present is refused. The load is a transaction over its own
 * modules: if any of them fails, every one of them is unwound, the running
 * set is left as it was, and no module outside the load sees their services
 * before it succeeds. */
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
 * reported as disabled rather than treated as a random load-order choice.
 * If the selected provider is absent or cannot be loaded, a strict profile is
 * rejected; with ALLOW_PARTIAL the service simply has no provider: competing
 * providers stay disabled, its consumers are reported as missing a
 * dependency, and independent modules still start. */
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

/* With this flag LaiueModuleHostUnloadV1 also unloads every module that uses
 * the given one, directly or through other modules. */
#define LAIUE_MODULE_UNLOAD_DEPENDENTS (UINT32_C(1) << 0)

/* Stops, destroys and closes one running module without a restart. A module
 * uses another when it declared a requirement on one of its services or
 * obtained one of its tables through queryService. Users leave before the
 * modules they use; without LAIUE_MODULE_UNLOAD_DEPENDENTS a module in use is
 * refused with LAIUE_MODULE_IN_USE, an unknown id with
 * LAIUE_MODULE_NOT_LOADED. The application drops the tables it obtained from
 * these modules before the call. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostUnloadV1(LaiueModuleHost *host, const char *id,
                                                        uint32_t flags,
                                                        LaiueModuleDiagnostic *diagnostic);

typedef struct LaiueModuleIdV1
{
    char id[LAIUE_MODULE_MAX_NAME];
} LaiueModuleIdV1;

/* Lists the modules that LaiueModuleHostUnloadV1 with
 * LAIUE_MODULE_UNLOAD_DEPENDENTS would unload together with `id`, in the
 * order it would unload them; `id` itself is not listed. *outCount receives
 * the full number. When it exceeds capacity, the first capacity entries are
 * written and LAIUE_MODULE_CAPACITY is returned. LAIUE_MODULE_BUSY means a
 * lifecycle operation is in progress. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostListDependentsV1(const LaiueModuleHost *host,
                                                                const char *id,
                                                                LaiueModuleIdV1 *output,
                                                                uint32_t capacity,
                                                                uint32_t *outCount);

/* === Modules by name ===
 *
 * An engine module is built as `<prefix>laiue_<name><suffix>`: laiue_window.dll
 * on Windows, liblaiue_window.so on Linux and Android, liblaiue_window.dylib on
 * Apple platforms. The prefix and suffix come from the toolchain that built the
 * engine, so the name always matches the produced artifact and an application
 * does not repeat the platform table. `name` is the module's build name
 * ("window", "graphics_vulkan"): ASCII lowercase letters, digits and '_',
 * starting with a letter or digit. */
#define LAIUE_MODULE_ARTIFACT_NAME_CAPACITY 128u

/* Writes the NUL-terminated file name. INVALID_ARGUMENT rejects a bad name or
 * output, CAPACITY a short buffer; on failure output holds an empty string. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleArtifactFileNameV1(const char *name, wchar_t *output,
                                                              uint32_t capacity);

typedef struct LaiueModuleNamedProfileV1
{
    uint32_t structSize;
    /* LAIUE_MODULE_PROFILE_ALLOW_PARTIAL makes every named artifact optional:
     * a missing or broken module is reported, and independent modules still
     * start. Without it the profile is the strict transactional load. */
    uint32_t flags;
    /* Directory that holds the artifacts. NULL selects the directory of the
     * running executable, where builds and bundles place them. */
    const wchar_t *directory;
    const char *const *names;
    uint32_t nameCount;
    const LaiueModuleProviderSelectionV1 *providerSelections;
    uint32_t providerSelectionCount;
    uintptr_t reserved[4];
} LaiueModuleNamedProfileV1;

/* Resolves every name with LaiueModuleArtifactFileNameV1 inside the directory
 * and loads the result exactly like LaiueModuleHostLoadProfileV1, including
 * the report and provider selections. A name appears at most once. */
LAIUE_MOD_API LaiueModuleStatus LaiueModuleHostLoadNamedProfileV1(
    LaiueModuleHost *host, const LaiueModuleNamedProfileV1 *profile,
    LaiueModuleLoadReportV1 *report, LaiueModuleDiagnostic *diagnostic);

/* These snapshots return zero while a lifecycle operation is in progress. */
LAIUE_MOD_API uint32_t LaiueModuleHostLoadedCount(const LaiueModuleHost *host);
/* Fixed-width C ABI result: non-zero when the selected module is loaded. */
LAIUE_MOD_API uint32_t LaiueModuleHostIsLoaded(const LaiueModuleHost *host, const char *id);

/* Lifecycle calls (load, unload, service registration and destroy) may run
 * while the application is running, at its own safe point, for example
 * between ticks. One runs at a time, and a call made while another is in
 * progress returns LAIUE_MODULE_BUSY; none may be called reentrantly from
 * module callbacks. Service
 * queries are thread-safe, but return borrowed pointers rather than leases.
 * Before an unload, the application must stop and join every callback, job,
 * or thread of its own that may still use the services of the modules it
 * unloads. */
