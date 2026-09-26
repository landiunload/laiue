// ROUND2 harness 19-mod-host: cost of the mod host registry, service lookup and
// loaded-mod ordering. `laiue_mod` is a shared module, so one fixed benchmark
// exe plus the fixed query extension DLL are used for both A and B; only
// laiue_mod.dll is swapped.
//
// Rows: `r3modhost,<workload>,<iterations>,<elapsed_ns>,<checksum>`.
// Checksums are exact and must be identical for baseline and candidate; the
// extension writes its query checksum into a benchmark-owned service struct.
//
// Pack root is `LAIUE_R3_52_MOD_HOST_ROOT` when set, else the executable
// directory, so both sides can share one identical on-disk fixture.

#include "mod/mod_host.h"
#include "mod/mod_manifest.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define R2_SERVICE_NAME_LENGTH 16u
#define R2_TYPICAL_SERVICES 4u
#define R2_MANY_SERVICES LAIUE_MOD_HOST_MAX_SERVICES
#define R2_MANY_MODS LAIUE_MOD_HOST_MAX_LOADED
#define R2_QUERY_LOOP 400000u
#define R2_LOOKUP_ROUNDS 4u
#define R2_REGISTER_ROUNDS 8000u
#define R2_ENUMERATE_ROUNDS 1600u
#define R2_LIFECYCLE_ROUNDS 2000u
#define R2_WARMUP_DIVISOR 20u

#if defined(_WIN32)
#define R2_EXTENSION_FILE_NAME L"laiue_r3_52_mod_host_query_extension.dll"
#define R2_EXTENSION_FILE_NAME_UTF8 "laiue_r3_52_mod_host_query_extension.dll"
#define R2_NATIVE_MANIFEST_KEY "entry_windows_x86_64"
#elif defined(__linux__)
#define R2_EXTENSION_FILE_NAME L"liblaiue_r3_52_mod_host_query_extension.so"
#define R2_EXTENSION_FILE_NAME_UTF8 "liblaiue_r3_52_mod_host_query_extension.so"
#define R2_NATIVE_MANIFEST_KEY "entry_linux_x86_64_gnu"
#elif defined(__APPLE__)
#define R2_EXTENSION_FILE_NAME L"liblaiue_r3_52_mod_host_query_extension.dylib"
#define R2_EXTENSION_FILE_NAME_UTF8 "liblaiue_r3_52_mod_host_query_extension.dylib"
#define R2_NATIVE_MANIFEST_KEY "entry_macos_x86_64"
#else
#error Unsupported r2 mod-host benchmark platform
#endif

// Service implementation shared with the extension: [0]/[1] receive the query
// checksum, the rest is padding so the interface is clearly at least 2 words.
typedef struct R2Service
{
    uint32_t checksumLow;
    uint32_t checksumHigh;
    uint32_t calls;
    uint32_t reserved;
} R2Service;

