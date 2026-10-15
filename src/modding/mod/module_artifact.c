#include "mod/module_host.h"

#include "mod/mod_internal.h"
#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The build passes the toolchain's shared-library prefix and suffix, so the
 * names below are the ones CMake gave the module artifacts. */
#if !defined(LAIUE_MODULE_ARTIFACT_PREFIX) || !defined(LAIUE_MODULE_ARTIFACT_SUFFIX)
#error "The build must define LAIUE_MODULE_ARTIFACT_PREFIX and LAIUE_MODULE_ARTIFACT_SUFFIX"
#endif

static LaiueModuleStatus Fail(LaiueModuleDiagnostic *diagnostic, LaiueModuleStatus status,
                              const char *message)
{
    if (diagnostic != NULL)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = status;
        uint32_t index = 0u;
        while (message[index] != '\0' && index + 1u < LAIUE_MODULE_DIAGNOSTIC_CAPACITY)
        {
            diagnostic->message[index] = message[index];
            ++index;
        }
        diagnostic->message[index] = '\0';
    }
    return status;
}

static bool ArtifactNameIsValid(const char *name)
{
    if (name == NULL)
        return false;
    uint32_t length = 0u;
    while (name[length] != '\0')
    {
        const char character = name[length];
        const bool alphaNumeric =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if ((!alphaNumeric && (character != '_' || length == 0u)) ||
            length + 1u >= LAIUE_MODULE_MAX_NAME)
            return false;
        ++length;
    }
    return length != 0u;
}

/* Appends ASCII text widened to wchar_t; the prefix, suffix and validated
 * names are ASCII, so widening is the exact conversion. */
static bool AppendAscii(wchar_t *output, uint32_t capacity, uint32_t *length, const char *text)
{
    for (uint32_t index = 0u; text[index] != '\0'; ++index)
    {
        if (*length + 1u >= capacity)
            return false;
        output[(*length)++] = (wchar_t)(unsigned char)text[index];
    }
    output[*length] = L'\0';
    return true;
}

LaiueModuleStatus LaiueModuleArtifactFileNameV1(const char *name, wchar_t *output,
                                                uint32_t capacity)
{
    if (output == NULL || capacity == 0u)
        return LAIUE_MODULE_INVALID_ARGUMENT;
    output[0] = L'\0';
    if (!ArtifactNameIsValid(name))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    uint32_t length = 0u;
    if (!AppendAscii(output, capacity, &length, LAIUE_MODULE_ARTIFACT_PREFIX "laiue_") ||
        !AppendAscii(output, capacity, &length, name) ||
        !AppendAscii(output, capacity, &length, LAIUE_MODULE_ARTIFACT_SUFFIX))
    {
        output[0] = L'\0';
        return LAIUE_MODULE_CAPACITY;
    }
    return LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleHostLoadNamedProfileV1(LaiueModuleHost *host,
                                                    const LaiueModuleNamedProfileV1 *profile,
                                                    LaiueModuleLoadReportV1 *report,
                                                    LaiueModuleDiagnostic *diagnostic)
{
    if (host == NULL || profile == NULL ||
        profile->structSize < offsetof(LaiueModuleNamedProfileV1, reserved) ||
        profile->names == NULL || profile->nameCount == 0u ||
        profile->nameCount > LAIUE_MODULE_HOST_MAX_MODULES)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT,
                    "module names and count are required");
    for (uint32_t index = 0u; index < profile->nameCount; ++index)
    {
        if (!ArtifactNameIsValid(profile->names[index]))
            return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module name is invalid");
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (LaiueModAsciiEquals(profile->names[prior], profile->names[index]))
                return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module name is duplicated");
    }

    wchar_t executableDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    const wchar_t *directory = profile->directory;
    if (directory == NULL)
    {
        if (!PlatformExecutableDirectory(executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY))
            return Fail(diagnostic, LAIUE_MODULE_LOAD_FAILED,
                        "executable directory is unavailable");
        directory = executableDirectory;
    }
    else if (directory[0] == L'\0')
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "module directory is empty");

    /* Up to 64 full paths do not belong on the stack of an arbitrary caller. */
    wchar_t *paths = (wchar_t *)PlatformAllocate(
        (size_t)profile->nameCount * LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    LaiueModuleBinaryV1 *binaries = (LaiueModuleBinaryV1 *)PlatformAllocate(
        (size_t)profile->nameCount * sizeof(LaiueModuleBinaryV1), true);
    if (paths == NULL || binaries == NULL)
    {
        PlatformFree(paths);
        PlatformFree(binaries);
        return Fail(diagnostic, LAIUE_MODULE_OUT_OF_MEMORY, "module paths are unavailable");
    }

    const bool partial = (profile->flags & LAIUE_MODULE_PROFILE_ALLOW_PARTIAL) != 0u;
    LaiueModuleStatus status = LAIUE_MODULE_OK;
    for (uint32_t index = 0u; index < profile->nameCount && status == LAIUE_MODULE_OK; ++index)
    {
        wchar_t fileName[LAIUE_MODULE_ARTIFACT_NAME_CAPACITY];
        wchar_t *path = paths + (size_t)index * LAIUE_PLATFORM_PATH_CAPACITY;
        if (LaiueModuleArtifactFileNameV1(profile->names[index], fileName,
                                          LAIUE_MODULE_ARTIFACT_NAME_CAPACITY) != LAIUE_MODULE_OK ||
            !LaiueModPathJoin(path, LAIUE_PLATFORM_PATH_CAPACITY, directory, fileName, NULL))
        {
            status = Fail(diagnostic, LAIUE_MODULE_CAPACITY, "module path is too long");
            break;
        }
        binaries[index] = (LaiueModuleBinaryV1){
            .path = path,
            .flags = partial ? LAIUE_MODULE_BINARY_OPTIONAL : 0u,
            .staticApi = NULL,
        };
    }
    if (status == LAIUE_MODULE_OK)
    {
        const LaiueModuleProfileV1 resolved = {
            .structSize = sizeof(resolved),
            .flags = profile->flags,
            .binaries = binaries,
            .binaryCount = profile->nameCount,
            .providerSelections = profile->providerSelections,
            .providerSelectionCount = profile->providerSelectionCount,
        };
        status = LaiueModuleHostLoadProfileV1(host, &resolved, report, diagnostic);
    }
    /* The host copies what it keeps; the paths are no longer referenced. */
    PlatformFree(binaries);
    PlatformFree(paths);
    return status;
}
