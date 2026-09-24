#include "mod/module_host.h"
#include "physics/physics_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define PHYSICS_MODULE_NAME L"laiue_physics.dll"
#elif defined(__APPLE__)
#define PHYSICS_MODULE_NAME L"liblaiue_physics.dylib"
#else
#define PHYSICS_MODULE_NAME L"liblaiue_physics.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index] = root[index], ++index;
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

LAIUE_TEST_ENTRY(PhysicsModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(path, directory, PHYSICS_MODULE_NAME), "physics module path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {path, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_DEPENDENCY_MISSING,
           "physics without numeric reports missing dependencies");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed physics graph rolls back");
    LaiueModuleHostDestroy(host);

    /* The service is loaded through the same dependency graph as an
     * application: providers are listed in reverse order to exercise the
     * stable resolver. */
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t taskPath[LAIUE_PLATFORM_PATH_CAPACITY];
#if defined(_WIN32)
    const wchar_t *numericName = L"laiue_numeric.dll";
    const wchar_t *taskName = L"laiue_task.dll";
#elif defined(__APPLE__)
    const wchar_t *numericName = L"liblaiue_numeric.dylib";
    const wchar_t *taskName = L"liblaiue_task.dylib";
#else
    const wchar_t *numericName = L"liblaiue_numeric.so";
    const wchar_t *taskName = L"liblaiue_task.so";
#endif
    Expect(Join(numericPath, directory, numericName), "numeric module path fits");
    Expect(Join(taskPath, directory, taskName), "task module path fits");

    /* jobs.dll is an optional acceleration provider. Physics must remain
     * usable with only the numeric provider and keep its deterministic
     * sequential fallback. */
    host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "numeric-only physics host creates");
    LaiueModuleBinaryV1 numericOnly[] = {{path, 0u, NULL}, {numericPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(host, numericOnly,
                               (uint32_t)(sizeof(numericOnly) / sizeof(numericOnly[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiuePhysicsServiceV1 *sequentialService =
        (const LaiuePhysicsServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_PHYSICS_SERVICE_NAME, LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
            sizeof(LaiuePhysicsServiceV1), NULL, NULL);
    Expect(sequentialService != NULL && sequentialService->step != NULL,
           "physics starts without jobs provider");

    /* The legacy numeric bridge is process-global for source compatibility,
     * so a second host must fail cleanly instead of stealing the first
     * host's service or clearing it during create/rollback. */
    LaiueModuleHost *secondHost = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(secondHost != NULL, "second concurrent physics host creates");
    LaiueModuleBinaryV1 secondBinaries[] = {{path, 0u, NULL}, {numericPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(secondHost, secondBinaries,
                               (uint32_t)(sizeof(secondBinaries) /
                                          sizeof(secondBinaries[0])),
                               &diagnostic) == LAIUE_MODULE_START_FAILED,
           "second concurrent physics host is rejected by the owner guard");
    Expect(LaiueModuleHostLoadedCount(secondHost) == 0u,
           "failed concurrent physics host rolls back independently");
    LaiueModuleHostDestroy(secondHost);
    Expect(LaiueModuleHostQueryService(host, LAIUE_PHYSICS_SERVICE_NAME,
                                       LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
                                       sizeof(LaiuePhysicsServiceV1), NULL, NULL) != NULL,
           "first physics host remains usable after second host failure");
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);

    host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "second module host creates");
    LaiueModuleBinaryV1 binaries[] = {
        {path, 0u, NULL}, {taskPath, 0u, NULL}, {numericPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(host, binaries,
                               (uint32_t)(sizeof(binaries) / sizeof(binaries[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiuePhysicsServiceV1 *service =
        (const LaiuePhysicsServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_PHYSICS_SERVICE_NAME, LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
            sizeof(LaiuePhysicsServiceV1), &version, &size);
    Expect(service != NULL && version == LAIUE_PHYSICS_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*service) && service->step != NULL &&
               service->stepCompoundEx != NULL && service->compoundMergeBoxes != NULL,
           "physics service is published");
    service->configureThread();
    Expect(service->threadIsConfigured() != 0u, "physics thread configuration failed");
    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_PHYSICS_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "physics service survives unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
