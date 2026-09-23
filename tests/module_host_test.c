#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stddef.h>
#include <stdbool.h>

#if defined(_WIN32)
#define PROVIDER_NAME L"laiue_module_provider.dll"
#define CONSUMER_NAME L"laiue_module_consumer.dll"
#elif defined(__APPLE__)
#define PROVIDER_NAME L"liblaiue_module_provider.dylib"
#define CONSUMER_NAME L"liblaiue_module_consumer.dylib"
#else
#define PROVIDER_NAME L"liblaiue_module_provider.so"
#define CONSUMER_NAME L"liblaiue_module_consumer.so"
#endif

typedef struct CounterState
{
    uint32_t starts;
    uint32_t stops;
} CounterState;

typedef struct StaticModuleState
{
    uint32_t starts;
    uint32_t stops;
} StaticModuleState;

static StaticModuleState staticState;

static uint32_t LAIUE_MODULE_CALL StaticCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL)
        return false;
    staticState.starts = 0u;
    staticState.stops = 0u;
    *outContext = &staticState;
    return true;
}

static uint32_t LAIUE_MODULE_CALL StaticStart(void *context)
{
    StaticModuleState *state = context;
    if (state == NULL)
        return false;
    ++state->starts;
    return true;
}

static void LAIUE_MODULE_CALL StaticStop(void *context)
{
    StaticModuleState *state = context;
    if (state != NULL)
        ++state->stops;
}

static void LAIUE_MODULE_CALL StaticDestroy(void *context)
{
    (void)context;
}

static const LaiueModuleApiV1 staticApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.static",
        .version = "1.0.0",
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};

static uint32_t LAIUE_MODULE_CALL FailingStart(void *context)
{
    (void)context;
    return false;
}

static const char *const failingProvides[] = {"example.profile.failing"};
static const LaiueModuleApiV1 failingStartApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.profile.failing",
        .version = "1.0.0",
        .providesServices = failingProvides,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = FailingStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};

static const char *const cycleAProvides[] = {"example.cycle.a"};
static const char *const cycleBProvides[] = {"example.cycle.b"};
static const LaiueModuleRequirementV1 cycleARequires[] = {{"example.cycle.b", 1u}};
static const LaiueModuleRequirementV1 cycleBRequires[] = {{"example.cycle.a", 1u}};
static const LaiueModuleApiV1 cycleAApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.cycle.a",
        .version = "1.0.0",
        .requiresServices = cycleARequires,
        .requiresCount = 1u,
        .providesServices = cycleAProvides,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};
static const LaiueModuleApiV1 cycleBApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.cycle.b",
        .version = "1.0.0",
        .requiresServices = cycleBRequires,
        .requiresCount = 1u,
        .providesServices = cycleBProvides,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};

static const LaiueModuleApiV1 duplicateProviderApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.cycle.duplicate",
        .version = "1.0.0",
        .providesServices = cycleAProvides,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};

static const char *const selectedProviderServices[] = {"example.selection"};
static const LaiueModuleApiV1 selectedProviderAlphaApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.provider.alpha",
        .version = "1.0.0",
        .providesServices = selectedProviderServices,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};
static const LaiueModuleApiV1 selectedProviderZetaApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.provider.zeta",
        .version = "1.0.0",
        .providesServices = selectedProviderServices,
        .providesCount = 1u,
    },
    .create = StaticCreate,
    .start = StaticStart,
    .stop = StaticStop,
    .destroy = StaticDestroy,
};

static const LaiueModuleApiV1 badAbiApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = 99u,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.bad_abi",
        .version = "1.0.0",
    },
    .create = StaticCreate,
    .destroy = StaticDestroy,
};

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        output[index] = root[index];
        ++index;
    }
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

