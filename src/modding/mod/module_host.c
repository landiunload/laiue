#include "mod/module_host.h"

#include "mod/mod_internal.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

/* Module sets are 64-bit masks and every module keeps one bit per service. */
_Static_assert(LAIUE_MODULE_HOST_MAX_MODULES <= 64u, "a module set must fit in 64 bits");
_Static_assert(LAIUE_MODULE_HOST_MAX_SERVICES % 32u == 0u, "service bits fill whole words");
#define SERVICE_WORDS (LAIUE_MODULE_HOST_MAX_SERVICES / 32u)

typedef struct ModuleService
{
    bool used;
    /* Published by a module of the load in progress. Only modules of that
     * load see it until the load commits, so nothing outside can keep a
     * pointer into a module that the load may still roll back. */
    bool pending;
    uint32_t owner;
    /* Precomputed from value.name at publish time. It is only a prefilter:
     * an equal hash still falls back to the exact name comparison, so a
     * collision cannot change which service is found. The service contract
     * keeps published names immutable for the lifetime of the registration. */
    uint32_t nameHash;
    LaiueModuleServiceV1 value;
} ModuleService;

typedef struct LoadedModule
{
    bool used;
    bool optional;
    bool created;
    bool started;
    /* Part of the load in progress; cleared when that load commits. */
    bool pending;
    /* Chosen for unloading. Its services are hidden from every module that
     * is not leaving with it, so no new user can appear before it stops. */
    bool unloading;
    PlatformDynamicLibrary library;
    const LaiueModuleApiV1 *api;
    LaiueModuleHost *owner;
    LaiueModuleHostV1 hostApi;
    void *context;
    char id[LAIUE_MODULE_MAX_NAME];
    char version[LAIUE_MODULE_MAX_NAME];
    uint64_t createOrder;
    uint64_t startOrder;
    /* One bit per service slot: the module obtained that table or declared
     * it as required. A provider never leaves while another loaded module
     * still holds one of its tables. */
    uint32_t usedServices[SERVICE_WORDS];
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
    /* Lifecycle order across every load of this host, so modules added at
     * run time unwind after the ones that started before them. */
    uint64_t nextCreateOrder;
    uint64_t nextStartOrder;
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
    static const char *const names[] = {"ok",
                                        "invalid argument",
                                        "ABI mismatch",
                                        "invalid descriptor",
                                        "duplicate module id",
                                        "duplicate service",
                                        "missing dependency",
                                        "dependency cycle",
                                        "library load failed",
                                        "entry point missing",
                                        "out of memory",
                                        "module capacity",
                                        "create failed",
                                        "start failed",
                                        "host busy",
                                        "invalid service",
                                        "service not found",
                                        "partial profile",
                                        "module not loaded",
                                        "in use"};
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

/* FNV-1a over the exact service name bytes. SafeName already guarantees the
 * string is bounded and NUL-terminated, so this cannot walk past it. */
static uint32_t NameHash(const char *name)
{
    uint32_t hash = 2166136261u;
    while (*name != '\0')
    {
        hash ^= (unsigned char)*name;
        hash *= 16777619u;
        ++name;
    }
    return hash;
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

/* Makes the modules and services of a successful load visible to everyone in
 * one step. Only one lifecycle operation runs at a time, so every pending
 * entry belongs to the load that commits. */
static void Commit(LaiueModuleHost *host)
{
    PlatformRwLockAcquireExclusive(&host->lock);
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
        host->modules[index].pending = false;
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        host->services[index].pending = false;
    PlatformRwLockReleaseExclusive(&host->lock);
}

static uint64_t SlotBit(uint32_t slot)
{
    return UINT64_C(1) << slot;
}

static bool UsesService(const LoadedModule *module, uint32_t service)
{
    return (module->usedServices[service / 32u] & (UINT32_C(1) << (service % 32u))) != 0u;
}

/* The caller holds the lock exclusively. Clearing the bit in every module
 * keeps a reused slot from inheriting the users of the service it held. */
static void ForgetService(LaiueModuleHost *host, uint32_t service)
{
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
        host->modules[index].usedServices[service / 32u] &= ~(UINT32_C(1) << (service % 32u));
    memset(&host->services[service], 0, sizeof(host->services[service]));
}

/* A load in progress and an unload in progress each see their own services;
 * everyone else sees neither. The application has no module and sees only
 * settled services. */
static bool ServiceVisible(const LaiueModuleHost *host, const ModuleService *service,
                           const LoadedModule *consumer)
{
    if (service->pending)
        return consumer != NULL && consumer->pending;
    if (service->owner < LAIUE_MODULE_HOST_MAX_MODULES && host->modules[service->owner].unloading)
        return consumer != NULL && consumer->unloading;
    return true;
}

static uint32_t FindService(const LaiueModuleHost *host, const char *name, uint32_t nameHash,
                            uint32_t minimumVersion, uint32_t minimumSize,
                            const LoadedModule *consumer)
{
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        const ModuleService *service = &host->services[index];
        if (service->used && service->nameHash == nameHash &&
            service->value.version >= minimumVersion && service->value.tableSize >= minimumSize &&
            ServiceVisible(host, service, consumer) &&
            LaiueModAsciiEquals(service->value.name, name))
            return index;
    }
    return LAIUE_MODULE_HOST_MAX_SERVICES;
}

/* Looks a service up for a module or, with consumer NULL, for the
 * application. With record set, a module that receives a table is noted as
 * its user, which keeps the provider loaded for as long as that module is. */
static const void *QueryFor(LaiueModuleHost *host, LoadedModule *consumer, const char *name,
                            uint32_t minimumVersion, uint32_t minimumSize, uint32_t *outVersion,
                            uint32_t *outSize, bool record)
{
    if (outVersion != NULL)
        *outVersion = 0u;
    if (outSize != NULL)
        *outSize = 0u;
    if (host == NULL || !SafeName(name))
        return NULL;
    const uint32_t nameHash = NameHash(name);
    const void *result = NULL;
    uint32_t version = 0u;
    uint32_t size = 0u;
    bool noted = true;
    PlatformRwLockAcquireShared(&host->lock);
    uint32_t index = FindService(host, name, nameHash, minimumVersion, minimumSize, consumer);
    if (index < LAIUE_MODULE_HOST_MAX_SERVICES)
    {
        result = host->services[index].value.table;
        version = host->services[index].value.version;
        size = host->services[index].value.tableSize;
        noted = !record || consumer == NULL || UsesService(consumer, index);
    }
    PlatformRwLockReleaseShared(&host->lock);
    if (!noted)
    {
        /* First use by this module. Look again under the exclusive lock, so
         * the service cannot go away between the lookup and the note. */
        result = NULL;
        PlatformRwLockAcquireExclusive(&host->lock);
        index = FindService(host, name, nameHash, minimumVersion, minimumSize, consumer);
        if (index < LAIUE_MODULE_HOST_MAX_SERVICES)
        {
            result = host->services[index].value.table;
            version = host->services[index].value.version;
            size = host->services[index].value.tableSize;
            consumer->usedServices[index / 32u] |= UINT32_C(1) << (index % 32u);
        }
        PlatformRwLockReleaseExclusive(&host->lock);
    }
    if (result != NULL && outVersion != NULL)
        *outVersion = version;
    if (result != NULL && outSize != NULL)
        *outSize = size;
    return result;
}

static void SetModuleStarted(LaiueModuleHost *host, LoadedModule *module, bool started)
{
    PlatformRwLockAcquireExclusive(&host->lock);
    module->started = started;
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
    return QueryFor(module == NULL ? NULL : module->owner, module, name, minimumVersion,
                    minimumSize, outVersion, outSize, true);
}

/* Kept as a separate public read path so applications can inspect optional
 * capabilities without retaining the private module callback table. */
const void *LaiueModuleHostQueryService(const LaiueModuleHost *host, const char *name,
                                        uint32_t minimumVersion, uint32_t minimumSize,
                                        uint32_t *outVersion, uint32_t *outSize)
{
    return QueryFor((LaiueModuleHost *)host, NULL, name, minimumVersion, minimumSize, outVersion,
                    outSize, false);
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
    const uint32_t serviceHash = NameHash(service->name);
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        ModuleService *current = &host->services[index];
        if (current->used && current->nameHash == serviceHash &&
            LaiueModAsciiEquals(current->value.name, service->name))
            return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_SERVICE, "service already published");
    }
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        ModuleService *current = &host->services[index];
        if (!current->used)
        {
            current->used = true;
            current->pending =
                owner < LAIUE_MODULE_HOST_MAX_MODULES && host->modules[owner].pending;
            current->owner = owner;
            current->nameHash = serviceHash;
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
    /* A module publishes only from its own create and start, while the load
     * that adds it is still in progress. */
    LaiueModuleStatus status =
        !module->pending || module->started
            ? LAIUE_MODULE_BUSY
            : PublishFor(host, (uint32_t)(module - host->modules), service, &diagnostic);
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
    const uint32_t nameHash = NameHash(name);
    PlatformRwLockAcquireExclusive(&host->lock);
    LaiueModuleStatus status =
        !module->pending || module->started ? LAIUE_MODULE_BUSY : LAIUE_MODULE_SERVICE_NOT_FOUND;
    for (uint32_t index = 0u; status != LAIUE_MODULE_BUSY &&
                                index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
    {
        if (host->services[index].used && host->services[index].owner == (uint32_t)(module - host->modules) &&
            host->services[index].nameHash == nameHash &&
            LaiueModAsciiEquals(host->services[index].value.name, name))
        {
            ForgetService(host, index);
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

static bool ServiceAvailable(LaiueModuleHost *host, LoadedModule *module, const char *name,
                             uint32_t minimumVersion)
{
    return QueryFor(host, module, name, minimumVersion, 1u, NULL, NULL, false) != NULL;
}

static bool RequiredServicesReady(LaiueModuleHost *host, LoadedModule *module)
{
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    for (uint32_t requirement = 0u; requirement < descriptor->requiresCount; ++requirement)
    {
        const LaiueModuleRequirementV1 *required = &descriptor->requiresServices[requirement];
        if (module->owner == NULL || required->name == NULL || required->minimumVersion == 0u ||
            !ServiceAvailable(host, module, required->name, required->minimumVersion))
            return false;
    }
    return true;
}

/* A declared requirement binds the consumer to its provider even if the
 * consumer never asks for the table itself. */
static void NoteRequiredServices(LaiueModuleHost *host, LoadedModule *module)
{
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    for (uint32_t requirement = 0u; requirement < descriptor->requiresCount; ++requirement)
    {
        const LaiueModuleRequirementV1 *required = &descriptor->requiresServices[requirement];
        (void)QueryFor(host, module, required->name, required->minimumVersion, 1u, NULL, NULL,
                       true);
    }
}

static bool DescriptorHasOptionalServices(const LaiueModuleDescriptorV1 *descriptor)
{
    return descriptor != NULL &&
           descriptor->optionalMagic == LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC;
}

static void RemoveOwnedServices(LaiueModuleHost *host, uint32_t owner)
{
    PlatformRwLockAcquireExclusive(&host->lock);
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        if (host->services[index].used && host->services[index].owner == owner)
            ForgetService(host, index);
    PlatformRwLockReleaseExclusive(&host->lock);
}

static void ReleaseSlot(LaiueModuleHost *host, uint32_t slot)
{
    /* create() and start() are allowed to publish services before they report
     * an error. Remove those entries before closing the library; otherwise the
     * service registry can retain pointers into an unloaded module. */
    RemoveOwnedServices(host, slot);
    PlatformDynamicLibraryClose(host->modules[slot].library);
    PlatformRwLockAcquireExclusive(&host->lock);
    memset(&host->modules[slot], 0, sizeof(host->modules[slot]));
    PlatformRwLockReleaseExclusive(&host->lock);
}

/* Bit c of users[p] is set when module c holds a table that module p
 * provides. The caller holds the lock. */
static void CollectUsers(const LaiueModuleHost *host, uint64_t users[LAIUE_MODULE_HOST_MAX_MODULES])
{
    memset(users, 0, sizeof(uint64_t) * LAIUE_MODULE_HOST_MAX_MODULES);
    for (uint32_t service = 0u; service < LAIUE_MODULE_HOST_MAX_SERVICES; ++service)
    {
        const ModuleService *entry = &host->services[service];
        if (!entry->used || entry->owner >= LAIUE_MODULE_HOST_MAX_MODULES)
            continue;
        for (uint32_t consumer = 0u; consumer < LAIUE_MODULE_HOST_MAX_MODULES; ++consumer)
            if (consumer != entry->owner && host->modules[consumer].used &&
                UsesService(&host->modules[consumer], service))
                users[entry->owner] |= SlotBit(consumer);
    }
}

/* Adds every module that holds a table of a member, until nothing changes. */
static uint64_t WithUsers(const uint64_t users[LAIUE_MODULE_HOST_MAX_MODULES], uint64_t members)
{
    uint64_t previous = 0u;
    while (previous != members)
    {
        previous = members;
        for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
            if ((members & SlotBit(slot)) != 0u)
                members |= users[slot];
    }
    return members;
}

static uint64_t UnwindKey(const LoadedModule *module)
{
    /* Started modules leave newest first. A module that was created but did
     * not start, or never got that far, was the last one its load touched. */
    if (module->started)
        return module->startOrder;
    return module->created ? UINT64_MAX - 1u : UINT64_MAX;
}

/* Orders members so that every module leaves before the modules whose tables
 * it holds, newest first among those that are free to go. A cycle of users,
 * possible only through tables obtained after start, falls back to the
 * newest member. The caller holds the lock. */
static uint32_t UnwindOrder(const LaiueModuleHost *host, uint64_t members,
                            uint32_t order[LAIUE_MODULE_HOST_MAX_MODULES])
{
    uint64_t users[LAIUE_MODULE_HOST_MAX_MODULES];
    CollectUsers(host, users);
    uint32_t count = 0u;
    while (members != 0u)
    {
        uint32_t best = LAIUE_MODULE_HOST_MAX_MODULES;
        uint32_t newest = LAIUE_MODULE_HOST_MAX_MODULES;
        for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        {
            if ((members & SlotBit(slot)) == 0u)
                continue;
            const uint64_t key = UnwindKey(&host->modules[slot]);
            if (newest == LAIUE_MODULE_HOST_MAX_MODULES || key > UnwindKey(&host->modules[newest]))
                newest = slot;
            if ((users[slot] & members & ~SlotBit(slot)) == 0u &&
                (best == LAIUE_MODULE_HOST_MAX_MODULES || key > UnwindKey(&host->modules[best])))
                best = slot;
        }
        const uint32_t next = best != LAIUE_MODULE_HOST_MAX_MODULES ? best : newest;
        order[count++] = next;
        members &= ~SlotBit(next);
    }
    return count;
}

/* Stops, destroys and closes a set of modules. Callers make sure no module
 * outside the set holds a table of a member; inside the set, users leave
 * before their providers. */
static void UnwindModules(LaiueModuleHost *host, uint64_t members)
{
    uint32_t order[LAIUE_MODULE_HOST_MAX_MODULES];
    PlatformRwLockAcquireExclusive(&host->lock);
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if ((members & SlotBit(slot)) != 0u && !host->modules[slot].used)
            members &= ~SlotBit(slot);
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if ((members & SlotBit(slot)) != 0u)
            host->modules[slot].unloading = true;
    const uint32_t count = UnwindOrder(host, members, order);
    PlatformRwLockReleaseExclusive(&host->lock);
    for (uint32_t index = 0u; index < count; ++index)
    {
        LoadedModule *module = &host->modules[order[index]];
        if (!module->started)
            continue;
        if (module->api->stop != NULL)
            module->api->stop(module->context);
        PlatformRwLockAcquireExclusive(&host->lock);
        module->started = false;
        if (host->loadedCount != 0u)
            --host->loadedCount;
        PlatformRwLockReleaseExclusive(&host->lock);
        RemoveOwnedServices(host, order[index]);
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        LoadedModule *module = &host->modules[order[index]];
        if (!module->created)
            continue;
        if (module->api->destroy != NULL)
            module->api->destroy(module->context);
        module->created = false;
    }
    for (uint32_t index = 0u; index < count; ++index)
        ReleaseSlot(host, order[index]);
}

static void RemoveUnstartedModule(LaiueModuleHost *host, uint32_t index)
{
    if (host == NULL || index >= LAIUE_MODULE_HOST_MAX_MODULES)
        return;
    LoadedModule *module = &host->modules[index];
    if (!module->used || module->started || module->created)
        return;
    ReleaseSlot(host, index);
}

static void RemoveStartedModule(LaiueModuleHost *host, uint32_t index)
{
    if (host == NULL || index >= LAIUE_MODULE_HOST_MAX_MODULES)
        return;
    LoadedModule *module = &host->modules[index];
    if (!module->used)
        return;
    if (module->started)
    {
        if (module->api->stop != NULL)
            module->api->stop(module->context);
        SetModuleStarted(host, module, false);
        RemoveOwnedServices(host, index);
        PlatformRwLockAcquireExclusive(&host->lock);
        if (host->loadedCount != 0u)
            --host->loadedCount;
        PlatformRwLockReleaseExclusive(&host->lock);
    }
    /* start() may publish a service and then fail before the started flag is
     * committed.  Service ownership is independent of that flag and must be
     * cleared on every failure path before the DLL is closed. */
    RemoveOwnedServices(host, index);
    if (module->created)
    {
        if (module->api->destroy != NULL)
            module->api->destroy(module->context);
        module->created = false;
    }
    ReleaseSlot(host, index);
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

/* Writes "<prefix><name>" as the diagnostic message. */
static LaiueModuleStatus FailNamed(LaiueModuleDiagnostic *diagnostic, LaiueModuleStatus status,
                                   const char *prefix, const char *name)
{
    char message[LAIUE_MODULE_DIAGNOSTIC_CAPACITY];
    uint32_t length = 0u;
    for (const char *part = prefix; *part != '\0' && length + 1u < sizeof(message); ++part)
        message[length++] = *part;
    for (const char *part = name; *part != '\0' && length + 1u < sizeof(message); ++part)
        message[length++] = *part;
    message[length] = '\0';
    return Fail(diagnostic, status, message);
}

/* Returns the first loaded module that holds the table in this slot, or
 * LAIUE_MODULE_HOST_MAX_MODULES. The caller holds the lock. */
static uint32_t ServiceUser(const LaiueModuleHost *host, uint32_t service)
{
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if (host->modules[slot].used && slot != host->services[service].owner &&
            UsesService(&host->modules[slot], service))
            return slot;
    return LAIUE_MODULE_HOST_MAX_MODULES;
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
    const uint32_t nameHash = NameHash(name);
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        if (host->services[index].used && host->services[index].owner == LAIUE_MODULE_HOST_MAX_MODULES &&
            host->services[index].nameHash == nameHash &&
            LaiueModAsciiEquals(host->services[index].value.name, name))
        {
            /* Checked and removed under one lock: a module cannot obtain the
             * table between the check and the removal. */
            const uint32_t user = ServiceUser(host, index);
            if (user < LAIUE_MODULE_HOST_MAX_MODULES)
                status = FailNamed(diagnostic, LAIUE_MODULE_IN_USE, "service is in use by ",
                                   host->modules[user].id);
            else
            {
                ForgetService(host, index);
                status = LAIUE_MODULE_OK;
            }
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

/* True while a module of the load in progress that has not started yet
 * declares the service. */
static bool HasPendingProvider(const LaiueModuleHost *host, const char *name)
{
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_MODULES; ++index)
    {
        const LoadedModule *module = &host->modules[index];
        if (!module->used || !module->pending || module->started)
            continue;
        const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
            if (LaiueModAsciiEquals(descriptor->providesServices[provided], name))
                return true;
    }
    return false;
}

static bool OptionalServicesReady(LaiueModuleHost *host, LoadedModule *module,
                                  const uint32_t *ignoredOptionalServices,
                                  uint32_t *outBlockedIndex)
{
    const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
    if (outBlockedIndex != NULL)
        *outBlockedIndex = LAIUE_MODULE_HOST_MAX_SERVICES;
    if (!DescriptorHasOptionalServices(descriptor)) return true;
    for (uint32_t index = 0u; index < descriptor->optionalCount; ++index)
    {
        if (ignoredOptionalServices != NULL &&
            (ignoredOptionalServices[index / 32u] & (UINT32_C(1) << (index % 32u))) != 0u)
            continue;
        const LaiueModuleRequirementV1 *optional = &descriptor->optionalServices[index];
        if (ServiceAvailable(host, module, optional->name, optional->minimumVersion))
            continue;
        /* Prefer a compatible module provider when it can still start. If the
         * graph stalls on optional edges, the caller retries without waiting:
         * optional capabilities must never turn fallback into a hard cycle. */
        if (HasPendingProvider(host, optional->name))
        {
            if (outBlockedIndex != NULL)
                *outBlockedIndex = index;
            return false;
        }
    }
    return true;
}

/* Resolves provider choice before invoking user code. A duplicate service
 * must be a deterministic graph error, not a start-order accident. Returns
 * why the new modules cannot provide their services, or NULL. The registry
 * already holds the services of every module loaded before. */
static const char *ProviderConflict(const LaiueModuleHost *host, const uint32_t *slots,
                                    uint32_t moduleCount)
{
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        const LoadedModule *module = &host->modules[slots[index]];
        const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
        {
            const char *name = descriptor->providesServices[provided];
            for (uint32_t service = 0u; service < LAIUE_MODULE_HOST_MAX_SERVICES; ++service)
                if (host->services[service].used &&
                    LaiueModAsciiEquals(host->services[service].value.name, name))
                    return "service is already provided";
            for (uint32_t prior = 0u; prior < index; ++prior)
            {
                const LaiueModuleDescriptorV1 *priorDescriptor =
                    &host->modules[slots[prior]].api->descriptor;
                for (uint32_t priorProvided = 0u;
                     priorProvided < priorDescriptor->providesCount; ++priorProvided)
                    if (LaiueModAsciiEquals(priorDescriptor->providesServices[priorProvided], name))
                        return "service provider selection is ambiguous";
            }
        }
    }
    return NULL;
}

/* Returns why a descriptor's dependency and service lists are unusable, or
 * NULL when they are valid. */
static const char *DescriptorProblem(const LaiueModuleDescriptorV1 *descriptor)
{
    if ((descriptor->requiresCount != 0u && descriptor->requiresServices == NULL) ||
        (descriptor->providesCount != 0u && descriptor->providesServices == NULL) ||
        descriptor->requiresCount > LAIUE_MODULE_HOST_MAX_SERVICES ||
        descriptor->providesCount > LAIUE_MODULE_HOST_MAX_SERVICES)
        return "module dependency arrays are invalid";
    const bool hasOptional = DescriptorHasOptionalServices(descriptor);
    const uint32_t optionalCount = hasOptional ? descriptor->optionalCount : 0u;
    const LaiueModuleRequirementV1 *optionalServices =
        hasOptional ? descriptor->optionalServices : NULL;
    if ((optionalCount != 0u && optionalServices == NULL) ||
        optionalCount > LAIUE_MODULE_HOST_MAX_SERVICES ||
        (hasOptional && descriptor->optionalReserved != 0u))
        return "module optional dependency arrays are invalid";
    for (uint32_t index = 0u; index < descriptor->requiresCount; ++index)
        if (descriptor->requiresServices[index].minimumVersion == 0u ||
            !SafeName(descriptor->requiresServices[index].name))
            return "module service requirement is invalid";
    for (uint32_t index = 0u; index < optionalCount; ++index)
        if (optionalServices[index].minimumVersion == 0u || !SafeName(optionalServices[index].name))
            return "module optional service requirement is invalid";
    for (uint32_t index = 0u; index < descriptor->providesCount; ++index)
    {
        if (!SafeName(descriptor->providesServices[index]))
            return "module service declaration is invalid";
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (LaiueModAsciiEquals(descriptor->providesServices[prior],
                                    descriptor->providesServices[index]))
                return "module service declaration is duplicated";
    }
    return NULL;
}

/* Checks a module API before any of its code runs and copies its id and
 * version. On failure *outProblem says what is wrong. */
static LaiueModuleStatus ValidateApi(const LaiueModuleApiV1 *api, char id[LAIUE_MODULE_MAX_NAME],
                                     char version[LAIUE_MODULE_MAX_NAME], const char **outProblem)
{
    *outProblem = "module descriptor is invalid";
    if (api == NULL || api->structSize < offsetof(LaiueModuleApiV1, reserved))
        return LAIUE_MODULE_DESCRIPTOR_INVALID;
    if (api->abiVersion != LAIUE_MODULE_ABI_VERSION_1 ||
        api->descriptor.abiVersion != LAIUE_MODULE_ABI_VERSION_1)
    {
        *outProblem = "module ABI version is unsupported";
        return LAIUE_MODULE_ABI_MISMATCH;
    }
    if (api->descriptor.structSize < offsetof(LaiueModuleDescriptorV1, reserved))
    {
        *outProblem = "module descriptor is truncated";
        return LAIUE_MODULE_DESCRIPTOR_INVALID;
    }
    if (api->create == NULL || api->destroy == NULL || !CopyName(id, api->descriptor.id) ||
        !CopyName(version, api->descriptor.version))
        return LAIUE_MODULE_DESCRIPTOR_INVALID;
    *outProblem = DescriptorProblem(&api->descriptor);
    return *outProblem == NULL ? LAIUE_MODULE_OK : LAIUE_MODULE_DESCRIPTOR_INVALID;
}

/* Returns the slot of the loaded module with this id, or
 * LAIUE_MODULE_HOST_MAX_MODULES. */
static uint32_t FindModule(const LaiueModuleHost *host, const char *id)
{
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if (host->modules[slot].used && LaiueModAsciiEquals(host->modules[slot].id, id))
            return slot;
    return LAIUE_MODULE_HOST_MAX_MODULES;
}

static uint32_t SetSize(uint64_t set)
{
    uint32_t size = 0u;
    for (; set != 0u; set &= set - 1u)
        ++size;
    return size;
}

/* Returns the lowest slot in a set, or LAIUE_MODULE_HOST_MAX_MODULES. */
static uint32_t FirstSlot(uint64_t set)
{
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if ((set & SlotBit(slot)) != 0u)
            return slot;
    return LAIUE_MODULE_HOST_MAX_MODULES;
}

typedef struct ProfileCandidate
{
    bool active;
    bool planned;
    bool optional;
    PlatformDynamicLibrary probeLibrary;
    const LaiueModuleApiV1 *api;
    LaiueModuleStatus status;
    uint32_t flags;
    char id[LAIUE_MODULE_MAX_NAME];
    char message[LAIUE_MODULE_DIAGNOSTIC_CAPACITY];
} ProfileCandidate;

static void ProfileCopyMessage(char destination[LAIUE_MODULE_DIAGNOSTIC_CAPACITY],
                               const char *source)
{
    uint32_t index = 0u;
    if (source != NULL)
        while (source[index] != '\0' && index + 1u < LAIUE_MODULE_DIAGNOSTIC_CAPACITY)
        {
            destination[index] = source[index];
            ++index;
        }
    destination[index] = '\0';
}

static void ProfileCopyId(char destination[LAIUE_MODULE_MAX_NAME], const char *source)
{
    uint32_t index = 0u;
    if (source != NULL)
        while (source[index] != '\0' && index + 1u < LAIUE_MODULE_MAX_NAME)
        {
            destination[index] = source[index];
            ++index;
        }
    destination[index] = '\0';
}

static void ProfileSetFailure(ProfileCandidate *candidate, LaiueModuleStatus status,
                              const char *message)
{
    candidate->active = false;
    candidate->status = status;
    candidate->flags = LAIUE_MODULE_PROFILE_ENTRY_SKIPPED |
                       LAIUE_MODULE_PROFILE_ENTRY_DISABLED;
    ProfileCopyMessage(candidate->message, message);
}

static LaiueModuleStatus ProfileValidateApi(ProfileCandidate *candidate,
                                            const LaiueModuleApiV1 *api)
{
    char version[LAIUE_MODULE_MAX_NAME];
    const char *problem = NULL;
    const LaiueModuleStatus status = ValidateApi(api, candidate->id, version, &problem);
    if (status != LAIUE_MODULE_OK)
        return status;
    candidate->api = api;
    candidate->active = true;
    candidate->status = LAIUE_MODULE_OK;
    candidate->flags = 0u;
    candidate->message[0] = '\0';
    return LAIUE_MODULE_OK;
}

static LaiueModuleStatus ProfileProbeBinary(const LaiueModuleBinaryV1 *binary,
                                            ProfileCandidate *candidate)
{
    memset(candidate, 0, sizeof(*candidate));
    candidate->optional = binary != NULL &&
                          (binary->flags & LAIUE_MODULE_BINARY_OPTIONAL) != 0u;
    if (binary == NULL)
    {
        ProfileSetFailure(candidate, LAIUE_MODULE_INVALID_ARGUMENT, "module entry is null");
        return candidate->status;
    }
    const LaiueModuleApiV1 *api = binary->staticApi;
    if ((binary->flags & LAIUE_MODULE_BINARY_STATIC) != 0u)
    {
        if (api == NULL)
        {
            ProfileSetFailure(candidate, LAIUE_MODULE_INVALID_ARGUMENT,
                              "static module API is missing");
            return candidate->status;
        }
    }
    else
    {
        if (api != NULL)
        {
            ProfileSetFailure(candidate, LAIUE_MODULE_INVALID_ARGUMENT,
                              "static API requires static module flag");
            return candidate->status;
        }
        if (binary->path == NULL || binary->path[0] == L'\0')
        {
            if (candidate->optional)
            {
                candidate->status = LAIUE_MODULE_OK;
                candidate->flags = LAIUE_MODULE_PROFILE_ENTRY_SKIPPED;
                ProfileCopyMessage(candidate->message, "optional module path is empty");
                return LAIUE_MODULE_OK;
            }
            ProfileSetFailure(candidate, LAIUE_MODULE_INVALID_ARGUMENT,
                              "module path is empty");
            return candidate->status;
        }
        candidate->probeLibrary = PlatformDynamicLibraryOpen(binary->path);
        if (candidate->probeLibrary == NULL)
        {
            if (candidate->optional && !PlatformPathExists(binary->path))
            {
                candidate->status = LAIUE_MODULE_OK;
                candidate->flags = LAIUE_MODULE_PROFILE_ENTRY_SKIPPED;
                ProfileCopyMessage(candidate->message, "optional artifact is not present");
                return LAIUE_MODULE_OK;
            }
            ProfileSetFailure(candidate, LAIUE_MODULE_LOAD_FAILED,
                              "module library could not be loaded");
            return candidate->status;
        }
        void *symbol = PlatformDynamicLibrarySymbol(candidate->probeLibrary,
                                                    LAIUE_MODULE_ENTRY_NAME_V1);
        if (symbol == NULL)
        {
            PlatformDynamicLibraryClose(candidate->probeLibrary);
            candidate->probeLibrary = NULL;
            ProfileSetFailure(candidate, LAIUE_MODULE_ENTRY_MISSING,
                              "module entry point is missing");
            return candidate->status;
        }
        LaiueModuleGetApiFnV1 getApi = NULL;
        memcpy((void *)&getApi, (const void *)&symbol, sizeof(getApi));
        api = getApi == NULL ? NULL : getApi();
    }
    const LaiueModuleStatus status = ProfileValidateApi(candidate, api);
    if (status != LAIUE_MODULE_OK)
    {
        if (candidate->probeLibrary != NULL)
        {
            PlatformDynamicLibraryClose(candidate->probeLibrary);
            candidate->probeLibrary = NULL;
        }
        ProfileSetFailure(candidate, status, LaiueModuleStatusString(status));
    }
    return status;
}

static void ProfileCloseProbes(ProfileCandidate *candidates, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
        if (candidates[index].probeLibrary != NULL)
        {
            PlatformDynamicLibraryClose(candidates[index].probeLibrary);
            candidates[index].probeLibrary = NULL;
        }
}

static bool ProfileCandidateProvides(const ProfileCandidate *candidate, const char *name)
{
    if (candidate == NULL || !candidate->active || candidate->api == NULL)
        return false;
    const LaiueModuleDescriptorV1 *descriptor = &candidate->api->descriptor;
    for (uint32_t index = 0u; index < descriptor->providesCount; ++index)
        if (LaiueModAsciiEquals(descriptor->providesServices[index], name))
            return true;
    return false;
}

static bool ProfileHostProvides(const LaiueModuleHost *host, const char *name)
{
    for (uint32_t index = 0u; index < LAIUE_MODULE_HOST_MAX_SERVICES; ++index)
        if (host->services[index].used &&
            LaiueModAsciiEquals(host->services[index].value.name, name))
            return true;
    return false;
}

static bool ProfilePlannedProvides(const ProfileCandidate *candidates, uint32_t count,
                                   const bool planned[LAIUE_MODULE_HOST_MAX_MODULES],
                                   const char *name)
{
    for (uint32_t index = 0u; index < count; ++index)
        if (planned[index] && ProfileCandidateProvides(&candidates[index], name))
            return true;
    return false;
}

static const char *ProfileSelectionFor(const LaiueModuleProfileV1 *profile,
                                       const char *serviceName)
{
    if (profile == NULL || serviceName == NULL)
        return NULL;
    for (uint32_t index = 0u; index < profile->providerSelectionCount; ++index)
    {
        const LaiueModuleProviderSelectionV1 *selection =
            &profile->providerSelections[index];
        if (LaiueModAsciiEquals(selection->serviceName, serviceName))
            return selection->moduleId;
    }
    return NULL;
}

static void ProfileFillReport(LaiueModuleLoadReportV1 *report,
                              const ProfileCandidate *candidates, uint32_t count,
                              uint32_t loadedCount)
{
    if (report == NULL)
        return;
    report->count = count < report->capacity ? count : report->capacity;
    report->loadedCount = loadedCount;
    report->skippedCount = 0u;
    for (uint32_t index = 0u; index < report->count; ++index)
    {
        LaiueModuleLoadReportEntryV1 *entry = &report->entries[index];
        memset(entry, 0, sizeof(*entry));
        entry->structSize = sizeof(*entry);
        entry->status = candidates[index].status;
        entry->flags = candidates[index].flags;
        ProfileCopyId(entry->id, candidates[index].id);
        ProfileCopyMessage(entry->message, candidates[index].message);
        if ((entry->flags & LAIUE_MODULE_PROFILE_ENTRY_SKIPPED) != 0u)
            ++report->skippedCount;
    }
}

void LaiueModuleLoadReportInitialize(LaiueModuleLoadReportV1 *report,
                                     LaiueModuleLoadReportEntryV1 *entries,
                                     uint32_t capacity)
{
    if (report == NULL)
        return;
    memset(report, 0, sizeof(*report));
    report->structSize = sizeof(*report);
    report->entries = entries;
    report->capacity = capacity;
    if (entries != NULL)
        memset(entries, 0, sizeof(*entries) * capacity);
}

/* Opens one entry of a load into a free slot and checks it. On success the
 * slot belongs to the load in progress. A missing optional artifact returns
 * OK with *outSkipped set; any other failure closes the library, leaves the
 * slot free and fills the diagnostic. */
static LaiueModuleStatus OpenEntry(LaiueModuleHost *host, const LaiueModuleBinaryV1 *binary,
                                   uint32_t slot, bool *outSkipped,
                                   LaiueModuleDiagnostic *diagnostic)
{
    *outSkipped = false;
    const bool optional = (binary->flags & LAIUE_MODULE_BINARY_OPTIONAL) != 0u;
    PlatformDynamicLibrary library = NULL;
    const LaiueModuleApiV1 *api = binary->staticApi;
    if ((binary->flags & LAIUE_MODULE_BINARY_STATIC) != 0u)
    {
        if (api == NULL)
            return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "static module API is missing");
    }
    else
    {
        if (api != NULL)
            return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                        "static API requires static module flag");
        if (binary->path == NULL || binary->path[0] == L'\0')
        {
            *outSkipped = optional;
            return optional
                       ? LAIUE_MODULE_OK
                       : Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module path is empty");
        }
        library = PlatformDynamicLibraryOpen(binary->path);
        if (library == NULL)
        {
            /* Optional means absent, not silently ignored corruption or a
             * missing transitive dependency. Once the selected path exists,
             * every load/entry/ABI error remains fatal. */
            *outSkipped = optional && !PlatformPathExists(binary->path);
            return *outSkipped ? LAIUE_MODULE_OK
                               : Fail(diagnostic, LAIUE_MODULE_LOAD_FAILED,
                                      "module library could not be loaded");
        }
        void *symbol = PlatformDynamicLibrarySymbol(library, LAIUE_MODULE_ENTRY_NAME_V1);
        if (symbol == NULL)
        {
            PlatformDynamicLibraryClose(library);
            return Fail(diagnostic, LAIUE_MODULE_ENTRY_MISSING, "module entry point is missing");
        }
        LaiueModuleGetApiFnV1 getApi = NULL;
        memcpy((void *)&getApi, (const void *)&symbol, sizeof(getApi));
        api = getApi();
    }
    LoadedModule *module = &host->modules[slot];
    const char *problem = NULL;
    LaiueModuleStatus status = ValidateApi(api, module->id, module->version, &problem);
    for (uint32_t other = 0u; status == LAIUE_MODULE_OK && other < LAIUE_MODULE_HOST_MAX_MODULES;
         ++other)
        if (host->modules[other].used && LaiueModAsciiEquals(host->modules[other].id, module->id))
        {
            status = LAIUE_MODULE_DUPLICATE_ID;
            problem = host->modules[other].pending ? "module id is duplicated"
                                                   : "module is already loaded";
        }
    if (status != LAIUE_MODULE_OK)
    {
        PlatformDynamicLibraryClose(library);
        return Fail(diagnostic, status, problem);
    }
    PlatformRwLockAcquireExclusive(&host->lock);
    module->api = api;
    module->owner = host;
    module->library = library;
    module->optional = optional;
    module->pending = true;
    module->used = true;
    PlatformRwLockReleaseExclusive(&host->lock);
    return LAIUE_MODULE_OK;
}

/* Starts the given modules next to the ones already running; the caller has
 * begun a lifecycle operation. The load is a transaction over its own modules:
 * on failure they are unwound and the running set stays as it was, and until
 * it commits nothing outside the load sees their services. With
 * allowOptionalFailures, an optional module whose callback fails, or that is
 * left without a dependency, leaves alone; failureKinds then marks its entry
 * with 1 or 2. outLoaded receives the slots that started. */
static LaiueModuleStatus LoadModules(LaiueModuleHost *host, const LaiueModuleBinaryV1 *binaries,
                                     uint32_t count, LaiueModuleDiagnostic *diagnostic,
                                     bool allowOptionalFailures, uint8_t *failureKinds,
                                     uint64_t *outLoaded)
{
    if (failureKinds != NULL)
        memset(failureKinds, 0, count * sizeof(*failureKinds));
    if (outLoaded != NULL)
        *outLoaded = 0u;
    uint32_t slots[LAIUE_MODULE_HOST_MAX_MODULES] = {0u};
    uint32_t inputs[LAIUE_MODULE_HOST_MAX_MODULES] = {0u};
    uint64_t members = 0u;
    uint32_t moduleCount = 0u;
    for (uint32_t input = 0u; input < count; ++input)
    {
        uint32_t slot = 0u;
        while (slot < LAIUE_MODULE_HOST_MAX_MODULES && host->modules[slot].used)
            ++slot;
        if (slot == LAIUE_MODULE_HOST_MAX_MODULES)
        {
            UnwindModules(host, members);
            return Fail(diagnostic, LAIUE_MODULE_CAPACITY, "every module slot is in use");
        }
        bool skipped = false;
        const LaiueModuleStatus status =
            OpenEntry(host, &binaries[input], slot, &skipped, diagnostic);
        if (status != LAIUE_MODULE_OK)
        {
            UnwindModules(host, members);
            return status;
        }
        if (skipped)
            continue;
        slots[moduleCount++] = slot;
        inputs[slot] = input;
        members |= SlotBit(slot);
    }
    const char *conflict = ProviderConflict(host, slots, moduleCount);
    if (conflict != NULL)
    {
        UnwindModules(host, members);
        return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_SERVICE, conflict);
    }
    StableSortIndices(host, slots, moduleCount);
    uint32_t activeCount = moduleCount;
    uint32_t started = 0u;
    bool partial = false;
    uint32_t ignoredOptionalServices[LAIUE_MODULE_HOST_MAX_MODULES][SERVICE_WORDS] = {{0u}};
    bool progress = true;
    while (started < activeCount && progress)
    {
        progress = false;
        bool deferredForOptional = false;
        uint32_t fallbackModuleIndex = LAIUE_MODULE_HOST_MAX_MODULES;
        uint32_t fallbackOptionalIndex = LAIUE_MODULE_HOST_MAX_SERVICES;
        for (uint32_t order = 0u; order < moduleCount; ++order)
        {
            const uint32_t index = slots[order];
            LoadedModule *module = &host->modules[index];
            // A failed optional module is removed in place and its slot
            // cleared; later passes must not read its API through it.
            if (!module->used || module->created || !RequiredServicesReady(host, module))
                continue;
            uint32_t blockedOptionalIndex = LAIUE_MODULE_HOST_MAX_SERVICES;
            if (!OptionalServicesReady(host, module, ignoredOptionalServices[index],
                                       &blockedOptionalIndex))
            {
                deferredForOptional = true;
                if (fallbackModuleIndex == LAIUE_MODULE_HOST_MAX_MODULES)
                {
                    fallbackModuleIndex = index;
                    fallbackOptionalIndex = blockedOptionalIndex;
                }
                continue;
            }
            NoteRequiredServices(host, module);
            BuildApi(host, module);
            if (!module->api->create(&module->hostApi, &module->context))
            {
                if (allowOptionalFailures && module->optional)
                {
                    if (failureKinds != NULL)
                        failureKinds[inputs[index]] |= 1u;
                    RemoveUnstartedModule(host, index);
                    members &= ~SlotBit(index);
                    --activeCount;
                    partial = true;
                    progress = true;
                    continue;
                }
                UnwindModules(host, members);
                return Fail(diagnostic, LAIUE_MODULE_CREATE_FAILED, "module create callback failed");
            }
            module->created = true;
            module->createOrder = host->nextCreateOrder++;
            if (module->api->start != NULL && !module->api->start(module->context))
            {
                if (allowOptionalFailures && module->optional)
                {
                    if (failureKinds != NULL)
                        failureKinds[inputs[index]] |= 1u;
                    RemoveStartedModule(host, index);
                    members &= ~SlotBit(index);
                    --activeCount;
                    partial = true;
                    progress = true;
                    continue;
                }
                UnwindModules(host, members);
                return Fail(diagnostic, LAIUE_MODULE_START_FAILED, "module start callback failed");
            }
            PlatformRwLockAcquireExclusive(&host->lock);
            module->started = true;
            module->startOrder = host->nextStartOrder++;
            ++host->loadedCount;
            PlatformRwLockReleaseExclusive(&host->lock);
            ++started;
            progress = true;
        }
        if (!progress && deferredForOptional &&
            fallbackModuleIndex != LAIUE_MODULE_HOST_MAX_MODULES &&
            fallbackOptionalIndex != LAIUE_MODULE_HOST_MAX_SERVICES)
        {
            /* Drop one blocked optional edge at a time. Starting that consumer
             * may unlock its provider and the rest of the dependency chain. */
            ignoredOptionalServices[fallbackModuleIndex][fallbackOptionalIndex / 32u] |=
                UINT32_C(1) << (fallbackOptionalIndex % 32u);
            progress = true;
        }
    }
    if (allowOptionalFailures)
    {
        bool removed = true;
        while (removed)
        {
            removed = false;
            for (uint32_t order = 0u; order < moduleCount; ++order)
            {
                const uint32_t index = slots[order];
                const LoadedModule *module = &host->modules[index];
                if (!module->used || module->created || !module->optional)
                    continue;
                if (failureKinds != NULL)
                    failureKinds[inputs[index]] |= 2u;
                RemoveUnstartedModule(host, index);
                members &= ~SlotBit(index);
                --activeCount;
                partial = true;
                removed = true;
            }
        }
    }
    if (started != activeCount)
    {
        bool missing = false;
        for (uint32_t order = 0u; order < moduleCount && !missing; ++order)
        {
            const LoadedModule *module = &host->modules[slots[order]];
            if (!module->used || module->created)
                continue;
            const LaiueModuleDescriptorV1 *descriptor = &module->api->descriptor;
            for (uint32_t requirement = 0u; requirement < descriptor->requiresCount;
                 ++requirement)
                if (!HasPendingProvider(host, descriptor->requiresServices[requirement].name))
                {
                    missing = true;
                    break;
                }
        }
        UnwindModules(host, members);
        return Fail(diagnostic, missing ? LAIUE_MODULE_DEPENDENCY_MISSING
                                        : LAIUE_MODULE_DEPENDENCY_CYCLE,
                    missing ? "a module dependency is missing"
                            : "module dependencies contain a cycle");
    }
    Commit(host);
    if (outLoaded != NULL)
        *outLoaded = members;
    return partial ? Fail(diagnostic, LAIUE_MODULE_PARTIAL, "profile disabled an optional module")
                   : LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleHostLoad(LaiueModuleHost *host, const LaiueModuleBinaryV1 *binaries,
                                      uint32_t count, LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || binaries == NULL || count == 0u || count > LAIUE_MODULE_HOST_MAX_MODULES)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "module paths and count are required");
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    const LaiueModuleStatus status =
        LoadModules(host, binaries, count, diagnostic, false, NULL, NULL);
    End(host);
    return status;
}

/* Plans and loads a profile next to the modules already running. The caller
 * has begun a lifecycle operation and owns the scratch arrays. */
static LaiueModuleStatus ProfileLoad(LaiueModuleHost *host, const LaiueModuleProfileV1 *profile,
                                     LaiueModuleLoadReportV1 *report,
                                     LaiueModuleDiagnostic *diagnostic,
                                     ProfileCandidate *candidates, LaiueModuleBinaryV1 *selected,
                                     bool *planned)
{
    const LaiueModuleBinaryV1 *binaries = profile->binaries;
    const uint32_t count = profile->binaryCount;
    const bool allowPartial = (profile->flags & LAIUE_MODULE_PROFILE_ALLOW_PARTIAL) != 0u;
    for (uint32_t index = 0u; index < count; ++index)
        (void)ProfileProbeBinary(&binaries[index], &candidates[index]);

    /* Duplicate IDs and providers are resolved before any module callback.
     * A profile must pin an alternative provider explicitly. ID order is used
     * only for the stable lifecycle order; it never silently chooses a
     * technology implementation. */
    for (uint32_t left = 0u; left < count; ++left)
        if (candidates[left].active)
            for (uint32_t right = left + 1u; right < count; ++right)
                if (candidates[right].active &&
                    LaiueModAsciiEquals(candidates[left].id, candidates[right].id))
                    ProfileSetFailure(&candidates[right], LAIUE_MODULE_DUPLICATE_ID,
                                      "module id is duplicated");
    for (uint32_t index = 0u; index < count; ++index)
    {
        ProfileCandidate *candidate = &candidates[index];
        if (!candidate->active)
            continue;
        if (FindModule(host, candidate->id) < LAIUE_MODULE_HOST_MAX_MODULES)
        {
            ProfileSetFailure(candidate, LAIUE_MODULE_DUPLICATE_ID, "module is already loaded");
            continue;
        }
        const LaiueModuleDescriptorV1 *descriptor = &candidate->api->descriptor;
        for (uint32_t provided = 0u; provided < descriptor->providesCount; ++provided)
        {
            const char *name = descriptor->providesServices[provided];
            if (ProfileHostProvides(host, name))
            {
                ProfileSetFailure(candidate, LAIUE_MODULE_DUPLICATE_SERVICE,
                                  "service is already provided");
                break;
            }
        }
    }
    bool invalidProviderSelection = false;
    for (uint32_t selectionIndex = 0u;
         selectionIndex < profile->providerSelectionCount; ++selectionIndex)
    {
        const LaiueModuleProviderSelectionV1 *selection =
            &profile->providerSelections[selectionIndex];
        bool selectedProviderFound = false;
        bool serviceProviderFound = false;
        if (ProfileHostProvides(host, selection->serviceName))
        {
            invalidProviderSelection = true;
            continue;
        }
        for (uint32_t candidateIndex = 0u; candidateIndex < count; ++candidateIndex)
        {
            ProfileCandidate *candidate = &candidates[candidateIndex];
            if (!candidate->active ||
                !ProfileCandidateProvides(candidate, selection->serviceName))
                continue;
            serviceProviderFound = true;
            if (LaiueModAsciiEquals(candidate->id, selection->moduleId))
                selectedProviderFound = true;
        }
        if (!selectedProviderFound || !serviceProviderFound)
        {
            /* A partial profile treats an absent or unloadable selected
             * artifact like any other missing optional technology: the
             * service has no provider, competing providers stay disabled
             * instead of becoming a hidden fallback, and independent modules
             * still start.  A strict profile keeps rejecting the mismatch. */
            if (!allowPartial)
                invalidProviderSelection = true;
            for (uint32_t candidateIndex = 0u; candidateIndex < count; ++candidateIndex)
            {
                ProfileCandidate *candidate = &candidates[candidateIndex];
                if (candidate->active &&
                    ProfileCandidateProvides(candidate, selection->serviceName))
                    ProfileSetFailure(candidate,
                                      allowPartial ? LAIUE_MODULE_DUPLICATE_SERVICE
                                                   : LAIUE_MODULE_INVALID_ARGUMENT,
                                      allowPartial ? "selected service provider is unavailable"
                                                   : "selected service provider is not present");
            }
        }
    }
    if (invalidProviderSelection)
    {
        ProfileFillReport(report, candidates, count, 0u);
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "provider selection does not match the profile");
    }

    for (uint32_t left = 0u; left < count; ++left)
        if (candidates[left].active)
            for (uint32_t right = left + 1u; right < count; ++right)
                if (candidates[right].active)
                {
                    const LaiueModuleDescriptorV1 *leftDescriptor =
                        &candidates[left].api->descriptor;
                    const LaiueModuleDescriptorV1 *rightDescriptor =
                        &candidates[right].api->descriptor;
                    bool conflict = false;
                    const char *conflictService = NULL;
                    for (uint32_t a = 0u; a < leftDescriptor->providesCount && !conflict; ++a)
                        for (uint32_t b = 0u; b < rightDescriptor->providesCount; ++b)
                            if (LaiueModAsciiEquals(leftDescriptor->providesServices[a],
                                                    rightDescriptor->providesServices[b]))
                            {
                                conflict = true;
                                conflictService = leftDescriptor->providesServices[a];
                                break;
                            }
                    if (conflict)
                    {
                        const char *selection = ProfileSelectionFor(profile, conflictService);
                        ProfileCandidate *loser = NULL;
                        if (selection != NULL)
                        {
                            const bool leftSelected =
                                LaiueModAsciiEquals(candidates[left].id, selection);
                            const bool rightSelected =
                                LaiueModAsciiEquals(candidates[right].id, selection);
                            loser = leftSelected ? &candidates[right]
                                                  : (rightSelected ? &candidates[left] : NULL);
                        }
                        else
                        {
                            ProfileSetFailure(&candidates[left], LAIUE_MODULE_DUPLICATE_SERVICE,
                                              "provider selection is required for this service");
                            ProfileSetFailure(&candidates[right], LAIUE_MODULE_DUPLICATE_SERVICE,
                                              "provider selection is required for this service");
                            continue;
                        }
                        if (loser != NULL)
                            ProfileSetFailure(loser, LAIUE_MODULE_DUPLICATE_SERVICE,
                                              "service provider disabled by profile selection");
                    }
                }

    bool progress = true;
    while (progress)
    {
        progress = false;
        for (uint32_t index = 0u; index < count; ++index)
        {
            ProfileCandidate *candidate = &candidates[index];
            if (!candidate->active || planned[index])
                continue;
            const LaiueModuleDescriptorV1 *descriptor = &candidate->api->descriptor;
            bool ready = true;
            for (uint32_t requirement = 0u; requirement < descriptor->requiresCount;
                 ++requirement)
            {
                const char *name = descriptor->requiresServices[requirement].name;
                if (!ProfileHostProvides(host, name) &&
                    !ProfilePlannedProvides(candidates, count, planned, name))
                {
                    ready = false;
                    break;
                }
            }
            if (ready)
            {
                planned[index] = true;
                candidate->planned = true;
                progress = true;
            }
        }
    }
    for (uint32_t index = 0u; index < count; ++index)
        if (candidates[index].active && !planned[index])
        {
            const LaiueModuleDescriptorV1 *descriptor = &candidates[index].api->descriptor;
            bool providerExists = true;
            for (uint32_t requirement = 0u; requirement < descriptor->requiresCount;
                 ++requirement)
            {
                const char *name = descriptor->requiresServices[requirement].name;
                bool found = ProfileHostProvides(host, name);
                for (uint32_t provider = 0u; provider < count && !found; ++provider)
                    if (candidates[provider].active &&
                        ProfileCandidateProvides(&candidates[provider], name))
                        found = true;
                if (!found)
                {
                    providerExists = false;
                    break;
                }
            }
            ProfileSetFailure(&candidates[index],
                              providerExists ? LAIUE_MODULE_DEPENDENCY_CYCLE
                                             : LAIUE_MODULE_DEPENDENCY_MISSING,
                              providerExists ? "module is in a dependency cycle"
                                             : "a module dependency is missing");
        }

    /* A strict profile fails before any module code runs when an entry it
     * needs cannot load. An absent optional artifact and a provider the
     * profile disabled on purpose are not such entries. */
    if (!allowPartial)
        for (uint32_t index = 0u; index < count; ++index)
            if ((candidates[index].flags & LAIUE_MODULE_PROFILE_ENTRY_SKIPPED) != 0u &&
                candidates[index].status != LAIUE_MODULE_OK &&
                candidates[index].status != LAIUE_MODULE_DUPLICATE_SERVICE)
            {
                ProfileFillReport(report, candidates, count, 0u);
                return Fail(diagnostic, candidates[index].status, candidates[index].message);
            }

    uint32_t selectedCount = 0u;
    uint32_t selectedCandidates[LAIUE_MODULE_HOST_MAX_MODULES];
    for (uint32_t index = 0u; index < count; ++index)
        if (planned[index])
        {
            selected[selectedCount] = binaries[index];
            selectedCandidates[selectedCount++] = index;
        }
    if (selectedCount == 0u)
    {
        ProfileFillReport(report, candidates, count, 0u);
        return Fail(diagnostic, LAIUE_MODULE_PARTIAL, "profile has no startable modules");
    }

    uint8_t failureKinds[LAIUE_MODULE_HOST_MAX_MODULES];
    uint64_t loaded = 0u;
    const LaiueModuleStatus status =
        LoadModules(host, selected, selectedCount, diagnostic, allowPartial, failureKinds, &loaded);
    if (status != LAIUE_MODULE_OK && status != LAIUE_MODULE_PARTIAL)
    {
        const char *message = diagnostic != NULL ? diagnostic->message
                                                  : LaiueModuleStatusString(status);
        for (uint32_t index = 0u; index < count; ++index)
            if (planned[index])
                ProfileSetFailure(&candidates[index], status, message);
        ProfileFillReport(report, candidates, count, 0u);
        return status;
    }
    if (status == LAIUE_MODULE_PARTIAL)
    {
        /* LoadModules removes only the optional module that failed and any
         * optional modules left without a runnable dependency. The successful
         * branch is already running; do not replay create/start for it. */
        for (uint32_t selectedIndex = 0u; selectedIndex < selectedCount; ++selectedIndex)
        {
            const uint32_t candidateIndex = selectedCandidates[selectedIndex];
            ProfileCandidate *candidate = &candidates[candidateIndex];
            if (FindModule(host, candidate->id) < LAIUE_MODULE_HOST_MAX_MODULES)
            {
                candidate->status = LAIUE_MODULE_OK;
                candidate->flags = LAIUE_MODULE_PROFILE_ENTRY_LOADED;
                candidate->message[0] = '\0';
            }
            else if ((failureKinds[selectedIndex] & 1u) != 0u)
                ProfileSetFailure(candidate, LAIUE_MODULE_PARTIAL,
                                  "optional module create/start callback failed");
            else if ((failureKinds[selectedIndex] & 2u) != 0u)
                ProfileSetFailure(candidate, LAIUE_MODULE_DEPENDENCY_MISSING,
                                  "optional module was disabled after a dependency failure");
            else if (candidate->active)
                ProfileSetFailure(candidate, LAIUE_MODULE_DEPENDENCY_MISSING,
                                  "optional module was not started");
        }
        ProfileFillReport(report, candidates, count, SetSize(loaded));
        return Fail(diagnostic, LAIUE_MODULE_PARTIAL, "profile disabled an optional module");
    }
    for (uint32_t index = 0u; index < count; ++index)
        if (planned[index])
        {
            candidates[index].status = LAIUE_MODULE_OK;
            candidates[index].flags = LAIUE_MODULE_PROFILE_ENTRY_LOADED;
            candidates[index].message[0] = '\0';
        }
    ProfileFillReport(report, candidates, count, SetSize(loaded));
    if (allowPartial)
        for (uint32_t index = 0u; index < count; ++index)
            if ((candidates[index].flags & LAIUE_MODULE_PROFILE_ENTRY_SKIPPED) != 0u)
                return Fail(diagnostic, LAIUE_MODULE_PARTIAL,
                            "profile loaded with disabled modules");
    return LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleHostLoadProfileV1(LaiueModuleHost *host,
                                               const LaiueModuleProfileV1 *profile,
                                               LaiueModuleLoadReportV1 *report,
                                               LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || profile == NULL ||
        profile->structSize < offsetof(LaiueModuleProfileV1, reserved) ||
        profile->binaries == NULL || profile->binaryCount == 0u ||
        profile->binaryCount > LAIUE_MODULE_HOST_MAX_MODULES)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "module paths and count are required");
    const uint32_t count = profile->binaryCount;
    if (profile->providerSelectionCount > LAIUE_MODULE_HOST_MAX_SERVICES ||
        (profile->providerSelectionCount != 0u && profile->providerSelections == NULL))
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "provider selection storage is invalid");
    for (uint32_t index = 0u; index < profile->providerSelectionCount; ++index)
    {
        const LaiueModuleProviderSelectionV1 *selection = &profile->providerSelections[index];
        if (selection->structSize < offsetof(LaiueModuleProviderSelectionV1, reserved) ||
            !SafeName(selection->serviceName) || !SafeName(selection->moduleId))
            return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "provider selection is invalid");
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (LaiueModAsciiEquals(profile->providerSelections[prior].serviceName,
                                    selection->serviceName))
                return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                            "provider selection is duplicated");
    }
    if (report != NULL && (report->structSize < sizeof(*report) || report->capacity < count ||
                           report->entries == NULL))
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "profile report storage is invalid");
    if (report != NULL)
        report->flags = profile->flags;
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    LaiueModuleStatus status = LAIUE_MODULE_OK;
    if ((profile->flags & LAIUE_MODULE_PROFILE_ALLOW_PARTIAL) == 0u &&
        profile->providerSelectionCount == 0u)
    {
        uint64_t loaded = 0u;
        status = LoadModules(host, profile->binaries, count, diagnostic, false, NULL, &loaded);
        if (report != NULL)
        {
            report->count = 0u;
            report->loadedCount = SetSize(loaded);
            report->skippedCount = 0u;
        }
    }
    else
    {
        ProfileCandidate *candidates = (ProfileCandidate *)PlatformAllocate(
            sizeof(*candidates) * LAIUE_MODULE_HOST_MAX_MODULES, true);
        LaiueModuleBinaryV1 *selected = (LaiueModuleBinaryV1 *)PlatformAllocate(
            sizeof(*selected) * LAIUE_MODULE_HOST_MAX_MODULES, true);
        bool *planned =
            (bool *)PlatformAllocate(sizeof(*planned) * LAIUE_MODULE_HOST_MAX_MODULES, true);
        if (candidates == NULL || selected == NULL || planned == NULL)
            status =
                Fail(diagnostic, LAIUE_MODULE_OUT_OF_MEMORY, "profile scratch allocation failed");
        else
        {
            memset(candidates, 0, sizeof(*candidates) * LAIUE_MODULE_HOST_MAX_MODULES);
            memset(planned, 0, sizeof(*planned) * LAIUE_MODULE_HOST_MAX_MODULES);
            status = ProfileLoad(host, profile, report, diagnostic, candidates, selected, planned);
            ProfileCloseProbes(candidates, count);
        }
        PlatformFree(candidates);
        PlatformFree(selected);
        PlatformFree(planned);
    }
    End(host);
    return status;
}

