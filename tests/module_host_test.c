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
static uint32_t failingStartCalls;
static uint32_t publishingFailureCreateCalls;
static uint32_t createFailureDestroyCalls;
static uint32_t startFailureStopCalls;
static uint32_t startFailureDestroyCalls;
static CounterState publishedFailureState;

static void LAIUE_MODULE_CALL StaticDestroy(void *context);
static void LAIUE_MODULE_CALL CountCreateFailureDestroy(void *context);
static void LAIUE_MODULE_CALL CountStartFailureStop(void *context);
static void LAIUE_MODULE_CALL CountStartFailureDestroy(void *context);

static const char *const publishedFailureServices[] = {"example.profile.failing.published"};

static uint32_t LAIUE_MODULE_CALL PublishFailureService(const LaiueModuleHostV1 *host)
{
    if (host == NULL || host->publishService == NULL)
        return false;
    LaiueModuleServiceV1 service = {
        .name = publishedFailureServices[0],
        .version = 1u,
        .table = &publishedFailureState,
        .tableSize = sizeof(publishedFailureState),
    };
    return host->publishService(host->context, &service) == LAIUE_MODULE_OK;
}

static uint32_t LAIUE_MODULE_CALL PublishingFailCreate(const LaiueModuleHostV1 *host,
                                                        void **outContext)
{
    if (host == NULL || outContext == NULL)
        return false;
    ++publishingFailureCreateCalls;
    *outContext = NULL;
    (void)PublishFailureService(host);
    return false;
}

static const LaiueModuleApiV1 publishingFailCreateApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.profile.create_published_failure",
        .version = "1.0.0",
        .providesServices = publishedFailureServices,
        .providesCount = 1u,
    },
    .create = PublishingFailCreate,
    .destroy = CountCreateFailureDestroy,
};

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

static void LAIUE_MODULE_CALL CountCreateFailureDestroy(void *context)
{
    (void)context;
    ++createFailureDestroyCalls;
}

static void LAIUE_MODULE_CALL CountStartFailureStop(void *context)
{
    (void)context;
    ++startFailureStopCalls;
}

static void LAIUE_MODULE_CALL CountStartFailureDestroy(void *context)
{
    (void)context;
    ++startFailureDestroyCalls;
}

/* A consumer that is not ready in the first start pass forces the host to
 * walk the module table again after an optional module failed and was
 * removed. The provider sorts after the consumer by ID. */
static const char regressionServiceName[] = "example.regression.provided";
static const char *const regressionProvidedServices[] = {regressionServiceName};
static const LaiueModuleRequirementV1 regressionRequirement[] = {{regressionServiceName, 1u}};
static uint32_t regressionServiceTable;
static uint32_t regressionConsumerStarts;

static uint32_t LAIUE_MODULE_CALL RegressionConsumerCreate(const LaiueModuleHostV1 *host,
                                                           void **outContext)
{
    if (host == NULL || outContext == NULL)
        return false;
    *outContext = &regressionConsumerStarts;
    return true;
}

static uint32_t LAIUE_MODULE_CALL RegressionConsumerStart(void *context)
{
    ++*(uint32_t *)context;
    return true;
}

static uint32_t LAIUE_MODULE_CALL RegressionProviderCreate(const LaiueModuleHostV1 *host,
                                                           void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    LaiueModuleServiceV1 service = {
        .name = regressionServiceName,
        .version = 1u,
        .table = &regressionServiceTable,
        .tableSize = sizeof(regressionServiceTable),
    };
    *outContext = &regressionServiceTable;
    return host->publishService(host->context, &service) == LAIUE_MODULE_OK;
}

static void LAIUE_MODULE_CALL RegressionDestroy(void *context)
{
    (void)context;
}

static const LaiueModuleApiV1 regressionConsumerApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "example.regression.a_consumer",
            .version = "1.0.0",
            .requiresServices = regressionRequirement,
            .requiresCount = 1u,
        },
    .create = RegressionConsumerCreate,
    .start = RegressionConsumerStart,
    .destroy = RegressionDestroy,
};