LAIUE_TEST_ENTRY(ModuleHostTestEntryPoint)
{
    /* Keep the no-CRT test entry below the Windows stack-probe threshold. */
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t missingPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t consumerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(providerPath, directory, PROVIDER_NAME), "provider path fits");
    Expect(Join(consumerPath, directory, CONSUMER_NAME), "consumer path fits");
    Expect(Join(missingPath, directory, L"laiue_module_file_that_does_not_exist.dll"),
           "missing path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 missing = {missingPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &missing, 1u, &diagnostic) == LAIUE_MODULE_LOAD_FAILED,
           "missing module is reported without aborting the process");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed load leaves no partially loaded modules");

    LaiueModuleBinaryV1 optionalMissing = {
        missingPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    Expect(LaiueModuleHostLoad(host, &optionalMissing, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "missing optional module is skipped");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "skipping an optional module keeps the host usable");

    const LaiueModuleApiV1 *staticApis[] = {&staticApi};
    Expect(LaiueModuleHostLoadStatic(host, staticApis, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "static registry uses the same module ABI");
    Expect(staticState.starts == 1u && LaiueModuleHostLoadedCount(host) == 1u,
           "static module starts through bootstrap");
    LaiueModuleHostUnloadAll(host);
    Expect(staticState.stops == 1u && LaiueModuleHostLoadedCount(host) == 0u,
           "static module unloads through bootstrap");

    /* A best-effort profile keeps an independent provider alive while
     * disabling a cyclic component. The strict loader below remains a hard
     * transaction and still rejects the same cycle. */
    static LaiueModuleLoadReportEntryV1 profileEntries[3];
    LaiueModuleLoadReportV1 profileReport;
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 3u);
    LaiueModuleBinaryV1 profileBinaries[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &cycleAApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &cycleBApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, profileBinaries,
               (uint32_t)(sizeof(profileBinaries) / sizeof(profileBinaries[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "partial profile reports disabled cycle");
    Expect(LaiueModuleHostLoadedCount(host) == 1u && staticState.starts == 1u,
           "partial profile keeps independent provider running");
    Expect(profileReport.count == 3u && profileReport.loadedCount == 1u &&
               (profileEntries[2].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u &&
               (profileEntries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u,
           "partial profile report contains per-module state");
    LaiueModuleHostUnloadAll(host);

    /* A profile explicitly selects a provider instead of relying on the
     * order in which artifacts happen to be listed. */
    static LaiueModuleLoadReportEntryV1 selectionEntries[2];
    LaiueModuleLoadReportV1 selectionReport;
    LaiueModuleLoadReportInitialize(&selectionReport, selectionEntries, 2u);
    static const LaiueModuleProviderSelectionV1 providerSelection = {
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = "example.selection",
        .moduleId = "example.provider.zeta",
    };
    static const LaiueModuleBinaryV1 selectionBinaries[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC, &selectedProviderAlphaApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &selectedProviderZetaApi},
    };
    LaiueModuleProfileV1 selectionProfile = {
        .structSize = sizeof(selectionProfile),
        .flags = 0u,
        .binaries = selectionBinaries,
        .binaryCount = 2u,
        .providerSelections = &providerSelection,
        .providerSelectionCount = 1u,
    };
    Expect(LaiueModuleHostLoadProfileV1(host, &selectionProfile, &selectionReport,
                                        &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    Expect(LaiueModuleHostLoadedCount(host) == 1u &&
               LaiueModuleHostIsLoaded(host, "example.provider.zeta") &&
               !LaiueModuleHostIsLoaded(host, "example.provider.alpha") &&
               (selectionEntries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
               (selectionEntries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u,
           "profile provider selection is explicit and deterministic");
    LaiueModuleHostUnloadAll(host);

    static const LaiueModuleProviderSelectionV1 invalidSelection = {
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = "example.selection",
        .moduleId = "example.provider.missing",
    };
    selectionProfile.providerSelections = &invalidSelection;
    LaiueModuleLoadReportInitialize(&selectionReport, selectionEntries, 2u);
    Expect(LaiueModuleHostLoadProfileV1(host, &selectionProfile, &selectionReport,
                                        &diagnostic) == LAIUE_MODULE_INVALID_ARGUMENT &&
               LaiueModuleHostLoadedCount(host) == 0u,
           "invalid provider selection is rejected before callbacks");

    /* A present optional provider may fail in create/start. The profile
     * retries without that provider, while the independent static module
     * still reaches a clean running graph. */
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 2u);
    LaiueModuleBinaryV1 failingProfile[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &failingStartApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, failingProfile,
               (uint32_t)(sizeof(failingProfile) / sizeof(failingProfile[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "partial profile isolates optional start failure");
    Expect(LaiueModuleHostLoadedCount(host) == 1u &&
               (profileEntries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
               (profileEntries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u,
           "optional start failure leaves independent module running");
    LaiueModuleHostUnloadAll(host);

    const LaiueModuleApiV1 *badApis[] = {&badAbiApi};
    Expect(LaiueModuleHostLoadStatic(host, badApis, 1u, &diagnostic) ==
               LAIUE_MODULE_ABI_MISMATCH,
           "incompatible module ABI is rejected");
    const LaiueModuleApiV1 *duplicateApis[] = {&cycleAApi, &duplicateProviderApi};
    Expect(LaiueModuleHostLoadStatic(host, duplicateApis, 2u, &diagnostic) ==
               LAIUE_MODULE_DUPLICATE_SERVICE,
           "ambiguous service providers are rejected before callbacks");
    const LaiueModuleApiV1 *cycleApis[] = {&cycleAApi, &cycleBApi};
    Expect(LaiueModuleHostLoadStatic(host, cycleApis, 2u, &diagnostic) ==
               LAIUE_MODULE_DEPENDENCY_CYCLE,
           "dependency cycle is reported");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "cycle failure rolls back static modules");

    LaiueModuleBinaryV1 onlyConsumer = {consumerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &onlyConsumer, 1u, &diagnostic) ==
               LAIUE_MODULE_DEPENDENCY_MISSING,
           "missing service dependency is reported");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "dependency failure rolls back the module graph");

    LaiueModuleBinaryV1 binaries[2] = {{providerPath, 0u, NULL}, {consumerPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(host, binaries, 2u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    Expect(LaiueModuleHostLoadedCount(host) == 2u, "both optional modules started");
    Expect(LaiueModuleHostIsLoaded(host, "example.provider"), "provider is loaded");
    Expect(LaiueModuleHostIsLoaded(host, "example.consumer"), "consumer is loaded");
    uint32_t version = 0u;
    uint32_t size = 0u;
    CounterState *counter = (CounterState *)LaiueModuleHostQueryService(
        host, "example.counter", 1u, sizeof(*counter), &version, &size);
    Expect(counter != NULL && version == 1u && size >= sizeof(*counter),
           "provider service is visible");
    Expect(counter->starts == 2u, "dependency order starts provider before consumer");

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostLoadedCount(host) == 0u, "unload clears all modules");
    Expect(LaiueModuleHostQueryService(host, "example.counter", 1u, 1u, NULL, NULL) == NULL,
           "unload removes module services");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
