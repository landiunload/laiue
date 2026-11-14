#include "mod/module_host.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Modules come and go while others keep running: loads add to the running
 * set as one transaction, unloads take out one module or a module with its
 * users, and nothing outside a lifecycle operation sees a half-done state. */

#define EVENT_CREATE 0u
#define EVENT_START 1u
#define EVENT_STOP 2u
#define EVENT_DESTROY 3u
#define EVENT_CAPACITY 64u
#define FILLER_COUNT 62u

typedef struct RuntimeModule RuntimeModule;

typedef struct RuntimeTable
{
    RuntimeModule *owner;
    const void *(*probe)(const char *name);
} RuntimeTable;

struct RuntimeModule
{
    uint32_t tag;
    const char *publishes;
    const char *queries;
    bool failStart;
    void (*onCreate)(RuntimeModule *module);
    void (*onStop)(RuntimeModule *module);
    RuntimeTable table;
    const LaiueModuleHostV1 *host;
    const void *obtained;
    uint32_t creates;
    uint32_t starts;
    uint32_t stops;
    uint32_t destroys;
};

static LaiueModuleHost *host;
static uint32_t events[EVENT_CAPACITY];
static uint32_t eventCount;
static const RuntimeTable *observerTable;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool TextEquals(const char *first, const char *second)
{
    uint32_t index = 0u;
    while (first[index] != '\0' && first[index] == second[index])
        ++index;
    return first[index] == second[index];
}

static bool TextStartsWith(const char *text, const char *prefix)
{
    uint32_t index = 0u;
    while (prefix[index] != '\0' && text[index] == prefix[index])
        ++index;
    return prefix[index] == '\0';
}

static void Record(const RuntimeModule *module, uint32_t kind)
{
    if (eventCount < EVENT_CAPACITY)
        events[eventCount++] = module->tag * 4u + kind;
}

/* Position of the event in the log, or UINT32_MAX when it did not happen. */
static uint32_t EventAt(const RuntimeModule *module, uint32_t kind)
{
    for (uint32_t index = 0u; index < eventCount; ++index)
        if (events[index] == module->tag * 4u + kind)
            return index;
    return UINT32_MAX;
}

static uint32_t GenericCreate(RuntimeModule *module, const LaiueModuleHostV1 *moduleHost,
                              void **outContext)
{
    ++module->creates;
    Record(module, EVENT_CREATE);
    module->host = moduleHost;
    if (module->publishes != NULL)
    {
        module->table.owner = module;
        LaiueModuleServiceV1 service = {
            .name = module->publishes,
            .version = 1u,
            .table = &module->table,
            .tableSize = sizeof(module->table),
        };
        if (moduleHost->publishService(moduleHost->context, &service) != LAIUE_MODULE_OK)
            return 0u;
    }
    if (module->queries != NULL)
        module->obtained = moduleHost->queryService(moduleHost->context, module->queries, 1u,
                                                    sizeof(RuntimeTable), NULL, NULL);
    if (module->onCreate != NULL)
        module->onCreate(module);
    *outContext = module;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL GenericStart(void *context)
{
    RuntimeModule *module = context;
    ++module->starts;
    Record(module, EVENT_START);
    return module->failStart ? 0u : 1u;
}

static void LAIUE_MODULE_CALL GenericStop(void *context)
{
    RuntimeModule *module = context;
    ++module->stops;
    Record(module, EVENT_STOP);
    if (module->onStop != NULL)
        module->onStop(module);
}

static void LAIUE_MODULE_CALL GenericDestroy(void *context)
{
    RuntimeModule *module = context;
    ++module->destroys;
    Record(module, EVENT_DESTROY);
}

#define RUNTIME_API(name, moduleId, requirements, requirementCount, services, serviceCount)        \
    static uint32_t LAIUE_MODULE_CALL name##Create(const LaiueModuleHostV1 *moduleHost,            \
                                                   void **outContext)                              \
    {                                                                                              \
        return GenericCreate(&name, moduleHost, outContext);                                       \
    }                                                                                              \
    static const LaiueModuleApiV1 name##Api = {                                                    \
        .structSize = sizeof(LaiueModuleApiV1),                                                    \
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,                                                  \
        .descriptor =                                                                              \
            {                                                                                      \
                .structSize = sizeof(LaiueModuleDescriptorV1),                                     \
                .abiVersion = LAIUE_MODULE_ABI_VERSION_1,                                          \
                .id = moduleId,                                                                    \
                .version = "1.0.0",                                                                \
                .requiresServices = requirements,                                                  \
                .requiresCount = requirementCount,                                                 \
                .providesServices = services,                                                      \
                .providesCount = serviceCount,                                                     \
            },                                                                                     \
        .create = name##Create,                                                                    \
        .start = GenericStart,                                                                     \
        .stop = GenericStop,                                                                       \
        .destroy = GenericDestroy,                                                                 \
    }

