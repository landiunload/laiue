#include "mod/mod_internal.h"

#include <stddef.h>
#include <string.h>

void LaiueModDiagnosticClear(LaiueModDiagnostic *diagnostic)
{
    if (diagnostic == NULL)
    {
        return;
    }
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = LAIUE_MOD_STATUS_OK;
}

LaiueModStatus LaiueModDiagnosticSet(LaiueModDiagnostic *diagnostic, LaiueModStatus status,
                                     int32_t modResult, const char *message)
{
    if (diagnostic != NULL)
    {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->status = status;
        diagnostic->modResult = modResult;
        if (message != NULL)
        {
            uint32_t index = 0;
            while (message[index] != '\0' && index + 1u < LAIUE_MOD_DIAGNOSTIC_CAPACITY)
            {
                diagnostic->message[index] = message[index];
                ++index;
            }
            diagnostic->message[index] = '\0';
        }
    }
    return status;
}

const char *LaiueModStatusString(LaiueModStatus status)
{
    switch (status)
    {
    case LAIUE_MOD_STATUS_OK:
        return "ok";
    case LAIUE_MOD_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case LAIUE_MOD_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case LAIUE_MOD_STATUS_BUSY:
        return "host busy";
    case LAIUE_MOD_STATUS_CAPACITY_EXCEEDED:
        return "capacity exceeded";
    case LAIUE_MOD_STATUS_UNSAFE_PACK_NAME:
        return "unsafe pack name";
    case LAIUE_MOD_STATUS_PACK_NOT_FOUND:
        return "pack not found";
    case LAIUE_MOD_STATUS_PACK_NOT_DIRECTORY:
        return "pack is not a directory";
    case LAIUE_MOD_STATUS_PACK_IS_SYMBOLIC_LINK:
        return "pack is a symbolic link";
    case LAIUE_MOD_STATUS_MANIFEST_NOT_FOUND:
        return "manifest not found";
    case LAIUE_MOD_STATUS_MANIFEST_TOO_LARGE:
        return "manifest too large";
    case LAIUE_MOD_STATUS_MANIFEST_INVALID:
        return "manifest invalid";
    case LAIUE_MOD_STATUS_ENGINE_INCOMPATIBLE:
        return "engine version incompatible";
    case LAIUE_MOD_STATUS_ABI_INCOMPATIBLE:
        return "mod ABI incompatible";
    case LAIUE_MOD_STATUS_PLATFORM_UNSUPPORTED:
        return "platform artifact unavailable";
    case LAIUE_MOD_STATUS_NATIVE_ARTIFACT_INVALID:
        return "native artifact invalid";
    case LAIUE_MOD_STATUS_NATIVE_ARTIFACT_NOT_FOUND:
        return "native artifact not found";
    case LAIUE_MOD_STATUS_NATIVE_ARTIFACT_IS_SYMBOLIC_LINK:
        return "native artifact is a symbolic link";
    case LAIUE_MOD_STATUS_LIBRARY_LOAD_FAILED:
        return "dynamic library load failed";
    case LAIUE_MOD_STATUS_ENTRY_POINT_MISSING:
        return "mod entry point missing";
    case LAIUE_MOD_STATUS_INITIALIZATION_FAILED:
        return "mod initialization failed";
    case LAIUE_MOD_STATUS_EXPORTS_INVALID:
        return "mod exports invalid";
    case LAIUE_MOD_STATUS_ALREADY_LOADED:
        return "mod already loaded";
    case LAIUE_MOD_STATUS_NOT_LOADED:
        return "mod not loaded";
    case LAIUE_MOD_STATUS_SERVICE_NAME_INVALID:
        return "service name invalid";
    case LAIUE_MOD_STATUS_SERVICE_ALREADY_REGISTERED:
        return "service already registered";
    case LAIUE_MOD_STATUS_SERVICE_NOT_FOUND:
        return "service not found";
    case LAIUE_MOD_STATUS_PACK_NAME_COLLISION:
        return "pack name collision";
    }
    return "unknown mod status";
}

bool LaiueModAsciiEquals(const char *first, const char *second)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }
    uint32_t index = 0;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

bool LaiueModServiceNameIsSafe(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return false;
    }

    uint32_t length = 0;
    bool previousDot = false;
    while (name[length] != '\0')
    {
        unsigned char character = (unsigned char)name[length];
        bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            (character >= '0' && character <= '9');
        bool punctuation = character == '.' || character == '_' || character == '-';
        if ((!alphaNumeric && !punctuation) || length + 1u >= LAIUE_MOD_SERVICE_NAME_CAPACITY ||
            (character == '.' && previousDot))
        {
            return false;
        }
        previousDot = character == '.';
        ++length;
    }

    unsigned char first = (unsigned char)name[0];
    unsigned char last = (unsigned char)name[length - 1u];
    bool firstAlphaNumeric = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                             (first >= '0' && first <= '9');
    bool lastAlphaNumeric = (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') ||
                            (last >= '0' && last <= '9');
    return firstAlphaNumeric && lastAlphaNumeric;
}

bool LaiueModWideCopy(wchar_t *destination, uint32_t capacity, const wchar_t *source)
{
    if (destination == NULL || capacity == 0u || source == NULL)
    {
        return false;
    }
    uint32_t index = 0;
    while (source[index] != L'\0' && index + 1u < capacity)
    {
        destination[index] = source[index];
        ++index;
    }
    if (source[index] != L'\0')
    {
        destination[0] = L'\0';
        return false;
    }
    destination[index] = L'\0';
    return true;
}

static bool AppendPathPart(wchar_t *destination, uint32_t capacity, uint32_t *length,
                           const wchar_t *part)
{
    if (part == NULL || part[0] == L'\0')
    {
        return true;
    }
    if (*length > 0u && destination[*length - 1u] != L'/' && destination[*length - 1u] != L'\\')
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        destination[(*length)++] = L'/';
    }
    uint32_t partIndex = 0;
    while (part[partIndex] != L'\0')
    {
        if (*length + 1u >= capacity)
        {
            return false;
        }
        destination[(*length)++] = part[partIndex++];
    }
    destination[*length] = L'\0';
    return true;
}

bool LaiueModPathJoin(wchar_t *destination, uint32_t capacity, const wchar_t *first,
                      const wchar_t *second, const wchar_t *third)
{
    if (destination == NULL || capacity == 0u || first == NULL || first[0] == L'\0')
    {
        return false;
    }
    uint32_t length = 0;
    destination[0] = L'\0';
    return AppendPathPart(destination, capacity, &length, first) &&
           AppendPathPart(destination, capacity, &length, second) &&
           AppendPathPart(destination, capacity, &length, third);
}
