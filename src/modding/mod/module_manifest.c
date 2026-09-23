#include "mod/module_manifest.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

static void Clear(LaiueModuleManifestDiagnostic *diagnostic)
{
    if (diagnostic != NULL)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = LAIUE_MODULE_OK;
    }
}

static LaiueModuleStatus Fail(LaiueModuleManifestDiagnostic *diagnostic,
                              LaiueModuleStatus status, const char *message)
{
    if (diagnostic != NULL)
    {
        diagnostic->status = status;
        uint32_t index = 0u;
        if (message != NULL)
            while (message[index] != '\0' && index + 1u < LAIUE_MODULE_MANIFEST_MAX_TEXT)
            {
                diagnostic->message[index] = message[index];
                ++index;
            }
        diagnostic->message[index] = '\0';
    }
    return status;
}

static bool Equal(const char *first, const char *second)
{
    if (first == NULL || second == NULL)
        return false;
    uint32_t index = 0u;
    while (first[index] != '\0' && first[index] == second[index])
        ++index;
    return first[index] == second[index];
}

static bool Token(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return false;
    uint32_t length = 0u;
    bool previousDot = false;
    while (value[length] != '\0')
    {
        unsigned char character = (unsigned char)value[length];
        bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            (character >= '0' && character <= '9');
        if ((!alphaNumeric && character != '.' && character != '_' && character != '-') ||
            length + 1u >= LAIUE_MODULE_MAX_NAME ||
            (character == '.' && previousDot))
            return false;
        previousDot = character == '.';
        ++length;
    }
    unsigned char first = (unsigned char)value[0];
    unsigned char last = (unsigned char)value[length - 1u];
    bool firstAlphaNumeric = (first >= 'a' && first <= 'z') ||
                             (first >= 'A' && first <= 'Z') ||
                             (first >= '0' && first <= '9');
    bool lastAlphaNumeric = (last >= 'a' && last <= 'z') ||
                            (last >= 'A' && last <= 'Z') ||
                            (last >= '0' && last <= '9');
    return firstAlphaNumeric && lastAlphaNumeric;
}

static bool Leaf(const char *value)
{
    if (!Token(value) || value[0] == '.' || value[0] == '-')
        return false;
    for (uint32_t index = 0u; value[index] != '\0'; ++index)
        if (value[index] == '.' && value[index + 1u] == '.')
            return false;
    return true;
}

LaiueModuleStatus LaiueModuleManifestValidate(const LaiueModuleManifestV1 *manifest,
                                              LaiueModuleManifestDiagnostic *diagnostic)
{
    Clear(diagnostic);
    if (manifest == NULL)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "manifest is missing");
    if (manifest->structSize < offsetof(LaiueModuleManifestV1, reserved))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "manifest ABI is invalid");
    if (manifest->abiVersion != LAIUE_MODULE_MANIFEST_ABI_VERSION_1)
        return Fail(diagnostic, LAIUE_MODULE_ABI_MISMATCH, "manifest ABI is unsupported");
    if (!Token(manifest->id) || !Token(manifest->version) || !Token(manifest->platform) ||
        !Leaf(manifest->binary))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "manifest identity or artifact is invalid");
    if (manifest->author == NULL || manifest->author[0] == '\0')
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "manifest author is missing");
    if ((manifest->providesCount != 0u && manifest->provides == NULL) ||
        (manifest->requiresCount != 0u && manifest->requiresServices == NULL) ||
        manifest->providesCount > LAIUE_MODULE_MANIFEST_MAX_SERVICES ||
        manifest->requiresCount > LAIUE_MODULE_MANIFEST_MAX_SERVICES)
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "manifest service arrays are invalid");
    for (uint32_t index = 0u; index < manifest->providesCount; ++index)
    {
        if (!Token(manifest->provides[index]))
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest service name is invalid");
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (Equal(manifest->provides[prior], manifest->provides[index]))
                return Fail(diagnostic, LAIUE_MODULE_DUPLICATE_SERVICE,
                            "manifest service is duplicated");
    }
    for (uint32_t index = 0u; index < manifest->requiresCount; ++index)
        if (manifest->requiresServices[index].minimumVersion == 0u ||
            !Token(manifest->requiresServices[index].name))
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest requirement is invalid");
    return LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleManifestValidateApi(const LaiueModuleManifestV1 *manifest,
                                                 const LaiueModuleApiV1 *api,
                                                 LaiueModuleManifestDiagnostic *diagnostic)
{
    LaiueModuleStatus status = LaiueModuleManifestValidate(manifest, diagnostic);
    if (status != LAIUE_MODULE_OK)
        return status;
    if (api == NULL || api->structSize < offsetof(LaiueModuleApiV1, reserved) ||
        api->abiVersion != LAIUE_MODULE_ABI_VERSION_1)
        return Fail(diagnostic, LAIUE_MODULE_ABI_MISMATCH, "module API ABI is invalid");
    if (api->descriptor.structSize < offsetof(LaiueModuleDescriptorV1, reserved))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "module descriptor is truncated");
    if (api->descriptor.abiVersion != LAIUE_MODULE_ABI_VERSION_1)
        return Fail(diagnostic, LAIUE_MODULE_ABI_MISMATCH,
                    "module descriptor ABI is invalid");
    if (!Equal(manifest->id, api->descriptor.id) ||
        !Equal(manifest->version, api->descriptor.version))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "manifest and module identity differ");
    if ((api->descriptor.providesCount != 0u && api->descriptor.providesServices == NULL) ||
        (api->descriptor.requiresCount != 0u && api->descriptor.requiresServices == NULL))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "module service arrays are invalid");
    if (api->descriptor.providesCount != manifest->providesCount ||
        api->descriptor.requiresCount != manifest->requiresCount)
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "manifest and module services differ");
    for (uint32_t index = 0u; index < manifest->providesCount; ++index)
        if (!Equal(manifest->provides[index], api->descriptor.providesServices[index]))
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest provider list differs");
    for (uint32_t index = 0u; index < manifest->requiresCount; ++index)
    {
        const LaiueModuleRequirementV1 *required = &manifest->requiresServices[index];
        const LaiueModuleRequirementV1 *declared = &api->descriptor.requiresServices[index];
        if (!Equal(required->name, declared->name) || required->minimumVersion != declared->minimumVersion)
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest requirement list differs");
    }
    return LAIUE_MODULE_OK;
}
