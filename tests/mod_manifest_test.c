#include "mod/mod_manifest.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool TextEquals(const char *first, const char *second)
{
    uint32_t index = 0;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

static void TestValidManifest(void)
{
    static const char text[] = "LAIUE MOD 3\n"
                               "id = example.weather\n"
                               "name = Weather Extension\n"
                               "version = 1.2.0-beta\n"
                               "engine = 0.6\n"
                               "\n"
                               "[native]\n"
                               "abi = 1\n"
                               "entry_windows_x86_64 = weather.windows-x86_64.dll\n"
                               "entry_windows_arm64 = weather.windows-arm64.dll\n"
                               "entry_linux_x86_64_gnu = weather.linux-x86_64-gnu.so\n"
                               "entry_linux_x86_64_musl = weather.linux-x86_64-musl.so\n"
                               "entry_linux_arm64_gnu = weather.linux-arm64-gnu.so\n"
                               "entry_linux_arm64_musl = weather.linux-arm64-musl.so\n"
                               "entry_macos_x86_64 = weather.macos-x86_64.dylib\n"
                               "entry_macos_arm64 = weather.macos-arm64.dylib\n";
    LaiueModManifest manifest;
    LaiueModDiagnostic diagnostic;
    Expect(LaiueModManifestParse(text, sizeof(text) - 1u, &manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_OK,
           "valid manifest was rejected");
    Expect(TextEquals(manifest.id, "example.weather") &&
               TextEquals(manifest.displayName, "Weather Extension") &&
               TextEquals(manifest.version, "1.2.0-beta"),
           "manifest metadata changed during parsing");
    Expect(manifest.requiredEngineMajor == 0u && manifest.requiredEngineMinor == 6u &&
               manifest.requiredAbi == 1u,
           "manifest compatibility fields were parsed incorrectly");
    Expect(manifest.entryWindowsArm64[0] != L'\0' && manifest.entryLinuxArm64Gnu[0] != L'\0' &&
               manifest.entryLinuxArm64Musl[0] != L'\0' && manifest.entryMacosX86_64[0] != L'\0' &&
               manifest.entryMacosArm64[0] != L'\0',
           "cross-platform native entries were not parsed");

    wchar_t nativeName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    Expect(LaiueModManifestSelectNativeEntry(&manifest, nativeName, LAIUE_MOD_NATIVE_NAME_CAPACITY,
                                             &diagnostic) == LAIUE_MOD_STATUS_OK &&
               nativeName[0] != L'\0',
           "current platform artifact was not selected");
}

static void ExpectInvalid(const char *text, uint32_t length, const char *message)
{
    LaiueModManifest manifest;
    LaiueModDiagnostic diagnostic;
    Expect(LaiueModManifestParse(text, length, &manifest, &diagnostic) ==
               LAIUE_MOD_STATUS_MANIFEST_INVALID,
           message);
    Expect(diagnostic.message[0] != '\0', "invalid manifest had no diagnostic");
}

static void TestInvalidManifests(void)
{
    static const char oldFormat[] = "LAIUE MOD 2\nid = example.old\nversion = 1\nengine = 0.6\n"
                                    "[native]\nabi = 1\nentry_windows_x86_64 = old.dll\n";
    static const char traversal[] = "LAIUE MOD 3\nid = example.bad\nversion = 1\nengine = 0.6\n"
                                    "[native]\nabi = 1\nentry_windows_x86_64 = ../bad.dll\n";
    static const char duplicate[] =
        "LAIUE MOD 3\nid = example.duplicate\nversion = 1\nengine = 0.6\n"
        "[native]\nabi = 1\nabi = 1\nentry_windows_x86_64 = mod.dll\n";
    static const char uppercaseId[] = "LAIUE MOD 3\nid = Example.Bad\nversion = 1\nengine = 0.6\n"
                                      "[native]\nabi = 1\nentry_windows_x86_64 = mod.dll\n";
    static const char wrongExtension[] =
        "LAIUE MOD 3\nid = example.bad_extension\nversion = 1\nengine = 0.6\n"
        "[native]\nabi = 1\nentry_windows_x86_64 = mod.so\n";
    static const char wrongMacosExtension[] =
        "LAIUE MOD 3\nid = example.bad_macos_extension\nversion = 1\nengine = 0.6\n"
        "[native]\nabi = 1\nentry_macos_arm64 = mod.so\n";
    static const char missingEngine[] = "LAIUE MOD 3\nid = example.missing\nversion = 1\n"
                                        "[native]\nabi = 1\nentry_windows_x86_64 = mod.dll\n";
    static const char embeddedNul[] = "LAIUE MOD 3\nid = example.nul\0version = 1\nengine = 0.6\n";

    ExpectInvalid(oldFormat, sizeof(oldFormat) - 1u, "legacy game manifest was accepted");
    ExpectInvalid(traversal, sizeof(traversal) - 1u, "path traversal was accepted");
    ExpectInvalid(duplicate, sizeof(duplicate) - 1u, "duplicate known field was accepted");
    ExpectInvalid(uppercaseId, sizeof(uppercaseId) - 1u, "non-canonical id was accepted");
    ExpectInvalid(wrongExtension, sizeof(wrongExtension) - 1u,
                  "wrong platform artifact extension was accepted");
    ExpectInvalid(wrongMacosExtension, sizeof(wrongMacosExtension) - 1u,
                  "wrong macOS artifact extension was accepted");
    ExpectInvalid(missingEngine, sizeof(missingEngine) - 1u, "missing engine version was accepted");
    ExpectInvalid(embeddedNul, sizeof(embeddedNul) - 1u, "embedded NUL byte was accepted");
}

static void TestPackNames(void)
{
    Expect(LaiueModPackNameIsSafe(L"author_weather.lmp"), "safe pack name was rejected");
    Expect(!LaiueModPackNameIsSafe(L"../weather.lmp"), "parent path escaped pack root");
    Expect(!LaiueModPackNameIsSafe(L"folder/weather.lmp"), "nested pack path was accepted");
    Expect(!LaiueModPackNameIsSafe(L"C:\\weather.lmp"), "absolute pack path was accepted");
    Expect(!LaiueModPackNameIsSafe(L"weather?.lmp"),
           "non-portable wildcard pack name was accepted");
    Expect(!LaiueModPackNameIsSafe(L"CON.lmp"), "Windows device name was accepted");
    Expect(!LaiueModPackNameIsSafe(L".lmp"), "empty pack stem was accepted");
}

LAIUE_TEST_ENTRY(ModManifestTestEntry)
{
    TestValidManifest();
    TestInvalidManifests();
    TestPackNames();
    LaiueTestRuntimeWrite("mod_manifest_test passed\n");
    LAIUE_TEST_SUCCESS();
}
