#include "mod/mod_host.h"

#include "mod/mod_internal.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

typedef struct RegisteredService
{
    bool used;
    char name[LAIUE_MOD_SERVICE_NAME_CAPACITY];
    uint32_t version;
    const void *implementation;
    uint32_t implementationSize;
} RegisteredService;

typedef struct LoadedMod
{
    bool reserved;
    bool used;
    uint64_t loadSequence;
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    LaiueModManifest manifest;
    PlatformDynamicLibrary library;
    LaiueModExportsV1 exports;
    LaiueModHostApiV1 api;
    struct LaiueModHost *owner;
} LoadedMod;

typedef struct ModLoadScratch
{
    LaiueModPackInfo pack;
    wchar_t nativeName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t nativePath[LAIUE_PLATFORM_PATH_CAPACITY];
} ModLoadScratch;

struct LaiueModHost
{
    wchar_t packRootDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    void *logContext;
    LaiueModHostLogCallback log;

    PlatformRwLock lock;
    RegisteredService services[LAIUE_MOD_HOST_MAX_SERVICES];
    LoadedMod loaded[LAIUE_MOD_HOST_MAX_LOADED];
    uint32_t loadedCount;
    uint64_t nextLoadSequence;
    bool lifecycleBusy;
};

_Static_assert(sizeof(LaiueModLoadFunctionV1) == sizeof(void *),
               "dynamic-library symbols must fit ABI function pointers");

static bool CopyAscii(char *destination, uint32_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0u || source == NULL)
    {
        return false;
    }
    uint32_t index = 0;
    while (source[index] != '\0' && index + 1u < capacity)
    {
        destination[index] = source[index];
        ++index;
    }
    if (source[index] != '\0')
    {
        destination[0] = '\0';
        return false;
    }
    destination[index] = '\0';
    return true;
}

static void DefaultLog(LaiueModLogLevel level, const char *modId, const char *message)
{
    char line[512];
    uint32_t length = 0;
    const char *levelText = level == LAIUE_MOD_LOG_DEBUG         ? "debug"
                            : level == LAIUE_MOD_LOG_INFORMATION ? "info"
                            : level == LAIUE_MOD_LOG_WARNING     ? "warning"
                                                                 : "error";
    const char *segments[] = {"[mod:", modId == NULL ? "host" : modId, "][", levelText,
                              "] ",    message == NULL ? "" : message, "\n"};
    for (uint32_t segment = 0; segment < (uint32_t)(sizeof(segments) / sizeof(segments[0]));
         ++segment)
    {
        for (uint32_t index = 0;
             segments[segment][index] != '\0' && length + 1u < (uint32_t)sizeof(line); ++index)
        {
            line[length++] = segments[segment][index];
        }
    }
    line[length] = '\0';
    PlatformWriteConsoleUtf8(line);
}

static void HostLog(LaiueModHost *host, LaiueModLogLevel level, const char *modId,
                    const char *message)
{
    if (host->log != NULL)
    {
        host->log(host->logContext, level, modId == NULL ? "" : modId,
                  message == NULL ? "" : message);
    }
    else
    {
        DefaultLog(level, modId, message);
    }
}

static void LAIUE_MOD_CALL ApiLog(void *hostContext, LaiueModLogLevel level,
                                  const char *messageUtf8)
{
    LoadedMod *mod = hostContext;
    if (mod == NULL || mod->owner == NULL || messageUtf8 == NULL)
    {
        return;
    }
    if ((uint32_t)level > (uint32_t)LAIUE_MOD_LOG_ERROR)
    {
        level = LAIUE_MOD_LOG_ERROR;
    }
    HostLog(mod->owner, level, mod->manifest.id, messageUtf8);
}

