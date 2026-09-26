// ROUND 2 harness for the mod manifest parser and pack reader.
//
// Purpose: measure `LaiueModManifestParse` on representative inputs (small,
// representative, large, escaped/non-ASCII, malformed, duplicate, embedded
// NUL) and the file-backed `LaiueModPackEnumerate` / `LaiueModPackInspect`
// paths, plus the peak process memory that the reusable manifest read buffer
// contributes.
//
// The executable links `laiue_mod` (a shared module): the SAME executable is
// run with the baseline module and the candidate module in the A/B script, so
// no harness difference can leak into the comparison.
//
// Output lines:
//   r2mod,<workload>,<iterations>,<elapsed_ns>,<checksum>
//   r2mem,<mode>,<peak_working_set>,<working_set>,<peak_pagefile>,<pagefile>
//
// `LAIUE_MOD_BENCH_ROOT` selects the pack root; without it the executable
// directory is used. `LAIUE_R2_MOD_MODE` selects an optional single-purpose
// mode (`memory_empty`, `memory_pack`); without it all workloads run.

#include "mod/mod_manifest.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define R2_TYPICAL_PACKS 32u
#define R2_MANY_PACKS 1024u
#define R2_PARSE_ITERATIONS 100000u
#define R2_PARSE_LARGE_ITERATIONS 4000u
#define R2_ENUMERATE_MANY_ITERATIONS 10u
#define R2_INSPECT_MANY_ITERATIONS 1000u
#define R2_WARMUP_DIVISOR 20u
#define R2_LARGE_MANIFEST_BYTES (16u * 1024u)
#define R2_READ_MIN_BYTES 4096u

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#define R2_ENTRY_KEY "entry_windows_arm64"
#define R2_ARTIFACT_SUFFIX L".dll"
#define R2_ARTIFACT_SUFFIX_UTF8 ".dll"
#elif defined(_WIN32)
#define R2_ENTRY_KEY "entry_windows_x86_64"
#define R2_ARTIFACT_SUFFIX L".dll"
#define R2_ARTIFACT_SUFFIX_UTF8 ".dll"
#elif defined(__linux__) && defined(__aarch64__) && defined(LAIUE_LINUX_LIBC_MUSL)
#define R2_ENTRY_KEY "entry_linux_arm64_musl"
#define R2_ARTIFACT_SUFFIX L".so"
#define R2_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__linux__) && defined(__aarch64__)
#define R2_ENTRY_KEY "entry_linux_arm64_gnu"
#define R2_ARTIFACT_SUFFIX L".so"
#define R2_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(LAIUE_LINUX_LIBC_MUSL)
#define R2_ENTRY_KEY "entry_linux_x86_64_musl"
#define R2_ARTIFACT_SUFFIX L".so"
#define R2_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__linux__)
#define R2_ENTRY_KEY "entry_linux_x86_64_gnu"
#define R2_ARTIFACT_SUFFIX L".so"
#define R2_ARTIFACT_SUFFIX_UTF8 ".so"
#elif defined(__APPLE__) && defined(__x86_64__)
#define R2_ENTRY_KEY "entry_macos_x86_64"
#define R2_ARTIFACT_SUFFIX L".dylib"
#define R2_ARTIFACT_SUFFIX_UTF8 ".dylib"
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define R2_ENTRY_KEY "entry_macos_arm64"
#define R2_ARTIFACT_SUFFIX L".dylib"
#define R2_ARTIFACT_SUFFIX_UTF8 ".dylib"
#else
#error Unsupported mod benchmark platform
#endif

typedef struct R2Scratch
{
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t artifactPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t artifactLeaf[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    char manifest[512];
} R2Scratch;

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
    WriteText("r2mod,");
    WriteText(name);
    WriteText(",");
    WriteUnsigned(iterations);
    WriteText(",");
    WriteUnsigned(nanoseconds);
    WriteText(",");
    WriteUnsigned(checksum);
    WriteText("\n");
}