static const char counterName[] = "example.runtime.counter";
static const char observerName[] = "example.runtime.observer";
static const char lateName[] = "example.runtime.late";
static const char hostServiceName[] = "example.runtime.host";

static const char *const counterServices[] = {counterName};
static const char *const observerServices[] = {observerName};
static const char *const lateServices[] = {lateName};
static const char *const solo2Services[] = {"example.runtime.solo2"};
static const LaiueModuleRequirementV1 counterRequirement[] = {{counterName, 1u}};
static const LaiueModuleRequirementV1 lateRequirement[] = {{lateName, 1u}};
static const LaiueModuleRequirementV1 absentRequirement[] = {{"example.runtime.absent", 1u}};

static const void *ObserverProbe(const char *name);
static void LateCreated(RuntimeModule *module);
static void BrokenCreated(RuntimeModule *module);
static void ConsumerStopping(RuntimeModule *module);
static void SoloCreated(RuntimeModule *module);
static void SoloStopping(RuntimeModule *module);

static RuntimeModule provider = {.tag = 1u, .publishes = counterName};
static RuntimeModule consumer = {.tag = 2u, .queries = counterName, .onStop = ConsumerStopping};
static RuntimeModule observer = {
    .tag = 3u, .publishes = observerName, .table = {.probe = ObserverProbe}};
static RuntimeModule rival = {.tag = 4u, .publishes = counterName};
static RuntimeModule late = {.tag = 5u, .publishes = lateName, .onCreate = LateCreated};
static RuntimeModule broken = {
    .tag = 6u, .failStart = true, .queries = lateName, .onCreate = BrokenCreated};
static RuntimeModule partner = {.tag = 7u, .queries = lateName};
static RuntimeModule hostUser = {.tag = 8u, .queries = hostServiceName};
static RuntimeModule solo = {.tag = 9u, .onCreate = SoloCreated, .onStop = SoloStopping};
static RuntimeModule needy = {.tag = 10u};
static RuntimeModule solo2 = {.tag = 11u, .publishes = "example.runtime.solo2"};

RUNTIME_API(provider, "example.runtime.provider", NULL, 0u, counterServices, 1u);
RUNTIME_API(consumer, "example.runtime.consumer", counterRequirement, 1u, NULL, 0u);
RUNTIME_API(observer, "example.runtime.observer", NULL, 0u, observerServices, 1u);
RUNTIME_API(rival, "example.runtime.rival", NULL, 0u, counterServices, 1u);
RUNTIME_API(late, "example.runtime.late", NULL, 0u, lateServices, 1u);
RUNTIME_API(broken, "example.runtime.broken", lateRequirement, 1u, NULL, 0u);
RUNTIME_API(partner, "example.runtime.partner", lateRequirement, 1u, NULL, 0u);
RUNTIME_API(hostUser, "example.runtime.host_user", NULL, 0u, NULL, 0u);
RUNTIME_API(solo, "example.runtime.solo", NULL, 0u, NULL, 0u);
RUNTIME_API(needy, "example.runtime.needy", absentRequirement, 1u, NULL, 0u);
RUNTIME_API(solo2, "example.runtime.solo2", NULL, 0u, solo2Services, 1u);

/* What a running module sees through its own host table. */
static const void *ObserverProbe(const char *name)
{
    return observer.host->queryService(observer.host->context, name, 1u, 1u, NULL, NULL);
}

static bool lateHiddenFromApplication;
static bool lateHiddenFromRunningModule;
static bool brokenSawLate;
static bool consumerSawCounterWhileLeaving;
static bool applicationSawCounterWhileLeaving;
static uint32_t soloNestedUnload;
static uint32_t soloNestedLoad;
static uint32_t soloNestedList;
static uint32_t fillerStarts;
static uint32_t fillerStops;
static uint32_t fillerStopsBeforeSolo;