static const void *LAIUE_MOD_CALL ApiQueryService(void *hostContext, const char *serviceName,
                                                  uint32_t minimumVersion, uint32_t minimumSize,
                                                  uint32_t *outVersion, uint32_t *outSize)
{
    if (outVersion != NULL)
    {
        *outVersion = 0u;
    }
    if (outSize != NULL)
    {
        *outSize = 0u;
    }
    LoadedMod *mod = hostContext;
    if (mod == NULL || mod->owner == NULL || !LaiueModServiceNameIsSafe(serviceName))
    {
        return NULL;
    }

    LaiueModHost *host = mod->owner;
    const void *result = NULL;
    PlatformRwLockAcquireShared(&host->lock);
    for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_SERVICES; ++index)
    {
        const RegisteredService *service = &host->services[index];
        if (service->used && service->version >= minimumVersion &&
            service->implementationSize >= minimumSize &&
            LaiueModAsciiEquals(service->name, serviceName))
        {
            result = service->implementation;
            if (outVersion != NULL)
            {
                *outVersion = service->version;
            }
            if (outSize != NULL)
            {
                *outSize = service->implementationSize;
            }
            break;
        }
    }
    PlatformRwLockReleaseShared(&host->lock);
    return result;
}

static bool BeginLifecycle(LaiueModHost *host)
{
    bool started = false;
    PlatformRwLockAcquireExclusive(&host->lock);
    if (!host->lifecycleBusy)
    {
        host->lifecycleBusy = true;
        started = true;
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    return started;
}

static void EndLifecycle(LaiueModHost *host)
{
    PlatformRwLockAcquireExclusive(&host->lock);
    host->lifecycleBusy = false;
    PlatformRwLockReleaseExclusive(&host->lock);
}

static LoadedMod *FindLoadedById(LaiueModHost *host, const char *modId)
{
    for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_LOADED; ++index)
    {
        LoadedMod *mod = &host->loaded[index];
        if (mod->used && LaiueModAsciiEquals(mod->manifest.id, modId))
        {
            return mod;
        }
    }
    return NULL;
}

static LoadedMod *FindFreeSlot(LaiueModHost *host)
{
    for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_LOADED; ++index)
    {
        if (!host->loaded[index].used && !host->loaded[index].reserved)
        {
            return &host->loaded[index];
        }
    }
    return NULL;
}

static void FillLoadedInfo(const LoadedMod *mod, LaiueModLoadedInfo *info)
{
    if (info == NULL)
    {
        return;
    }
    memset(info, 0, sizeof(*info));
    CopyAscii(info->id, LAIUE_MOD_ID_CAPACITY, mod->manifest.id);
    CopyAscii(info->version, LAIUE_MOD_VERSION_CAPACITY, mod->manifest.version);
    LaiueModWideCopy(info->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, mod->packName);
    info->abiVersion = mod->manifest.requiredAbi;
}

void LaiueModHostConfigInitialize(LaiueModHostConfig *config, const wchar_t *packRootDirectory)
{
    if (config == NULL)
    {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->structSize = sizeof(*config);
    config->packRootDirectory = packRootDirectory;
    config->engineVersionMajor = LAIUE_VERSION_MAJOR;
    config->engineVersionMinor = LAIUE_VERSION_MINOR;
    config->engineVersionPatch = LAIUE_VERSION_PATCH;
}

LaiueModHost *LaiueModHostCreate(const LaiueModHostConfig *config, LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (config == NULL || config->structSize < sizeof(*config) ||
        config->packRootDirectory == NULL || config->packRootDirectory[0] == L'\0')
    {
        LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                              "host config and pack root directory are required");
        return NULL;
    }

    LaiueModHost *host = PlatformAllocate(sizeof(*host), true);
    if (host == NULL)
    {
        LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY, 0,
                              "could not allocate the mod host");
        return NULL;
    }
    if (!LaiueModWideCopy(host->packRootDirectory, LAIUE_PLATFORM_PATH_CAPACITY,
                          config->packRootDirectory) ||
        !PlatformRwLockInitialize(&host->lock))
    {
        PlatformFree(host);
        LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                              "pack root path is too long or host lock initialization failed");
        return NULL;
    }
    host->engineVersionMajor = config->engineVersionMajor;
    host->engineVersionMinor = config->engineVersionMinor;
    host->engineVersionPatch = config->engineVersionPatch;
    host->logContext = config->logContext;
    host->log = config->log;
    return host;
}

