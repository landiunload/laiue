// ROUND3 53-module-host: versioned `laiue::bootstrap` module host
// (`src/modding/mod/module_host.c`). The older r3_52 harness measures the
// legacy `LaiueModHost` adapter; this one measures the V1 module registry that
// every current technology provider and `examples/walk` actually uses.
//
// The host is a static archive here, so no external DLL fixture is needed.
// Workloads:
//   lookup_few/many_hit   - LaiueModuleHostQueryService over registered services
//   lookup_few/many_miss  - the same lookup for an absent service name
//   register128           - register/unregister a full 128-entry registry
//   lifecycle_flat        - 32 modules requiring 32 host-owned services
//   lifecycle_chain       - 16 chained modules publishing their own services
//
// Rows: `r3modhost2,<workload>,<iterations>,<elapsed_ns>,<checksum>`.
// Checksums are exact and must be identical for baseline and candidate.

#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define R3V2_SERVICE_NAME_CAPACITY 24u
#define R3V2_FEW_SERVICES 4u
#define R3V2_MANY_SERVICES LAIUE_MODULE_HOST_MAX_SERVICES
#define R3V2_FLAT_MODS 32u
#define R3V2_CHAIN_MODS 16u
#define R3V2_LOOKUP_ITERATIONS 200000u
#define R3V2_REGISTER_ROUNDS 4000u
#define R3V2_LIFECYCLE_ROUNDS 2000u
#define R3V2_WARMUP_DIVISOR 20u

static volatile uint64_t g_sink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void Report(const char *name, uint32_t iterations, double elapsed, uint64_t checksum)
{
    uint64_t nanoseconds = elapsed > 0.0 ? (uint64_t)(elapsed * 1000000000.0 + 0.5) : 0u;
    WriteText("r3modhost2,");
    WriteText(name);
    WriteText(",");
    WriteUnsigned(iterations);
    WriteText(",");
    WriteUnsigned(nanoseconds);
    WriteText(",");
    WriteUnsigned(checksum);
    WriteText("\n");
}

static void Fail(const char *message)
{
    WriteText("r3 module-host benchmark failure: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

static void FormatName(char output[R3V2_SERVICE_NAME_CAPACITY], const char *prefix, uint32_t index)
{
    uint32_t length = 0u;
    for (uint32_t position = 0u; prefix[position] != '\0'; ++position)
    {
        output[length++] = prefix[position];
    }
    output[length++] = (char)('0' + (index / 1000u) % 10u);
    output[length++] = (char)('0' + (index / 100u) % 10u);
    output[length++] = (char)('0' + (index / 10u) % 10u);
    output[length++] = (char)('0' + index % 10u);
    output[length] = '\0';
}

/* ------------------------------------------------------------------ */
/* Host-owned services                                                 */
/* ------------------------------------------------------------------ */

typedef struct BenchHostServices
{
    char names[R3V2_MANY_SERVICES][R3V2_SERVICE_NAME_CAPACITY];
    LaiueModuleServiceV1 descriptors[R3V2_MANY_SERVICES];
    uint32_t tables[R3V2_MANY_SERVICES];
} BenchHostServices;

static BenchHostServices g_hostServices;

static void InitializeHostServices(void)
{
    for (uint32_t index = 0u; index < R3V2_MANY_SERVICES; ++index)
    {
        FormatName(g_hostServices.names[index], "bench.svc.", index);
        g_hostServices.tables[index] = index;
        g_hostServices.descriptors[index].name = g_hostServices.names[index];
        g_hostServices.descriptors[index].version = 1u;
        g_hostServices.descriptors[index].table = &g_hostServices.tables[index];
        g_hostServices.descriptors[index].tableSize = (uint32_t)sizeof(uint32_t);
    }
}

static LaiueModuleHost *CreateHostWithServices(uint32_t serviceCount)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    config.log = NULL;
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        Fail("could not create the module host");
    }
    for (uint32_t index = 0u; index < serviceCount; ++index)
    {
        if (LaiueModuleHostRegisterService(host, &g_hostServices.descriptors[index],
                                          &diagnostic) != LAIUE_MODULE_OK)
        {
            Fail("could not register a host service");
        }
    }
    return host;
}