LaiueModuleStatus LaiueModuleHostLoadProfile(LaiueModuleHost *host,
                                              const LaiueModuleBinaryV1 *binaries,
                                              uint32_t count, uint32_t profileFlags,
                                              LaiueModuleLoadReportV1 *report,
                                              LaiueModuleDiagnostic *diagnostic)
{
    LaiueModuleProfileV1 profile;
    memset(&profile, 0, sizeof(profile));
    profile.structSize = sizeof(profile);
    profile.flags = profileFlags;
    profile.binaries = binaries;
    profile.binaryCount = count;
    return LaiueModuleHostLoadProfileV1(host, &profile, report, diagnostic);
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
    uint64_t members = 0u;
    for (uint32_t slot = 0u; slot < LAIUE_MODULE_HOST_MAX_MODULES; ++slot)
        if (host->modules[slot].used)
            members |= SlotBit(slot);
    UnwindModules(host, members);
    End(host);
}

LaiueModuleStatus LaiueModuleHostUnloadV1(LaiueModuleHost *host, const char *id, uint32_t flags,
                                          LaiueModuleDiagnostic *diagnostic)
{
    DiagnosticClear(diagnostic);
    if (host == NULL || !SafeName(id) || (flags & ~LAIUE_MODULE_UNLOAD_DEPENDENTS) != 0u)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "module id and known unload flags are required");
    if (!Begin(host))
        return Fail(diagnostic, LAIUE_MODULE_BUSY, "module lifecycle is active");
    const uint32_t slot = FindModule(host, id);
    if (slot == LAIUE_MODULE_HOST_MAX_MODULES)
    {
        End(host);
        return Fail(diagnostic, LAIUE_MODULE_NOT_LOADED, "module is not loaded");
    }
    /* The set is taken and marked under one lock, so no module outside it
     * can obtain a table of a member in between. */
    uint64_t users[LAIUE_MODULE_HOST_MAX_MODULES];
    PlatformRwLockAcquireExclusive(&host->lock);
    CollectUsers(host, users);
    const uint64_t members = WithUsers(users, SlotBit(slot));
    const bool refused = members != SlotBit(slot) && (flags & LAIUE_MODULE_UNLOAD_DEPENDENTS) == 0u;
    for (uint32_t member = 0u; !refused && member < LAIUE_MODULE_HOST_MAX_MODULES; ++member)
        if ((members & SlotBit(member)) != 0u)
            host->modules[member].unloading = true;
    PlatformRwLockReleaseExclusive(&host->lock);
    LaiueModuleStatus status = LAIUE_MODULE_OK;
    if (refused)
    {
        const uint32_t user = FirstSlot(users[slot]);
        status = FailNamed(diagnostic, LAIUE_MODULE_IN_USE, "module is in use by ",
                           user < LAIUE_MODULE_HOST_MAX_MODULES ? host->modules[user].id
                                                                : "another module");
    }
    else
        UnwindModules(host, members);
    End(host);
    return status;
}

