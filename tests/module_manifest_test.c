#include "mod/module_manifest.h"
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
    LAIUE_TEST_SUCCESS();
}
