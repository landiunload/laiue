// Кеш конвейеров на настоящем драйвере: первый запуск пишет файл, второй
// загружает его, испорченный файл отвергается без ошибки рендера, а
// выключенный кеш ничего не пишет. Проверяются оба бэкенда, какие есть.
// D3D12 нужен настоящий HWND, поэтому окно создаётся и не показывается.

#include "render/renderer.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define TEST_WIDTH 320
#define TEST_HEIGHT 180
#define SKIP_EXIT_CODE 125

static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t filePath[LAIUE_PLATFORM_PATH_CAPACITY];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Pipeline cache check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - 1u - index];
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

#if defined(_WIN32)
static HWND CreateHiddenWindow(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *className = L"LaiuePipelineCacheTestWindow";
    WNDCLASSEXW windowClass = {0};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExW(0, className, L"laiue pipeline cache", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, TEST_WIDTH, TEST_HEIGHT, NULL, NULL,
                           instance, NULL);
}
#endif

/* Единственный файл кеша в каталоге теста. */
static bool FindCacheFile(void)
{
    PlatformDirectoryIterator iterator;
    PlatformDirectoryEntry entry;
    if (!PlatformDirectoryOpen(&iterator, directory))
        return false;
    bool found = false;
    while (!found && PlatformDirectoryNext(&iterator, &entry))
    {
        if (entry.isDirectory)
            continue;
        uint32_t length = 0u;
        while (directory[length] != L'\0')
        {
            filePath[length] = directory[length];
            ++length;
        }
        filePath[length++] = L'/';
        for (uint32_t index = 0u; entry.name[index] != L'\0'; ++index)
            filePath[length++] = entry.name[index];
        filePath[length] = L'\0';
        found = true;
    }
    PlatformDirectoryClose(&iterator);
    return found;
}

typedef struct RunResult
{
    RendererPipelineCacheStatusV1 status;
    double seconds;
} RunResult;

static bool Run(RendererBackendKind backend, void *window, RunResult *result)
{
    const double begin = PlatformMonotonicSeconds();
    Renderer *renderer = RendererCreateWithBackend(window, TEST_WIDTH, TEST_HEIGHT, backend);
    if (renderer == NULL)
        return false;
    Expect(RendererPrepareWorld(renderer), "world prepares with any cache state");
    result->seconds = PlatformMonotonicSeconds() - begin;
    result->status.structSize = sizeof(result->status);
    RendererGetPipelineCacheStatusV1(renderer, &result->status);
    RendererDestroy(renderer);
    return true;
}

static void Report(const char *label, const RunResult *result)
{
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(": state ");
    WriteUnsigned(result->status.state);
    LaiueTestRuntimeWrite(", pipelines ");
    WriteUnsigned(result->status.pipelinesCreated);
    LaiueTestRuntimeWrite(", from cache ");
    if (result->status.pipelinesFromCache == UINT32_MAX)
        LaiueTestRuntimeWrite("unknown");
    else
        WriteUnsigned(result->status.pipelinesFromCache);
    LaiueTestRuntimeWrite(", create+prepare ");
    WriteUnsigned((uint64_t)(result->seconds * 1000000.0));
    LaiueTestRuntimeWrite(" us\n");
}

static void Corrupt(void)
{
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    Expect(PlatformReadEntireFile(filePath, 1u << 28, &bytes, &size) && size > 100u,
           "cache file readable");
    bytes[size - 1u] ^= 0x5au;
    Expect(PlatformWriteEntireFile(filePath, bytes, size), "write corrupted cache");
    PlatformFree(bytes);
}