static void ReportMemory(const char *mode)
{
#if defined(_WIN32)
    typedef struct R2ProcessMemoryCounters
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
    } R2ProcessMemoryCounters;
    typedef int(WINAPI * R2GetProcessMemoryInfo)(HANDLE, R2ProcessMemoryCounters *, uint32_t);
    R2ProcessMemoryCounters counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = (uint32_t)sizeof(counters);
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    R2GetProcessMemoryInfo query =
        kernel != NULL
            ? (R2GetProcessMemoryInfo)(void *)GetProcAddress(kernel, "K32GetProcessMemoryInfo")
            : NULL;
    bool ok =
        query != NULL && query(GetCurrentProcess(), &counters, (uint32_t)sizeof(counters)) != 0;
    WriteText("r2mem,");
    WriteText(mode);
    WriteText(",");
    WriteUnsigned(ok ? counters.peakWorkingSetSize : 0u);
    WriteText(",");
    WriteUnsigned(ok ? counters.workingSetSize : 0u);
    WriteText(",");
    WriteUnsigned(ok ? counters.peakPagefileUsage : 0u);
    WriteText(",");
    WriteUnsigned(ok ? counters.pagefileUsage : 0u);
    WriteText("\n");
#else
    WriteText("r2mem,");
    WriteText(mode);
    WriteText(",0,0,0,0\n");
#endif
}

static bool Join(wchar_t *output, uint32_t capacity, const wchar_t *first, const wchar_t *second,
                 const wchar_t *third)
{
    uint32_t length = 0;
    const wchar_t *parts[] = {first, second, third};
    for (uint32_t partIndex = 0u; partIndex < 3u; ++partIndex)
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
        for (uint32_t index = 0u; part[index] != L'\0'; ++index)
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
    for (uint32_t index = 0u; text[index] != '\0'; ++index)
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

static bool BuildPackName(wchar_t *output, uint32_t capacity, uint32_t index, const wchar_t *suffix)
{
    uint32_t length = 0u;
    const wchar_t *prefix = L"bench_";
    for (uint32_t index2 = 0u; prefix[index2] != L'\0'; ++index2)
    {
        if (length + 1u >= capacity)
        {
            return false;
        }
        output[length++] = prefix[index2];
    }
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

static bool CreatePack(const wchar_t *root, R2Scratch *scratch, uint32_t index,
                       wchar_t *outPackName)
{
    if (!BuildPackName(scratch->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, index, L".lmp") ||
        !BuildPackName(scratch->artifactLeaf, LAIUE_MOD_NATIVE_NAME_CAPACITY, index,
                       R2_ARTIFACT_SUFFIX) ||
        !Join(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName, NULL) ||
        !Join(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName,
              LAIUE_MOD_MANIFEST_FILE_NAME) ||
        !Join(scratch->artifactPath, LAIUE_PLATFORM_PATH_CAPACITY, root, scratch->packName,
              scratch->artifactLeaf))
    {
        return false;
    }
    if (PlatformPathExists(scratch->manifestPath) && PlatformPathExists(scratch->artifactPath))
    {
        if (outPackName != NULL)
        {
            for (uint32_t index2 = 0u; index2 < LAIUE_MOD_NATIVE_NAME_CAPACITY; ++index2)
            {
                outPackName[index2] = scratch->packName[index2];
                if (scratch->packName[index2] == L'\0')
                {
                    break;
                }
            }
        }
        return true;
    }

    uint32_t length = 0u;
    if (!AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "LAIUE MOD 3\nid = bench.mod") ||
        !AppendUnsigned(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest), index,
                        4u) ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    "\nname = Bench Pack\nversion = 1.0.0\nengine = 0.7\n\n[native]\nabi = "
                    "1\n" R2_ENTRY_KEY " = bench_") ||
        !AppendUnsigned(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest), index,
                        4u) ||
        !AppendText(scratch->manifest, &length, (uint32_t)sizeof(scratch->manifest),
                    R2_ARTIFACT_SUFFIX_UTF8 "\n"))
    {
        return false;
    }
    if (!PlatformCreateDirectory(scratch->packPath) ||
        !PlatformWriteEntireFile(scratch->manifestPath, scratch->manifest, length))
    {
        return false;
    }
    uint8_t byte = (uint8_t)('M' + (index & 0x0fu));
    if (!PlatformWriteEntireFile(scratch->artifactPath, &byte, 1u))
    {
        return false;
    }
    if (outPackName != NULL)
    {
        for (uint32_t index2 = 0u; index2 < LAIUE_MOD_NATIVE_NAME_CAPACITY; ++index2)
        {
            outPackName[index2] = scratch->packName[index2];
            if (scratch->packName[index2] == L'\0')
            {
                break;
            }
        }
    }
    return true;
}

