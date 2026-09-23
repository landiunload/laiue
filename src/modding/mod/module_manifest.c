#include "mod/module_manifest.h"

#include "platform/system.h"

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

static bool SpanEquals(const char *text, uint32_t begin, uint32_t end,
                       const char *literal)
{
    uint32_t index = 0u;
    while (literal[index] != '\0' && begin + index < end &&
           text[begin + index] == literal[index])
        ++index;
    return literal[index] == '\0' && begin + index == end;
}

static bool IsSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r';
}

static void TrimSpan(const char *text, uint32_t *begin, uint32_t *end)
{
    while (*begin < *end && IsSpace(text[*begin]))
        ++*begin;
    while (*end > *begin && IsSpace(text[*end - 1u]))
        --*end;
}

static bool CopySpan(char *destination, uint32_t capacity, const char *text,
                     uint32_t begin, uint32_t end)
{
    if (destination == NULL || capacity == 0u || end <= begin || end - begin >= capacity)
        return false;
    uint32_t index = 0u;
    while (begin < end)
        destination[index++] = text[begin++];
    destination[index] = '\0';
    return true;
}

static bool ParseUnsignedSpan(const char *text, uint32_t begin, uint32_t end,
                              uint32_t *outValue)
{
    TrimSpan(text, &begin, &end);
    if (begin == end || outValue == NULL)
        return false;
    uint32_t value = 0u;
    while (begin < end)
    {
        const unsigned char digit = (unsigned char)text[begin++];
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9')
            return false;
        const uint32_t next = value * 10u + (uint32_t)(digit - (unsigned char)'0');
        if (next < value)
            return false;
        value = next;
    }
    *outValue = value;
    return true;
}

static bool ParseServiceList(const char *text, uint32_t begin, uint32_t end,
                             char names[LAIUE_MODULE_MANIFEST_MAX_SERVICES][LAIUE_MODULE_MAX_NAME],
                             LaiueModuleRequirementV1 *requirements,
                             uint32_t versions[LAIUE_MODULE_MANIFEST_MAX_SERVICES],
                             const char **outProvides,
                             uint32_t *outCount)
{
    if (outCount == NULL)
        return false;
    *outCount = 0u;
    TrimSpan(text, &begin, &end);
    if (begin == end)
        return true;
    while (begin < end)
    {
        if (*outCount >= LAIUE_MODULE_MANIFEST_MAX_SERVICES)
            return false;
        uint32_t itemEnd = begin;
        while (itemEnd < end && text[itemEnd] != ',')
            ++itemEnd;
        uint32_t itemBegin = begin;
        TrimSpan(text, &itemBegin, &itemEnd);
        if (itemBegin == itemEnd)
            return false;
        uint32_t colon = itemBegin;
        while (colon < itemEnd && text[colon] != ':')
            ++colon;
        uint32_t nameEnd = colon;
        TrimSpan(text, &itemBegin, &nameEnd);
        if (!CopySpan(names[*outCount], LAIUE_MODULE_MAX_NAME, text,
                      itemBegin, nameEnd) || !Token(names[*outCount]))
            return false;
        uint32_t version = 1u;
        if (colon < itemEnd)
        {
            uint32_t versionBegin = colon + 1u;
            if (!ParseUnsignedSpan(text, versionBegin, itemEnd, &version) || version == 0u)
                return false;
        }
        if (requirements != NULL)
        {
            requirements[*outCount].name = names[*outCount];
            requirements[*outCount].minimumVersion = version;
        }
        if (versions != NULL)
            versions[*outCount] = version;
        if (outProvides != NULL)
            outProvides[*outCount] = names[*outCount];
        ++*outCount;
        begin = itemEnd;
        if (begin < end)
            ++begin;
        while (begin < end && IsSpace(text[begin]))
            ++begin;
    }
    return true;
}