typedef struct R2Scratch
{
    wchar_t executableDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t baseRoot[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packRoot[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t artifactSource[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t artifactDestination[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t nativeName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t lightNames[R2_MANY_MODS][LAIUE_MOD_NATIVE_NAME_CAPACITY];
    const wchar_t *lightPointers[R2_MANY_MODS];
    char serviceNames[R2_MANY_SERVICES][R2_SERVICE_NAME_LENGTH];
    char manifest[1024];
    LaiueModService serviceDescriptors[R2_MANY_SERVICES];
    R2Service services[R2_MANY_SERVICES];
} R2Scratch;

#if defined(_WIN32)
typedef struct R2MemoryCounters
{
    uint32_t cb;
    uint32_t pageFaultCount;
    uint64_t peakWorkingSetSize;
    uint64_t workingSetSize;
    uint64_t quotaPeakPagedPoolUsage;
    uint64_t quotaPagedPoolUsage;
    uint64_t quotaPeakNonPagedPoolUsage;
    uint64_t quotaNonPagedPoolUsage;
    uint64_t pagefileUsage;
    uint64_t peakPagefileUsage;
} R2MemoryCounters;

__declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(void *process, R2MemoryCounters *counters,
                                                            uint32_t size);
#endif

static volatile uint64_t g_sink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void Report(const char *name, uint32_t iterations, double elapsed, uint64_t checksum)
{
    uint64_t nanoseconds = elapsed > 0.0 ? (uint64_t)(elapsed * 1000000000.0 + 0.5) : 0u;
    WriteText("r3modhost,");
    WriteText(name);
    WriteText(",");
    WriteUnsigned(iterations);
    WriteText(",");
    WriteUnsigned(nanoseconds);
    WriteText(",");
    WriteUnsigned(checksum);
    WriteText("\n");
}

static void Fail(const char *message)
{
    WriteText("r2 mod-host benchmark failure: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

static void PrintMemory(void)
{
#if defined(_WIN32)
    R2MemoryCounters counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = (uint32_t)sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, (uint32_t)sizeof(counters)) != 0)
    {
        WriteText("MEM commit=");
        WriteUnsigned(counters.pagefileUsage);
        WriteText(" peak_commit=");
        WriteUnsigned(counters.peakPagefileUsage);
        WriteText(" peak_working=");
        WriteUnsigned(counters.peakWorkingSetSize);
        WriteText("\n");
        return;
    }
#endif
    WriteText("MEM unavailable\n");
}

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

static bool AppendText(char *buffer, uint32_t *length, uint32_t capacity, const char *text)
{
    for (uint32_t index = 0; text[index] != '\0'; ++index)
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        buffer[(*length)++] = text[index];
    }
    return true;
}

static bool AppendUnsigned(char *buffer, uint32_t *length, uint32_t capacity, uint32_t value,
                           uint32_t minimumDigits)
{
    char digits[10];
    uint32_t digitCount = 0u;
    if (value == 0u)
    {
        digits[digitCount++] = '0';
    }
    while (value != 0u)
    {
        digits[digitCount++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (digitCount < minimumDigits)
    {
        digits[digitCount++] = '0';
    }
    for (uint32_t index = 0u; index < digitCount; ++index)
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        buffer[(*length)++] = digits[digitCount - index - 1u];
    }
    return true;
}

static bool BuildPackName(wchar_t *output, uint32_t capacity, const char *prefix, uint32_t index,
                          bool indexed, const wchar_t *suffix)
{
    uint32_t length = 0u;
    for (uint32_t index2 = 0u; prefix[index2] != '\0'; ++index2)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = (wchar_t)(unsigned char)prefix[index2];
    }
    if (indexed)
    {
        wchar_t digits[8];
        uint32_t digitCount = 0u;
        uint32_t value = index;
        if (value == 0u)
        {
            digits[digitCount++] = L'0';
        }
        while (value != 0u)
        {
            digits[digitCount++] = (wchar_t)(L'0' + (value % 10u));
            value /= 10u;
        }
        while (digitCount < 4u)
        {
            digits[digitCount++] = L'0';
        }
        for (uint32_t digit = 0u; digit < digitCount; ++digit)
        {
            if (length + 1u >= capacity)
            {
                return false;
            }
            output[length++] = digits[digitCount - digit - 1u];
        }
    }
    for (uint32_t index2 = 0u; suffix[index2] != L'\0'; ++index2)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = suffix[index2];
    }
    output[length] = L'\0';
    return true;
}

// Writes one pack: directory + mod.lm + a copy of the fixed query extension.
static bool WritePack(R2Scratch *scratch, const wchar_t *packRoot, const wchar_t *packName,
                      const char *id)
{
    if (!Join(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName, NULL) ||
        !Join(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
              LAIUE_MOD_MANIFEST_FILE_NAME) ||
        !Join(scratch->artifactDestination, LAIUE_PLATFORM_PATH_CAPACITY, packRoot, packName,
              R2_EXTENSION_FILE_NAME))
    {
        return false;
    }
    if (PlatformPathExists(scratch->manifestPath) &&
        PlatformPathExists(scratch->artifactDestination))
    {
        return true;
    }

    uint32_t length = 0u;
    if (!AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "LAIUE MOD 3\nid = ") ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest), id) ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "\nname = R2 Mod Host Bench\nversion = 1.0.0\nengine = 0.7\n[native]\nabi = "
                    "1\n" R2_NATIVE_MANIFEST_KEY " = " R2_EXTENSION_FILE_NAME_UTF8 "\n"))
    {
        return false;
    }
    if (!PlatformCreateDirectory(scratch->packPath) ||
        !PlatformWriteEntireFile(scratch->manifestPath, scratch->manifest, length))
    {
        return false;
    }

    uint8_t *bytes = NULL;
    uint64_t byteCount = 0u;
    if (!PlatformReadEntireFile(scratch->artifactSource, LAIUE_MOD_NATIVE_MAX_BYTES, &bytes,
                                &byteCount) ||
        !PlatformWriteEntireFile(scratch->artifactDestination, bytes, byteCount))
    {
        PlatformFree(bytes);
        return false;
    }
    PlatformFree(bytes);
    return true;
}

