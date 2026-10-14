#include "mod/module_host.h"
#include "platform/system.h"
#include "profile/profile_service.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define GRAPHICS_PROFILE_PROVIDER_NAME L"laiue_graphics_profile.dll"
#elif defined(__APPLE__)
#define GRAPHICS_PROFILE_PROVIDER_NAME L"liblaiue_graphics_profile.dylib"
#else
#define GRAPHICS_PROFILE_PROVIDER_NAME L"liblaiue_graphics_profile.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY],
                 const wchar_t *root, const wchar_t *name)
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

LAIUE_TEST_ENTRY(GraphicsProfileModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;

    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "profile test executable directory is available");
    Expect(Join(providerPath, directory, GRAPHICS_PROFILE_PROVIDER_NAME),
           "profile provider path fits");

    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "profile module host creates");
    LaiueModuleBinaryV1 binary = {providerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueGraphicsProfileServiceV1 *service =
        (const LaiueGraphicsProfileServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_GRAPHICS_PROFILE_SERVICE_NAME,
            LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1,
            sizeof(LaiueGraphicsProfileServiceV1), &version, &size);
    Expect(service != NULL && version == LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*service) && service->historyCreate != NULL &&
               service->submitTiming != NULL && service->getSummary != NULL,
           "profile module publishes the complete service table");

    LaiueGraphicsProfileHistory *history = NULL;
    Expect(service->historyCreate(service->context, &history) == LAIUE_GRAPHICS_PROFILE_OK &&
               history != NULL,
           "loaded module creates a host-owned rolling history");
    RendererGpuTimingV1 timing = {
        .structSize = sizeof(timing),
        .flags = RENDERER_GPU_TIMING_SUPPORTED | RENDERER_GPU_TIMING_VALID,
        .frameIndex = 4u,
        .durationNanoseconds = 9000000u,
    };
    Expect(service->submitTiming(history, &timing) == LAIUE_GRAPHICS_PROFILE_OK,
           "loaded profile module accepts completed GPU timing");
    LaiueGraphicsProfileSummaryV1 summary = {
        .structSize = sizeof(summary),
    };
    Expect(service->getSummary(history, &summary) == LAIUE_GRAPHICS_PROFILE_OK &&
               summary.sampleCount == 1u && summary.latestFrameIndex == 4u &&
               summary.p95Nanoseconds == timing.durationNanoseconds,
           "loaded profile service returns its sample summary");
    service->historyDestroy(history);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_GRAPHICS_PROFILE_SERVICE_NAME,
                                       LAIUE_GRAPHICS_PROFILE_SERVICE_ABI_VERSION_1,
                                       1u, NULL, NULL) == NULL,
           "profile service disappears after provider unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
