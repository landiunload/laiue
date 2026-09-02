#include "mod/mod_host.h"
#include "platform/system.h"
#include "mod_test_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define TEST_EXTENSION_FILE_NAME L"laiue_mod_test_extension.dll"
#define TEST_NATIVE_MANIFEST_KEY "entry_windows_arm64"
#define TEST_EXTENSION_FILE_NAME_UTF8 "laiue_mod_test_extension.dll"
#elif defined(_WIN32)
#define TEST_EXTENSION_FILE_NAME L"laiue_mod_test_extension.dll"
#define TEST_NATIVE_MANIFEST_KEY "entry_windows_x86_64"
#define TEST_EXTENSION_FILE_NAME_UTF8 "laiue_mod_test_extension.dll"
#elif defined(__linux__) && defined(__aarch64__) && defined(LAIUE_LINUX_LIBC_MUSL)
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define TEST_NATIVE_MANIFEST_KEY "entry_linux_arm64_musl"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__linux__) && defined(__aarch64__)
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define TEST_NATIVE_MANIFEST_KEY "entry_linux_arm64_gnu"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(LAIUE_LINUX_LIBC_MUSL)
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define TEST_NATIVE_MANIFEST_KEY "entry_linux_x86_64_musl"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__linux__)
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.so"
#define TEST_NATIVE_MANIFEST_KEY "entry_linux_x86_64_gnu"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.so"
#elif defined(__APPLE__) && defined(__x86_64__)
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.dylib"
#define TEST_NATIVE_MANIFEST_KEY "entry_macos_x86_64"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.dylib"
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define TEST_EXTENSION_FILE_NAME L"liblaiue_mod_test_extension.dylib"
#define TEST_NATIVE_MANIFEST_KEY "entry_macos_arm64"
#define TEST_EXTENSION_FILE_NAME_UTF8 "liblaiue_mod_test_extension.dylib"
#else
#error Unsupported mod-host test platform
#endif