static void BuildServiceDescriptors(R2Scratch *scratch, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        char name[R2_SERVICE_NAME_LENGTH];
        uint32_t length = 0u;
        const char *prefix = "bench.svc.";
        while (*prefix != '\0')
        {
            name[length++] = *prefix++;
        }
        name[length++] = (char)('0' + (index / 1000u) % 10u);
        name[length++] = (char)('0' + (index / 100u) % 10u);
        name[length++] = (char)('0' + (index / 10u) % 10u);
        name[length++] = (char)('0' + index % 10u);
        name[length] = '\0';
        for (uint32_t index2 = 0u; index2 <= length; ++index2)
        {
            scratch->serviceNames[index][index2] = name[index2];
        }
        scratch->services[index].checksumLow = 0u;
        scratch->services[index].checksumHigh = 0u;
        scratch->services[index].calls = 0u;
        scratch->services[index].reserved = 0u;
        scratch->serviceDescriptors[index].name = scratch->serviceNames[index];
        scratch->serviceDescriptors[index].version = 1u;
        scratch->serviceDescriptors[index].implementation = &scratch->services[index];
        scratch->serviceDescriptors[index].implementationSize = (uint32_t)sizeof(R2Service);
    }
}

// Console logging would dominate the measured lifecycle cost and flood the
// harness output, so the host callback is a no-op here.
static void SilentLog(void *context, LaiueModLogLevel level, const char *modId,
                      const char *message)
{
    (void)context;
    (void)level;
    (void)modId;
    (void)message;
}

static LaiueModHost *CreateHostWithServices(R2Scratch *scratch, uint32_t serviceCount)
{
    LaiueModHostConfig config;
    LaiueModHostConfigInitialize(&config, scratch->packRoot);
    config.logContext = NULL;
    config.log = SilentLog;
    LaiueModDiagnostic diagnostic;
    LaiueModHost *host = LaiueModHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        Fail("could not create the mod host");
    }
    for (uint32_t index = 0u; index < serviceCount; ++index)
    {
        if (LaiueModHostRegisterService(host, &scratch->serviceDescriptors[index], &diagnostic) !=
            LAIUE_MOD_STATUS_OK)
        {
            Fail("could not register a benchmark service");
        }
    }
    return host;
}