static const LaiueModuleApiV1 regressionProviderApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "example.regression.z_provider",
            .version = "1.0.0",
            .providesServices = regressionProvidedServices,
            .providesCount = 1u,
        },
    .create = RegressionProviderCreate,
    .destroy = RegressionDestroy,
};

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

typedef struct OptionalFallbackState
{
    const LaiueModuleHostV1 *host;
    uint32_t starts;
    bool observedMissingService;
} OptionalFallbackState;

static OptionalFallbackState optionalFallbackState;
static uint32_t optionalProviderServiceTable;
static const char optionalVersionedServiceName[] = "example.optional.versioned";
static const char optionalConsumerServiceName[] = "example.optional.consumer.ready";
static const char optionalThirdServiceName[] = "example.optional.third";
static const char *const optionalConsumerServices[] = {optionalConsumerServiceName};
static const char *const optionalProviderServices[] = {optionalVersionedServiceName};
static const char *const optionalThirdServices[] = {optionalThirdServiceName};
static const LaiueModuleRequirementV1 optionalConsumerRequirement[] = {
    {optionalVersionedServiceName, 2u},
};
static const LaiueModuleRequirementV1 optionalProviderRequirement[] = {
    {optionalConsumerServiceName, 1u},
};
static const LaiueModuleRequirementV1 optionalMiddleRequirement[] = {
    {optionalThirdServiceName, 1u},
};
static const LaiueModuleRequirementV1 optionalThirdRequirement[] = {
    {optionalConsumerServiceName, 1u},
};

static uint32_t LAIUE_MODULE_CALL OptionalFallbackCreate(const LaiueModuleHostV1 *host,
                                                         void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    LaiueModuleServiceV1 service = {
        .name = optionalConsumerServiceName,
        .version = 1u,
        .table = &optionalFallbackState,
        .tableSize = sizeof(optionalFallbackState),
    };
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
        return false;
    optionalFallbackState.host = host;
    optionalFallbackState.starts = 0u;
    optionalFallbackState.observedMissingService = false;
    *outContext = &optionalFallbackState;
    return true;
}

static uint32_t LAIUE_MODULE_CALL OptionalFallbackStart(void *context)
{
    OptionalFallbackState *state = context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return false;
    ++state->starts;
    state->observedMissingService =
        state->host->queryService(state->host->context, optionalVersionedServiceName, 2u,
                                  1u, NULL, NULL) == NULL;
    return state->observedMissingService;
}

static uint32_t LAIUE_MODULE_CALL OptionalProviderCreate(const LaiueModuleHostV1 *host,
                                                         void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    LaiueModuleServiceV1 service = {
        .name = optionalVersionedServiceName,
        .version = 1u,
        .table = &optionalProviderServiceTable,
        .tableSize = sizeof(optionalProviderServiceTable),
    };
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
        return false;
    *outContext = &optionalProviderServiceTable;
    return true;
}

static uint32_t LAIUE_MODULE_CALL OptionalProviderStart(void *context)
{
    return context != NULL;
}

typedef struct OptionalCycleNodeState
{
    const LaiueModuleHostV1 *host;
    uint32_t serviceValue;
    bool observedOptionalService;
} OptionalCycleNodeState;

static OptionalCycleNodeState optionalMiddleState;
static OptionalCycleNodeState optionalThirdState;

static uint32_t LAIUE_MODULE_CALL OptionalMiddleCreate(const LaiueModuleHostV1 *host,
                                                       void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    optionalMiddleState.host = host;
    optionalMiddleState.serviceValue = 1u;
    optionalMiddleState.observedOptionalService = false;
    LaiueModuleServiceV1 service = {
        .name = optionalVersionedServiceName,
        .version = 1u,
        .table = &optionalMiddleState,
        .tableSize = sizeof(optionalMiddleState),
    };
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
        return false;
    *outContext = &optionalMiddleState;
    return true;
}