/* A load in progress is invisible outside it, even to running modules. */
static void LateCreated(RuntimeModule *module)
{
    (void)module;
    lateHiddenFromApplication =
        LaiueModuleHostQueryService(host, lateName, 1u, 1u, NULL, NULL) == NULL;
    lateHiddenFromRunningModule = observerTable->probe(lateName) == NULL;
}

static void BrokenCreated(RuntimeModule *module)
{
    brokenSawLate = module->obtained == &late.table;
}

/* Modules leaving together still see each other; nobody else sees them. */
static void ConsumerStopping(RuntimeModule *module)
{
    consumerSawCounterWhileLeaving =
        module->host->queryService(module->host->context, counterName, 1u, 1u, NULL, NULL) ==
        &provider.table;
    applicationSawCounterWhileLeaving =
        LaiueModuleHostQueryService(host, counterName, 1u, 1u, NULL, NULL) != NULL;
}

/* Lifecycle calls from inside a callback are refused, not nested. */
static void SoloCreated(RuntimeModule *module)
{
    (void)module;
    LaiueModuleDiagnostic diagnostic;
    soloNestedUnload = LaiueModuleHostUnloadV1(host, "example.runtime.late", 0u, &diagnostic);
    const LaiueModuleApiV1 *apis[] = {&rivalApi};
    soloNestedLoad = LaiueModuleHostLoadStatic(host, apis, 1u, &diagnostic);
    uint32_t count = 0u;
    soloNestedList =
        LaiueModuleHostListDependentsV1(host, "example.runtime.late", NULL, 0u, &count);
}

static void SoloStopping(RuntimeModule *module)
{
    (void)module;
    fillerStopsBeforeSolo = fillerStops;
}

static char fillerIds[FILLER_COUNT][32];
static LaiueModuleApiV1 fillerApis[FILLER_COUNT];
static const LaiueModuleApiV1 *fillerApiPointers[FILLER_COUNT];