void LaiueModHostDestroy(LaiueModHost *host)
{
    if (host == NULL)
    {
        return;
    }
    LaiueModHostUnloadAll(host);
    PlatformRwLockDestroy(&host->lock);
    memset(host, 0, sizeof(*host));
    PlatformFree(host);
}

LaiueModStatus LaiueModHostRegisterService(LaiueModHost *host, const LaiueModService *service,
                                           LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (host == NULL || service == NULL || service->version == 0u ||
        service->implementation == NULL || service->implementationSize == 0u)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "service, version, implementation and size are required");
    }
    if (!LaiueModServiceNameIsSafe(service->name))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_SERVICE_NAME_INVALID, 0,
                                     "service name must be a safe case-sensitive ASCII identifier");
    }

    LaiueModStatus status = LAIUE_MOD_STATUS_OK;
    PlatformRwLockAcquireExclusive(&host->lock);
    if (host->lifecycleBusy || host->loadedCount != 0u)
    {
        status = LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_BUSY, 0,
                                       "service registry is frozen while extensions are active");
    }
    else
    {
        RegisteredService *freeService = NULL;
        for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_SERVICES; ++index)
        {
            RegisteredService *current = &host->services[index];
            if (current->used && LaiueModAsciiEquals(current->name, service->name))
            {
                status =
                    LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_SERVICE_ALREADY_REGISTERED,
                                          0, "service name is already registered");
                break;
            }
            if (!current->used && freeService == NULL)
            {
                freeService = current;
            }
        }
        if (status == LAIUE_MOD_STATUS_OK && freeService == NULL)
        {
            status = LAIUE_MOD_STATUS_CAPACITY_EXCEEDED;
            LaiueModDiagnosticSet(diagnostic, status, 0, "service registry capacity is exhausted");
        }
        if (status == LAIUE_MOD_STATUS_OK)
        {
            memset(freeService, 0, sizeof(*freeService));
            CopyAscii(freeService->name, LAIUE_MOD_SERVICE_NAME_CAPACITY, service->name);
            freeService->version = service->version;
            freeService->implementation = service->implementation;
            freeService->implementationSize = service->implementationSize;
            freeService->used = true;
        }
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    return status;
}

LaiueModStatus LaiueModHostUnregisterService(LaiueModHost *host, const char *serviceName,
                                             LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (host == NULL || !LaiueModServiceNameIsSafe(serviceName))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_SERVICE_NAME_INVALID, 0,
                                     "valid host and service name are required");
    }

    LaiueModStatus status = LAIUE_MOD_STATUS_SERVICE_NOT_FOUND;
    PlatformRwLockAcquireExclusive(&host->lock);
    if (host->lifecycleBusy || host->loadedCount != 0u)
    {
        status = LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_BUSY, 0,
                                       "service registry is frozen while extensions are active");
    }
    else
    {
        for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_SERVICES; ++index)
        {
            RegisteredService *service = &host->services[index];
            if (service->used && LaiueModAsciiEquals(service->name, serviceName))
            {
                memset(service, 0, sizeof(*service));
                status = LAIUE_MOD_STATUS_OK;
                break;
            }
        }
        if (status != LAIUE_MOD_STATUS_OK)
        {
            LaiueModDiagnosticSet(diagnostic, status, 0, "service name is not registered");
        }
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    return status;
}

static LaiueModStatus FailReservedLoad(LaiueModHost *host, LoadedMod *slot,
                                       PlatformDynamicLibrary library,
                                       LaiueModDiagnostic *diagnostic, LaiueModStatus status,
                                       int32_t modResult, const char *message)
{
    char modId[LAIUE_MOD_ID_CAPACITY];
    modId[0] = '\0';
    if (slot != NULL)
    {
        CopyAscii(modId, LAIUE_MOD_ID_CAPACITY, slot->manifest.id);
    }
    if (library != NULL)
    {
        PlatformDynamicLibraryClose(library);
    }
    if (slot != NULL)
    {
        PlatformRwLockAcquireExclusive(&host->lock);
        memset(slot, 0, sizeof(*slot));
        PlatformRwLockReleaseExclusive(&host->lock);
    }
    EndLifecycle(host);
    LaiueModDiagnosticSet(diagnostic, status, modResult, message);
    HostLog(host, LAIUE_MOD_LOG_ERROR, modId[0] == '\0' ? NULL : modId, message);
    return status;
}