static double MeasureRegister(R2Scratch *scratch, uint32_t rounds, uint64_t *checksum)
{
    LaiueModDiagnostic diagnostic;
    LaiueModHostConfig config;
    LaiueModHostConfigInitialize(&config, scratch->packRoot);
    config.logContext = NULL;
    config.log = SilentLog;
    LaiueModHost *host = LaiueModHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        Fail("could not create the register host");
    }
    uint64_t sum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        for (uint32_t index = 0u; index < R2_MANY_SERVICES; ++index)
        {
            sum += LaiueModHostRegisterService(host, &scratch->serviceDescriptors[index],
                                               &diagnostic) == LAIUE_MOD_STATUS_OK
                       ? 1u
                       : 0u;
        }
        for (uint32_t index = 0u; index < R2_MANY_SERVICES; ++index)
        {
            sum += LaiueModHostUnregisterService(host, scratch->serviceNames[index],
                                                 &diagnostic) == LAIUE_MOD_STATUS_OK
                       ? 1u
                       : 0u;
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    if (LaiueModHostLoadedCount(host) != 0u)
    {
        Fail("register host unexpectedly has loaded mods");
    }
    LaiueModHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

// One heavy pack load executes R2_QUERY_LOOP*3 queryService calls inside the
// extension. The extension stores its checksum into services[0].
static double MeasureLookup(R2Scratch *scratch, uint32_t serviceCount, uint32_t rounds,
                            uint64_t *checksum)
{
    if (!BuildPackName(scratch->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, "bench_query", 0u, false,
                       L".lmp"))
    {
        Fail("could not build the heavy pack name");
    }
    LaiueModHost *host = CreateHostWithServices(scratch, serviceCount);
    uint64_t sum = 0u;
    LaiueModLoadedInfo info;
    LaiueModDiagnostic diagnostic;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        scratch->services[0].checksumLow = 0u;
        scratch->services[0].checksumHigh = 0u;
        if (LaiueModHostLoad(host, scratch->packName, &info, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("heavy query pack failed to load");
        }
        sum += (uint64_t)scratch->services[0].checksumLow |
               ((uint64_t)scratch->services[0].checksumHigh << 32);
        if (LaiueModHostUnload(host, info.id, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("heavy query pack failed to unload");
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

static double MeasureEnumerate(R2Scratch *scratch, uint32_t rounds, uint64_t *checksum)
{
    LaiueModHost *host = CreateHostWithServices(scratch, 1u);
    LaiueModDiagnostic diagnostic;
    uint32_t loadedCount = 0u;
    if (LaiueModHostLoadMany(host, scratch->lightPointers, R2_MANY_MODS, &loadedCount, &diagnostic) !=
            LAIUE_MOD_STATUS_OK ||
        loadedCount != R2_MANY_MODS)
    {
        Fail("could not preload the light mods for enumeration");
    }

    uint64_t sum = 0u;
    LaiueModLoadedInfo info;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        for (uint32_t index = 0u; index < R2_MANY_MODS; ++index)
        {
            if (!LaiueModHostGetLoaded(host, index, &info))
            {
                Fail("GetLoaded failed for a loaded index");
            }
            sum += (uint64_t)(uint8_t)info.id[0];
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

static double MeasureLifecycleTiny(R2Scratch *scratch, uint32_t rounds, uint64_t *checksum)
{
    if (!BuildPackName(scratch->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, "bench_mod", 0u, true,
                       L".lmp"))
    {
        Fail("could not build the tiny pack name");
    }
    LaiueModHost *host = CreateHostWithServices(scratch, 1u);
    uint64_t sum = 0u;
    LaiueModLoadedInfo info;
    LaiueModDiagnostic diagnostic;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t round = 0u; round < rounds; ++round)
    {
        if (LaiueModHostLoad(host, scratch->packName, &info, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("tiny light pack failed to load");
        }
        sum += (uint64_t)(uint8_t)info.id[0];
        if (LaiueModHostUnload(host, info.id, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("tiny light pack failed to unload");
        }
    }
    double elapsed = PlatformMonotonicSeconds() - begin;
    LaiueModHostDestroy(host);
    *checksum = sum;
    return elapsed;
}

LAIUE_TEST_ENTRY(R3ModHostRegistryBenchmarkEntry)
{
    R2Scratch *scratch = PlatformAllocate(sizeof(*scratch), true);
    if (scratch == NULL)
    {
        Fail("could not allocate benchmark scratch");
    }
    if (!PlatformExecutableDirectory(scratch->executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY))
    {
        Fail("could not obtain the executable directory");
    }
    if (!Join(scratch->artifactSource, LAIUE_PLATFORM_PATH_CAPACITY,
              scratch->executableDirectory, R2_EXTENSION_FILE_NAME, NULL))
    {
        Fail("extension source path overflowed");
    }
    if (!PlatformPathExists(scratch->artifactSource))
    {
        Fail("the benchmark query extension is missing next to the executable");
    }

    char *environmentPath = PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * 4u, false);
    if (environmentPath == NULL)
    {
        Fail("could not allocate the environment buffer");
    }
    uint32_t environmentLength = PlatformGetEnvironmentUtf8(
        "LAIUE_R3_52_MOD_HOST_ROOT", environmentPath, (uint32_t)(LAIUE_PLATFORM_PATH_CAPACITY * 4u));
    uint32_t wideLength = 0u;
    if (environmentLength > 0u &&
        PlatformUtf8ToWide(environmentPath, environmentLength, scratch->baseRoot,
                           LAIUE_PLATFORM_PATH_CAPACITY, &wideLength) &&
        wideLength > 0u)
    {
        // baseRoot is the shared root from the environment.
    }
    else
    {
        for (uint32_t index = 0u; index < LAIUE_PLATFORM_PATH_CAPACITY; ++index)
        {
            scratch->baseRoot[index] = scratch->executableDirectory[index];
            if (scratch->executableDirectory[index] == L'\0')
            {
                break;
            }
        }
    }
    PlatformFree(environmentPath);

    if (!Join(scratch->packRoot, LAIUE_PLATFORM_PATH_CAPACITY, scratch->baseRoot,
              L"r3_52_mod_host_packs", NULL))
    {
        Fail("pack root path overflowed");
    }
    if (!PlatformCreateDirectory(scratch->baseRoot) || !PlatformCreateDirectory(scratch->packRoot))
    {
        Fail("could not create the benchmark roots");
    }

    BuildServiceDescriptors(scratch, R2_MANY_SERVICES);

    // Fixtures: one heavy pack and R2_MANY_MODS light packs.
    if (!BuildPackName(scratch->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, "bench_query", 0u, false,
                       L".lmp") ||
        !WritePack(scratch, scratch->packRoot, scratch->packName, "bench.query"))
    {
        Fail("could not prepare the heavy query pack");
    }
    for (uint32_t index = 0u; index < R2_MANY_MODS; ++index)
    {
        char id[32];
        uint32_t idLength = 0u;
        if (!AppendText(id, &idLength, (uint32_t)sizeof(id), "bench.mod.") ||
            !AppendUnsigned(id, &idLength, (uint32_t)sizeof(id), index, 4u))
        {
            Fail("could not build a light mod id");
        }
        id[idLength] = '\0';
        if (!BuildPackName(scratch->lightNames[index], LAIUE_MOD_NATIVE_NAME_CAPACITY, "bench_mod",
                           index, true, L".lmp") ||
            !WritePack(scratch, scratch->packRoot, scratch->lightNames[index], id))
        {
            Fail("could not prepare a light pack");
        }
        scratch->lightPointers[index] = scratch->lightNames[index];
    }

    // Warmup outside statistics.
    uint64_t warmupChecksum = 0u;
    (void)MeasureRegister(scratch, R2_REGISTER_ROUNDS / R2_WARMUP_DIVISOR, &warmupChecksum);
    (void)MeasureLookup(scratch, R2_TYPICAL_SERVICES, 1u, &warmupChecksum);
    (void)MeasureLookup(scratch, R2_MANY_SERVICES, 1u, &warmupChecksum);
    (void)MeasureEnumerate(scratch, R2_ENUMERATE_ROUNDS / R2_WARMUP_DIVISOR, &warmupChecksum);
    (void)MeasureLifecycleTiny(scratch, R2_LIFECYCLE_ROUNDS / R2_WARMUP_DIVISOR, &warmupChecksum);

    uint64_t checksum = 0u;
    Report("register64", R2_MANY_SERVICES * R2_REGISTER_ROUNDS * 2u,
           MeasureRegister(scratch, R2_REGISTER_ROUNDS, &checksum), checksum);
    Report("lookup_few", R2_QUERY_LOOP * 3u * R2_LOOKUP_ROUNDS,
           MeasureLookup(scratch, R2_TYPICAL_SERVICES, R2_LOOKUP_ROUNDS, &checksum), checksum);
    Report("lookup_many", R2_QUERY_LOOP * 3u * R2_LOOKUP_ROUNDS,
           MeasureLookup(scratch, R2_MANY_SERVICES, R2_LOOKUP_ROUNDS, &checksum), checksum);
    Report("getloaded64", R2_MANY_MODS * R2_ENUMERATE_ROUNDS,
           MeasureEnumerate(scratch, R2_ENUMERATE_ROUNDS, &checksum), checksum);
    Report("lifecycle_tiny", R2_LIFECYCLE_ROUNDS * 2u,
           MeasureLifecycleTiny(scratch, R2_LIFECYCLE_ROUNDS, &checksum), checksum);

    g_sink += checksum + warmupChecksum;
    WriteText("r3modhost done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    PrintMemory();
    PlatformFree(scratch);
    LAIUE_TEST_SUCCESS();
}