static uint32_t LAIUE_MODULE_CALL FillerCreate(const LaiueModuleHostV1 *moduleHost,
                                               void **outContext)
{
    (void)moduleHost;
    *outContext = &fillerStarts;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL FillerStart(void *context)
{
    (void)context;
    ++fillerStarts;
    return 1u;
}

static void LAIUE_MODULE_CALL FillerStop(void *context)
{
    (void)context;
    ++fillerStops;
}

static void LAIUE_MODULE_CALL FillerDestroy(void *context)
{
    (void)context;
}

static void BuildFillers(void)
{
    static const char prefix[] = "example.runtime.filler.";
    for (uint32_t index = 0u; index < FILLER_COUNT; ++index)
    {
        char *id = fillerIds[index];
        uint32_t length = 0u;
        while (prefix[length] != '\0')
        {
            id[length] = prefix[length];
            ++length;
        }
        id[length++] = (char)('0' + index / 10u);
        id[length++] = (char)('0' + index % 10u);
        id[length] = '\0';
        LaiueModuleApiV1 *api = &fillerApis[index];
        api->structSize = sizeof(*api);
        api->abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.structSize = sizeof(api->descriptor);
        api->descriptor.abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.id = id;
        api->descriptor.version = "1.0.0";
        api->create = FillerCreate;
        api->start = FillerStart;
        api->stop = FillerStop;
        api->destroy = FillerDestroy;
        fillerApiPointers[index] = api;
    }
}

static LaiueModuleStatus LoadOne(const LaiueModuleApiV1 *api, LaiueModuleDiagnostic *diagnostic)
{
    const LaiueModuleApiV1 *apis[] = {api};
    return LaiueModuleHostLoadStatic(host, apis, 1u, diagnostic);
}

static void ExpectAdditiveLoads(void)
{
    LaiueModuleDiagnostic diagnostic;
    Expect(LoadOne(&providerApi, &diagnostic) == LAIUE_MODULE_OK, diagnostic.message);
    Expect(LoadOne(&consumerApi, &diagnostic) == LAIUE_MODULE_OK, diagnostic.message);
    Expect(consumer.starts == 1u && consumer.obtained == &provider.table,
           "a later load uses the service of a running module");
    Expect(LoadOne(&observerApi, &diagnostic) == LAIUE_MODULE_OK, diagnostic.message);
    observerTable =
        LaiueModuleHostQueryService(host, observerName, 1u, sizeof(RuntimeTable), NULL, NULL);
    Expect(observerTable == &observer.table && LaiueModuleHostLoadedCount(host) == 3u,
           "three separate loads run side by side");

    Expect(LoadOne(&providerApi, &diagnostic) == LAIUE_MODULE_DUPLICATE_ID &&
               TextEquals(diagnostic.message, "module is already loaded") && provider.creates == 1u,
           "a running module id is refused before its code runs");
    Expect(LoadOne(&rivalApi, &diagnostic) == LAIUE_MODULE_DUPLICATE_SERVICE &&
               TextEquals(diagnostic.message, "service is already provided") && rival.creates == 0u,
           "a second provider of a running service is refused before its code runs");
    Expect(LaiueModuleHostLoadedCount(host) == 3u && provider.stops == 0u && consumer.stops == 0u &&
               observer.stops == 0u,
           "refused loads leave the running modules alone");
}

static void ExpectTransactionalLoads(void)
{
    LaiueModuleDiagnostic diagnostic;
    eventCount = 0u;
    const LaiueModuleApiV1 *failing[] = {&lateApi, &brokenApi};
    Expect(LaiueModuleHostLoadStatic(host, failing, 2u, &diagnostic) == LAIUE_MODULE_START_FAILED,
           "a failed start fails its load");
    Expect(lateHiddenFromApplication && lateHiddenFromRunningModule && brokenSawLate,
           "only the load in progress sees its own services");
    Expect(late.starts == 1u && late.stops == 1u && late.destroys == 1u && broken.creates == 1u &&
               broken.stops == 0u && broken.destroys == 1u,
           "the failed load unwinds every module it started");
    Expect(EventAt(&broken, EVENT_DESTROY) < EventAt(&late, EVENT_DESTROY),
           "the failed load unwinds users before their providers");
    Expect(LaiueModuleHostQueryService(host, lateName, 1u, 1u, NULL, NULL) == NULL &&
               LaiueModuleHostLoadedCount(host) == 3u &&
               !LaiueModuleHostIsLoaded(host, "example.runtime.late") && provider.stops == 0u &&
               consumer.stops == 0u && observer.stops == 0u,
           "a failed load leaves the running modules as they were");

    const LaiueModuleApiV1 *working[] = {&lateApi, &partnerApi};
    Expect(LaiueModuleHostLoadStatic(host, working, 2u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    Expect(partner.obtained == &late.table &&
               LaiueModuleHostQueryService(host, lateName, 1u, 1u, NULL, NULL) == &late.table &&
               observerTable->probe(lateName) == &late.table &&
               LaiueModuleHostLoadedCount(host) == 5u,
           "a finished load is visible to everyone");
}

static void ExpectUnloads(void)
{
    LaiueModuleDiagnostic diagnostic;
    Expect(observerTable->probe(counterName) == &provider.table,
           "a running module obtains a table after its start");
    Expect(LaiueModuleHostUnloadV1(host, "example.runtime.provider", 0u, &diagnostic) ==
                   LAIUE_MODULE_IN_USE &&
               TextStartsWith(diagnostic.message, "module is in use by example.runtime.") &&
               provider.stops == 0u && LaiueModuleHostLoadedCount(host) == 5u,
           "a module in use is not unloaded on its own");

    static LaiueModuleIdV1 dependents[4];
    uint32_t count = 0u;
    Expect(LaiueModuleHostListDependentsV1(host, "example.runtime.provider", NULL, 0u, &count) ==
                   LAIUE_MODULE_CAPACITY &&
               count == 2u,
           "dependents report their number without storage");
    Expect(LaiueModuleHostListDependentsV1(host, "example.runtime.provider", dependents, 4u,
                                           &count) == LAIUE_MODULE_OK &&
               count == 2u,
           "dependents fit the storage");
    const bool consumerListed = TextEquals(dependents[0].id, "example.runtime.consumer") ||
                                TextEquals(dependents[1].id, "example.runtime.consumer");
    const bool observerListed = TextEquals(dependents[0].id, "example.runtime.observer") ||
                                TextEquals(dependents[1].id, "example.runtime.observer");
    Expect(consumerListed && observerListed,
           "both the declared and the run-time user are dependents");
    Expect(LaiueModuleHostListDependentsV1(host, "example.runtime.missing", dependents, 4u,
                                           &count) == LAIUE_MODULE_NOT_LOADED &&
               LaiueModuleHostListDependentsV1(host, "example.runtime.provider", dependents, 4u,
                                               NULL) == LAIUE_MODULE_INVALID_ARGUMENT,
           "dependents of an unknown module or without a count are refused");

    Expect(LaiueModuleHostUnloadV1(host, "example.runtime.consumer", 0u, &diagnostic) ==
                   LAIUE_MODULE_OK &&
               consumer.stops == 1u && consumer.destroys == 1u && provider.stops == 0u &&
               !LaiueModuleHostIsLoaded(host, "example.runtime.consumer") &&
               LaiueModuleHostLoadedCount(host) == 4u,
           "an unused module unloads alone");
    Expect(LaiueModuleHostUnloadV1(host, "example.runtime.consumer", 0u, &diagnostic) ==
                   LAIUE_MODULE_NOT_LOADED &&
               LaiueModuleHostUnloadV1(host, "example.runtime.provider", UINT32_C(1) << 1,
                                       &diagnostic) == LAIUE_MODULE_INVALID_ARGUMENT &&
               LaiueModuleHostUnloadV1(host, "Not a module", 0u, &diagnostic) ==
                   LAIUE_MODULE_INVALID_ARGUMENT &&
               LaiueModuleHostUnloadV1(NULL, "example.runtime.provider", 0u, &diagnostic) ==
                   LAIUE_MODULE_INVALID_ARGUMENT,
           "unknown modules and invalid unload arguments are refused");
    Expect(LoadOne(&consumerApi, &diagnostic) == LAIUE_MODULE_OK && consumer.starts == 2u &&
               LaiueModuleHostLoadedCount(host) == 5u,
           "an unloaded module loads again into a free slot");

    eventCount = 0u;
    Expect(LaiueModuleHostUnloadV1(host, "example.runtime.provider", LAIUE_MODULE_UNLOAD_DEPENDENTS,
                                   &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    Expect(provider.stops == 1u && provider.destroys == 1u && consumer.stops == 2u &&
               observer.stops == 1u && LaiueModuleHostLoadedCount(host) == 2u &&
               LaiueModuleHostIsLoaded(host, "example.runtime.late") &&
               LaiueModuleHostIsLoaded(host, "example.runtime.partner"),
           "a module unloads together with its users and nothing else");
    Expect(EventAt(&consumer, EVENT_STOP) < EventAt(&provider, EVENT_STOP) &&
               EventAt(&observer, EVENT_STOP) < EventAt(&provider, EVENT_STOP) &&
               EventAt(&consumer, EVENT_DESTROY) < EventAt(&provider, EVENT_DESTROY),
           "users stop and are destroyed before their provider");
    Expect(consumerSawCounterWhileLeaving && !applicationSawCounterWhileLeaving,
           "a leaving service is visible only to the modules leaving with it");
    Expect(LaiueModuleHostQueryService(host, counterName, 1u, 1u, NULL, NULL) == NULL &&
               LaiueModuleHostQueryService(host, observerName, 1u, 1u, NULL, NULL) == NULL,
           "unloaded modules leave no services behind");
}

static void ExpectHostServices(void)
{
    LaiueModuleDiagnostic diagnostic;
    static RuntimeTable hostTable;
    static RuntimeTable spareTable;
    const LaiueModuleServiceV1 hostService = {
        .name = hostServiceName,
        .version = 1u,
        .table = &hostTable,
        .tableSize = sizeof(hostTable),
    };
    const LaiueModuleServiceV1 spareService = {
        .name = "example.runtime.spare",
        .version = 1u,
        .table = &spareTable,
        .tableSize = sizeof(spareTable),
    };
    Expect(LaiueModuleHostRegisterService(host, &hostService, &diagnostic) == LAIUE_MODULE_OK,
           "a host service registers while modules run");
    Expect(LoadOne(&hostUserApi, &diagnostic) == LAIUE_MODULE_OK && hostUser.obtained == &hostTable,
           "a module loaded later uses the new host service");
    Expect(LaiueModuleHostUnregisterService(host, hostServiceName, &diagnostic) ==
                   LAIUE_MODULE_IN_USE &&
               TextEquals(diagnostic.message, "service is in use by example.runtime.host_user") &&
               LaiueModuleHostQueryService(host, hostServiceName, 1u, 1u, NULL, NULL) == &hostTable,
           "a host service in use stays registered");
    Expect(LaiueModuleHostRegisterService(host, &spareService, &diagnostic) == LAIUE_MODULE_OK &&
               LaiueModuleHostUnregisterService(host, "example.runtime.spare", &diagnostic) ==
                   LAIUE_MODULE_OK,
           "an unused host service comes and goes while modules run");
    Expect(LaiueModuleHostUnloadV1(host, "example.runtime.host_user", 0u, &diagnostic) ==
                   LAIUE_MODULE_OK &&
               LaiueModuleHostUnregisterService(host, hostServiceName, &diagnostic) ==
                   LAIUE_MODULE_OK,
           "a host service unregisters once its user is gone");
}

static void ExpectProfiles(void)
{
    LaiueModuleDiagnostic diagnostic;
    static LaiueModuleLoadReportEntryV1 entries[2];
    LaiueModuleLoadReportV1 report;
    LaiueModuleLoadReportInitialize(&report, entries, 2u);
    static const LaiueModuleBinaryV1 partial[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &lateApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &soloApi},
    };
    Expect(LaiueModuleHostLoadProfile(host, partial, 2u, LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
                                      &report, &diagnostic) == LAIUE_MODULE_PARTIAL,
           "a partial profile loads next to running modules");
    Expect(entries[0].status == LAIUE_MODULE_DUPLICATE_ID &&
               TextEquals(entries[0].message, "module is already loaded") &&
               (entries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_SKIPPED) != 0u &&
               (entries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u &&
               report.loadedCount == 1u && late.creates == 2u && solo.starts == 1u &&
               LaiueModuleHostLoadedCount(host) == 3u,
           "the report counts only the modules this profile started");
    Expect(soloNestedUnload == LAIUE_MODULE_BUSY && soloNestedLoad == LAIUE_MODULE_BUSY &&
               soloNestedList == LAIUE_MODULE_BUSY && rival.creates == 0u,
           "lifecycle calls from a callback are refused");

    static const LaiueModuleBinaryV1 strict[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC, &needyApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &solo2Api},
    };
    static const LaiueModuleProviderSelectionV1 selection = {
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = "example.runtime.solo2",
        .moduleId = "example.runtime.solo2",
    };
    LaiueModuleProfileV1 profile = {
        .structSize = sizeof(profile),
        .binaries = strict,
        .binaryCount = 2u,
        .providerSelections = &selection,
        .providerSelectionCount = 1u,
    };
    Expect(LaiueModuleHostLoadProfileV1(host, &profile, NULL, &diagnostic) ==
                   LAIUE_MODULE_DEPENDENCY_MISSING &&
               needy.creates == 0u && solo2.creates == 0u && LaiueModuleHostLoadedCount(host) == 3u,
           "a strict profile fails before any of its code runs");
}

static void ExpectCapacityAndTeardown(void)
{
    LaiueModuleDiagnostic diagnostic;
    BuildFillers();
    const uint32_t freeSlots = LAIUE_MODULE_HOST_MAX_MODULES - LaiueModuleHostLoadedCount(host);
    Expect(freeSlots < FILLER_COUNT, "the fillers cover every free slot");
    Expect(LaiueModuleHostLoadStatic(host, fillerApiPointers, freeSlots, &diagnostic) ==
                   LAIUE_MODULE_OK &&
               LaiueModuleHostLoadedCount(host) == LAIUE_MODULE_HOST_MAX_MODULES,
           diagnostic.message);
    Expect(LaiueModuleHostLoadStatic(host, &fillerApiPointers[freeSlots], 1u, &diagnostic) ==
                   LAIUE_MODULE_CAPACITY &&
               LaiueModuleHostLoadedCount(host) == LAIUE_MODULE_HOST_MAX_MODULES &&
               fillerStarts == freeSlots,
           "a load past the last slot is refused and keeps the running set");

    eventCount = 0u;
    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostLoadedCount(host) == 0u && fillerStops == freeSlots &&
               fillerStopsBeforeSolo == freeSlots,
           "the newest load stops first");
    Expect(EventAt(&solo, EVENT_STOP) < EventAt(&partner, EVENT_STOP) &&
               EventAt(&partner, EVENT_STOP) < EventAt(&late, EVENT_STOP) && late.destroys == 2u &&
               partner.destroys == 1u && solo.destroys == 1u,
           "teardown follows dependencies and then load order");
}

LAIUE_TEST_ENTRY(ModuleHostRuntimeTestEntryPoint)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    ExpectAdditiveLoads();
    ExpectTransactionalLoads();
    ExpectUnloads();
    ExpectHostServices();
    ExpectProfiles();
    ExpectCapacityAndTeardown();

    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
