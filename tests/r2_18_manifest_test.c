// ROUND 2 regression test for the mod manifest reader.
//
// Covers the code paths changed this round:
//   * the eight-byte embedded-NUL scan, including a NUL in a middle word and
//     in the trailing bytes;
//   * the ASCII fast paths for the display name and native entry names, plus
//     the fallback that still rejects control bytes, over-long entries and
//     keeps non-ASCII UTF-8 working;
//   * the lazily grown reusable read buffer: a pack whose `mod.lm` is larger
//     than the small first allocation must still be read in full.
//
// It is a black-box test through the public module API only. Every large
// temporary lives in one heap scratch: the no-CRT build has no __chkstk, so
// multi-kilobyte stack frames are not allowed.

#include "mod/mod_manifest.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
#error Unsupported mod manifest R2 test platform
#endif

#define R2_TEST_TEXT_CAPACITY (20000u + 1024u)
#define R2_TEST_FILE_BYTES 9000u

typedef struct R2PathScratch
{
    wchar_t executableDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packRoot[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t artifactPath[LAIUE_PLATFORM_PATH_CAPACITY];
} R2PathScratch;

typedef struct R2Scratch
{
    LaiueModManifest manifest;
    LaiueModPackInfo info;
    char text[R2_TEST_TEXT_CAPACITY];
    char utf8[1024];
    wchar_t selected[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    R2PathScratch paths;
} R2Scratch;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
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

static bool Append(char *buffer, uint32_t *length, uint32_t capacity, const char *text)
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

static bool AppendUnsigned(char *buffer, uint32_t *length, uint32_t capacity, uint32_t value)
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

// Builds a valid manifest of at least `targetBytes` bytes: real fields plus
// ignored unknown keys, so the parser walks the whole input.
static bool BuildLargeManifest(char *buffer, uint32_t capacity, uint32_t targetBytes,
                               uint32_t *outLength)
{
    uint32_t length = 0u;
    if (!Append(buffer, &length, capacity,
                "LAIUE MOD 3\nid = r2.big\nname = R2 Big\nversion = 3.1.4\nengine = 12.34\n\n"
                "[native]\nabi = 7\n" R2_ENTRY_KEY " = r2big" R2_ARTIFACT_SUFFIX_UTF8 "\n"))
    {
        return false;
    }
    uint32_t line = 0u;
    while (length < targetBytes)
    {
        if (!Append(buffer, &length, capacity, "unknown_padding_") ||
            !AppendUnsigned(buffer, &length, capacity, line) ||
            !Append(
                buffer, &length, capacity,
                " = 0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz\n"))
        {
            return false;
        }
        ++line;
    }
    buffer[length] = '\0';
    *outLength = length;
    return true;
}

static bool Join(wchar_t *output, uint32_t capacity, const wchar_t *first, const wchar_t *second,
                 const wchar_t *third)
{
    uint32_t length = 0u;
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

static void TestLargeManifest(R2Scratch *scratch)
{
    uint32_t length = 0u;
    Expect(BuildLargeManifest(scratch->text, R2_TEST_TEXT_CAPACITY, 20000u, &length),
           "could not build the large manifest");
    Expect(length >= 20000u, "large manifest was shorter than requested");

    LaiueModDiagnostic diagnostic;
    Expect(LaiueModManifestParse(scratch->text, length, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "large manifest was rejected");
    Expect(AsciiEquals(scratch->manifest.id, "r2.big") &&
               AsciiEquals(scratch->manifest.displayName, "R2 Big") &&
               AsciiEquals(scratch->manifest.version, "3.1.4") &&
               scratch->manifest.requiredEngineMajor == 12u &&
               scratch->manifest.requiredEngineMinor == 34u && scratch->manifest.requiredAbi == 7u,
           "large manifest fields were parsed incorrectly");

    // NUL embedded in a middle 8-byte word.
    scratch->text[8192] = '\0';
    Expect(LaiueModManifestParse(scratch->text, length, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_MANIFEST_INVALID,
           "NUL in a middle word was accepted");
    scratch->text[8192] = 'q';

    // NUL in the trailing bytes that the eight-byte loop cannot cover.
    scratch->text[length - 1u] = '\0';
    Expect(LaiueModManifestParse(scratch->text, length, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_MANIFEST_INVALID,
           "NUL in the trailing bytes was accepted");
    scratch->text[length - 1u] = '\n';

    Expect(LaiueModManifestParse(scratch->text, length, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "large manifest broke after NUL round-trip");
}

static void TestEscapedValues(R2Scratch *scratch)
{
    static const char escaped[] =
        "LAIUE MOD 3\nid = r2.cafe\nname = Caf\xc3\xa9 \xc3\x96tker\nversion = 2.0+b_1\n"
        "engine = 1.2\n[native]\nabi = 3\n" R2_ENTRY_KEY " = r2.caf\xc3\xa9" R2_ARTIFACT_SUFFIX_UTF8
        "\n";
    LaiueModDiagnostic diagnostic;
    Expect(LaiueModManifestParse(escaped, sizeof(escaped) - 1u, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "escaped manifest was rejected");
    Expect(AsciiEquals(scratch->manifest.displayName, "Caf\xc3\xa9 \xc3\x96tker"),
           "escaped display name changed");
    Expect(AsciiEquals(scratch->manifest.version, "2.0+b_1"), "punctuated version changed");

    Expect(LaiueModManifestSelectNativeEntry(&scratch->manifest, scratch->selected,
                                             LAIUE_MOD_NATIVE_NAME_CAPACITY,
                                             &diagnostic) == LAIUE_MOD_STATUS_OK,
           "escaped native entry was not selected");
    uint32_t utf8Length = 0u;
    Expect(PlatformWideToUtf8(scratch->selected, scratch->utf8, (uint32_t)sizeof(scratch->utf8),
                              &utf8Length) &&
               AsciiEquals(scratch->utf8, "r2.caf\xc3\xa9" R2_ARTIFACT_SUFFIX_UTF8),
           "escaped native entry name changed");

    // A control byte in the display name is still rejected: the ASCII fast
    // path must not weaken the previous control-byte check.
    static const char control[] =
        "LAIUE MOD 3\nid = r2.ctl\nname = bad\x01name\nversion = 1\n"
        "engine = 1.0\n[native]\nabi = 1\n" R2_ENTRY_KEY " = r2" R2_ARTIFACT_SUFFIX_UTF8 "\n";
    Expect(LaiueModManifestParse(control, sizeof(control) - 1u, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_MANIFEST_INVALID,
           "control byte in the display name was accepted");

    // An over-long ASCII native entry is rejected (length >= capacity).
    uint32_t overLength = 0u;
    Expect(
        Append(
            scratch->text, &overLength, R2_TEST_TEXT_CAPACITY,
            "LAIUE MOD 3\nid = r2.long\nversion = 1\nengine = 1.0\n[native]\nabi = 1\n" R2_ENTRY_KEY
            " = ") &&
            overLength + 140u < R2_TEST_TEXT_CAPACITY,
        "could not build the over-long entry manifest");
    for (uint32_t index = 0u; index < 130u; ++index)
    {
        scratch->text[overLength++] = 'a';
    }
    for (uint32_t index = 0u;
         index < (uint32_t)(sizeof(R2_ARTIFACT_SUFFIX_UTF8) - 1u); ++index)
    {
        scratch->text[overLength++] = R2_ARTIFACT_SUFFIX_UTF8[index];
    }
    scratch->text[overLength++] = '\n';
    Expect(LaiueModManifestParse(scratch->text, overLength, &scratch->manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_MANIFEST_INVALID,
           "over-long native entry was accepted");
}

// Writes a pack whose `mod.lm` is larger than the small first read buffer and
// a minimal native artifact, then checks that inspection reads it in full.
static void TestLargeManifestFile(R2Scratch *scratch)
{
    R2PathScratch *paths = &scratch->paths;
    Expect(PlatformExecutableDirectory(paths->executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY),
           "could not obtain executable directory");
    Expect(Join(paths->packRoot, LAIUE_PLATFORM_PATH_CAPACITY, paths->executableDirectory,
                L"r2_18_manifest_packs", NULL),
           "test pack root overflowed");
    Expect(PlatformCreateDirectory(paths->packRoot), "could not create the R2 test pack root");
    Expect(
        Join(paths->packPath, LAIUE_PLATFORM_PATH_CAPACITY, paths->packRoot, L"r2big.lmp", NULL) &&
            Join(paths->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, paths->packRoot, L"r2big.lmp",
                 LAIUE_MOD_MANIFEST_FILE_NAME) &&
            Join(paths->artifactPath, LAIUE_PLATFORM_PATH_CAPACITY, paths->packRoot, L"r2big.lmp",
                 L"r2big" R2_ARTIFACT_SUFFIX),
        "test pack path overflowed");
    Expect(PlatformCreateDirectory(paths->packPath), "could not create the R2 test pack");

    uint32_t length = 0u;
    Expect(BuildLargeManifest(scratch->text, R2_TEST_TEXT_CAPACITY, R2_TEST_FILE_BYTES, &length),
           "could not build the file manifest");
    // Rename the id so the file pack is distinguishable from the parse sample.
    Expect(length > 24u, "file manifest too short");
    scratch->text[20] = 'f';
    scratch->text[21] = 'i';
    scratch->text[22] = 'l';
    Expect(PlatformWriteEntireFile(paths->manifestPath, scratch->text, length),
           "could not write the large manifest file");

    uint8_t artifact = (uint8_t)'M';
    Expect(PlatformWriteEntireFile(paths->artifactPath, &artifact, 1u),
           "could not write the native artifact");

    LaiueModDiagnostic diagnostic;
    Expect(LaiueModPackInspect(paths->packRoot, L"r2big.lmp", &scratch->info, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "large-manifest pack inspection failed");
    Expect(AsciiEquals(scratch->info.manifest.id, "r2.fil") &&
               scratch->info.manifest.requiredAbi == 7u,
           "large-manifest pack fields were not read in full");

    LaiueModPackList list;
    Expect(LaiueModPackEnumerate(paths->packRoot, &list, &diagnostic) == LAIUE_MOD_STATUS_OK &&
               list.count == 1u && AsciiEquals(list.entries[0].manifest.id, "r2.fil"),
           "large-manifest pack enumeration did not read the manifest in full");
    LaiueModPackListRelease(&list);
}

LAIUE_TEST_ENTRY(R2ModManifestTestEntry)
{
    R2Scratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    Expect(scratch != NULL, "could not allocate the R2 test scratch");
    TestLargeManifest(scratch);
    TestEscapedValues(scratch);
    TestLargeManifestFile(scratch);
    PlatformFree(scratch);
    LaiueTestRuntimeWrite("r2_18_manifest_test passed\n");
    LAIUE_TEST_SUCCESS();
}