static uint32_t LAIUE_MODULE_CALL OptionalMiddleStart(void *context)
{
    OptionalCycleNodeState *state = context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return false;
    state->observedOptionalService =
        state->host->queryService(state->host->context, optionalThirdServiceName, 1u,
                                  1u, NULL, NULL) != NULL;
    return state->observedOptionalService;
}

static uint32_t LAIUE_MODULE_CALL OptionalThirdCreate(const LaiueModuleHostV1 *host,
                                                      void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    optionalThirdState.host = host;
    optionalThirdState.serviceValue = 1u;
    optionalThirdState.observedOptionalService = false;
    LaiueModuleServiceV1 service = {
        .name = optionalThirdServiceName,
        .version = 1u,
        .table = &optionalThirdState,
        .tableSize = sizeof(optionalThirdState),
    };
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
        return false;
    *outContext = &optionalThirdState;
    return true;
}

static uint32_t LAIUE_MODULE_CALL OptionalThirdStart(void *context)
{
    OptionalCycleNodeState *state = context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return false;
    state->observedOptionalService =
        state->host->queryService(state->host->context, optionalConsumerServiceName, 1u,
                                  1u, NULL, NULL) != NULL;
    return state->observedOptionalService;
}

static const LaiueModuleApiV1 optionalConsumerApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.optional.a",
        .version = "1.0.0",
        .providesServices = optionalConsumerServices,
        .providesCount = 1u,
        .optionalServices = optionalConsumerRequirement,
        .optionalCount = 1u,
        .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
    },
    .create = OptionalFallbackCreate,
    .start = OptionalFallbackStart,
    .destroy = StaticDestroy,
};

static const LaiueModuleApiV1 optionalProviderApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.optional.provider",
        .version = "1.0.0",
        .providesServices = optionalProviderServices,
        .providesCount = 1u,
    },
    .create = OptionalProviderCreate,
    .start = OptionalProviderStart,
    .destroy = StaticDestroy,
};

static const LaiueModuleApiV1 optionalCycleProviderApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.optional.b",
        .version = "1.0.0",
        .requiresServices = optionalProviderRequirement,
        .requiresCount = 1u,
        .providesServices = optionalProviderServices,
        .providesCount = 1u,
        .optionalServices = optionalMiddleRequirement,
        .optionalCount = 1u,
        .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
    },
    .create = OptionalMiddleCreate,
    .start = OptionalMiddleStart,
    .destroy = StaticDestroy,
};

static const LaiueModuleApiV1 optionalThirdCycleApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.optional.c",
        .version = "1.0.0",
        .providesServices = optionalThirdServices,
        .providesCount = 1u,
        .optionalServices = optionalThirdRequirement,
        .optionalCount = 1u,
        .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
    },
    .create = OptionalThirdCreate,
    .start = OptionalThirdStart,
    .destroy = StaticDestroy,
};

typedef struct ServiceLifetimeState
{
    const LaiueModuleHostV1 *host;
    uint32_t value;
} ServiceLifetimeState;

static ServiceLifetimeState serviceLifetimeState;
static uint32_t hostLifetimeServiceTable;
static uint32_t serviceLifetimeStopUnpublishStatus = UINT32_MAX;
static const char moduleLifetimeServiceName[] = "example.module.lifetime";
static const char *const moduleLifetimeServices[] = {moduleLifetimeServiceName};

static uint32_t LAIUE_MODULE_CALL ServiceLifetimeCreate(const LaiueModuleHostV1 *host,
                                                        void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL)
        return false;
    serviceLifetimeState.host = host;
    serviceLifetimeState.value = 1u;
    LaiueModuleServiceV1 service = {
        .name = moduleLifetimeServiceName,
        .version = 1u,
        .table = &serviceLifetimeState,
        .tableSize = sizeof(serviceLifetimeState),
    };
    if (host->publishService(host->context, &service) != LAIUE_MODULE_OK)
        return false;
    *outContext = &serviceLifetimeState;
    return true;
}

static uint32_t LAIUE_MODULE_CALL ServiceLifetimeStart(void *context)
{
    return context == &serviceLifetimeState;
}