LaiueModuleStatus LaiueModuleManifestParseTextV1(
    const char *text, uint32_t textSize, LaiueModuleManifestStorageV1 *storage,
    LaiueModuleManifestDiagnostic *diagnostic)
{
    Clear(diagnostic);
    if (text == NULL || storage == NULL || textSize == 0u ||
        textSize > LAIUE_MODULE_MANIFEST_TEXT_CAPACITY)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "manifest text is invalid");
    memset(storage, 0, sizeof(*storage));
    storage->structSize = sizeof(*storage);
    storage->abiVersion = LAIUE_MODULE_MANIFEST_ABI_VERSION_1;
    uint32_t offset = 0u;
    bool header = false;
    bool seenId = false;
    bool seenAuthor = false;
    bool seenVersion = false;
    bool seenPlatform = false;
    bool seenBinary = false;
    bool seenProvides = false;
    bool seenRequires = false;
    bool seenOptional = false;
    uint32_t optional = 0u;
    while (offset < textSize)
    {
        uint32_t lineBegin = offset;
        while (offset < textSize && text[offset] != '\n')
            ++offset;
        uint32_t lineEnd = offset;
        if (offset < textSize)
            ++offset;
        TrimSpan(text, &lineBegin, &lineEnd);
        if (lineBegin == lineEnd || text[lineBegin] == '#')
            continue;
        if (!header)
        {
            if (!SpanEquals(text, lineBegin, lineEnd, "LAIUE MODULE 1"))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest header is invalid");
            header = true;
            continue;
        }
        uint32_t equals = lineBegin;
        while (equals < lineEnd && text[equals] != '=')
            ++equals;
        if (equals == lineEnd)
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest assignment is invalid");
        uint32_t keyBegin = lineBegin;
        uint32_t keyEnd = equals;
        uint32_t valueBegin = equals + 1u;
        uint32_t valueEnd = lineEnd;
        TrimSpan(text, &keyBegin, &keyEnd);
        TrimSpan(text, &valueBegin, &valueEnd);
        if (SpanEquals(text, keyBegin, keyEnd, "id"))
        {
            if (seenId || !CopySpan(storage->id, sizeof(storage->id), text,
                                    valueBegin, valueEnd))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest id is duplicated or invalid");
            seenId = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "author"))
        {
            if (seenAuthor || !CopySpan(storage->author, sizeof(storage->author), text,
                                        valueBegin, valueEnd))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest author is duplicated or invalid");
            seenAuthor = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "version"))
        {
            if (seenVersion || !CopySpan(storage->version, sizeof(storage->version), text,
                                         valueBegin, valueEnd))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest version is duplicated or invalid");
            seenVersion = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "platform"))
        {
            if (seenPlatform || !CopySpan(storage->platform, sizeof(storage->platform), text,
                                          valueBegin, valueEnd))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest platform is duplicated or invalid");
            seenPlatform = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "binary"))
        {
            if (seenBinary || !CopySpan(storage->binary, sizeof(storage->binary), text,
                                        valueBegin, valueEnd))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest binary is duplicated or invalid");
            seenBinary = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "provides"))
        {
            if (seenProvides || !ParseServiceList(
                                    text, valueBegin, valueEnd, storage->providedNames, NULL,
                                    storage->providedVersions, &storage->provides[0],
                                    &storage->providesCount))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest providers are duplicated or invalid");
            seenProvides = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "requires"))
        {
            if (seenRequires || !ParseServiceList(
                                    text, valueBegin, valueEnd, storage->requiredNames,
                                    storage->requiresServices, NULL, NULL,
                                    &storage->requiresCount))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest requirements are duplicated or invalid");
            seenRequires = true;
        }
        else if (SpanEquals(text, keyBegin, keyEnd, "optional"))
        {
            if (seenOptional)
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest optional flag is duplicated");
            if (SpanEquals(text, valueBegin, valueEnd, "true"))
                optional = LAIUE_MODULE_MANIFEST_OPTIONAL;
            else if (!SpanEquals(text, valueBegin, valueEnd, "false"))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "manifest optional flag is invalid");
            seenOptional = true;
        }
        else
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "manifest key is unknown");
    }
    if (!header || !seenId || !seenAuthor || !seenVersion || !seenPlatform || !seenBinary)
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "manifest is missing a required field");
    storage->manifest = (LaiueModuleManifestV1){
        .structSize = sizeof(storage->manifest),
        .abiVersion = LAIUE_MODULE_MANIFEST_ABI_VERSION_1,
        .id = storage->id,
        .author = storage->author,
        .version = storage->version,
        .platform = storage->platform,
        .binary = storage->binary,
        .provides = storage->provides,
        .providesCount = storage->providesCount,
        .requiresServices = storage->requiresServices,
        .requiresCount = storage->requiresCount,
        .flags = optional,
    };
    return LaiueModuleManifestValidate(&storage->manifest, diagnostic);
}

LaiueModuleStatus LaiueModuleManifestParseFileV1(
    const wchar_t *path, uint64_t maximumBytes,
    LaiueModuleManifestStorageV1 *storage, LaiueModuleManifestDiagnostic *diagnostic)
{
    Clear(diagnostic);
    if (path == NULL || path[0] == L'\0' || storage == NULL || maximumBytes == 0u ||
        maximumBytes > LAIUE_MODULE_MANIFEST_TEXT_CAPACITY)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "manifest file arguments are invalid");
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!PlatformReadEntireFile(path, maximumBytes, &bytes, &size))
        return Fail(diagnostic, LAIUE_MODULE_LOAD_FAILED, "manifest file could not be read");
    LaiueModuleStatus status = size > UINT32_MAX
                                   ? LAIUE_MODULE_DESCRIPTOR_INVALID
                                   : LaiueModuleManifestParseTextV1(
                                         (const char *)bytes, (uint32_t)size,
                                         storage, diagnostic);
    PlatformFree(bytes);
    if (status == LAIUE_MODULE_DESCRIPTOR_INVALID && diagnostic != NULL &&
        diagnostic->message[0] == '\0')
        return Fail(diagnostic, status, "manifest file is invalid");
    return status;
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
