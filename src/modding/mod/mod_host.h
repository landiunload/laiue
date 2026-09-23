#pragma once

#include "mod/mod_api.h"
#include "mod/mod_manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_MOD_HOST_MAX_SERVICES 64u
#define LAIUE_MOD_HOST_MAX_LOADED 64u

typedef struct LaiueModHost LaiueModHost;

typedef void (*LaiueModHostLogCallback)(void *context, LaiueModLogLevel level, const char *modId,
                                        const char *messageUtf8);

typedef struct LaiueModHostConfig
{
    uint32_t structSize;
    const wchar_t *packRootDirectory;
    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    void *logContext;
    LaiueModHostLogCallback log;
} LaiueModHostConfig;

typedef struct LaiueModService
{
    const char *name;
    uint32_t version;
    const void *implementation;
    uint32_t implementationSize;
} LaiueModService;

typedef struct LaiueModLoadedInfo
{
    char id[LAIUE_MOD_ID_CAPACITY];
    char version[LAIUE_MOD_VERSION_CAPACITY];
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    uint32_t abiVersion;
} LaiueModLoadedInfo;

// Initializes a host descriptor with this engine build's semantic version.
// The root remains application-owned and must outlive LaiueModHostCreate only.
LAIUE_MOD_API void LaiueModHostConfigInitialize(LaiueModHostConfig *config,
                                                const wchar_t *packRootDirectory);

/*
 * The application owns every registered service implementation and must keep
 * it alive until the service is unregistered or the host is destroyed.
 * Registry changes are rejected while an extension is loading, unloading or
 * loaded. This makes queried pointers stable for every extension lifetime.
 *
 * LaiueModHostLoad/Unload are lifecycle operations and must be serialized by
 * the application. Service queries made by extensions are thread-safe.
 */
LAIUE_MOD_API LaiueModHost *LaiueModHostCreate(const LaiueModHostConfig *config,
                                               LaiueModDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModHostDestroy(LaiueModHost *host);

LAIUE_MOD_API LaiueModStatus LaiueModHostRegisterService(LaiueModHost *host,
                                                         const LaiueModService *service,
                                                         LaiueModDiagnostic *diagnostic);
LAIUE_MOD_API LaiueModStatus LaiueModHostUnregisterService(LaiueModHost *host,
                                                           const char *serviceName,
                                                           LaiueModDiagnostic *diagnostic);

LAIUE_MOD_API LaiueModStatus LaiueModHostLoad(LaiueModHost *host, const wchar_t *packName,
                                              LaiueModLoadedInfo *outInfo,
                                              LaiueModDiagnostic *diagnostic);

/*
 * Loads packs in the exact caller-provided order. On failure, every extension
 * loaded by this call is unloaded in reverse order; extensions that were
 * already active before the call are untouched. Applications may persist this
 * ordered list in any format (for example enabled.txt).
 */
LAIUE_MOD_API LaiueModStatus LaiueModHostLoadMany(LaiueModHost *host,
                                                  const wchar_t *const *packNames, uint32_t count,
                                                  uint32_t *outLoadedCount,
                                                  LaiueModDiagnostic *diagnostic);
LAIUE_MOD_API LaiueModStatus LaiueModHostUnload(LaiueModHost *host, const char *modId,
                                                LaiueModDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModHostUnloadAll(LaiueModHost *host);

LAIUE_MOD_API uint32_t LaiueModHostLoadedCount(const LaiueModHost *host);
LAIUE_MOD_API bool LaiueModHostGetLoaded(const LaiueModHost *host, uint32_t index,
                                         LaiueModLoadedInfo *outInfo);