LaiueModStatus LaiueModHostLoad(LaiueModHost *host, const wchar_t *packName,
                                LaiueModLoadedInfo *outInfo, LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (outInfo != NULL)
    {
        memset(outInfo, 0, sizeof(*outInfo));
    }
    if (host == NULL || packName == NULL)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "host and pack name are required");
    }
    if (!BeginLifecycle(host))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_BUSY, 0,
                                     "another mod lifecycle operation is active");
    }

    ModLoadScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return FailReservedLoad(host, NULL, NULL, diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY, 0,
                                "could not allocate mod loading scratch memory");
    }
    LaiueModDiagnostic inspectionDiagnostic;
    LaiueModStatus status = LaiueModPackInspect(host->packRootDirectory, packName, &scratch->pack,
                                                &inspectionDiagnostic);
    if (status != LAIUE_MOD_STATUS_OK)
    {
        PlatformFree(scratch);
        EndLifecycle(host);
        if (diagnostic != NULL)
        {
            *diagnostic = inspectionDiagnostic;
        }
        HostLog(host, LAIUE_MOD_LOG_ERROR, NULL, inspectionDiagnostic.message);
        return status;
    }
    if (scratch->pack.manifest.requiredEngineMajor != host->engineVersionMajor ||
        scratch->pack.manifest.requiredEngineMinor > host->engineVersionMinor)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, NULL, NULL, diagnostic, LAIUE_MOD_STATUS_ENGINE_INCOMPATIBLE,
                                0, "mod requires an incompatible engine major/minor version");
    }
    if (scratch->pack.manifest.requiredAbi != LAIUE_MOD_ABI_VERSION_1)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, NULL, NULL, diagnostic, LAIUE_MOD_STATUS_ABI_INCOMPATIBLE, 0,
                                "mod requires an unsupported native ABI version");
    }

    LoadedMod *slot = NULL;
    PlatformRwLockAcquireExclusive(&host->lock);
    if (FindLoadedById(host, scratch->pack.manifest.id) != NULL)
    {
        status = LAIUE_MOD_STATUS_ALREADY_LOADED;
    }
    else if (host->loadedCount >= LAIUE_MOD_HOST_MAX_LOADED || (slot = FindFreeSlot(host)) == NULL)
    {
        status = LAIUE_MOD_STATUS_CAPACITY_EXCEEDED;
    }
    else
    {
        memset(slot, 0, sizeof(*slot));
        slot->reserved = true;
        slot->owner = host;
        slot->manifest = scratch->pack.manifest;
        LaiueModWideCopy(slot->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, packName);
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    if (status == LAIUE_MOD_STATUS_ALREADY_LOADED)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, NULL, NULL, diagnostic, status, 0,
                                "a mod with the same manifest id is already loaded");
    }
    if (status == LAIUE_MOD_STATUS_CAPACITY_EXCEEDED)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, NULL, NULL, diagnostic, status, 0,
                                "loaded mod capacity is exhausted");
    }

    LaiueModManifestSelectNativeEntry(&scratch->pack.manifest, scratch->nativeName,
                                      LAIUE_MOD_NATIVE_NAME_CAPACITY, NULL);
    if (!LaiueModPathJoin(scratch->nativePath, LAIUE_PLATFORM_PATH_CAPACITY,
                          host->packRootDirectory, packName, scratch->nativeName))
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, slot, NULL, diagnostic,
                                LAIUE_MOD_STATUS_NATIVE_ARTIFACT_INVALID, 0,
                                "native artifact path exceeds the platform limit");
    }

    PlatformDynamicLibrary library = PlatformDynamicLibraryOpen(scratch->nativePath);
    if (library == NULL)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, slot, NULL, diagnostic, LAIUE_MOD_STATUS_LIBRARY_LOAD_FAILED,
                                0, "validated native artifact could not be loaded");
    }
    void *symbol = PlatformDynamicLibrarySymbol(library, LAIUE_MOD_ENTRY_NAME_V1);
    if (symbol == NULL)
    {
        PlatformFree(scratch);
        return FailReservedLoad(host, slot, library, diagnostic,
                                LAIUE_MOD_STATUS_ENTRY_POINT_MISSING, 0,
                                "native artifact does not export LaiueModLoadV1");
    }

    PlatformFree(scratch);
    LaiueModLoadFunctionV1 entry = NULL;
    memcpy(&entry, &symbol, sizeof(entry));
    slot->api.structSize = sizeof(slot->api);
    slot->api.abiVersion = LAIUE_MOD_ABI_VERSION_1;
    slot->api.engineVersionMajor = host->engineVersionMajor;
    slot->api.engineVersionMinor = host->engineVersionMinor;
    slot->api.engineVersionPatch = host->engineVersionPatch;
    slot->api.hostContext = slot;
    slot->api.modId = slot->manifest.id;
    slot->api.modVersion = slot->manifest.version;
    slot->api.log = ApiLog;
    slot->api.queryService = ApiQueryService;
    memset(&slot->exports, 0, sizeof(slot->exports));
    slot->exports.structSize = sizeof(slot->exports);
    slot->exports.abiVersion = LAIUE_MOD_ABI_VERSION_1;

    LaiueModResult modResult = entry(&slot->api, &slot->exports);
    if (modResult != LAIUE_MOD_RESULT_OK)
    {
        return FailReservedLoad(host, slot, library, diagnostic,
                                LAIUE_MOD_STATUS_INITIALIZATION_FAILED, modResult,
                                "LaiueModLoadV1 returned a failure result");
    }

    size_t requiredExportsSize = offsetof(LaiueModExportsV1, reserved);
    bool exportsValid = slot->exports.structSize >= requiredExportsSize &&
                        slot->exports.abiVersion == LAIUE_MOD_ABI_VERSION_1;
    if (!exportsValid)
    {
        if (slot->exports.structSize >= requiredExportsSize && slot->exports.unload != NULL)
        {
            slot->exports.unload(slot->exports.modContext);
        }
        return FailReservedLoad(host, slot, library, diagnostic, LAIUE_MOD_STATUS_EXPORTS_INVALID,
                                0, "successful entry point returned an invalid exports table");
    }

    PlatformRwLockAcquireExclusive(&host->lock);
    slot->library = library;
    slot->loadSequence = ++host->nextLoadSequence;
    slot->used = true;
    slot->reserved = false;
    ++host->loadedCount;
    FillLoadedInfo(slot, outInfo);
    host->lifecycleBusy = false;
    PlatformRwLockReleaseExclusive(&host->lock);
    HostLog(host, LAIUE_MOD_LOG_INFORMATION, slot->manifest.id, "loaded");
    return LAIUE_MOD_STATUS_OK;
}

