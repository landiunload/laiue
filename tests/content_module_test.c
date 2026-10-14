#include "content/content_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define CONTENT_PROVIDER_NAME L"laiue_content.dll"
#elif defined(__APPLE__)
#define CONTENT_PROVIDER_NAME L"liblaiue_content.dylib"
#else
#define CONTENT_PROVIDER_NAME L"liblaiue_content.so"
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

LAIUE_TEST_ENTRY(ContentModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t root[LAIUE_CONTENT_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;

    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "content test executable directory is available");
    Expect(Join(providerPath, directory, CONTENT_PROVIDER_NAME),
           "content provider path fits");

    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "content module host creates");

    LaiueModuleBinaryV1 binary = {providerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
            sizeof(LaiueContentServiceV1), &version, &size);
    Expect(content != NULL && version == LAIUE_CONTENT_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*content) && content->createCatalog != NULL &&
               content->getRoot != NULL && content->destroyCatalog != NULL,
           "content service table is published");

    LaiueContentCatalog *catalog = content->createCatalog(NULL);
    Expect(catalog != NULL, "content service creates an explicit catalog");
    Expect(content->getRoot(catalog, root, LAIUE_CONTENT_PATH_CAPACITY) != 0u,
           "content service exposes the catalog root");
    content->destroyCatalog(catalog);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_CONTENT_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "content service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