static void Fail(const char *message)
{
    WriteText("mod r2 benchmark failure: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

// No-CRT harness: avoid strcmp from the C runtime.
static bool TextEqualsAscii(const char *first, const char *second)
{
    uint32_t index = 0u;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

// A representative manifest with every platform entry (the wave-1 control).
static const char g_typical[] = "LAIUE MOD 3\n"
                                "id = bench.sample\n"
                                "name = Bench Sample\n"
                                "version = 1.2.0-beta\n"
                                "engine = 0.7\n"
                                "\n"
                                "[native]\n"
                                "abi = 1\n"
                                "entry_windows_x86_64 = bench.windows-x86_64.dll\n"
                                "entry_windows_arm64 = bench.windows-arm64.dll\n"
                                "entry_linux_x86_64_gnu = bench.linux-x86_64-gnu.so\n"
                                "entry_linux_x86_64_musl = bench.linux-x86_64-musl.so\n"
                                "entry_linux_arm64_gnu = bench.linux-arm64-gnu.so\n"
                                "entry_linux_arm64_musl = bench.linux-arm64-musl.so\n"
                                "entry_macos_x86_64 = bench.macos-x86_64.dylib\n"
                                "entry_macos_arm64 = bench.macos-arm64.dylib\n";

// The smallest manifest accepted by the grammar.
static const char g_small[] = "LAIUE MOD 3\nid = a1\nversion = 1\nengine = 0.1\n[native]\nabi = "
                              "1\n" R2_ENTRY_KEY " = a" R2_ARTIFACT_SUFFIX_UTF8 "\n";

// Non-ASCII display name and entry plus a punctuated version: the escaped/
// UTF-8 path. UTF-8 bytes are written explicitly so the source stays portable.
static const char g_escaped[] =
    "LAIUE MOD 3\n"
    "id = bench.cafe-x_1.0\n"
    "name = Caf\xc3\xa9 \xc3\x96tker \xe2\x80\x94 weather\n"
    "version = 1.2.0+build_7-x\n"
    "engine = 0.7\n"
    "[native]\nabi = 1\n" R2_ENTRY_KEY " = bench.caf\xc3\xa9" R2_ARTIFACT_SUFFIX_UTF8 "\n";

// Rejected at the line grammar: a line without `key = value`.
static const char g_malformed[] = "LAIUE MOD 3\n"
                                  "id = bench.malformed\n"
                                  "this line has no assignment\n"
                                  "version = 1\nengine = 0.7\n[native]\nabi = 1\n" R2_ENTRY_KEY
                                  " = mod" R2_ARTIFACT_SUFFIX_UTF8 "\n";

// Rejected for duplicate known field.
static const char g_duplicate[] =
    "LAIUE MOD 3\n"
    "id = bench.dup\nversion = 1\nversion = 2\nengine = 0.7\n"
    "[native]\nabi = 1\n" R2_ENTRY_KEY " = mod" R2_ARTIFACT_SUFFIX_UTF8 "\n";

// Embedded NUL byte: rejected by the dedicated scan.
static const char g_nul[] = "LAIUE MOD 3\nid = bench.nul\0version = 1\nengine = 0.7\n";

static char *g_large = NULL;
static uint32_t g_largeLength = 0u;

static double MeasureParse(const char *text, uint32_t length, uint32_t iterations, bool expectOk,
                           uint64_t *outChecksum)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModManifest manifest;
        LaiueModDiagnostic diagnostic;
        LaiueModStatus status = LaiueModManifestParse(text, length, &manifest, &diagnostic);
        if (expectOk)
        {
            if (status != LAIUE_MOD_STATUS_OK)
            {
                Fail("valid manifest was rejected");
            }
            checksum += (uint64_t)(uint8_t)manifest.id[0] + manifest.requiredEngineMinor;
        }
        else
        {
            if (status == LAIUE_MOD_STATUS_OK)
            {
                Fail("invalid manifest was accepted");
            }
            checksum += (uint64_t)status + (uint64_t)(uint8_t)diagnostic.message[0];
        }
    }
    *outChecksum = checksum;
    return PlatformMonotonicSeconds() - begin;
}

static double MeasureEnumerate(const wchar_t *root, uint32_t iterations, uint64_t *outChecksum)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModPackList list;
        LaiueModDiagnostic diagnostic;
        if (LaiueModPackEnumerate(root, &list, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("pack enumeration failed");
        }
        checksum += list.count;
        if (list.count > 0u)
        {
            checksum += (uint64_t)(uint8_t)list.entries[list.count - 1u].packName[0];
            checksum += (uint64_t)(uint8_t)list.entries[0].manifest.id[0];
        }
        LaiueModPackListRelease(&list);
    }
    *outChecksum = checksum;
    return PlatformMonotonicSeconds() - begin;
}

