#include "mod/module_host.h"

#include "mod/mod_internal.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

typedef struct ModuleService
{
    bool used;
    uint32_t owner;
    LaiueModuleServiceV1 value;
} ModuleService;

typedef struct LoadedModule
{
    bool used;
    bool created;
    bool started;
    PlatformDynamicLibrary library;
    const LaiueModuleApiV1 *api;
    LaiueModuleHost *owner;
    LaiueModuleHostV1 hostApi;
    void *context;
    char id[LAIUE_MODULE_MAX_NAME];
    char version[LAIUE_MODULE_MAX_NAME];
    uint32_t createOrder;
    uint32_t startOrder;
} LoadedModule;

struct LaiueModuleHost
{
    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    void *logContext;
    LaiueModuleHostLogCallback log;
    PlatformRwLock lock;
    bool lifecycleBusy;
    LoadedModule modules[LAIUE_MODULE_HOST_MAX_MODULES];
    ModuleService services[LAIUE_MODULE_HOST_MAX_SERVICES];
    uint32_t loadedCount;
};

static void DiagnosticClear(LaiueModuleDiagnostic *diagnostic)
{
    if (diagnostic != NULL)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = LAIUE_MODULE_OK;
    }
}

static LaiueModuleStatus Fail(LaiueModuleDiagnostic *diagnostic, LaiueModuleStatus status,
                              const char *message)
{
    if (diagnostic != NULL)
    {
        diagnostic->status = status;
        uint32_t index = 0u;
        if (message != NULL)
        {
            while (message[index] != '\0' && index + 1u < LAIUE_MODULE_DIAGNOSTIC_CAPACITY)
            {
                diagnostic->message[index] = message[index];
                ++index;
            }
        }
        diagnostic->message[index] = '\0';
    }
    return status;
}

const char *LaiueModuleStatusString(LaiueModuleStatus status)
{
    static const char *const names[] = {
        "ok", "invalid argument", "ABI mismatch", "invalid descriptor", "duplicate module id",
        "duplicate service", "missing dependency", "dependency cycle", "library load failed",
        "entry point missing", "out of memory", "module capacity", "create failed",
        "start failed", "host busy", "invalid service", "service not found"};
    return (uint32_t)status < (uint32_t)(sizeof(names) / sizeof(names[0])) ? names[status]
                                                                            : "unknown";
}

