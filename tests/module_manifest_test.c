#include "mod/module_manifest.h"
#include "mod/module_profile.h"
#include "test_runtime.h"

#include <stddef.h>
#include <stdbool.h>

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

LAIUE_TEST_ENTRY(ModuleManifestTestEntryPoint)
{
    static const char *const provides[] = {"example.graphics"};
    static const LaiueModuleRequirementV1 requires[] = {{"example.window", 2u}};
    static const LaiueModuleManifestV1 manifest = {
        .structSize = sizeof(LaiueModuleManifestV1),
        .abiVersion = LAIUE_MODULE_MANIFEST_ABI_VERSION_1,
        .id = "example.renderer",
        .author = "example.author",
        .version = "1.0.0",
        .platform = "windows-x86_64",
        .binary = "example_renderer.windows-x86_64.dll",
        .provides = provides,
        .providesCount = 1u,
        .requiresServices = requires,
        .requiresCount = 1u,
    };
    LaiueModuleManifestDiagnostic diagnostic;
    Expect(LaiueModuleManifestValidate(&manifest, &diagnostic) == LAIUE_MODULE_OK,
           "valid native manifest is accepted");

    LaiueModuleManifestV1 unsafe = manifest;
    unsafe.binary = "../renderer.dll";
    Expect(LaiueModuleManifestValidate(&unsafe, &diagnostic) == LAIUE_MODULE_DESCRIPTOR_INVALID,
           "manifest traversal is rejected");

    static const LaiueModuleApiV1 api = {
        .structSize = sizeof(LaiueModuleApiV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .descriptor = {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "example.renderer",
            .version = "1.0.0",
            .requiresServices = requires,
            .requiresCount = 1u,
            .providesServices = provides,
            .providesCount = 1u,
        },
    };
    Expect(LaiueModuleManifestValidateApi(&manifest, &api, &diagnostic) == LAIUE_MODULE_OK,
           "manifest matches module descriptor");
    unsafe = manifest;
    unsafe.id = "example.other";
    Expect(LaiueModuleManifestValidateApi(&unsafe, &api, &diagnostic) ==
               LAIUE_MODULE_DESCRIPTOR_INVALID,
           "manifest identity mismatch is rejected");

    static const char text[] =
        "# comments are ignored\n"
        "LAIUE MODULE 1\n"
        "id = example.renderer\n"
        "author = example.author\n"
        "version = 1.0.0\n"
        "platform = windows-x86_64\n"
        "binary = example_renderer.dll\n"
        "provides = example.graphics:1\n"
        "requires = example.window:2\n"
        "optional = false\n";
    static LaiueModuleManifestStorageV1 storage;
    Expect(LaiueModuleManifestParseTextV1(text, (uint32_t)(sizeof(text) - 1u),
                                           &storage, &diagnostic) == LAIUE_MODULE_OK,
           "text module manifest parses without loading an artifact");
    Expect(storage.manifest.providesCount == 1u && storage.manifest.requiresCount == 1u &&
               storage.manifest.requiresServices[0].minimumVersion == 2u,
           "text manifest preserves service versions");
    static const char malformed[] =
        "LAIUE MODULE 1\n"
        "id = example.renderer\n"
        "author = example.author\n"
        "version = 1.0.0\n"
        "platform = windows-x86_64\n"
        "binary = ../renderer.dll\n";
    Expect(LaiueModuleManifestParseTextV1(malformed, (uint32_t)(sizeof(malformed) - 1u),
                                          &storage, &diagnostic) ==
               LAIUE_MODULE_DESCRIPTOR_INVALID,
           "text manifest rejects traversal before artifact loading");

    static const char profileText[] =
        "LAIUE PROFILE 1\n"
        "flags = partial\n"
        "module = laiue_character.dll\n"
        "module = laiue_voxel.dll optional\n"
        "provider = laiue.graphics.device:laiue.graphics\n";
    static LaiueModuleProfileStorageV1 profileStorage;
    LaiueModuleDiagnostic profileDiagnostic;
    Expect(LaiueModuleProfileParseTextV1(
               profileText, (uint32_t)(sizeof(profileText) - 1u),
               &profileStorage, &profileDiagnostic) == LAIUE_MODULE_OK,
           "application profile parses without opening modules");
    Expect(profileStorage.profile.flags == LAIUE_MODULE_PROFILE_ALLOW_PARTIAL &&
               profileStorage.profile.binaryCount == 2u &&
               (profileStorage.profile.binaries[1].flags & LAIUE_MODULE_BINARY_OPTIONAL) != 0u &&
               profileStorage.profile.providerSelectionCount == 1u,
           "profile parser preserves optional modules and provider choices");
    LAIUE_TEST_SUCCESS();
}