static double MeasureInspect(const wchar_t *root, const wchar_t *packName, uint32_t iterations,
                             uint64_t *outChecksum)
{
    uint64_t checksum = 0u;
    double begin = PlatformMonotonicSeconds();
    for (uint32_t sample = 0u; sample < iterations; ++sample)
    {
        LaiueModPackInfo info;
        LaiueModDiagnostic diagnostic;
        if (LaiueModPackInspect(root, packName, &info, &diagnostic) != LAIUE_MOD_STATUS_OK)
        {
            Fail("direct pack inspection failed");
        }
        checksum += (uint64_t)(uint8_t)info.packName[0] + (uint64_t)(uint8_t)info.manifest.id[0];
    }
    *outChecksum = checksum;
    return PlatformMonotonicSeconds() - begin;
}

// Builds a ~16 KiB valid manifest: the required root/native fields plus many
// ignored unknown keys, so the parse loop really walks a large input.
static bool BuildLargeManifest(void)
{
    uint32_t capacity = R2_LARGE_MANIFEST_BYTES + 4096u;
    char *text = PlatformAllocate(capacity, false);
    if (text == NULL)
    {
        return false;
    }
    uint32_t length = 0u;
    if (!AppendText(text, &length, capacity,
                    "LAIUE MOD 3\nid = bench.large\nname = Large Bench\nversion = 9.9.9\n"
                    "engine = 0.7\n\n[native]\nabi = 1\n" R2_ENTRY_KEY
                    " = large" R2_ARTIFACT_SUFFIX_UTF8 "\n"))
    {
        PlatformFree(text);
        return false;
    }
    uint32_t line = 0u;
    while (length < R2_LARGE_MANIFEST_BYTES)
    {
        if (!AppendText(text, &length, capacity, "padding_") ||
            !AppendUnsigned(text, &length, capacity, line, 5u) ||
            !AppendText(text, &length, capacity,
                        " = 0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnop\n"))
        {
            PlatformFree(text);
            return false;
        }
        ++line;
    }
    g_large = text;
    g_largeLength = length;
    return true;
}