static bool SafeName(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return false;
    uint32_t length = 0u;
    bool previousDot = false;
    while (name[length] != '\0')
    {
        unsigned char c = (unsigned char)name[length];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if ((!alpha && c != '.' && c != '_' && c != '-') || length + 1u >= LAIUE_MODULE_MAX_NAME ||
            (c == '.' && previousDot))
            return false;
        previousDot = c == '.';
        ++length;
    }
    unsigned char first = (unsigned char)name[0];
    unsigned char last = (unsigned char)name[length - 1u];
    bool firstAlpha = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                      (first >= '0' && first <= '9');
    bool lastAlpha = (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') ||
                     (last >= '0' && last <= '9');
    return firstAlpha && lastAlpha;
}

static bool CopyName(char destination[LAIUE_MODULE_MAX_NAME], const char *source)
{
    if (!SafeName(source))
        return false;
    uint32_t index = 0u;
    while (source[index] != '\0')
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

static bool Begin(LaiueModuleHost *host)
{
    bool result = false;
    PlatformRwLockAcquireExclusive(&host->lock);
    if (!host->lifecycleBusy)
    {
        host->lifecycleBusy = true;
        result = true;
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    return result;
}

static void End(LaiueModuleHost *host)
{
    PlatformRwLockAcquireExclusive(&host->lock);
    host->lifecycleBusy = false;
    PlatformRwLockReleaseExclusive(&host->lock);
}

static void HostLog(LaiueModuleHost *host, LaiueModuleLogLevel level, const char *id,
                    const char *message)
{
    if (host->log != NULL)
        host->log(host->logContext, level, id == NULL ? "" : id, message == NULL ? "" : message);
}

static void LAIUE_MODULE_CALL ApiLog(void *context, LaiueModuleLogLevel level,
                                     const char *moduleId, const char *message)
{
    LoadedModule *module = (LoadedModule *)context;
    if (module != NULL && module->owner != NULL)
        HostLog(module->owner, level, moduleId, message);
}

static void *LAIUE_MODULE_CALL ApiAllocate(void *context, uint64_t size)
{
    (void)context;
    if (size > (uint64_t)SIZE_MAX)
        return NULL;
    return PlatformAllocate((size_t)size, true);
}

static void *LAIUE_MODULE_CALL ApiReallocate(void *context, void *memory, uint64_t size)
{
    (void)context;
    if (size > (uint64_t)SIZE_MAX)
        return NULL;
    return PlatformReallocate(memory, (size_t)size, false);
}

static void LAIUE_MODULE_CALL ApiFree(void *context, void *memory)
{
    (void)context;
    PlatformFree(memory);
}

static const void *LAIUE_MODULE_CALL ApiQuery(void *context, const char *name,
                                               uint32_t minimumVersion, uint32_t minimumSize,
                                               uint32_t *outVersion, uint32_t *outSize)
{
    LoadedModule *module = (LoadedModule *)context;
    LaiueModuleHost *host = module == NULL ? NULL : module->owner;
    if (outVersion != NULL)
        *outVersion = 0u;
    if (outSize != NULL)
        *outSize = 0u;
    if (host == NULL || !SafeName(name))
        return NULL;
    const void *result = LaiueModuleHostQueryService(host, name, minimumVersion, minimumSize,
                                                     outVersion, outSize);
    return result;
/* Kept as a separate public read path so applications can inspect optional
 * capabilities without retaining the private module callback table. */
}

const void *LaiueModuleHostQueryService(const LaiueModuleHost *host, const char *name,
                                        uint32_t minimumVersion, uint32_t minimumSize,
                                        uint32_t *outVersion, uint32_t *outSize)
{
    if (outVersion != NULL)
        *outVersion = 0u;
    if (outSize != NULL)
        *outSize = 0u;
    if (host == NULL || !SafeName(name))
        return NULL;
    const void *result = NULL;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        const ModuleService *service = &host->services[index];
        if (service->used && service->value.version >= minimumVersion &&
            service->value.tableSize >= minimumSize && LaiueModAsciiEquals(service->value.name, name))
        {
            result = service->value.table;
            if (outVersion != NULL)
                *outVersion = service->value.version;
            if (outSize != NULL)
                *outSize = service->value.tableSize;
            break;
        }
    }
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return result;
}

static bool DeclaresService(const LoadedModule *module, const char *name)
{
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    for (uint32_t index = 0u; index < descriptor->providesCount; ++index)
        if (descriptor->providesServices[index] != NULL &&
            LaiueModAsciiEquals(descriptor->providesServices[index], name))
            return true;
    return false;
}

static LaiueModuleStatus PublishFor(LaiueModuleHost *host, uint32_t owner,
                                    const LaiueModuleServiceV1 *service,
                                    LaiueModuleDiagnostic *diagnostic)
{
    if (service == NULL || !SafeName(service->name) || service->version == 0u ||
        service->table == NULL || service->tableSize == 0u || owner > LAIUE_MODULE_HOST_MAX_MODULES ||
        (owner < LAIUE_MODULE_HOST_MAX_MODULES &&
         !DeclaresService(&host->modules[owner], service->name)))
        return Fail(diagnostic, LAIUE_MODULE_SERVICE_INVALID, "service is not declared by module");
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        ModuleService *current = &host->services[index];
        if (current->used && LaiueModAsciiEquals(current->value.name, service->name))
            return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_SERVICE, "service already published");
    }
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        ModuleService *current = &host->services[index];
        if (!current->used)
        {
            current->used = true;
            current->owner = owner;
            current->value = *service;
            return LAIUE_MODULE_OK;
        }
    }
    return Fail(diagnostic, LAIUE_MODULE_CAPACITY, "service registry is full");
}

static LaiueModuleStatus LAIUE_MODULE_CALL ApiPublish(void *context,
                                                       const LaiueModuleServiceV1 *service)
{
    LoadedModule *module = (LoadedModule *)context;
    LaiueModuleHost *host = module == NULL ? NULL : module->owner;
    if (host == NULL)
        return LAIUE_MODULE_INVALID_ARGUMENT;
    LaiueModuleDiagnostic diagnostic;
    PlatformRwLockAcquireExclusive(&host->lock);
    LaiueModuleStatus status = PublishFor(host, (uint32_t)(module - host->modules), service, &diagnostic);
    PlatformRwLockReleaseExclusive(&host->lock);
    return status;
}