LaiueModStatus LaiueModHostLoadMany(LaiueModHost *host, const wchar_t *const *packNames,
                                    uint32_t count, uint32_t *outLoadedCount,
                                    LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (outLoadedCount != NULL)
    {
        *outLoadedCount = 0u;
    }
    if (host == NULL || (count != 0u && packNames == NULL) || count > LAIUE_MOD_HOST_MAX_LOADED)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "host, pack array and supported count are required");
    }

    char (*loadedIds)[LAIUE_MOD_ID_CAPACITY] =
        count == 0u ? NULL : PlatformAllocate((size_t)count * sizeof(*loadedIds), false);
    if (count != 0u && loadedIds == NULL)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY, 0,
                                     "could not allocate multi-load rollback state");
    }
    uint32_t loadedCount = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        LaiueModLoadedInfo info;
        LaiueModDiagnostic loadDiagnostic;
        LaiueModStatus status = LaiueModHostLoad(host, packNames[index], &info, &loadDiagnostic);
        if (status != LAIUE_MOD_STATUS_OK)
        {
            for (uint32_t rollback = loadedCount; rollback > 0u; --rollback)
            {
                LaiueModHostUnload(host, loadedIds[rollback - 1u], NULL);
            }
            if (outLoadedCount != NULL)
            {
                *outLoadedCount = 0u;
            }
            if (diagnostic != NULL)
            {
                *diagnostic = loadDiagnostic;
            }
            PlatformFree(loadedIds);
            return status;
        }
        CopyAscii(loadedIds[loadedCount++], LAIUE_MOD_ID_CAPACITY, info.id);
        if (outLoadedCount != NULL)
        {
            *outLoadedCount = loadedCount;
        }
    }
    PlatformFree(loadedIds);
    return LAIUE_MOD_STATUS_OK;
}