static double MeasureLookup(uint32_t serviceCount, bool miss, uint32_t rounds,
                            uint64_t *checksum)
{
    LaiueModuleHost *host = CreateHostWithServices(serviceCount);
    const char *missingName = "bench.svc.absent";
    uint64_t sum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        const char *name = miss ? missingName : g_hostServices.names[round % serviceCount];
        uint32_t version = 0u;
        uint32_t size = 0u;
        const void *found =
            LaiueModuleHostQueryService(host, name, 1u, 1u, &version, &size);
        sum += found != NULL ? 1u : 0u;
        sum += version;
        sum += size >= 1u ? 1u : 0u;
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModuleHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

static double MeasureRegister(uint32_t rounds, uint64_t *checksum)
{
    LaiueModuleHost *host = CreateHostWithServices(0u);
    LaiueModuleDiagnostic diagnostic;
    uint64_t sum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        for (uint32_t index = 0u; index < R3V2_MANY_SERVICES; ++index)
        {
            sum += LaiueModuleHostRegisterService(host, &g_hostServices.descriptors[index],
                                                  &diagnostic) == LAIUE_MODULE_OK
                       ? 1u
                       : 0u;
        }
        for (uint32_t index = 0u; index < R3V2_MANY_SERVICES; ++index)
        {
            sum += LaiueModuleHostUnregisterService(host, g_hostServices.names[index],
                                                    &diagnostic) == LAIUE_MODULE_OK
                       ? 1u
                       : 0u;
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModuleHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

/* ------------------------------------------------------------------ */
/* Flat graph: modules requiring host-owned services, no publishing    */
/* ------------------------------------------------------------------ */

typedef struct FlatFixture
{
    LaiueModuleApiV1 apis[R3V2_FLAT_MODS];
    const LaiueModuleApiV1 *apiPointers[R3V2_FLAT_MODS];
    LaiueModuleRequirementV1 requirements[R3V2_FLAT_MODS];
    uint32_t states[R3V2_FLAT_MODS];
} FlatFixture;

static FlatFixture g_flat;
static char g_flatIds[R3V2_FLAT_MODS][R3V2_SERVICE_NAME_CAPACITY];
static char g_flatRequirementNames[R3V2_FLAT_MODS][R3V2_SERVICE_NAME_CAPACITY];
static uint32_t g_flatStarts;

static uint32_t LAIUE_MODULE_CALL FlatCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL)
    {
        return 0u;
    }
    *outContext = &g_flatStarts;
    return 1u;
}

static uint32_t LAIUE_MODULE_CALL FlatStart(void *context)
{
    if (context == NULL)
    {
        return 0u;
    }
    ++g_flatStarts;
    return 1u;
}

static void LAIUE_MODULE_CALL SharedStop(void *context)
{
    (void)context;
}

static void LAIUE_MODULE_CALL SharedDestroy(void *context)
{
    uint32_t *state = (uint32_t *)context;
    if (state != NULL)
    {
        *state = 0u;
    }
}

static void InitializeFlat(void)
{
    static const char *const flatVersion = "1.0.0";
    for (uint32_t index = 0u; index < R3V2_FLAT_MODS; ++index)
    {
        FormatName(g_flatIds[index], "bench.flat.", index);
        FormatName(g_flatRequirementNames[index], "bench.svc.", index);
        g_flat.requirements[index].name = g_flatRequirementNames[index];
        g_flat.requirements[index].minimumVersion = 1u;
        LaiueModuleApiV1 *api = &g_flat.apis[index];
        memset(api, 0, sizeof(*api));
        api->structSize = (uint32_t)sizeof(*api);
        api->abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.structSize = (uint32_t)sizeof(LaiueModuleDescriptorV1);
        api->descriptor.abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.id = g_flatIds[index];
        api->descriptor.version = flatVersion;
        api->descriptor.requiresServices = &g_flat.requirements[index];
        api->descriptor.requiresCount = 1u;
        api->create = FlatCreate;
        api->start = FlatStart;
        api->stop = SharedStop;
        api->destroy = SharedDestroy;
        g_flat.apiPointers[index] = api;
    }
}

static double MeasureLifecycleFlat(uint32_t rounds, uint64_t *checksum)
{
    LaiueModuleHost *host = CreateHostWithServices(R3V2_FLAT_MODS);
    LaiueModuleDiagnostic diagnostic;
    uint64_t sum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        if (LaiueModuleHostLoadStatic(host, g_flat.apiPointers, R3V2_FLAT_MODS, &diagnostic) !=
                LAIUE_MODULE_OK ||
            LaiueModuleHostLoadedCount(host) != R3V2_FLAT_MODS)
        {
            Fail("flat module load failed");
        }
        sum += LaiueModuleHostLoadedCount(host);
        LaiueModuleHostUnloadAll(host);
        if (LaiueModuleHostLoadedCount(host) != 0u)
        {
            Fail("flat module unload failed");
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModuleHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

/* ------------------------------------------------------------------ */
/* Chain graph: modules publish a service and require the prior one    */
/* ------------------------------------------------------------------ */

typedef struct ChainFixture
{
    LaiueModuleApiV1 apis[R3V2_CHAIN_MODS];
    const LaiueModuleApiV1 *apiPointers[R3V2_CHAIN_MODS];
    char serviceNames[R3V2_CHAIN_MODS][R3V2_SERVICE_NAME_CAPACITY];
    char idNames[R3V2_CHAIN_MODS][R3V2_SERVICE_NAME_CAPACITY];
    const char *providePointers[R3V2_CHAIN_MODS];
    LaiueModuleRequirementV1 requirements[R3V2_CHAIN_MODS];
    uint32_t serviceTables[R3V2_CHAIN_MODS];
    uint32_t states[R3V2_CHAIN_MODS];
} ChainFixture;

static ChainFixture g_chain;

#define R3V2_CHAIN_INDICES                                                                        \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

#define X(index)                                                                                  \
    static uint32_t LAIUE_MODULE_CALL ChainCreate##index(const LaiueModuleHostV1 *host,           \
                                                        void **outContext);
R3V2_CHAIN_INDICES
#undef X

static uint32_t LAIUE_MODULE_CALL ChainCreateCommon(uint32_t index, const LaiueModuleHostV1 *host,
                                                    void **outContext)
{
    if (host == NULL || host->publishService == NULL || host->queryService == NULL ||
        outContext == NULL)
    {
        return 0u;
    }
    LaiueModuleServiceV1 service;
    service.name = g_chain.serviceNames[index];
    service.version = 1u;
    service.table = &g_chain.serviceTables[index];
    service.tableSize = (uint32_t)sizeof(g_chain.serviceTables[index]);
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
    {
        return 0u;
    }
    if (index > 0u)
    {
        uint32_t version = 0u;
        uint32_t size = 0u;
        const void *found = host->queryService(host->context, g_chain.serviceNames[index - 1u], 1u,
                                               1u, &version, &size);
        if (found == NULL || version != 1u || size < 1u)
        {
            return 0u;
        }
    }
    *outContext = &g_chain.states[index];
    return 1u;
}

#define X(index)                                                                                  \
    static uint32_t LAIUE_MODULE_CALL ChainCreate##index(const LaiueModuleHostV1 *host,           \
                                                        void **outContext)                        \
    {                                                                                             \
        return ChainCreateCommon(index, host, outContext);                                        \
    }
R3V2_CHAIN_INDICES
#undef X

static LaiueModuleCreateFn const g_chainCreates[R3V2_CHAIN_MODS] = {
#define X(index) ChainCreate##index,
    R3V2_CHAIN_INDICES
#undef X
};

static uint32_t LAIUE_MODULE_CALL ChainStart(void *context)
{
    return context != NULL ? 1u : 0u;
}

static void InitializeChain(void)
{
    static const char *const chainVersion = "1.0.0";
    for (uint32_t index = 0u; index < R3V2_CHAIN_MODS; ++index)
    {
        FormatName(g_chain.serviceNames[index], "bench.svc.", index);
        /* Reverse the ID order so the dependency chain resolves one module per
         * scan pass; this is the documented worst case for the resolver. */
        FormatName(g_chain.idNames[index], "bench.chain.", R3V2_CHAIN_MODS - 1u - index);
        g_chain.providePointers[index] = g_chain.serviceNames[index];
        g_chain.serviceTables[index] = index;
        g_chain.states[index] = index + 1u;
        LaiueModuleApiV1 *api = &g_chain.apis[index];
        memset(api, 0, sizeof(*api));
        api->structSize = (uint32_t)sizeof(*api);
        api->abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.structSize = (uint32_t)sizeof(LaiueModuleDescriptorV1);
        api->descriptor.abiVersion = LAIUE_MODULE_ABI_VERSION_1;
        api->descriptor.id = g_chain.idNames[index];
        api->descriptor.version = chainVersion;
        api->descriptor.providesServices = &g_chain.providePointers[index];
        api->descriptor.providesCount = 1u;
        if (index > 0u)
        {
            g_chain.requirements[index].name = g_chain.serviceNames[index - 1u];
            g_chain.requirements[index].minimumVersion = 1u;
            api->descriptor.requiresServices = &g_chain.requirements[index];
            api->descriptor.requiresCount = 1u;
        }
        api->create = g_chainCreates[index];
        api->start = ChainStart;
        api->stop = SharedStop;
        api->destroy = SharedDestroy;
        g_chain.apiPointers[index] = api;
    }
}

static double MeasureLifecycleChain(uint32_t rounds, uint64_t *checksum)
{
    LaiueModuleHost *host = CreateHostWithServices(0u);
    LaiueModuleDiagnostic diagnostic;
    uint64_t sum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        if (LaiueModuleHostLoadStatic(host, g_chain.apiPointers, R3V2_CHAIN_MODS, &diagnostic) !=
                LAIUE_MODULE_OK ||
            LaiueModuleHostLoadedCount(host) != R3V2_CHAIN_MODS)
        {
            Fail("chain module load failed");
        }
        sum += LaiueModuleHostLoadedCount(host);
        LaiueModuleHostUnloadAll(host);
        if (LaiueModuleHostLoadedCount(host) != 0u)
        {
            Fail("chain module unload failed");
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModuleHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

LAIUE_TEST_ENTRY(R3ModuleHostBenchmarkEntry)
{
    InitializeHostServices();
    InitializeFlat();
    InitializeChain();

    uint64_t checksum = 0u;
    (void)MeasureLookup(R3V2_FEW_SERVICES, false, R3V2_LOOKUP_ITERATIONS / R3V2_WARMUP_DIVISOR,
                        &checksum);
    (void)MeasureLookup(R3V2_MANY_SERVICES, true, R3V2_LOOKUP_ITERATIONS / R3V2_WARMUP_DIVISOR,
                        &checksum);
    (void)MeasureRegister(R3V2_REGISTER_ROUNDS / R3V2_WARMUP_DIVISOR, &checksum);
    (void)MeasureLifecycleFlat(R3V2_LIFECYCLE_ROUNDS / R3V2_WARMUP_DIVISOR, &checksum);
    (void)MeasureLifecycleChain(R3V2_LIFECYCLE_ROUNDS / R3V2_WARMUP_DIVISOR, &checksum);

    double elapsed = MeasureLookup(R3V2_FEW_SERVICES, false,
                                   R3V2_LOOKUP_ITERATIONS, &checksum);
    Report("lookup_few_hit", R3V2_LOOKUP_ITERATIONS, elapsed, checksum);
    elapsed = MeasureLookup(R3V2_MANY_SERVICES, false,
                            R3V2_LOOKUP_ITERATIONS, &checksum);
    Report("lookup_many_hit", R3V2_LOOKUP_ITERATIONS, elapsed, checksum);
    elapsed = MeasureLookup(R3V2_FEW_SERVICES, true,
                            R3V2_LOOKUP_ITERATIONS, &checksum);
    Report("lookup_few_miss", R3V2_LOOKUP_ITERATIONS, elapsed, checksum);
    elapsed = MeasureLookup(R3V2_MANY_SERVICES, true,
                            R3V2_LOOKUP_ITERATIONS, &checksum);
    Report("lookup_many_miss", R3V2_LOOKUP_ITERATIONS, elapsed, checksum);
    elapsed = MeasureRegister(R3V2_REGISTER_ROUNDS, &checksum);
    Report("register128", R3V2_MANY_SERVICES * R3V2_REGISTER_ROUNDS * 2u,
           elapsed, checksum);
    elapsed = MeasureLifecycleFlat(R3V2_LIFECYCLE_ROUNDS, &checksum);
    Report("lifecycle_flat", R3V2_FLAT_MODS * R3V2_LIFECYCLE_ROUNDS,
           elapsed, checksum);
    elapsed = MeasureLifecycleChain(R3V2_LIFECYCLE_ROUNDS, &checksum);
    Report("lifecycle_chain", R3V2_CHAIN_MODS * R3V2_LIFECYCLE_ROUNDS,
           elapsed, checksum);

    g_sink += checksum;
    WriteText("r3modhost2 done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