static LaiueModuleStatus LAIUE_MODULE_CALL ApiUnpublish(void *context, const char *name)
{
    LoadedModule *module = (LoadedModule *)context;
    LaiueModuleHost *host = module == NULL ? NULL : module->owner;
    if (host == NULL)
        return LAIUE_MODULE_INVALID_ARGUMENT;
    if (!SafeName(name))
        return LAIUE_MODULE_SERVICE_INVALID;
    PlatformRwLockAcquireExclusive(&host->lock);
    LaiueModuleStatus status = LAIUE_MODULE_SERVICE_NOT_FOUND;
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        if (host->services[index].used && host->services[index].owner == (uint32_t)(module - host->modules) &&
            LaiueModAsciiEquals(host->services[index].value.name, name))
        {
            memset(&host->services[index], 0, sizeof(host->services[index]));
            status = LAIUE_MODULE_OK;
            break;
        }
    }
    PlatformRwLockReleaseExclusive(&host->lock);
    return status;
}

static const LaiueModuleHostV1 *BuildApi(LaiueModuleHost *host, LoadedModule *module)
{
    LaiueModuleHostV1 *out = &module->hostApi;
    memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->abiVersion = LAIUE_MODULE_ABI_VERSION_1;
    out->engineVersionMajor = host->engineVersionMajor;
    out->engineVersionMinor = host->engineVersionMinor;
    out->engineVersionPatch = host->engineVersionPatch;
    out->context = module;
    out->log = ApiLog;
    out->allocate = ApiAllocate;
    out->reallocate = ApiReallocate;
    out->free = ApiFree;
    out->queryService = ApiQuery;
    out->publishService = ApiPublish;
    out->unpublishService = ApiUnpublish;
    return out;
}

static bool RequiredServicesReady(LaiueModuleHost *host, const LoadedModule *module)
{
    (void)host;
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    for (uint32_t requirement = 0u; requirement < descriptor->requiresCount; ++requirement)
    {
        uint32_t version = 0u;
        const LaiueModuleRequirementV1 *required = &descriptor->requiresServices[requirement];
        if (module->owner == NULL || required->name == NULL || required->minimumVersion == 0u ||
            ApiQuery((void *)module, required->name, required->minimumVersion, 1u, &version,
                     NULL) == NULL)
            return false;
    }
    return true;
}

static bool DescriptorHasOptionalServices(const LaiueModuleDescriptorV1 *descriptor)
{
    return descriptor != NULL &&
           descriptor->optionalMagic == LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC;
}

static void RemoveOwnedServices(LaiueModuleHost *host, uint32_t owner)
{
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        if (host->services[index].used && host->services[index].owner == owner)
            memset(&host->services[index], 0, sizeof(host->services[index]));
}

static void Rollback(LaiueModuleHost *host, uint32_t count)
{
    /* Callbacks are unwound by lifecycle order, never by descriptor/input
     * order. A consumer can therefore always release its references before
     * its provider is stopped. */
    for (uint32_t reverse = count; reverse > 0u; --reverse)
    {
        uint32_t order = reverse - 1u;
        for (uint32_t index = 0u; index < count; ++index)
        {
            LoadedModule *module = &host->modules[index];
            if (!module->used || !module->started || module->startOrder != order)
                continue;
            if (module->api->stop != NULL)
                module->api->stop(module->context);
            module->started = false;
            RemoveOwnedServices(host, index);
        }
    }
    for (uint32_t reverse = count; reverse > 0u; --reverse)
    {
        uint32_t order = reverse - 1u;
        for (uint32_t index = 0u; index < count; ++index)
        {
            LoadedModule *module = &host->modules[index];
            if (!module->used || !module->created || module->createOrder != order)
                continue;
            if (module->api->destroy != NULL)
                module->api->destroy(module->context);
            module->created = false;
        }
    }
    for (uint32_t reverse = count; reverse > 0u; --reverse)
    {
        LoadedModule *module = &host->modules[reverse - 1u];
        if (module->used)
        {
            RemoveOwnedServices(host, reverse - 1u);
            PlatformDynamicLibraryClose(module->library);
            memset(module, 0, sizeof(*module));
        }
    }
    host->loadedCount = 0u;
}

