#include "mod/module_profile.h"

#include "platform/system.h"

#include <stdbool.h>
#include <string.h>

static uint32_t Length(const char *text)
{
    uint32_t length = 0u;
    if (text != NULL)
        while (text[length] != '\0')
            ++length;
    return length;
}

static void Clear(LaiueModuleDiagnostic *diagnostic)
{
    if (diagnostic != NULL)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = LAIUE_MODULE_OK;
    }
}

static LaiueModuleStatus Fail(LaiueModuleDiagnostic *diagnostic,
                              LaiueModuleStatus status, const char *message)
{
    if (diagnostic != NULL)
    {
        diagnostic->status = status;
        uint32_t index = 0u;
        if (message != NULL)
            while (message[index] != '\0' && index + 1u < LAIUE_MODULE_DIAGNOSTIC_CAPACITY)
            {
                diagnostic->message[index] = message[index];
                ++index;
            }
        diagnostic->message[index] = '\0';
    }
    return status;
}

static bool Space(char value)
{
    return value == ' ' || value == '\t' || value == '\r';
}

static void Trim(const char *text, uint32_t *begin, uint32_t *end)
{
    while (*begin < *end && Space(text[*begin]))
        ++*begin;
    while (*end > *begin && Space(text[*end - 1u]))
        --*end;
}

static bool Equal(const char *text, uint32_t begin, uint32_t end, const char *literal)
{
    uint32_t index = 0u;
    while (literal[index] != '\0' && begin + index < end &&
           text[begin + index] == literal[index])
        ++index;
    return literal[index] == '\0' && begin + index == end;
}

static bool Copy(char *destination, uint32_t capacity, const char *text,
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

static bool Name(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return false;
    uint32_t length = 0u;
    while (value[length] != '\0')
    {
        const unsigned char character = (unsigned char)value[length++];
        const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if ((!alphaNumeric && character != '.' && character != '_' && character != '-') ||
            length >= LAIUE_MODULE_MAX_NAME)
            return false;
    }
    const unsigned char first = (unsigned char)value[0];
    const unsigned char last = (unsigned char)value[length - 1u];
    const bool firstOk = (first >= 'a' && first <= 'z') ||
                         (first >= 'A' && first <= 'Z') ||
                         (first >= '0' && first <= '9');
    const bool lastOk = (last >= 'a' && last <= 'z') ||
                        (last >= 'A' && last <= 'Z') ||
                        (last >= '0' && last <= '9');
    return firstOk && lastOk;
}

static LaiueModuleStatus AppendModule(LaiueModuleProfileStorageV1 *storage,
                                      const char *text, uint32_t begin, uint32_t end,
                                      LaiueModuleDiagnostic *diagnostic)
{
    if (storage->binaryCount >= LAIUE_MODULE_HOST_MAX_MODULES)
        return Fail(diagnostic, LAIUE_MODULE_CAPACITY, "profile module capacity exceeded");
    Trim(text, &begin, &end);
    bool optional = false;
    const uint32_t suffixLength = 9u;
    if (end > begin + suffixLength &&
        text[end - suffixLength] == ' ' && Equal(text, end - suffixLength + 1u, end,
                                                   "optional"))
    {
        optional = true;
        end -= suffixLength;
        Trim(text, &begin, &end);
    }
    char pathUtf8[LAIUE_MODULE_PROFILE_PATH_CAPACITY];
    if (!Copy(pathUtf8, sizeof(pathUtf8), text, begin, end) ||
        !PlatformUtf8ToWide(pathUtf8, Length(pathUtf8),
                            storage->paths[storage->binaryCount],
                            LAIUE_MODULE_PROFILE_PATH_CAPACITY, NULL))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "profile module path is not UTF-8");
    storage->binaries[storage->binaryCount] = (LaiueModuleBinaryV1){
        .path = storage->paths[storage->binaryCount],
        .flags = optional ? LAIUE_MODULE_BINARY_OPTIONAL : 0u,
        .staticApi = NULL,
    };
    ++storage->binaryCount;
    return LAIUE_MODULE_OK;
}