static void LAIUE_MODULE_CALL ServiceLifetimeStop(void *context)
{
    ServiceLifetimeState *state = context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        serviceLifetimeStopUnpublishStatus = state->host->unpublishService(
            state->host->context, moduleLifetimeServiceName);
}

static const LaiueModuleApiV1 serviceLifetimeApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.service_lifetime",
        .version = "1.0.0",
        .providesServices = moduleLifetimeServices,
        .providesCount = 1u,
    },
    .create = ServiceLifetimeCreate,
    .start = ServiceLifetimeStart,
    .stop = ServiceLifetimeStop,
    .destroy = StaticDestroy,
};

static uint32_t LAIUE_MODULE_CALL FailingStart(void *context)
{
    (void)context;
    ++failingStartCalls;
    return false;
}

static uint32_t LAIUE_MODULE_CALL PublishingFailStartCreate(const LaiueModuleHostV1 *host,
                                                            void **outContext)
{
    if (host == NULL || outContext == NULL)
        return false;
    *outContext = &staticState;
    (void)PublishFailureService(host);
    return true;
}

static const LaiueModuleApiV1 publishingFailStartApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "example.profile.start_published_failure",
        .version = "1.0.0",
        .providesServices = publishedFailureServices,
        .providesCount = 1u,
    },
    .create = PublishingFailStartCreate,
    .start = FailingStart,
    .stop = CountStartFailureStop,
    .destroy = CountStartFailureDestroy,
};

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
    .stop = CountStartFailureStop,
    .destroy = CountStartFailureDestroy,
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