void LaiueModuleHostConfigInitialize(LaiueModuleHostConfigV1 *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->structSize = sizeof(*config);
    config->engineVersionMajor = 0u;
    config->engineVersionMinor = 7u;
    config->engineVersionPatch = 0u;
}

LaiueModuleHost *LaiueModuleHostCreate(const LaiueModuleHostConfigV1 *config,
                                       LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (config == NULL || config->structSize < sizeof(*config))
    {
        Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "valid module host config is required");
        return NULL;
    }
    LaiueModuleHost *host = PlatformAllocate(sizeof(*host), true);
    if (host == NULL || !PlatformRwLockInitialize(&host->lock))
    {
        PlatformFree(host);
        Fail(diagnostic, LAIUE_MODULE_OUT_OF_MEMORY, "module host allocation failed");
        return NULL;
    }
    host->engineVersionMajor = config->engineVersionMajor;
    host->engineVersionMinor = config->engineVersionMinor;
    host->engineVersionPatch = config->engineVersionPatch;
    host->logContext = config->logContext;
    host->log = config->log;
    return host;
}

void LaiueModuleHostDestroy(LaiueModuleHost *host)
{
    if (host == NULL)
        return;
    LaiueModuleHostUnloadAll(host);
    PlatformRwLockDestroy(&host->lock);
    PlatformFree(host);
}

LaiueModuleStatus LaiueModuleHostRegisterService(LaiueModuleHost *host,
                                                 const LaiueModuleServiceV1 *service,
                                                 LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || service == NULL || !SafeName(service->name) || service->version == 0u ||
        service->table == NULL || service->tableSize == 0u)
        return Fail(diagnostic, LAIUE_MODULE_SERVICE_INVALID, "invalid host service");
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    PlatformRwLockAcquireExclusive(&host->lock);
    LaiueModuleStatus status = PublishFor(host, LAIUE_MODULE_HOST_MAX_MODULES, service, diagnostic);
    PlatformRwLockReleaseExclusive(&host->lock);
    End(host);
    return status;
}

LaiueModuleStatus LaiueModuleHostUnregisterService(LaiueModuleHost *host, const char *name,
                                                   LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || !SafeName(name))
        return Fail(diagnostic, LAIUE_MODULE_SERVICE_INVALID, "invalid service name");
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    PlatformRwLockAcquireExclusive(&host->lock);
    LaiueModuleStatus status = LAIUE_MODULE_SERVICE_NOT_FOUND;
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        if (host->services[index].used && host->services[index].owner == LAIUE_MODULE_HOST_MAX_MODULES &&
            LaiueModAsciiEquals(host->services[index].value.name, name))
        {
            memset(&host->services[index], 0, sizeof(host->services[index]));
            status = LAIUE_MODULE_OK;
            break;
        }
    PlatformRwLockReleaseExclusive(&host->lock);
    End(host);
    return status;
}

static int CompareModuleIds(const void *left, const void *right, void *context)
{
    const LaiueModuleHost *host = context;
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    const char *first = host->modules[a].id;
    const char *second = host->modules[b].id;
    uint32_t index = 0u;
    while (first[index] != '\0' && first[index] == second[index])
        ++index;
    return (unsigned char)first[index] - (unsigned char)second[index];
}

static void StableSortIndices(LaiueModuleHost *host, uint32_t *indices, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        uint32_t value = indices[index];
        uint32_t position = index;
        while (position > 0u && CompareModuleIds(&value, &indices[position - 1u], host) < 0)
        {
            indices[position] = indices[position - 1u];
            --position;
        }
        indices[position] = value;
    }
}

static bool HasPotentialProvider(const LaiueModuleHost *host, uint32_t moduleCount,
                                 const char *name)
{
    for (uint32_t service = 0u; service < LAIUE_MODULE_HOST_MAX_SERVICES; ++service)
        if (host->services[service].used &&
            LaiueModAsciiEquals(host->services[service].value.name, name))
            return true;
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        const LoadedModule *module = &host->modules[index];
        if (!module->used)
            continue;
        const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
            if (LaiueModAsciiEquals(descriptor->providesServices[provided], name))
                return true;
    }
    return false;
}