/* Возвращает false, если бэкенд недоступен на этой машине. */
static bool CheckBackend(RendererBackendKind backend, void *window, const char *name)
{
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\n");
    Expect(RendererSetPipelineCacheDirectory(directory), "set directory");
    RunResult first;
    if (!Run(backend, window, &first))
        return false;
    Report("  first run", &first);
    Expect(first.status.state == RENDERER_PIPELINE_CACHE_EMPTY, "first run starts empty");
    Expect(first.status.pipelinesCreated >= 4u, "renderer creates its pipelines");
    Expect(FindCacheFile(), "destroy writes the cache file");

    RunResult second;
    Expect(Run(backend, window, &second), "second run");
    Report("  second run", &second);
    Expect(second.status.state == RENDERER_PIPELINE_CACHE_LOADED && second.status.loadedBytes != 0u,
           "second run loads the file");
    Expect(second.status.pipelinesCreated == first.status.pipelinesCreated,
           "same pipelines on every run");
    if (backend == RENDERER_BACKEND_D3D12)
        Expect(second.status.pipelinesFromCache == second.status.pipelinesCreated,
               "every D3D12 pipeline comes from the library");
    Expect(second.status.pipelinesFromCache == UINT32_MAX ||
               second.status.pipelinesFromCache <= second.status.pipelinesCreated,
           "hit count is bounded");

    Corrupt();
    RunResult damaged;
    Expect(Run(backend, window, &damaged), "run with a damaged file");
    Report("  damaged file", &damaged);
    Expect(damaged.status.state == RENDERER_PIPELINE_CACHE_REJECTED &&
               damaged.status.loadedBytes == 0u,
           "damaged file is rejected, not fed to the driver");
    RunResult repaired;
    Expect(Run(backend, window, &repaired), "run after rejection");
    Expect(repaired.status.state == RENDERER_PIPELINE_CACHE_LOADED, "rejected file is rewritten");

    Expect(PlatformDeleteFile(filePath), "delete cache file");
    Expect(RendererSetPipelineCacheDirectory(NULL), "disable cache");
    RunResult disabled;
    Expect(Run(backend, window, &disabled), "run without cache");
    Expect(disabled.status.state == RENDERER_PIPELINE_CACHE_DISABLED, "disabled state");
    Expect(!FindCacheFile(), "disabled cache writes nothing");
    return true;
}

LAIUE_TEST_ENTRY(PipelineCacheTestEntryPoint)
{
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY), "executable dir");
    uint32_t length = 0u;
    while (directory[length] != L'\0')
        ++length;
    static const wchar_t name[] = L"/pipeline_cache_test_";
    for (uint32_t index = 0u; name[index] != L'\0'; ++index)
        directory[length++] = name[index];
    uint8_t random[6];
    Expect(PlatformRandomBytes(random, sizeof(random)), "random");
    static const wchar_t hex[] = L"0123456789abcdef";
    for (uint32_t index = 0u; index < sizeof(random); ++index)
    {
        directory[length++] = hex[random[index] >> 4u];
        directory[length++] = hex[random[index] & 15u];
    }
    directory[length] = L'\0';
    Expect(PlatformCreateDirectory(directory), "create directory");

    uint32_t checked = 0u;
#if defined(_WIN32)
    if (RendererBackendIsAvailable(RENDERER_BACKEND_D3D12))
    {
        HWND window = CreateHiddenWindow();
        if (window != NULL && CheckBackend(RENDERER_BACKEND_D3D12, window, "D3D12"))
            ++checked;
        if (window != NULL)
            DestroyWindow(window);
    }
#endif
    if (RendererBackendIsAvailable(RENDERER_BACKEND_VULKAN) &&
        CheckBackend(RENDERER_BACKEND_VULKAN, NULL, "Vulkan"))
        ++checked;

    (void)RendererSetPipelineCacheDirectory(NULL);
    if (FindCacheFile())
        (void)PlatformDeleteFile(filePath);
    Expect(PlatformRemoveDirectory(directory), "remove directory");
    if (checked == 0u)
    {
        LaiueTestRuntimeWrite("pipeline cache: no GPU backend available, skipped\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }
    LaiueTestRuntimeWrite("pipeline cache: passed\n");
    LAIUE_TEST_SUCCESS();
}