LaiueModuleStatus LaiueModuleHostListDependentsV1(const LaiueModuleHost *host, const char *id,
                                                  LaiueModuleIdV1 *output, uint32_t capacity,
                                                  uint32_t *outCount)
{
    if (outCount != NULL)
        *outCount = 0u;
    if (host == NULL || !SafeName(id) || outCount == NULL || (output == NULL && capacity != 0u))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    LaiueModuleStatus status = LAIUE_MODULE_OK;
    uint32_t count = 0u;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    const uint32_t slot =
        host->lifecycleBusy ? LAIUE_MODULE_HOST_MAX_MODULES : FindModule(host, id);
    if (host->lifecycleBusy)
        status = LAIUE_MODULE_BUSY;
    else if (slot == LAIUE_MODULE_HOST_MAX_MODULES)
        status = LAIUE_MODULE_NOT_LOADED;
    else
    {
        uint64_t users[LAIUE_MODULE_HOST_MAX_MODULES];
        uint32_t order[LAIUE_MODULE_HOST_MAX_MODULES];
        CollectUsers(host, users);
        const uint32_t orderCount = UnwindOrder(host, WithUsers(users, SlotBit(slot)), order);
        for (uint32_t index = 0u; index < orderCount; ++index)
        {
            if (order[index] == slot)
                continue;
            if (count < capacity)
                memcpy(output[count].id, host->modules[order[index]].id, LAIUE_MODULE_MAX_NAME);
            ++count;
        }
        if (count > capacity)
            status = LAIUE_MODULE_CAPACITY;
    }
    PlatformRwLockReleaseShared(&mutableHost->lock);
    *outCount = count;
    return status;
}

uint32_t LaiueModuleHostLoadedCount(const LaiueModuleHost *host)
{
    if (host == NULL)
        return 0u;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    uint32_t count = host->lifecycleBusy ? 0u : host->loadedCount;
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return count;
}

uint32_t LaiueModuleHostIsLoaded(const LaiueModuleHost *host, const char *id)
{
    if (host == NULL || !SafeName(id))
        return 0u;
    LaiueModuleHost *mutableHost = (LaiueModuleHost *)host;
    PlatformRwLockAcquireShared(&mutableHost->lock);
    const bool loaded =
        !host->lifecycleBusy && FindModule(host, id) < LAIUE_MODULE_HOST_MAX_MODULES;
    PlatformRwLockReleaseShared(&mutableHost->lock);
    return loaded ? 1u : 0u;
}
