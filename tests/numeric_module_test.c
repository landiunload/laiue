#include "mod/module_host.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define NUMERIC_PROVIDER_NAME L"laiue_numeric.dll"
#elif defined(__APPLE__)
#define NUMERIC_PROVIDER_NAME L"liblaiue_numeric.dylib"
#else
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

LAIUE_TEST_ENTRY(NumericModuleTestEntryPoint)
{
    /* Keep the standalone Windows entry below the stack-probe threshold. */
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;

    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "numeric test executable directory is available");
    Expect(Join(providerPath, directory, NUMERIC_PROVIDER_NAME),
           "numeric provider path fits");

    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "numeric module host creates");

    LaiueModuleBinaryV1 binary = {providerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueNumericServiceV1 *numeric =
        (const LaiueNumericServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_NUMERIC_SERVICE_NAME, LAIUE_NUMERIC_SERVICE_ABI_VERSION_1,
            sizeof(LaiueNumericServiceV1), &version, &size);
    Expect(numeric != NULL && version == LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*numeric) && numeric->init != NULL &&
               numeric->tryAddInt64InPlace != NULL && numeric->destroy != NULL,
           "numeric service table is published");

    InfiniteCoord value;
    numeric->init(&value);
    Expect(numeric->tryAddInt64InPlace(&value, 42) != 0u,
           "numeric service performs exact integer addition");
    Expect(numeric->compareAddInt64ToInt64(&value, 0, 42) == 0,
           "numeric service preserves exact comparison");
    Expect(numeric->toDoubleSaturating(&value) == 42.0,
           "numeric service converts the exact value");
    numeric->destroy(&value);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_NUMERIC_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "numeric service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
