#include "mod/mod_host.h"
#include "platform/system.h"
#include "mod_test_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define R3_EXTENSION_FILE_NAME L"laiue_mod_test_extension.dll"
#define R3_NATIVE_MANIFEST_KEY "entry_windows_arm64"
#define R3_EXTENSION_FILE_NAME_UTF8 "laiue_mod_test_extension.dll"
#elif defined(_WIN32)
#define R3_EXTENSION_FILE_NAME L"laiue_mod_test_extension.dll"
#define R3_NATIVE_MANIFEST_KEY "entry_windows_x86_64"
#define R3_EXTENSION_FILE_NAME_UTF8 "laiue_mod_test_extension.dll"
#elif defined(__linux__) && defined(__aarch64__) && defined(LAIUE_LINUX_LIBC_MUSL)
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define R3_NATIVE_MANIFEST_KEY "entry_linux_arm64_musl"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__linux__) && defined(__aarch64__)
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define R3_NATIVE_MANIFEST_KEY "entry_linux_arm64_gnu"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(LAIUE_LINUX_LIBC_MUSL)
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define R3_NATIVE_MANIFEST_KEY "entry_linux_x86_64_musl"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__linux__)
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define R3_NATIVE_MANIFEST_KEY "entry_linux_x86_64_gnu"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__APPLE__) && defined(__x86_64__)
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.dylib"
#define R3_NATIVE_MANIFEST_KEY "entry_macos_x86_64"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.dylib"
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define R3_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.dylib"
#define R3_NATIVE_MANIFEST_KEY "entry_macos_arm64"
#define R3_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.dylib"
#else
#error Unsupported r3 mod-host reload test platform
#endif

#define R3_RELOAD_TEST_OBSERVED_UNLOADS 8u

typedef struct R3Log
{
    uint32_t informationCount;
    uint32_t errorCount;
    uint32_t unloadCount;
    char unloadIds[R3_RELOAD_TEST_OBSERVED_UNLOADS][LAIUE_MOD_ID_CAPACITY];
} R3Log;

typedef struct R3Paths
{
    wchar_t executableDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packRoot[LAIUE_PLATFORM_PATH_CAPACITY];
} R3Paths;

