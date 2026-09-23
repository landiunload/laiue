#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "voxel/voxel_service.h"

#include <stdbool.h>

#if defined(_WIN32)
#define VOXEL_PROVIDER_NAME L"laiue_voxel.dll"
#define WORLD_PROVIDER_NAME L"laiue_world.dll"
#define NUMERIC_PROVIDER_NAME L"laiue_numeric.dll"
#elif defined(__APPLE__)
#define VOXEL_PROVIDER_NAME L"liblaiue_voxel.dylib"
#define WORLD_PROVIDER_NAME L"liblaiue_world.dylib"
#define NUMERIC_PROVIDER_NAME L"liblaiue_numeric.dylib"
#else
#define VOXEL_PROVIDER_NAME L"liblaiue_voxel.so"
#define WORLD_PROVIDER_NAME L"liblaiue_world.so"
#define NUMERIC_PROVIDER_NAME L"liblaiue_numeric.so"
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

typedef struct EnumerateState
{
    uint32_t count;
    LaiueVoxelCoordV1 coordinate;
    LaiueVoxelBlockV1 block;
} EnumerateState;

static uint32_t CaptureSolid(void *context, const LaiueVoxelCoordV1 *coordinate,
                             const LaiueVoxelBlockV1 *block)
{
    EnumerateState *state = (EnumerateState *)context;
    if (state == NULL || coordinate == NULL || block == NULL)
        return 0u;
    ++state->count;
    state->coordinate = *coordinate;
    state->block = *block;
    return 1u;
}

LAIUE_TEST_ENTRY(VoxelModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t worldPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "voxel test executable directory is available");
    Expect(Join(providerPath, directory, VOXEL_PROVIDER_NAME),
           "voxel provider path fits");
    Expect(Join(worldPath, directory, WORLD_PROVIDER_NAME),
           "world provider path fits");
    Expect(Join(numericPath, directory, NUMERIC_PROVIDER_NAME),
           "numeric provider path fits");
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "voxel module host creates");
    LaiueModuleBinaryV1 binaries[] = {
        {providerPath, 0u, NULL},
        {worldPath, 0u, NULL},
        {numericPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, binaries,
                               (uint32_t)(sizeof(binaries) / sizeof(binaries[0])),
                               &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueVoxelServiceV1 *voxel =
        (const LaiueVoxelServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_VOXEL_SERVICE_NAME, LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
            sizeof(LaiueVoxelServiceV1), &version, &size);
    Expect(voxel != NULL && version == LAIUE_VOXEL_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*voxel) && voxel->create != NULL &&
               voxel->getProvider != NULL && voxel->setBlock != NULL &&
               voxel->createWithContext != NULL && voxel->context != NULL,
           "voxel service table is published");

    LaiueVoxelWorldV1 *world = NULL;
    Expect(voxel->createWithContext(voxel->context, NULL, &world) != 0u && world != NULL,
           "sparse voxel world creates");
    LaiueVoxelProviderV1 provider = {0};
    Expect(voxel->getProvider(world, &provider) != 0u && provider.getBlock != NULL &&
               provider.getBlockState != NULL &&
               provider.enumerateSolid != NULL,
           "voxel world returns an independent provider table");

    const LaiueVoxelCoordV1 coordinate = {
        .x = INT64_C(1) << 60,
        .y = -(INT64_C(1) << 59),
        .z = -3,
    };
    const LaiueVoxelBlockV1 stone = {.material = 3u, .flags = 7u};
    Expect(voxel->setBlock(world, &coordinate, &stone) != 0u,
           "voxel override at an infinite coordinate is stored");
    LaiueVoxelBlockV1 result = {0};
    Expect(provider.getBlock(&provider, &coordinate, &result) != 0u &&
               result.material == stone.material && result.flags == stone.flags,
           "provider reads the exact override");
    uint32_t explicitEdit = 0u;
    Expect(provider.getBlockState(&provider, &coordinate, &result, &explicitEdit) != 0u &&
               explicitEdit != 0u && result.material == stone.material,
           "provider reports an explicit solid edit");
    Expect(voxel->getRevision(world) == 1u, "voxel revision advances once");

    LaiueVoxelAabbV1 bounds = {
        .minimum = {.x = coordinate.x, .y = coordinate.y, .z = coordinate.z},
        .maximum = {.x = coordinate.x, .y = coordinate.y, .z = coordinate.z},
    };
    EnumerateState state = {0};
    Expect(provider.enumerateSolid(&provider, &bounds, CaptureSolid, &state) != 0u &&
               state.count == 1u && state.coordinate.x == coordinate.x &&
               state.coordinate.y == coordinate.y && state.coordinate.z == coordinate.z,
           "solid enumeration sees only the requested sparse block");

    const LaiueVoxelBlockV1 air = {0};
    Expect(voxel->setBlock(world, &coordinate, &air) != 0u,
           "setting air stores an explicit removal");
    Expect(provider.getBlock(&provider, &coordinate, &result) != 0u &&
               result.material == 0u &&
               provider.getBlockState(&provider, &coordinate, &result, &explicitEdit) != 0u &&
               explicitEdit != 0u && voxel->getRevision(world) == 2u,
           "explicit air remains distinguishable from an untouched coordinate");
    const LaiueVoxelCoordV1 untouched = {.x = coordinate.x + 1, .y = coordinate.y,
                                         .z = coordinate.z};
    Expect(provider.getBlockState(&provider, &untouched, &result, &explicitEdit) != 0u &&
               explicitEdit == 0u && result.material == 0u,
           "untouched coordinate reports the default block");
    voxel->destroy(world);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_VOXEL_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "voxel service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