static bool OptionalServicesReady(LaiueModuleHost *host, const LoadedModule *module,
                                  uint32_t moduleCount)
{
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    if (!DescriptorHasOptionalServices(descriptor)) return true;
    for (uint32_t index = 0u; index < descriptor->optionalCount; ++index)
    {
        const LaiueModuleRequirementV1 *optional = &descriptor->optionalServices[index];
        uint32_t version = 0u;
        if (ApiQuery((void *)module, optional->name, optional->minimumVersion, 1u, &version,
                     NULL) != NULL)
            continue;
        /* If a selected module can publish the optional service, defer this
         * consumer until that provider has started. If no provider was
         * selected, absence is the documented fallback path. */
        if (HasPotentialProvider(host, moduleCount, optional->name)) return false;
    }
    return true;
}

static bool HasProviderConflict(const LaiueModuleHost *host, uint32_t moduleCount)
{
    /* Resolve provider choice before invoking user code. A duplicate service
     * must be a deterministic graph error, not a start-order accident. */
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        const LoadedModule *module = &host->modules[index];
        const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
        {
            const char *name = descriptor->providesServices[provided];
            for (uint32_t service = 0u; service < LAIUE_MODULE_HOST_MAX_SERVICES; ++service)
                if (host->services[service].used &&
                    LaiueModAsciiEquals(host->services[service].value.name, name))
                    return true;
            for (uint32_t prior = 0u; prior < index; ++prior)
            {
                const LaiueModuleDescriptorV1 *priorDescriptor =
                    &host->modules[prior].api->descriptor;
                for (uint32_t priorProvided = 0u;
                     priorProvided < priorDescriptor->providesCount; ++priorProvided)
                    if (LaiueModAsciiEquals(priorDescriptor->providesServices[priorProvided], name))
                        return true;
            }
        }
    }
    return false;
}