// Heap scratch: keeping these path buffers off the stack avoids the __chkstk
// dependency that /NODEFAULTLIB standalone executables do not provide.
typedef struct R3Scratch
{
    wchar_t source[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t destination[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    char manifest[1024];
} R3Scratch;

static bool R3AsciiEquals(const char *first, const char *second)
{
    uint32_t index = 0;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

static void R3Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool R3Join(wchar_t *output, uint32_t capacity, const wchar_t *first, const wchar_t *second,
                   const wchar_t *third)
{
    uint32_t length = 0;
    const wchar_t *parts[] = {first, second, third};
    for (uint32_t partIndex = 0; partIndex < 3u; ++partIndex)
    {
        const wchar_t *part = parts[partIndex];
        if (part == NULL || part[0] == L'\0')
        {
            continue;
        }
        if (length > 0u && output[length - 1u] != L'/' && output[length - 1u] != L'\\')
        {
            if (length + 1u >= capacity)
            {
                return false;
            }
            output[length++] = L'/';
        }
        for (uint32_t index = 0; part[index] != L'\0'; ++index)
        {
            if (length + 1u >= capacity)
            {
                return false;
            }
            output[length++] = part[index];
        }
    }
    output[length] = L'\0';
    return true;
}

static bool R3CopyExtension(const wchar_t *executableDirectory, const wchar_t *packRoot,
                            const wchar_t *packName)
{
    R3Scratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return false;
    }
    bool succeeded = R3Join(scratch->source, LAIUE_PLATFORM_PATH_CAPACITY, executableDirectory,
                            R3_EXTENSION_FILE_NAME, NULL) &&
                     R3Join(scratch->destination, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
                            R3_EXTENSION_FILE_NAME);
    uint8_t *bytes = NULL;
    uint64_t size = 0;
    if (succeeded)
    {
        succeeded =
            PlatformReadEntireFile(scratch->source, LAIUE_MOD_NATIVE_MAX_BYTES, &bytes, &size) &&
            PlatformWriteEntireFile(scratch->destination, bytes, size);
    }
    PlatformFree(bytes);
    PlatformFree(scratch);
    return succeeded;
}

static bool R3WritePack(const wchar_t *executableDirectory, const wchar_t *packRoot,
                        const wchar_t *packName, const char *id)
{
    R3Scratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return false;
    }
    if (!R3Join(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName, NULL) ||
        !R3Join(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
                LAIUE_MOD_MANIFEST_FILE_NAME) ||
        !PlatformCreateDirectory(scratch->packPath))
    {
        PlatformFree(scratch);
        return false;
    }

    uint32_t length = 0;
    const char *parts[] = {
        "LAIUE MOD 3\nid = ",
        id,
        "\nname = R3 Reload Test\nversion = 1.0.0\nengine = 0.6\n[native]\nabi = 1\n",
        R3_NATIVE_MANIFEST_KEY,
        " = ",
        R3_EXTENSION_FILE_NAME_UTF8,
        "\n"};
    for (uint32_t partIndex = 0; partIndex < (uint32_t)(sizeof(parts) / sizeof(parts[0]));
         ++partIndex)
    {
        for (uint32_t index = 0; parts[partIndex][index] != '\0'; ++index)
        {
            if (length + 1u >= (uint32_t)sizeof(scratch->manifest))
            {
                PlatformFree(scratch);
                return false;
            }
            scratch->manifest[length++] = parts[partIndex][index];
        }
    }
    bool succeeded = PlatformWriteEntireFile(scratch->manifestPath, scratch->manifest, length) &&
                     R3CopyExtension(executableDirectory, packRoot, packName);
    PlatformFree(scratch);
    return succeeded;
}

static void R3CaptureLog(void *context, LaiueModLogLevel level, const char *modId,
                         const char *messageUtf8)
{
    R3Log *log = context;
    if (level == LAIUE_MOD_LOG_ERROR)
    {
        ++log->errorCount;
        return;
    }
    if (level != LAIUE_MOD_LOG_INFORMATION)
    {
        return;
    }
    ++log->informationCount;
    if (modId != NULL && messageUtf8 != NULL && R3AsciiEquals(messageUtf8, "unloading") &&
        log->unloadCount < R3_RELOAD_TEST_OBSERVED_UNLOADS)
    {
        uint32_t index = 0;
        while (modId[index] != '\0' && index + 1u < LAIUE_MOD_ID_CAPACITY)
        {
            log->unloadIds[log->unloadCount][index] = modId[index];
            ++index;
        }
        log->unloadIds[log->unloadCount][index] = '\0';
        ++log->unloadCount;
    }
}

static void R3ExpectAt(const LaiueModHost *host, uint32_t index, const char *expectedId,
                       const char *message)
{
    LaiueModLoadedInfo info;
    R3Expect(LaiueModHostGetLoaded(host, index, &info), message);
    R3Expect(R3AsciiEquals(info.id, expectedId), message);
}

LAIUE_TEST_ENTRY(ModHostOrderTestEntry)
{
    R3Paths *paths = PlatformAllocate(sizeof(*paths), false);
    R3Expect(paths != NULL, "could not allocate reload test paths");
    R3Expect(PlatformExecutableDirectory(paths->executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY),
             "could not obtain executable directory");
    R3Expect(R3Join(paths->packRoot, LAIUE_PLATFORM_PATH_CAPACITY, paths->executableDirectory,
                    L"r3_52_mod_host_reload_packs", NULL),
             "reload test pack root path overflowed");
    R3Expect(PlatformCreateDirectory(paths->packRoot), "could not create reload test pack root");
    R3Expect(R3WritePack(paths->executableDirectory, paths->packRoot, L"r3a.lmp", "test.r3a"),
             "could not prepare r3a pack");
    R3Expect(R3WritePack(paths->executableDirectory, paths->packRoot, L"r3b.lmp", "test.r3b"),
             "could not prepare r3b pack");
    R3Expect(R3WritePack(paths->executableDirectory, paths->packRoot, L"r3c.lmp", "test.r3c"),
             "could not prepare r3c pack");
    R3Expect(R3WritePack(paths->executableDirectory, paths->packRoot, L"r3d.lmp", "test.r3d"),
             "could not prepare r3d pack");

    R3Log log;
    memset(&log, 0, sizeof(log));
    LaiueModHostConfig config;
    LaiueModHostConfigInitialize(&config, paths->packRoot);
    config.logContext = &log;
    config.log = R3CaptureLog;
    LaiueModDiagnostic diagnostic;
    LaiueModHost *host = LaiueModHostCreate(&config, &diagnostic);
    R3Expect(host != NULL, "reload test host creation failed");

    LaiueModTestCounterService counter = {0};
    LaiueModService service = {
        .name = LAIUE_MOD_TEST_COUNTER_SERVICE_NAME,
        .version = LAIUE_MOD_TEST_COUNTER_SERVICE_VERSION,
        .implementation = &counter,
        .implementationSize = sizeof(counter),
    };
    R3Expect(LaiueModHostRegisterService(host, &service, &diagnostic) == LAIUE_MOD_STATUS_OK,
             "reload test service registration failed");

    // 1. Load three mods in a known order.
    const wchar_t *firstBatch[] = {L"r3a.lmp", L"r3b.lmp", L"r3c.lmp"};
    uint32_t loadedCount = 0;
    R3Expect(LaiueModHostLoadMany(host, firstBatch, 3u, &loadedCount, &diagnostic) ==
                     LAIUE_MOD_STATUS_OK &&
                 loadedCount == 3u && LaiueModHostLoadedCount(host) == 3u,
             "initial ordered multi-load failed");
    R3ExpectAt(host, 0u, "test.r3a", "initial order index 0 is wrong");
    R3ExpectAt(host, 1u, "test.r3b", "initial order index 1 is wrong");
    R3ExpectAt(host, 2u, "test.r3c", "initial order index 2 is wrong");
    LaiueModLoadedInfo info;
    R3Expect(!LaiueModHostGetLoaded(host, 3u, &info), "out-of-range GetLoaded was accepted");

    // 2. Unload the MIDDLE entry; the remaining order must stay ascending.
    R3Expect(LaiueModHostUnload(host, "test.r3b", &diagnostic) == LAIUE_MOD_STATUS_OK &&
                 LaiueModHostLoadedCount(host) == 2u && counter.unloadCount == 1u,
             "unloading the middle entry failed");
    R3ExpectAt(host, 0u, "test.r3a", "middle unload disturbed index 0");
    R3ExpectAt(host, 1u, "test.r3c", "middle unload did not compact the order");
    R3Expect(!LaiueModHostGetLoaded(host, 2u, &info), "middle unload left a stale trailing index");

    // 3. Reload the same id: it must get a NEW sequence and sort to the END.
    R3Expect(LaiueModHostLoad(host, L"r3b.lmp", &info, &diagnostic) == LAIUE_MOD_STATUS_OK &&
                 R3AsciiEquals(info.id, "test.r3b") && LaiueModHostLoadedCount(host) == 3u &&
                 counter.loadCount == 4u,
             "reloading a previously unloaded id failed");
    R3ExpectAt(host, 0u, "test.r3a", "reload disturbed index 0");
    R3ExpectAt(host, 1u, "test.r3c", "reload disturbed index 1");
    R3ExpectAt(host, 2u, "test.r3b", "reloaded id did not append at the end");
    R3Expect(!LaiueModHostGetLoaded(host, 3u, &info), "reload left a stale trailing index");

    // 4. Unload the OLDEST (head) entry.
    R3Expect(LaiueModHostUnload(host, "test.r3a", &diagnostic) == LAIUE_MOD_STATUS_OK &&
                 LaiueModHostLoadedCount(host) == 2u && counter.unloadCount == 2u,
             "unloading the oldest entry failed");
    R3ExpectAt(host, 0u, "test.r3c", "head unload did not promote the next entry");
    R3ExpectAt(host, 1u, "test.r3b", "head unload disturbed the tail entry");

    // 5. Load a brand new id; it appends after the reloaded entry.
    R3Expect(LaiueModHostLoad(host, L"r3d.lmp", &info, &diagnostic) == LAIUE_MOD_STATUS_OK &&
                 LaiueModHostLoadedCount(host) == 3u && counter.loadCount == 5u,
             "loading a new id after churn failed");
    R3ExpectAt(host, 0u, "test.r3c", "new load disturbed index 0");
    R3ExpectAt(host, 1u, "test.r3b", "new load disturbed index 1");
    R3ExpectAt(host, 2u, "test.r3d", "new load did not append at the end");

    // 6. Unloading an unknown id must be rejected and preserve the order.
    R3Expect(LaiueModHostUnload(host, "test.absent", &diagnostic) == LAIUE_MOD_STATUS_NOT_LOADED,
             "unloading an unknown id was accepted");

    // 7. UnloadAll must drain newest-first: d (seq5), b (seq4), c (seq3).
    LaiueModHostUnloadAll(host);
    R3Expect(LaiueModHostLoadedCount(host) == 0u && counter.loadCount == 5u &&
                 counter.unloadCount == 5u,
             "unload-all lifecycle counters are wrong");
    R3Expect(!LaiueModHostGetLoaded(host, 0u, &info), "GetLoaded succeeded on an empty host");
    R3Expect(log.unloadCount == 5u, "unload log did not observe every unload");
    R3Expect(R3AsciiEquals(log.unloadIds[0], "test.r3b") &&
                 R3AsciiEquals(log.unloadIds[1], "test.r3a") &&
                 R3AsciiEquals(log.unloadIds[2], "test.r3d") &&
                 R3AsciiEquals(log.unloadIds[3], "test.r3b") &&
                 R3AsciiEquals(log.unloadIds[4], "test.r3c"),
             "unload order was not newest-first / lifecycle order");

    LaiueModHostDestroy(host);
    PlatformFree(paths);
    LaiueTestRuntimeWrite("r3_52_mod_host_reload_test passed\n");
    LAIUE_TEST_SUCCESS();
}