static void RunBenchmarks(const wchar_t *manyRoot, const wchar_t *inspectedPack,
                          const wchar_t *typicalRoot)
{
    uint64_t checksum = 0u;
    double elapsed = 0.0;

    // Warm-up: file cache and branch predictors.
    (void)MeasureParse(g_typical, sizeof(g_typical) - 1u, R2_PARSE_ITERATIONS / R2_WARMUP_DIVISOR,
                       true, &checksum);
    (void)MeasureParse(g_large, g_largeLength, R2_PARSE_LARGE_ITERATIONS / R2_WARMUP_DIVISOR, true,
                       &checksum);
    (void)MeasureEnumerate(manyRoot, R2_ENUMERATE_MANY_ITERATIONS / R2_WARMUP_DIVISOR + 1u,
                           &checksum);

    elapsed = MeasureParse(g_small, sizeof(g_small) - 1u, R2_PARSE_ITERATIONS, true, &checksum);
    Report("parse_small", R2_PARSE_ITERATIONS, elapsed, checksum);
    elapsed = MeasureParse(g_typical, sizeof(g_typical) - 1u, R2_PARSE_ITERATIONS, true, &checksum);
    Report("parse_typical", R2_PARSE_ITERATIONS, elapsed, checksum);
    elapsed = MeasureParse(g_large, g_largeLength, R2_PARSE_LARGE_ITERATIONS, true, &checksum);
    Report("parse_large", R2_PARSE_LARGE_ITERATIONS, elapsed, checksum);
    elapsed = MeasureParse(g_escaped, sizeof(g_escaped) - 1u, R2_PARSE_ITERATIONS, true, &checksum);
    Report("parse_escaped", R2_PARSE_ITERATIONS, elapsed, checksum);
    elapsed =
        MeasureParse(g_malformed, sizeof(g_malformed) - 1u, R2_PARSE_ITERATIONS, false, &checksum);
    Report("parse_malformed", R2_PARSE_ITERATIONS, elapsed, checksum);
    elapsed =
        MeasureParse(g_duplicate, sizeof(g_duplicate) - 1u, R2_PARSE_ITERATIONS, false, &checksum);
    Report("parse_duplicate", R2_PARSE_ITERATIONS, elapsed, checksum);
    elapsed = MeasureParse(g_nul, sizeof(g_nul) - 1u, R2_PARSE_ITERATIONS, false, &checksum);
    Report("parse_nul", R2_PARSE_ITERATIONS, elapsed, checksum);

    elapsed = MeasureEnumerate(manyRoot, R2_ENUMERATE_MANY_ITERATIONS, &checksum);
    Report("enumerate_many", R2_ENUMERATE_MANY_ITERATIONS, elapsed, checksum);
    elapsed = MeasureInspect(typicalRoot, L"bench_0000.lmp", R2_INSPECT_MANY_ITERATIONS, &checksum);
    Report("inspect_typical", R2_INSPECT_MANY_ITERATIONS, elapsed, checksum);
    elapsed = MeasureInspect(manyRoot, inspectedPack, R2_INSPECT_MANY_ITERATIONS, &checksum);
    Report("inspect_many", R2_INSPECT_MANY_ITERATIONS, elapsed, checksum);

    g_sink += checksum;
    WriteText("mod r2 benchmark done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(R2ModManifestBenchmarkEntry)
{
    R2Scratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    wchar_t *baseRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *packsRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *typicalRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *manyRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    char *environmentPath = PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * 4u, false);
    if (scratch == NULL || baseRoot == NULL || packsRoot == NULL || typicalRoot == NULL ||
        manyRoot == NULL || environmentPath == NULL)
    {
        Fail("could not allocate benchmark paths");
    }

    uint32_t environmentLength = PlatformGetEnvironmentUtf8(
        "LAIUE_MOD_BENCH_ROOT", environmentPath, (uint32_t)(LAIUE_PLATFORM_PATH_CAPACITY * 4u));
    uint32_t wideLength = 0u;
    if (environmentLength > 0u &&
        PlatformUtf8ToWide(environmentPath, environmentLength, baseRoot,
                           LAIUE_PLATFORM_PATH_CAPACITY, &wideLength) &&
        wideLength > 0u)
    {
        // baseRoot already holds the explicit root.
    }
    else if (!PlatformExecutableDirectory(baseRoot, LAIUE_PLATFORM_PATH_CAPACITY))
    {
        Fail("could not obtain executable directory");
    }
    if (!Join(packsRoot, LAIUE_PLATFORM_PATH_CAPACITY, baseRoot, L"mod_r2_packs", NULL) ||
        !Join(typicalRoot, LAIUE_PLATFORM_PATH_CAPACITY, packsRoot, L"typical", NULL) ||
        !Join(manyRoot, LAIUE_PLATFORM_PATH_CAPACITY, packsRoot, L"many", NULL))
    {
        Fail("benchmark root path overflowed");
    }
    if (!PlatformCreateDirectory(baseRoot) || !PlatformCreateDirectory(packsRoot) ||
        !PlatformCreateDirectory(typicalRoot) || !PlatformCreateDirectory(manyRoot))
    {
        Fail("could not create benchmark roots");
    }

    // The empty root is used both by the memory mode and as a control.
    wchar_t *emptyRoot =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    if (emptyRoot == NULL ||
        !Join(emptyRoot, LAIUE_PLATFORM_PATH_CAPACITY, packsRoot, L"empty", NULL) ||
        !PlatformCreateDirectory(emptyRoot))
    {
        Fail("could not create empty root");
    }

    for (uint32_t index = 0u; index < R2_TYPICAL_PACKS; ++index)
    {
        if (!CreatePack(typicalRoot, scratch, index, NULL))
        {
            Fail("could not create typical pack");
        }
    }
    wchar_t inspectedPack[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    for (uint32_t index = 0u; index < R2_MANY_PACKS; ++index)
    {
        if (!CreatePack(manyRoot, scratch, index, index == 0u ? inspectedPack : NULL))
        {
            Fail("could not create many pack");
        }
    }

    // Optional single-purpose modes for the memory A/B runner: one process
    // enumerates only an empty root, another only a one-pack root. The
    // difference between their peaks isolates the reusable read buffer.
    uint32_t modeLength = PlatformGetEnvironmentUtf8("LAIUE_R2_MOD_MODE", environmentPath, 64u);
    if (modeLength > 0u)
    {
        if (TextEqualsAscii(environmentPath, "memory_empty"))
        {
            LaiueModPackList list;
            LaiueModDiagnostic diagnostic;
            if (LaiueModPackEnumerate(emptyRoot, &list, &diagnostic) != LAIUE_MOD_STATUS_OK)
            {
                Fail("empty enumeration failed");
            }
            LaiueModPackListRelease(&list);
            ReportMemory("memory_empty");
            LaiueTestRuntimeExit(0);
        }
        if (TextEqualsAscii(environmentPath, "memory_pack"))
        {
            LaiueModPackList list;
            LaiueModDiagnostic diagnostic;
            if (LaiueModPackEnumerate(typicalRoot, &list, &diagnostic) != LAIUE_MOD_STATUS_OK)
            {
                Fail("typical enumeration failed");
            }
            LaiueModPackListRelease(&list);
            ReportMemory("memory_pack");
            LaiueTestRuntimeExit(0);
        }
    }

    if (!BuildLargeManifest())
    {
        Fail("could not build the large manifest");
    }

    ReportMemory("bench");
    RunBenchmarks(manyRoot, inspectedPack, typicalRoot);
    ReportMemory("bench_peak");
    LaiueTestRuntimeExit(0);
}