static const LaiueModuleRequirementV1 selectionRequirement[] = {
    {"example.selection", 1u},
};
static const LaiueModuleApiV1 selectionConsumerApi = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "example.selection.consumer",
            .version = "1.0.0",
            .requiresServices = selectionRequirement,
            .requiresCount = 1u,
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

    static const char hostLifetimeName[] = "example.host.lifetime";
    static const char lateHostServiceName[] = "example.host.late";
    static uint32_t lateHostServiceTable;
    LaiueModuleServiceV1 hostLifetimeService = {
        .name = hostLifetimeName,
        .version = 1u,
        .table = &hostLifetimeServiceTable,
        .tableSize = sizeof(hostLifetimeServiceTable),
    };
    LaiueModuleServiceV1 lateHostService = {
        .name = lateHostServiceName,
        .version = 1u,
        .table = &lateHostServiceTable,
        .tableSize = sizeof(lateHostServiceTable),
    };
    Expect(LaiueModuleHostRegisterService(host, &hostLifetimeService, &diagnostic) ==
               LAIUE_MODULE_OK,
           "host service registers before modules load");
    const LaiueModuleApiV1 *lifetimeApis[] = {&serviceLifetimeApi};
    Expect(LaiueModuleHostLoadStatic(host, lifetimeApis, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "service lifetime fixture loads");
    Expect(LaiueModuleHostUnregisterService(host, hostLifetimeName, &diagnostic) ==
               LAIUE_MODULE_BUSY &&
               LaiueModuleHostQueryService(host, hostLifetimeName, 1u, sizeof(hostLifetimeServiceTable),
                                           NULL, NULL) == &hostLifetimeServiceTable,
           "host service remains available while a module may retain its pointer");
    Expect(LaiueModuleHostRegisterService(host, &lateHostService, &diagnostic) ==
               LAIUE_MODULE_BUSY,
           "host service registry cannot change while modules are loaded");
    Expect(serviceLifetimeState.host->unpublishService(
               serviceLifetimeState.host->context, moduleLifetimeServiceName) ==
               LAIUE_MODULE_BUSY &&
               LaiueModuleHostQueryService(host, moduleLifetimeServiceName, 1u,
                                           sizeof(serviceLifetimeState), NULL, NULL) ==
                   &serviceLifetimeState,
           "a running provider cannot invalidate a cached service table");
    LaiueModuleHostUnloadAll(host);
    Expect(serviceLifetimeStopUnpublishStatus == LAIUE_MODULE_BUSY &&
               LaiueModuleHostQueryService(host, moduleLifetimeServiceName, 1u, 1u,
                                       NULL, NULL) == NULL,
           "the registry stays frozen during stop and host removes services after unload");
    Expect(LaiueModuleHostUnregisterService(host, hostLifetimeName, &diagnostic) ==
               LAIUE_MODULE_OK &&
               LaiueModuleHostRegisterService(host, &lateHostService, &diagnostic) ==
                   LAIUE_MODULE_OK &&
               LaiueModuleHostUnregisterService(host, lateHostServiceName, &diagnostic) ==
                   LAIUE_MODULE_OK,
           "host service registry becomes mutable again after unload");

    LaiueModuleServiceV1 oldHostService = {
        .name = optionalVersionedServiceName,
        .version = 1u,
        .table = &optionalProviderServiceTable,
        .tableSize = sizeof(optionalProviderServiceTable),
    };
    Expect(LaiueModuleHostRegisterService(host, &oldHostService, &diagnostic) == LAIUE_MODULE_OK,
           "older optional host service registers");
    const LaiueModuleApiV1 *optionalConsumerOnly[] = {&optionalConsumerApi};
    Expect(LaiueModuleHostLoadStatic(host, optionalConsumerOnly, 1u, &diagnostic) ==
               LAIUE_MODULE_OK && optionalFallbackState.starts == 1u &&
               optionalFallbackState.observedMissingService,
           "an incompatible optional host service uses the fallback path");
    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostUnregisterService(host, optionalVersionedServiceName, &diagnostic) ==
               LAIUE_MODULE_OK,
           "old optional host service unregisters after consumer unload");

    const LaiueModuleApiV1 *oldOptionalProvider[] = {&optionalConsumerApi,
                                                     &optionalProviderApi};
    optionalFallbackState.starts = 0u;
    optionalFallbackState.observedMissingService = false;
    Expect(LaiueModuleHostLoadStatic(host, oldOptionalProvider, 2u, &diagnostic) ==
               LAIUE_MODULE_OK && optionalFallbackState.starts == 1u &&
               optionalFallbackState.observedMissingService &&
               LaiueModuleHostLoadedCount(host) == 2u,
           "a started provider with an older service version does not block an optional consumer");
    LaiueModuleHostUnloadAll(host);

    const LaiueModuleApiV1 *optionalCycle[] = {&optionalThirdCycleApi,
                                                &optionalCycleProviderApi,
                                                &optionalConsumerApi};
    optionalFallbackState.starts = 0u;
    optionalFallbackState.observedMissingService = false;
    optionalMiddleState.observedOptionalService = false;
    optionalThirdState.observedOptionalService = false;
    Expect(LaiueModuleHostLoadStatic(host, optionalCycle, 3u, &diagnostic) ==
               LAIUE_MODULE_OK && optionalFallbackState.starts == 1u &&
               optionalFallbackState.observedMissingService &&
               optionalMiddleState.observedOptionalService &&
               optionalThirdState.observedOptionalService &&
               LaiueModuleHostLoadedCount(host) == 3u,
           "one optional fallback unlocks a three-module dependency cycle");
    LaiueModuleHostUnloadAll(host);

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
     * order in which artifacts happen to be listed. An unselected conflict
     * is disabled as a graph component instead of choosing by ID. */
    static LaiueModuleLoadReportEntryV1 selectionEntries[2];
    LaiueModuleLoadReportV1 selectionReport;
    LaiueModuleLoadReportInitialize(&selectionReport, selectionEntries, 2u);
    static const LaiueModuleBinaryV1 selectionBinaries[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &selectedProviderAlphaApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &selectedProviderZetaApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, selectionBinaries,
               (uint32_t)(sizeof(selectionBinaries) / sizeof(selectionBinaries[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &selectionReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "unselected provider conflict is partial");
    Expect(LaiueModuleHostLoadedCount(host) == 0u && selectionReport.loadedCount == 0u &&
               (selectionEntries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
               (selectionEntries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u,
           "unselected provider conflict does not choose by ID");
    LaiueModuleHostUnloadAll(host);

    LaiueModuleLoadReportInitialize(&selectionReport, selectionEntries, 2u);
    static const LaiueModuleProviderSelectionV1 providerSelection = {
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = "example.selection",
        .moduleId = "example.provider.zeta",
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

    /* The selected artifact may be present but unloadable (a Vulkan provider
     * on a system without the Vulkan loader) or absent. A partial profile
     * then treats the service as missing: the competing provider is not a
     * hidden fallback, its consumer is disabled, and the independent module
     * still starts instead of the whole graph being rejected. */
    static LaiueModuleLoadReportEntryV1 unavailableEntries[4];
    LaiueModuleLoadReportV1 unavailableReport;
    LaiueModuleBinaryV1 unavailableBinaries[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &badAbiApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &selectedProviderAlphaApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &selectionConsumerApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    static const LaiueModuleProviderSelectionV1 unavailableSelection = {
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = "example.selection",
        .moduleId = "example.provider.zeta",
    };
    LaiueModuleProfileV1 unavailableProfile = {
        .structSize = sizeof(unavailableProfile),
        .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
        .binaries = unavailableBinaries,
        .binaryCount = 4u,
        .providerSelections = &unavailableSelection,
        .providerSelectionCount = 1u,
    };
    for (uint32_t variant = 0u; variant < 2u; ++variant)
    {
        if (variant == 1u)
            unavailableBinaries[0] =
                (LaiueModuleBinaryV1){missingPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
        LaiueModuleLoadReportInitialize(&unavailableReport, unavailableEntries, 4u);
        Expect(LaiueModuleHostLoadProfileV1(host, &unavailableProfile, &unavailableReport,
                                            &diagnostic) == LAIUE_MODULE_PARTIAL,
               "unavailable selected provider keeps a partial profile running");
        Expect(LaiueModuleHostLoadedCount(host) == 1u &&
                   LaiueModuleHostIsLoaded(host, "example.static") &&
                   !LaiueModuleHostIsLoaded(host, "example.provider.alpha") &&
                   !LaiueModuleHostIsLoaded(host, "example.selection.consumer") &&
                   (unavailableEntries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
                   (unavailableEntries[2].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
                   (unavailableEntries[3].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u,
               "unavailable selected provider disables only its service branch");
        LaiueModuleHostUnloadAll(host);
    }
    unavailableProfile.flags = 0u;
    LaiueModuleLoadReportInitialize(&unavailableReport, unavailableEntries, 4u);
    Expect(LaiueModuleHostLoadProfileV1(host, &unavailableProfile, &unavailableReport,
                                        &diagnostic) == LAIUE_MODULE_INVALID_ARGUMENT &&
               LaiueModuleHostLoadedCount(host) == 0u,
           "strict profile still rejects an unavailable selected provider");

    /* A present optional provider may fail in create/start. The profile
     * disables it in the same transaction, while the independent static
     * module still reaches a clean running graph without a second start. */
    failingStartCalls = 0u;
    startFailureStopCalls = 0u;
    startFailureDestroyCalls = 0u;
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
    Expect(failingStartCalls == 1u && staticState.starts == 1u &&
               startFailureStopCalls == 0u && startFailureDestroyCalls == 1u,
           "failed start skips stop and destroys the successfully created context once");
    LaiueModuleHostUnloadAll(host);

    /* A callback may publish a service before create/start reports failure.
     * The failed branch must remove that service before its context/library is
     * discarded; querying it after the partial transaction must be safe. */
    publishingFailureCreateCalls = 0u;
    createFailureDestroyCalls = 0u;
    publishedFailureState.starts = 0u;
    publishedFailureState.stops = 0u;
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 2u);
    LaiueModuleBinaryV1 createPublishedFailure[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &publishingFailCreateApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, createPublishedFailure,
               (uint32_t)(sizeof(createPublishedFailure) / sizeof(createPublishedFailure[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "partial profile isolates optional create failure after publication");
    Expect(publishingFailureCreateCalls == 1u && createFailureDestroyCalls == 0u,
           "failed create cleans itself because destroy is not called");
    Expect(LaiueModuleHostQueryService(host, publishedFailureServices[0], 1u, 1u,
                                       NULL, NULL) == NULL,
           "create failure removes published service");
    LaiueModuleHostUnloadAll(host);

    failingStartCalls = 0u;
    startFailureStopCalls = 0u;
    startFailureDestroyCalls = 0u;
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 2u);
    LaiueModuleBinaryV1 startPublishedFailure[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &publishingFailStartApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, startPublishedFailure,
               (uint32_t)(sizeof(startPublishedFailure) / sizeof(startPublishedFailure[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "partial profile isolates optional start failure after publication");
    Expect(failingStartCalls == 1u && startFailureStopCalls == 0u &&
               startFailureDestroyCalls == 1u,
           "failed start is cleaned by destroy without calling stop");
    Expect(LaiueModuleHostQueryService(host, publishedFailureServices[0], 1u, 1u,
                                       NULL, NULL) == NULL,
           "start failure removes published service");
    LaiueModuleHostUnloadAll(host);

    /* An optional module that fails in create is removed from the table. A
     * later start pass, needed here because the consumer waits for a provider
     * that sorts after it, must skip the emptied slot instead of reading its
     * descriptor through a cleared API pointer. */
    publishingFailureCreateCalls = 0u;
    regressionConsumerStarts = 0u;
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 3u);
    LaiueModuleBinaryV1 laterPassAfterFailure[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC, &regressionConsumerApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL, &publishingFailCreateApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &regressionProviderApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, laterPassAfterFailure,
               (uint32_t)(sizeof(laterPassAfterFailure) / sizeof(laterPassAfterFailure[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport,
               &diagnostic) == LAIUE_MODULE_PARTIAL,
           "optional create failure before a later start pass stays partial");
    Expect(LaiueModuleHostLoadedCount(host) == 2u && regressionConsumerStarts == 1u &&
               publishingFailureCreateCalls == 1u,
           "consumer and provider start after the failed optional module is removed");
    LaiueModuleHostUnloadAll(host);

    /* Two independent optional callbacks may fail in the same transaction.
     * The report must retain both concrete callback failures instead of
     * overwriting the first one with the last failed module ID. */
    publishingFailureCreateCalls = 0u;
    failingStartCalls = 0u;
    createFailureDestroyCalls = 0u;
    startFailureStopCalls = 0u;
    startFailureDestroyCalls = 0u;
    LaiueModuleLoadReportInitialize(&profileReport, profileEntries, 3u);
    LaiueModuleBinaryV1 simultaneousFailures[] = {
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &publishingFailCreateApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC | LAIUE_MODULE_BINARY_OPTIONAL,
         &failingStartApi},
        {NULL, LAIUE_MODULE_BINARY_STATIC, &staticApi},
    };
    Expect(LaiueModuleHostLoadProfile(
               host, simultaneousFailures,
               (uint32_t)(sizeof(simultaneousFailures) /
                          sizeof(simultaneousFailures[0])),
               LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, &profileReport, &diagnostic) ==
               LAIUE_MODULE_PARTIAL,
           "partial profile isolates simultaneous callback failures");
    Expect(publishingFailureCreateCalls == 1u && failingStartCalls == 1u &&
               createFailureDestroyCalls == 0u && startFailureStopCalls == 0u &&
               startFailureDestroyCalls == 1u &&
               LaiueModuleHostLoadedCount(host) == 1u &&
               profileEntries[0].status == LAIUE_MODULE_PARTIAL &&
               profileEntries[1].status == LAIUE_MODULE_PARTIAL &&
               (profileEntries[0].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
               (profileEntries[1].flags & LAIUE_MODULE_PROFILE_ENTRY_DISABLED) != 0u &&
               (profileEntries[2].flags & LAIUE_MODULE_PROFILE_ENTRY_LOADED) != 0u,
           "report preserves both callback failures and independent success");
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
