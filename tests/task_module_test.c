#include "mod/module_host.h"
#include "platform/system.h"
#include "task/task_service.h"
#include "test_runtime.h"

#include <stdbool.h>

#if defined(_WIN32)
#define TASK_PROVIDER_NAME L"laiue_task.dll"
#elif defined(__APPLE__)
#define TASK_PROVIDER_NAME L"liblaiue_task.dylib"
#else
#define TASK_PROVIDER_NAME L"liblaiue_task.so"
#endif

static volatile uint32_t covered;

static void CountRange(void *context, uint32_t begin, uint32_t end)
{
    (void)context;
    for (uint32_t index = begin; index < end; ++index)
        ++covered;
}
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

LAIUE_TEST_ENTRY(TaskModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;

    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "task test executable directory is available");
    Expect(Join(providerPath, directory, TASK_PROVIDER_NAME),
           "task provider path fits");

    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "task module host creates");

    LaiueModuleBinaryV1 binary = {providerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueTaskServiceV1 *tasks =
        (const LaiueTaskServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_TASK_SERVICE_NAME, LAIUE_TASK_SERVICE_ABI_VERSION_1,
            sizeof(LaiueTaskServiceV1), &version, &size);
    Expect(tasks != NULL && version == LAIUE_TASK_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*tasks) && tasks->createPool != NULL &&
               tasks->getExecutor != NULL && tasks->destroyPool != NULL,
           "task service table is published");

    LaiueTaskPool *pool = tasks->createPool(1u);
    Expect(pool != NULL, "task service creates a pool");
    LaiueTaskExecutor executor = {.structSize = sizeof(executor)};
    Expect(tasks->getExecutor(pool, &executor) != 0u && executor.run != NULL,
           "task service returns an executor");
    covered = 0u;
    executor.run(executor.context, 32u, 4u, CountRange, NULL);
    Expect(covered == 32u, "task service executes every range exactly once");
    tasks->destroyPool(pool);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_TASK_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "task service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
