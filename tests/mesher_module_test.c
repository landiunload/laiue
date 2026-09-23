#include "mesh/mesher_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define MESHER_MODULE_NAME L"laiue_mesher.dll"
#elif defined(__APPLE__)
#define MESHER_MODULE_NAME L"liblaiue_mesher.dylib"
#else
#define MESHER_MODULE_NAME L"liblaiue_mesher.so"
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

static WorldRegionContents EmptyRegion(void *context,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ, BlockType *outBlocks)
{
    (void)context;
    (void)minBlockX;
    (void)minBlockY;
    (void)minBlockZ;
    (void)sizeX;
    (void)sizeY;
    (void)sizeZ;
    (void)outBlocks;
    return WORLD_REGION_ALL_AIR;
}

LAIUE_TEST_ENTRY(MesherModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(path, directory, MESHER_MODULE_NAME), "mesher module path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 binary = {path, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueMesherServiceV1 *service =
        (const LaiueMesherServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_MESHER_SERVICE_NAME, LAIUE_MESHER_SERVICE_ABI_VERSION_1,
            sizeof(LaiueMesherServiceV1), &version, &size);
    Expect(service != NULL && version == LAIUE_MESHER_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*service) && service->scratchCreate != NULL &&
               service->scratchDestroy != NULL && service->buildChunkMesh != NULL,
           "mesher service is published without world");

    ChunkMesherScratch *scratch = service->scratchCreate();
    Expect(scratch != NULL, "mesher scratch creates without world");
    ChunkMesherWorldSource source = {
        .context = NULL,
        .fillRegion = EmptyRegion,
    };
    ChunkQuad *quads = (ChunkQuad *)(void *)&version;
    uint32_t quadCount = UINT32_MAX;
    Expect(service->buildChunkMesh(&source, scratch, 0, 0, 0, &quads, &quadCount),
           "mesher accepts an independent source");
    Expect(quads == NULL && quadCount == 0u,
           "empty independent source produces an empty mesh");
    service->scratchDestroy(scratch);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_MESHER_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "mesher service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