typedef struct TestCopyScratch
{
    wchar_t source[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t destination[LAIUE_PLATFORM_PATH_CAPACITY];
} TestCopyScratch;

typedef struct TestPackScratch
{
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    char manifest[1024];
} TestPackScratch;

typedef struct TestRootPaths
{
    wchar_t executableDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packRoot[LAIUE_PLATFORM_PATH_CAPACITY];
} TestRootPaths;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool WideEquals(const wchar_t *first, const wchar_t *second)
{
    uint32_t index = 0;
    while (first[index] != L'\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

static bool AsciiEquals(const char *first, const char *second)
{
    uint32_t index = 0;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

#if !defined(_WIN32)
static bool DirectoryContainsExactName(const wchar_t *directory, const wchar_t *name)
{
    PlatformDirectoryIterator *iterator = PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry *entry = PlatformAllocate(sizeof(*entry), false);
    if (iterator == NULL || entry == NULL || !PlatformDirectoryOpen(iterator, directory))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return false;
    }
    bool found = false;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (WideEquals(entry->name, name))
        {
            found = true;
            break;
        }
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);
    return found;
}
#endif

static bool Join(wchar_t *output, uint32_t capacity, const wchar_t *first, const wchar_t *second,
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

static bool CopyExtension(const wchar_t *executableDirectory, const wchar_t *packRoot,
                          const wchar_t *packName)
{
    TestCopyScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return false;
    }
    if (!Join(scratch->source, LAIUE_PLATFORM_PATH_CAPACITY, executableDirectory,
              TEST_EXTENSION_FILE_NAME, NULL) ||
        !Join(scratch->destination, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
              TEST_EXTENSION_FILE_NAME))
    {
        PlatformFree(scratch);
        return false;
    }
    uint8_t *bytes = NULL;
    uint64_t size = 0;
    bool succeeded =
        PlatformReadEntireFile(scratch->source, LAIUE_MOD_NATIVE_MAX_BYTES, &bytes, &size) &&
        PlatformWriteEntireFile(scratch->destination, bytes, size);
    PlatformFree(bytes);
    PlatformFree(scratch);
    return succeeded;
}

static bool WritePack(const wchar_t *executableDirectory, const wchar_t *packRoot,
                      const wchar_t *packName, const char *id, bool includeArtifact)
{
    TestPackScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return false;
    }
    if (!Join(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName, NULL) ||
        !Join(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
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
        "\nname = Test Extension\nversion = 1.0.0\nengine = 0.6\n[native]\nabi = 1\n",
        TEST_NATIVE_MANIFEST_KEY,
        " = ",
        TEST_EXTENSION_FILE_NAME_UTF8,
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
    if (!PlatformWriteEntireFile(scratch->manifestPath, scratch->manifest, length))
    {
        PlatformFree(scratch);
        return false;
    }
    bool succeeded = !includeArtifact || CopyExtension(executableDirectory, packRoot, packName);
    PlatformFree(scratch);
    return succeeded;
}

typedef struct TestLog
{
    uint32_t informationCount;
    uint32_t errorCount;
} TestLog;

static void CaptureLog(void *context, LaiueModLogLevel level, const char *modId,
                       const char *messageUtf8)
{
    TestLog *log = context;
    (void)modId;
    (void)messageUtf8;
    if (level == LAIUE_MOD_LOG_INFORMATION)
    {
        ++log->informationCount;
    }
    else if (level == LAIUE_MOD_LOG_ERROR)
    {
        ++log->errorCount;
    }
}

static void PreparePackRoot(wchar_t *executableDirectory, wchar_t *packRoot)
{
    Expect(PlatformExecutableDirectory(executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY),
           "could not obtain executable directory");
    Expect(Join(packRoot, LAIUE_PLATFORM_PATH_CAPACITY, executableDirectory, L"mod_host_test_packs",
                NULL),
           "test pack root path overflowed");
    Expect(PlatformCreateDirectory(packRoot), "could not create test pack root");
    Expect(WritePack(executableDirectory, packRoot, L"zeta.lmp", "test.zeta", true),
           "could not prepare zeta test pack");
    Expect(WritePack(executableDirectory, packRoot, L"alpha.lmp", "test.alpha", true),
           "could not prepare alpha test pack");
    Expect(WritePack(executableDirectory, packRoot, L"broken.lmp", "test.broken", false),
           "could not prepare broken test pack");
}

static void TestDiscovery(const wchar_t *packRoot)
{
    LaiueModPackList list;
    LaiueModDiagnostic diagnostic;
    Expect(LaiueModPackEnumerate(packRoot, &list, &diagnostic) == LAIUE_MOD_STATUS_OK,
           "pack discovery failed");
    Expect(list.count == 3u, "pack discovery returned the wrong count");
    Expect(WideEquals(list.entries[0].packName, L"alpha.lmp") &&
               WideEquals(list.entries[1].packName, L"broken.lmp") &&
               WideEquals(list.entries[2].packName, L"zeta.lmp"),
           "pack discovery order is not deterministic");
    Expect(list.entries[0].status == LAIUE_MOD_STATUS_OK &&
               list.entries[1].status == LAIUE_MOD_STATUS_NATIVE_ARTIFACT_NOT_FOUND &&
               list.entries[1].diagnostic.message[0] != '\0' &&
               list.entries[2].status == LAIUE_MOD_STATUS_OK,
           "pack discovery diagnostics are incomplete");
    LaiueModPackListRelease(&list);
    Expect(list.entries == NULL && list.count == 0u, "pack list release did not reset ownership");

    LaiueModPackInfo inspected;
    Expect(LaiueModPackInspect(packRoot, L"ALPHA.lmp", &inspected, &diagnostic) ==
               LAIUE_MOD_STATUS_PACK_NOT_FOUND,
           "direct inspection accepted platform-dependent pack-name casing");

#if !defined(_WIN32)
    wchar_t *collisionPath =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    Expect(collisionPath != NULL &&
               Join(collisionPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, L"Alpha.lmp", NULL) &&
               PlatformCreateDirectory(collisionPath),
           "could not prepare case-collision test pack");
    if (DirectoryContainsExactName(packRoot, L"Alpha.lmp"))
    {
        Expect(LaiueModPackEnumerate(packRoot, &list, &diagnostic) ==
                       LAIUE_MOD_STATUS_PACK_NAME_COLLISION &&
                   list.entries == NULL && list.count == 0u,
               "ASCII case-colliding mod packs were not rejected");
        Expect(LaiueModPackInspect(packRoot, L"alpha.lmp", &inspected, &diagnostic) ==
                   LAIUE_MOD_STATUS_PACK_NAME_COLLISION,
               "direct inspection bypassed a case-colliding mod root");
        Expect(PlatformRemoveDirectory(collisionPath), "could not remove case-collision test pack");
    }
    PlatformFree(collisionPath);
#endif
}

static void TestLifecycle(const wchar_t *packRoot)
{
    TestLog log = {0};
    LaiueModHostConfig config;
    LaiueModHostConfigInitialize(&config, packRoot);
    config.logContext = &log;
    config.log = CaptureLog;
    LaiueModDiagnostic diagnostic;
    LaiueModHost *host = LaiueModHostCreate(&config, &diagnostic);
    Expect(host != NULL, "mod host creation failed");

    Expect(LaiueModHostLoad(host, L"alpha.lmp", NULL, &diagnostic) ==
                   LAIUE_MOD_STATUS_INITIALIZATION_FAILED &&
               diagnostic.modResult == -17,
           "missing service did not fail extension initialization");

    LaiueModTestCounterService counter = {0};
    LaiueModService service = {
        .name = LAIUE_MOD_TEST_COUNTER_SERVICE_NAME,
        .version = LAIUE_MOD_TEST_COUNTER_SERVICE_VERSION,
        .implementation = &counter,
        .implementationSize = sizeof(counter),
    };
    Expect(LaiueModHostRegisterService(host, &service, &diagnostic) == LAIUE_MOD_STATUS_OK,
           "service registration failed");
    Expect(LaiueModHostRegisterService(host, &service, &diagnostic) ==
               LAIUE_MOD_STATUS_SERVICE_ALREADY_REGISTERED,
           "duplicate service registration was accepted");

    LaiueModLoadedInfo info;
    Expect(LaiueModHostLoad(host, L"alpha.lmp", &info, &diagnostic) == LAIUE_MOD_STATUS_OK &&
               AsciiEquals(info.id, "test.alpha") && counter.loadCount == 1u,
           "extension did not load through the registered service");
    Expect(LaiueModHostLoadedCount(host) == 1u, "loaded extension count is incorrect");
    Expect(LaiueModHostUnregisterService(host, LAIUE_MOD_TEST_COUNTER_SERVICE_NAME, &diagnostic) ==
               LAIUE_MOD_STATUS_BUSY,
           "service registry changed while extension was loaded");
    Expect(LaiueModHostLoad(host, L"alpha.lmp", NULL, &diagnostic) ==
               LAIUE_MOD_STATUS_ALREADY_LOADED,
           "duplicate mod id was loaded");
    Expect(LaiueModHostUnload(host, "test.alpha", &diagnostic) == LAIUE_MOD_STATUS_OK &&
               counter.unloadCount == 1u,
           "extension unload callback was not called exactly once");

    const wchar_t *orderedPacks[] = {L"zeta.lmp", L"alpha.lmp"};
    uint32_t loadedCount = 0;
    Expect(LaiueModHostLoadMany(host, orderedPacks, 2u, &loadedCount, &diagnostic) ==
                   LAIUE_MOD_STATUS_OK &&
               loadedCount == 2u && LaiueModHostLoadedCount(host) == 2u,
           "ordered multi-load failed");
    Expect(LaiueModHostGetLoaded(host, 0u, &info) && AsciiEquals(info.id, "test.zeta") &&
               LaiueModHostGetLoaded(host, 1u, &info) && AsciiEquals(info.id, "test.alpha"),
           "multi-load order was not preserved");
    LaiueModHostUnloadAll(host);
    Expect(LaiueModHostLoadedCount(host) == 0u && counter.loadCount == 3u &&
               counter.unloadCount == 3u,
           "reverse unload-all lifecycle count is incorrect");

    const wchar_t *rollbackPacks[] = {L"alpha.lmp", L"broken.lmp"};
    loadedCount = 99u;
    Expect(LaiueModHostLoadMany(host, rollbackPacks, 2u, &loadedCount, &diagnostic) ==
                   LAIUE_MOD_STATUS_NATIVE_ARTIFACT_NOT_FOUND &&
               loadedCount == 0u && LaiueModHostLoadedCount(host) == 0u &&
               counter.loadCount == 4u && counter.unloadCount == 4u,
           "failed multi-load was not rolled back atomically");

    Expect(LaiueModHostUnregisterService(host, LAIUE_MOD_TEST_COUNTER_SERVICE_NAME, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "service unregistration failed after unload");
    LaiueModHostDestroy(host);
    Expect(log.informationCount >= 8u && log.errorCount >= 3u,
           "host lifecycle diagnostics were not emitted");
}

LAIUE_TEST_ENTRY(ModHostTestEntry)
{
    TestRootPaths *paths = PlatformAllocate(sizeof(*paths), false);
    Expect(paths != NULL, "could not allocate test root paths");
    PreparePackRoot(paths->executableDirectory, paths->packRoot);
    TestDiscovery(paths->packRoot);
    TestLifecycle(paths->packRoot);
    PlatformFree(paths);
    LaiueTestRuntimeWrite("mod_host_test passed\n");
    LAIUE_TEST_SUCCESS();
}