LaiueModStatus LaiueModHostUnload(LaiueModHost *host, const char *modId,
                                  LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (host == NULL || modId == NULL || modId[0] == '\0')
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "host and mod id are required");
    }
    if (!BeginLifecycle(host))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_BUSY, 0,
                                     "another mod lifecycle operation is active");
    }

    PlatformRwLockAcquireShared(&host->lock);
    LoadedMod *slot = FindLoadedById(host, modId);
    PlatformRwLockReleaseShared(&host->lock);
    if (slot == NULL)
    {
        EndLifecycle(host);
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_NOT_LOADED, 0,
                                     "mod id is not loaded");
    }

    HostLog(host, LAIUE_MOD_LOG_INFORMATION, slot->manifest.id, "unloading");
    if (slot->exports.unload != NULL)
    {
        slot->exports.unload(slot->exports.modContext);
    }
    PlatformDynamicLibraryClose(slot->library);

    PlatformRwLockAcquireExclusive(&host->lock);
    memset(slot, 0, sizeof(*slot));
    --host->loadedCount;
    host->lifecycleBusy = false;
    PlatformRwLockReleaseExclusive(&host->lock);
    return LAIUE_MOD_STATUS_OK;
}

void LaiueModHostUnloadAll(LaiueModHost *host)
{
    if (host == NULL)
    {
        return;
    }
    for (;;)
    {
        char latestId[LAIUE_MOD_ID_CAPACITY];
        latestId[0] = '\0';
        uint64_t latestSequence = 0u;
        PlatformRwLockAcquireShared(&host->lock);
        for (uint32_t index = 0; index < LAIUE_MOD_HOST_MAX_LOADED; ++index)
        {
            const LoadedMod *mod = &host->loaded[index];
            if (mod->used && mod->loadSequence > latestSequence)
            {
                latestSequence = mod->loadSequence;
                CopyAscii(latestId, LAIUE_MOD_ID_CAPACITY, mod->manifest.id);
            }
        }
        PlatformRwLockReleaseShared(&host->lock);
        if (latestId[0] == '\0')
        {
            return;
        }
        if (LaiueModHostUnload(host, latestId, NULL) != LAIUE_MOD_STATUS_OK)
        {
            return;
        }
    }
}

uint32_t LaiueModHostLoadedCount(const LaiueModHost *host)
{
    if (host == NULL)
    {
        return 0u;
    }
    LaiueModHost *mutableHost = (LaiueModHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    uint32_t count = host->loadedCount;
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return count;
}

bool LaiueModHostGetLoaded(const LaiueModHost *host, uint32_t index, LaiueModLoadedInfo *outInfo)
{
    if (host == NULL || outInfo == NULL)
    {
        return false;
    }
    LaiueModHost *mutableHost = (LaiueModHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    if (index >= host->loadedCount)
    {
        PlatformRwLockReleaseShared(&mutableHost->lock);
        return false;
    }

    const LoadedMod *selected = NULL;
    uint64_t previousSequence = 0u;
    for (uint32_t rank = 0; rank <= index; ++rank)
    {
        selected = NULL;
        for (uint32_t slotIndex = 0; slotIndex < LAIUE_MOD_HOST_MAX_LOADED; ++slotIndex)
        {
            const LoadedMod *candidate = &host->loaded[slotIndex];
            if (candidate->used && candidate->loadSequence > previousSequence &&
                (selected == NULL || candidate->loadSequence < selected->loadSequence))
            {
                selected = candidate;
            }
        }
        if (selected == NULL)
        {
            PlatformRwLockReleaseShared(&mutableHost->lock);
            return false;
        }
        previousSequence = selected->loadSequence;
    }
    FillLoadedInfo(selected, outInfo);
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return true;
}
