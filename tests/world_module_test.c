#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "world/world_service.h"

#include <stdbool.h>

#if defined(_WIN32)
#define WORLD_MODULE_NAME L"laiue_world.dll"
#elif defined(__APPLE__)
#define WORLD_MODULE_NAME L"liblaiue_world.dylib"
#else
#define WORLD_MODULE_NAME L"liblaiue_world.so"
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

LAIUE_TEST_ENTRY(WorldModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(path, directory, WORLD_MODULE_NAME), "world module path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {path, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) ==
               LAIUE_MODULE_DEPENDENCY_MISSING,
           "world without numeric reports missing dependency");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed world graph rolls back");
    LaiueModuleHostDestroy(host);

#if defined(_WIN32)
    const wchar_t *numericName = L"laiue_numeric.dll";
#elif defined(__APPLE__)
    const wchar_t *numericName = L"liblaiue_numeric.dylib";
#else
    const wchar_t *numericName = L"liblaiue_numeric.so";
#endif
    Expect(Join(numericPath, directory, numericName), "numeric module path fits");
    host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "second module host creates");
    LaiueModuleBinaryV1 binaries[] = {{path, 0u, NULL}, {numericPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(host, binaries,
                               (uint32_t)(sizeof(binaries) / sizeof(binaries[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueWorldServiceV1 *service =
        (const LaiueWorldServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_WORLD_SERVICE_NAME, LAIUE_WORLD_SERVICE_ABI_VERSION_1,
            sizeof(LaiueWorldServiceV1), &version, &size);
    Expect(service != NULL && version == LAIUE_WORLD_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*service) && service->create != NULL &&
               service->fillRegion != NULL && service->createWithContext != NULL &&
               service->context != NULL,
           "world service is published");

    World *boundWorld = service->createWithContext(service->context, NULL);
    Expect(boundWorld != NULL && service->getBlock(boundWorld, 0, 0, 0) == BLOCK_AIR,
           "world context-bound creation failed");
    service->destroy(boundWorld);

    World *world = service->create(NULL);
    Expect(world != NULL && service->trySetBlock(world, 9, -2, 3, (BlockType)4U) &&
               service->getBlock(world, 9, -2, 3) == (BlockType)4U &&
               service->getRevision(world) != 0u,
           "world service operations failed");
    service->destroy(world);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_WORLD_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "world service survives unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