static LaiueModuleStatus AppendProvider(LaiueModuleProfileStorageV1 *storage,
                                        const char *text, uint32_t begin, uint32_t end,
                                        LaiueModuleDiagnostic *diagnostic)
{
    if (storage->selectionCount >= LAIUE_MODULE_HOST_MAX_SERVICES)
        return Fail(diagnostic, LAIUE_MODULE_CAPACITY, "profile provider capacity exceeded");
    Trim(text, &begin, &end);
    uint32_t colon = begin;
    while (colon < end && text[colon] != ':')
        ++colon;
    if (colon == begin || colon + 1u >= end)
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "profile provider is invalid");
    uint32_t moduleBegin = colon + 1u;
    uint32_t serviceEnd = colon;
    Trim(text, &begin, &serviceEnd);
    Trim(text, &moduleBegin, &end);
    const uint32_t index = storage->selectionCount;
    if (!Copy(storage->providerServices[index], LAIUE_MODULE_MAX_NAME,
              text, begin, serviceEnd) || !Name(storage->providerServices[index]) ||
        !Copy(storage->providerModules[index], LAIUE_MODULE_MAX_NAME,
              text, moduleBegin, end) || !Name(storage->providerModules[index]))
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "profile provider name is invalid");
    storage->selections[index] = (LaiueModuleProviderSelectionV1){
        .structSize = sizeof(LaiueModuleProviderSelectionV1),
        .serviceName = storage->providerServices[index],
        .moduleId = storage->providerModules[index],
    };
    ++storage->selectionCount;
    return LAIUE_MODULE_OK;
}

LaiueModuleStatus LaiueModuleProfileParseTextV1(
    const char *text, uint32_t textSize, LaiueModuleProfileStorageV1 *storage,
    LaiueModuleDiagnostic *diagnostic)
{
    Clear(diagnostic);
    if (text == NULL || textSize == 0u || textSize > LAIUE_MODULE_PROFILE_TEXT_CAPACITY ||
        storage == NULL)
        return Fail(diagnostic, LAIUE_MODULE_INVALID_ARGUMENT, "profile text is invalid");
    memset(storage, 0, sizeof(*storage));
    storage->structSize = sizeof(*storage);
    storage->abiVersion = LAIUE_MODULE_ABI_VERSION_1;
    bool header = false;
    bool seenFlags = false;
    uint32_t offset = 0u;
    while (offset < textSize)
    {
        uint32_t lineBegin = offset;
        while (offset < textSize && text[offset] != '\n')
            ++offset;
        uint32_t lineEnd = offset;
        if (offset < textSize)
            ++offset;
        Trim(text, &lineBegin, &lineEnd);
        if (lineBegin == lineEnd || text[lineBegin] == '#')
            continue;
        if (!header)
        {
            if (!Equal(text, lineBegin, lineEnd, "LAIUE PROFILE 1"))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "profile header is invalid");
            header = true;
            continue;
        }
        uint32_t equals = lineBegin;
        while (equals < lineEnd && text[equals] != '=')
            ++equals;
        if (equals == lineEnd)
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                        "profile assignment is invalid");
        uint32_t keyBegin = lineBegin;
        uint32_t keyEnd = equals;
        uint32_t valueBegin = equals + 1u;
        uint32_t valueEnd = lineEnd;
        Trim(text, &keyBegin, &keyEnd);
        Trim(text, &valueBegin, &valueEnd);
        LaiueModuleStatus status = LAIUE_MODULE_OK;
        if (Equal(text, keyBegin, keyEnd, "flags"))
        {
            if (seenFlags || (!Equal(text, valueBegin, valueEnd, "partial") &&
                              !Equal(text, valueBegin, valueEnd, "strict")))
                return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                            "profile flags are invalid");
            if (Equal(text, valueBegin, valueEnd, "partial"))
                storage->profile.flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL;
            seenFlags = true;
        }
        else if (Equal(text, keyBegin, keyEnd, "module"))
            status = AppendModule(storage, text, valueBegin, valueEnd, diagnostic);
        else if (Equal(text, keyBegin, keyEnd, "provider"))
            status = AppendProvider(storage, text, valueBegin, valueEnd, diagnostic);
        else
            return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID, "profile key is unknown");
        if (status != LAIUE_MODULE_OK)
            return status;
    }
    if (!header || storage->binaryCount == 0u)
        return Fail(diagnostic, LAIUE_MODULE_DESCRIPTOR_INVALID,
                    "profile has no selected modules");
    storage->profile.structSize = sizeof(storage->profile);
    storage->profile.binaries = storage->binaries;
    storage->profile.binaryCount = storage->binaryCount;
    storage->profile.providerSelections = storage->selections;
    storage->profile.providerSelectionCount = storage->selectionCount;
    return LAIUE_MODULE_OK;
}