LaiueModuleStatus LaiueModuleHostLoad(LaiueModuleHost *host, const LaiueModuleBinaryV1 *binaries,
                                      uint32_t count, LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || binaries == NULL || count == 0u || count > LAIUE_MODULE_HOST_MAX_MODULES)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module paths and count are required");
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    if (host->loadedCount != 0u)
    {
        End(host);
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "modules are already loaded");
    }
    memset(host->modules, 0, sizeof(host->modules));
    uint32_t indices[LAIUE_MODULE_HOST_MAX_MODULES];
    uint32_t moduleCount = 0u;
    for (uint32_t input = 0u; input < count; ++input)
    {
        bool optional = (binaries[input].flags & LAIUE_MODULE_BINARY_OPTIONAL) != 0u;
        bool staticArtifact = (binaries[input].flags & LAIUE_MODULE_BINARY_STATIC) != 0u;
        PlatformDynamicLibrary library = NULL;
        const LaiueModuleApiV1 *api = binaries[input].staticApi;
        if (staticArtifact)
        {
            if (api == NULL)
            {
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                            "static module API is missing");
            }
        }
        else
        {
            if (binaries[input].staticApi != NULL)
            {
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                            "static API requires static module flag");
            }
            if (binaries[input].path == NULL || binaries[input].path[0] == L'\0')
            {
                if (optional)
                    continue;
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module path is empty");
            }
            library = PlatformDynamicLibraryOpen(binaries[input].path);
            if (library == NULL)
            {
                /* Optional means absent, not silently ignored corruption or a
                 * missing transitive dependency. Once the selected path
                 * exists, every load/entry/ABI error remains fatal. */
                if (optional && !PlatformPathExists(binaries[input].path))
                    continue;
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_LOAD_FAILED,
                            "module library could not be loaded");
            }
            void *symbol = PlatformDynamicLibrarySymbol(library, LAIUE_MODULE_ENTRY_NAME_V1);
            if (symbol == NULL)
            {
                PlatformDynamicLibraryClose(library);
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_ENTRY_MISSING,
                            "module entry point is missing");
            }
            LaiueModuleGetApiFnV1 getApi = NULL;
            memcpy(&getApi, &symbol, sizeof(getApi));
            api = getApi();
        }
        if (api == NULL || api->structSize < offsetof(LaiueModuleApiV1, reserved))
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "module descriptor is invalid");
        }
        if (api->abiVersion != LAIUE_MODULE_ABI_VERSION_1 ||
            api->descriptor.abiVersion != LAIUE_MODULE_ABI_VERSION_1)
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_ABI_MISMATCH, "module ABI version is unsupported");
        }
        if (api->descriptor.structSize < offsetof(LaiueModuleDescriptorV1, reserved))
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "module descriptor is truncated");
        }
        if (api->create == NULL || api->destroy == NULL ||
            !CopyName(host->modules[moduleCount].id, api->descriptor.id) ||
            !CopyName(host->modules[moduleCount].version, api->descriptor.version))
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "module descriptor is invalid");
        }
        for (uint32_t prior = 0u; prior < moduleCount; ++prior)
            if (LaiueModAsciiEquals(host->modules[prior].id, host->modules[moduleCount].id))
            {
                PlatformDynamicLibraryClose(library);
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_ID, "module id is duplicated");
            }
        const LaiueModuleDescriptorV1 *descriptor = &api->descriptor;
        if ((descriptor->requiresCount != 0u && descriptor->requiresServices == NULL) ||
            (descriptor->providesCount != 0u && descriptor->providesServices == NULL) ||
            descriptor->requiresCount > LAIUE_MODULE_HOST_MAX_SERVICES ||
            descriptor->providesCount > LAIUE_MODULE_HOST_MAX_SERVICES)
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "module dependency arrays are invalid");
        }
        uint32_t optionalCount = DescriptorHasOptionalServices(descriptor)
                                     ? descriptor->optionalCount
                                     : 0u;
        const LaiueModuleRequirementV1 *optionalServices =
            DescriptorHasOptionalServices(descriptor) ? descriptor->optionalServices : NULL;
        if ((optionalCount != 0u && optionalServices == NULL) ||
            optionalCount > LAIUE_MODULE_HOST_MAX_SERVICES ||
            (DescriptorHasOptionalServices(descriptor) && descriptor->optionalReserved != 0u))
        {
            PlatformDynamicLibraryClose(library);
            Rollback(host, moduleCount);
            End(host);
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "module optional dependency arrays are invalid");
        }
        for (uint32_t requirement = 0u; requirement < descriptor->requiresCount; ++requirement)
        {
            const LaiueModuleRequirementV1 *required = &descriptor->requiresServices[requirement];
            if (required->minimumVersion == 0u || !SafeName(required->name))
            {
                PlatformDynamicLibraryClose(library);
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "module service requirement is invalid");
            }
        }
        for (uint32_t requirement = 0u; requirement < optionalCount; ++requirement)
        {
            const LaiueModuleRequirementV1 *optional = &optionalServices[requirement];
            if (optional->minimumVersion == 0u || !SafeName(optional->name))
            {
                PlatformDynamicLibraryClose(library);
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "module optional service requirement is invalid");
            }
        }
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
        {
            if (!SafeName(descriptor->providesServices[provided]))
            {
                PlatformDynamicLibraryClose(library);
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "module service declaration is invalid");
            }
            for (uint32_t prior = 0u; prior < provided; ++prior)
                if (LaiueModAsciiEquals(descriptor->providesServices[prior],
                                        descriptor->providesServices[provided]))
                {
                    PlatformDynamicLibraryClose(library);
                    Rollback(host, moduleCount);
                    End(host);
                    return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                                "module service declaration is duplicated");
                }
        }
        host->modules[moduleCount].api = api;
        host->modules[moduleCount].owner = host;
        host->modules[moduleCount].library = library;
        host->modules[moduleCount].used = true;
        indices[moduleCount] = moduleCount;
        ++moduleCount;
    }
    if (HasProviderConflict(host, moduleCount))
    {
        Rollback(host, moduleCount);
        End(host);
        return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_SERVICE,
                    "service provider selection is ambiguous");
    }
    StableSortIndices(host, indices, moduleCount);
    uint32_t created = 0u;
    uint32_t started = 0u;
    bool progress = true;
    while (started < moduleCount && progress)
    {
        progress = false;
        for (uint32_t order = 0u; order < moduleCount; ++order)
        {
            uint32_t index = indices[order];
            LoadedModule *module = &host->modules[index];
            if (module->created || !RequiredServicesReady(host, module) ||
                !OptionalServicesReady(host, module, moduleCount))
                continue;
            BuildApi(host, module);
            if (!module->api->create(&module->hostApi, &module->context))
            {
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_CREATE_FAILED, "module create callback failed");
            }
            module->created = true;
            module->createOrder = created++;
            if (module->api->start != NULL && !module->api->start(module->context))
            {
                Rollback(host, moduleCount);
                End(host);
                return Fail(diagnostic, LAIUE_MODULE_START_FAILED, "module start callback failed");
            }
            module->started = true;
            module->startOrder = started;
            ++started;
            ++host->loadedCount;
            progress = true;
        }
    }
    if (started != moduleCount)
    {
        bool missing = false;
        for (uint32_t index = 0u; index < moduleCount && !missing; ++index)
        {
            const LoadedModule *module = &host->modules[index];
            if (!module->used || module->created)
                continue;
            const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
            for (uint32_t requirement = 0u; requirement < descriptor->requiresCount;
                 ++requirement)
                if (!HasPotentialProvider(host, moduleCount,
                                          descriptor->requiresServices[requirement].name))
                {
                    missing = true;
                    break;
                }
        }
        Rollback(host, moduleCount);
        End(host);
        return Fail(diagnostic, missing ? LAIUE_MODULE_DEPENDENCY_MISSING
                                        : LAIUE_MODULE_DEPENDENCY_CYCLE,
                    missing ? "a module dependency is missing"
                            : "module dependencies contain a cycle");
    }
    End(host);
    return LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleHostLoadStatic(LaiueModuleHost *host,
                                            const LaiueModuleApiV1 *const *apis,
                                            uint32_t count,
                                            LaiueModuleDiagnostic *diagnostic)
{
    if (apis == NULL || count == 0u || count > LAIUE_MODULE_HOST_MAX_MODULES)
    {
        DiagnosticClear(diagnostic);
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "static module APIs and count are required");
    }
    LaiueModuleBinaryV1 binaries[LAIUE_MODULE_HOST_MAX_MODULES];
    for (uint32_t index = 0u; index < count; ++index)
    {
        binaries[index].path = NULL;
        binaries[index].flags = LAIUE_MODULE_BINARY_STATIC;
        binaries[index].staticApi = apis[index];
    }
    return LaiueModuleHostLoad(host, binaries, count, diagnostic);
}

