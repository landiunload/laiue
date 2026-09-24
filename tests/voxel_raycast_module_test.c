#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "voxel/raycast_service.h"

#include <stdbool.h>

#if defined(_WIN32)
#define RAYCAST_MODULE_NAME L"laiue_voxel_raycast.dll"
#elif defined(__APPLE__)
#define RAYCAST_MODULE_NAME L"liblaiue_voxel_raycast.dylib"
#else
#define RAYCAST_MODULE_NAME L"liblaiue_voxel_raycast.so"
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

LAIUE_TEST_ENTRY(VoxelRaycastModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t worldPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(path, directory, RAYCAST_MODULE_NAME), "raycast module path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 binary = {path, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) ==
               LAIUE_MODULE_DEPENDENCY_MISSING,
           "raycast without world reports missing dependency");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed raycast graph rolls back");
    LaiueModuleHostDestroy(host);

#if defined(_WIN32)
    const wchar_t *worldName = L"laiue_world.dll";
    const wchar_t *numericName = L"laiue_numeric.dll";
#elif defined(__APPLE__)
    const wchar_t *worldName = L"liblaiue_world.dylib";
    const wchar_t *numericName = L"liblaiue_numeric.dylib";
#else
    const wchar_t *worldName = L"liblaiue_world.so";
    const wchar_t *numericName = L"liblaiue_numeric.so";
#endif
    Expect(Join(worldPath, directory, worldName), "world module path fits");
    Expect(Join(numericPath, directory, numericName), "numeric module path fits");
    host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "second module host creates");
    LaiueModuleBinaryV1 binaries[] = {
        {path, 0u, NULL}, {worldPath, 0u, NULL}, {numericPath, 0u, NULL}};
    Expect(LaiueModuleHostLoad(host, binaries,
                               (uint32_t)(sizeof(binaries) / sizeof(binaries[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueVoxelRaycastServiceV1 *service =
        (const LaiueVoxelRaycastServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_VOXEL_RAYCAST_SERVICE_NAME,
            LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1,
            sizeof(LaiueVoxelRaycastServiceV1), &version, &size);
    Expect(service != NULL && version == LAIUE_VOXEL_RAYCAST_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*service) && service->raycast != NULL &&
               service->raycastWithContext != NULL && service->context != NULL,
           "raycast service is published");

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_VOXEL_RAYCAST_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "raycast service survives unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