void LaiueModuleHostUnloadAll(LaiueModuleHost *host)
{
    if (host == NULL || !Begin(host))
        return;
    for (uint32_t reverse = LAIUE_MODULE_HOST_MAX_MODULES; reverse > 0u; --reverse)
    {
        uint32_t order = reverse - 1u;
        for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
        {
            LoadedModule *module = &host->modules[index];
            if (!module->used || !module->started || module->startOrder != order)
                continue;
            if (module->api->stop != NULL)
                module->api->stop(module->context);
            module->started = false;
            RemoveOwnedServices(host, index);
        }
    }
    for (uint32_t reverse = LAIUE_MODULE_HOST_MAX_MODULES; reverse > 0u; --reverse)
    {
        uint32_t order = reverse - 1u;
        for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
        {
            LoadedModule *module = &host->modules[index];
            if (!module->used || !module->created || module->createOrder != order)
                continue;
            if (module->api->destroy != NULL)
                module->api->destroy(module->context);
            module->created = false;
        }
    }
    for (uint32_t reverse = LAIUE_MODULE_HOST_MAX_MODULES; reverse > 0u; --reverse)
    {
        uint32_t index = reverse - 1u;
        LoadedModule *module = &host->modules[index];
        if (module->used)
        {
            RemoveOwnedServices(host, index);
            PlatformDynamicLibraryClose(module->library);
            memset(module, 0, sizeof(*module));
        }
    }
    host->loadedCount = 0u;
    End(host);
}

uint32_t LaiueModuleHostLoadedCount(const LaiueModuleHost *host)
{
    if (host == NULL)
        return 0u;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    uint32_t count = host->loadedCount;
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return count;
}

uint32_t LaiueModuleHostIsLoaded(const LaiueModuleHost *host, const char *id)
{
    if (host == NULL || !SafeName(id))
        return 0u;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    uint32_t result = 0u;
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
        if (host->modules[index].used && LaiueModAsciiEquals(host->modules[index].id, id))
        {
            result = 1u;
            break;
        }
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return result;
}
