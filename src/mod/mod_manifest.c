#include "mod/mod_manifest.h"

#include "mod/mod_internal.h"
#include "platform/system.h"

#include <limits.h>
#include <string.h>

typedef struct ByteSlice
{
    const uint8_t *data;
    uint32_t length;
} ByteSlice;

typedef struct ManifestParseState
{
    LaiueModManifest *manifest;
    bool idSeen;
    bool nameSeen;
    bool versionSeen;
    bool engineSeen;
    bool abiSeen;
    bool windowsSeen;
    bool windowsArmSeen;
    bool linuxGnuSeen;
    bool linuxMuslSeen;
    bool linuxArmGnuSeen;
    bool linuxArmMuslSeen;
    bool macosX86Seen;
    bool macosArmSeen;
} ManifestParseState;

typedef struct PackInspectionScratch
{
    wchar_t packPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifestPath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t nativePath[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t nativeName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
} PackInspectionScratch;

static ByteSlice SliceTrim(ByteSlice slice)
{
    while (slice.length > 0u &&
           (slice.data[0] == ' ' || slice.data[0] == '\t' || slice.data[0] == '\r'))
    {
        ++slice.data;
        --slice.length;
    }
    while (slice.length > 0u &&
           (slice.data[slice.length - 1u] == ' ' || slice.data[slice.length - 1u] == '\t' ||
            slice.data[slice.length - 1u] == '\r'))
    {
        --slice.length;
    }
    return slice;
}

static bool SliceEqualsAsciiIgnoreCase(ByteSlice slice, const char *text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    if (slice.length != length)
    {
        return false;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        uint8_t first = slice.data[index];
        uint8_t second = (uint8_t)text[index];
        if (first >= 'A' && first <= 'Z')
        {
            first = (uint8_t)(first + ('a' - 'A'));
        }
        if (second >= 'A' && second <= 'Z')
        {
            second = (uint8_t)(second + ('a' - 'A'));
        }
        if (first != second)
        {
            return false;
        }
    }
    return true;
}

static bool SliceParseUint32(ByteSlice slice, uint32_t *outValue)
{
    if (slice.length == 0u || outValue == NULL)
    {
        return false;
    }
    uint32_t value = 0;
    for (uint32_t index = 0; index < slice.length; ++index)
    {
        uint8_t digit = slice.data[index];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        uint32_t numeric = (uint32_t)(digit - '0');
        if (value > (UINT32_MAX - numeric) / 10u)
        {
            return false;
        }
        value = value * 10u + numeric;
    }
    *outValue = value;
    return true;
}

static bool SliceCopyUtf8(ByteSlice slice, char *destination, uint32_t capacity)
{
    if (slice.length == 0u || slice.length >= capacity)
    {
        return false;
    }
    wchar_t validation[LAIUE_MOD_DISPLAY_NAME_CAPACITY];
    uint32_t wideLength = 0;
    if (!PlatformUtf8ToWide((const char *)slice.data, slice.length, validation,
                            LAIUE_MOD_DISPLAY_NAME_CAPACITY, &wideLength) ||
        wideLength == 0u)
    {
        return false;
    }
    for (uint32_t index = 0; index < slice.length; ++index)
    {
        if (slice.data[index] == 0u || slice.data[index] == 0x7fu || slice.data[index] < 0x20u)
        {
            return false;
        }
    }
    memcpy(destination, slice.data, slice.length);
    destination[slice.length] = '\0';
    return true;
}

static bool SliceCopyIdentifier(ByteSlice slice, char *destination, uint32_t capacity)
{
    if (slice.length == 0u || slice.length >= capacity)
    {
        return false;
    }
    bool previousDot = false;
    for (uint32_t index = 0; index < slice.length; ++index)
    {
        uint8_t character = slice.data[index];
        bool alphaNumeric =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        bool punctuation = character == '.' || character == '_' || character == '-';
        if ((!alphaNumeric && !punctuation) || (character == '.' && previousDot))
        {
            return false;
        }
        destination[index] = (char)character;
        previousDot = character == '.';
    }
    uint8_t first = slice.data[0];
    uint8_t last = slice.data[slice.length - 1u];
    bool firstAlphaNumeric = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
    bool lastAlphaNumeric = (last >= 'a' && last <= 'z') || (last >= '0' && last <= '9');
    if (!firstAlphaNumeric || !lastAlphaNumeric)
    {
        return false;
    }
    destination[slice.length] = '\0';
    return true;
}

static bool SliceCopyVersion(ByteSlice slice, char *destination, uint32_t capacity)
{
    if (slice.length == 0u || slice.length >= capacity)
    {
        return false;
    }
    for (uint32_t index = 0; index < slice.length; ++index)
    {
        uint8_t character = slice.data[index];
        bool allowed = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' ||
                       character == '-' || character == '+' || character == '_';
        if (!allowed)
        {
            return false;
        }
        destination[index] = (char)character;
    }
    destination[slice.length] = '\0';
    return true;
}

static bool WideCharacterEqualIgnoreCase(wchar_t first, wchar_t second)
{
    if (first >= L'A' && first <= L'Z')
    {
        first += L'a' - L'A';
    }
    if (second >= L'A' && second <= L'Z')
    {
        second += L'a' - L'A';
    }
    return first == second;
}

static bool WideEndsWithIgnoreCase(const wchar_t *text, const wchar_t *suffix)
{
    uint32_t textLength = 0;
    uint32_t suffixLength = 0;
    while (text[textLength] != L'\0')
    {
        ++textLength;
    }
    while (suffix[suffixLength] != L'\0')
    {
        ++suffixLength;
    }
    if (textLength < suffixLength)
    {
        return false;
    }
    uint32_t offset = textLength - suffixLength;
    for (uint32_t index = 0; index < suffixLength; ++index)
    {
        if (!WideCharacterEqualIgnoreCase(text[offset + index], suffix[index]))
        {
            return false;
        }
    }
    return true;
}

static bool WideEqualsAsciiIgnoreCase(const wchar_t *text, const char *ascii)
{
    uint32_t index = 0;
    while (text[index] != L'\0' && ascii[index] != '\0')
    {
        wchar_t character = text[index];
        char expected = ascii[index];
        if (character >= L'A' && character <= L'Z')
        {
            character += L'a' - L'A';
        }
        if (expected >= 'A' && expected <= 'Z')
        {
            expected = (char)(expected + ('a' - 'A'));
        }
        if (character != (wchar_t)expected)
        {
            return false;
        }
        ++index;
    }
    return text[index] == L'\0' && ascii[index] == '\0';
}

static bool WideLeafNameIsSafe(const wchar_t *name, uint32_t capacity)
{
    if (name == NULL || name[0] == L'\0')
    {
        return false;
    }
    uint32_t length = 0;
    while (name[length] != L'\0')
    {
        wchar_t character = name[length];
        if (length + 1u >= capacity || character < L' ' || character == 0x7f || character == L'/' ||
            character == L'\\' || character == L':' || character == L'*' || character == L'?' ||
            character == L'"' || character == L'<' || character == L'>' || character == L'|')
        {
            return false;
        }
        ++length;
    }
    if (length == 0u || name[length - 1u] == L'.' || name[length - 1u] == L' ' ||
        (length == 1u && name[0] == L'.') || (length == 2u && name[0] == L'.' && name[1] == L'.'))
    {
        return false;
    }

    wchar_t stem[16];
    uint32_t stemLength = 0;
    while (stemLength + 1u < (uint32_t)(sizeof(stem) / sizeof(stem[0])) && stemLength < length &&
           name[stemLength] != L'.')
    {
        stem[stemLength] = name[stemLength];
        ++stemLength;
    }
    stem[stemLength] = L'\0';
    if (WideEqualsAsciiIgnoreCase(stem, "con") || WideEqualsAsciiIgnoreCase(stem, "prn") ||
        WideEqualsAsciiIgnoreCase(stem, "aux") || WideEqualsAsciiIgnoreCase(stem, "nul"))
    {
        return false;
    }
    if (stemLength == 4u &&
        ((WideCharacterEqualIgnoreCase(stem[0], L'c') &&
          WideCharacterEqualIgnoreCase(stem[1], L'o') &&
          WideCharacterEqualIgnoreCase(stem[2], L'm')) ||
         (WideCharacterEqualIgnoreCase(stem[0], L'l') &&
          WideCharacterEqualIgnoreCase(stem[1], L'p') &&
          WideCharacterEqualIgnoreCase(stem[2], L't'))) &&
        stem[3] >= L'1' && stem[3] <= L'9')
    {
        return false;
    }
    return true;
}

bool LaiueModPackNameIsSafe(const wchar_t *packName)
{
    return WideLeafNameIsSafe(packName, LAIUE_MOD_NATIVE_NAME_CAPACITY) &&
           WideEndsWithIgnoreCase(packName, LAIUE_MOD_PACK_EXTENSION) &&
           !WideEqualsAsciiIgnoreCase(packName, ".lmp");
}

static bool SliceCopyNativeEntry(ByteSlice slice, wchar_t *destination)
{
    if (slice.length == 0u)
    {
        return false;
    }
    uint32_t written = 0;
    if (!PlatformUtf8ToWide((const char *)slice.data, slice.length, destination,
                            LAIUE_MOD_NATIVE_NAME_CAPACITY, &written) ||
        written == 0u)
    {
        return false;
    }
    return WideLeafNameIsSafe(destination, LAIUE_MOD_NATIVE_NAME_CAPACITY);
}

static bool ParseEngineVersion(ByteSlice slice, uint32_t *outMajor, uint32_t *outMinor)
{
    uint32_t dot = 0;
    while (dot < slice.length && slice.data[dot] != '.')
    {
        ++dot;
    }
    if (dot == 0u || dot + 1u >= slice.length)
    {
        return false;
    }
    for (uint32_t index = dot + 1u; index < slice.length; ++index)
    {
        if (slice.data[index] == '.')
        {
            return false;
        }
    }
    return SliceParseUint32((ByteSlice){slice.data, dot}, outMajor) &&
           SliceParseUint32((ByteSlice){slice.data + dot + 1u, slice.length - dot - 1u}, outMinor);
}

static bool ParseRootField(ManifestParseState *state, ByteSlice key, ByteSlice value)
{
    LaiueModManifest *manifest = state->manifest;
    if (SliceEqualsAsciiIgnoreCase(key, "id"))
    {
        if (state->idSeen || !SliceCopyIdentifier(value, manifest->id, LAIUE_MOD_ID_CAPACITY))
        {
            return false;
        }
        state->idSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "name"))
    {
        if (state->nameSeen ||
            !SliceCopyUtf8(value, manifest->displayName, LAIUE_MOD_DISPLAY_NAME_CAPACITY))
        {
            return false;
        }
        state->nameSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "version"))
    {
        if (state->versionSeen ||
            !SliceCopyVersion(value, manifest->version, LAIUE_MOD_VERSION_CAPACITY))
        {
            return false;
        }
        state->versionSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "engine"))
    {
        if (state->engineSeen || !ParseEngineVersion(value, &manifest->requiredEngineMajor,
                                                     &manifest->requiredEngineMinor))
        {
            return false;
        }
        state->engineSeen = true;
    }
    return true;
}

static bool ParseNativeField(ManifestParseState *state, ByteSlice key, ByteSlice value)
{
    LaiueModManifest *manifest = state->manifest;
    if (SliceEqualsAsciiIgnoreCase(key, "abi"))
    {
        if (state->abiSeen || !SliceParseUint32(value, &manifest->requiredAbi) ||
            manifest->requiredAbi == 0u)
        {
            return false;
        }
        state->abiSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_windows_x86_64"))
    {
        if (state->windowsSeen || !SliceCopyNativeEntry(value, manifest->entryWindowsX86_64) ||
            !WideEndsWithIgnoreCase(manifest->entryWindowsX86_64, L".dll"))
        {
            return false;
        }
        state->windowsSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_windows_arm64"))
    {
        if (state->windowsArmSeen || !SliceCopyNativeEntry(value, manifest->entryWindowsArm64) ||
            !WideEndsWithIgnoreCase(manifest->entryWindowsArm64, L".dll"))
        {
            return false;
        }
        state->windowsArmSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_linux_x86_64_gnu"))
    {
        if (state->linuxGnuSeen || !SliceCopyNativeEntry(value, manifest->entryLinuxX86_64Gnu) ||
            !WideEndsWithIgnoreCase(manifest->entryLinuxX86_64Gnu, L".so"))
        {
            return false;
        }
        state->linuxGnuSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_linux_x86_64_musl"))
    {
        if (state->linuxMuslSeen || !SliceCopyNativeEntry(value, manifest->entryLinuxX86_64Musl) ||
            !WideEndsWithIgnoreCase(manifest->entryLinuxX86_64Musl, L".so"))
        {
            return false;
        }
        state->linuxMuslSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_linux_arm64_gnu"))
    {
        if (state->linuxArmGnuSeen || !SliceCopyNativeEntry(value, manifest->entryLinuxArm64Gnu) ||
            !WideEndsWithIgnoreCase(manifest->entryLinuxArm64Gnu, L".so"))
        {
            return false;
        }
        state->linuxArmGnuSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_linux_arm64_musl"))
    {
        if (state->linuxArmMuslSeen ||
            !SliceCopyNativeEntry(value, manifest->entryLinuxArm64Musl) ||
            !WideEndsWithIgnoreCase(manifest->entryLinuxArm64Musl, L".so"))
        {
            return false;
        }
        state->linuxArmMuslSeen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_macos_x86_64"))
    {
        if (state->macosX86Seen || !SliceCopyNativeEntry(value, manifest->entryMacosX86_64) ||
            !WideEndsWithIgnoreCase(manifest->entryMacosX86_64, L".dylib"))
        {
            return false;
        }
        state->macosX86Seen = true;
    }
    else if (SliceEqualsAsciiIgnoreCase(key, "entry_macos_arm64"))
    {
        if (state->macosArmSeen || !SliceCopyNativeEntry(value, manifest->entryMacosArm64) ||
            !WideEndsWithIgnoreCase(manifest->entryMacosArm64, L".dylib"))
        {
            return false;
        }
        state->macosArmSeen = true;
    }
    return true;
}

static bool ManifestComplete(const ManifestParseState *state)
{
    return state->idSeen && state->versionSeen && state->engineSeen && state->abiSeen &&
           (state->windowsSeen || state->windowsArmSeen || state->linuxGnuSeen ||
            state->linuxMuslSeen || state->linuxArmGnuSeen || state->linuxArmMuslSeen ||
            state->macosX86Seen || state->macosArmSeen);
}

LaiueModStatus LaiueModManifestParse(const void *bytes, size_t byteCount,
                                     LaiueModManifest *outManifest, LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (bytes == NULL || outManifest == NULL || byteCount == 0u)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "manifest bytes and destination are required");
    }
    if (byteCount > LAIUE_MOD_MANIFEST_MAX_BYTES || byteCount > UINT32_MAX)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_TOO_LARGE, 0,
                                     "manifest exceeds the 64 KiB limit");
    }

    const uint8_t *input = bytes;
    uint32_t length = (uint32_t)byteCount;
    for (uint32_t index = 0; index < length; ++index)
    {
        if (input[index] == 0u)
        {
            return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                         "manifest contains an embedded NUL byte");
        }
    }

    memset(outManifest, 0, sizeof(*outManifest));
    outManifest->structSize = sizeof(*outManifest);
    outManifest->formatVersion = LAIUE_MOD_MANIFEST_FORMAT_VERSION;
    ManifestParseState state = {.manifest = outManifest};

    ByteSlice section = {NULL, 0u};
    bool headerSeen = false;
    uint32_t offset =
        length >= 3u && input[0] == 0xefu && input[1] == 0xbbu && input[2] == 0xbfu ? 3u : 0u;

    while (offset < length)
    {
        uint32_t lineEnd = offset;
        while (lineEnd < length && input[lineEnd] != '\n')
        {
            ++lineEnd;
        }
        ByteSlice line = SliceTrim((ByteSlice){input + offset, lineEnd - offset});
        offset = lineEnd < length ? lineEnd + 1u : length;

        if (line.length == 0u || line.data[0] == '#')
        {
            continue;
        }
        if (line.length > 1024u)
        {
            return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                         "manifest line exceeds 1024 bytes");
        }
        if (!headerSeen)
        {
            if (!SliceEqualsAsciiIgnoreCase(line, "LAIUE MOD 3"))
            {
                return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                             "first meaningful line must be LAIUE MOD 3");
            }
            headerSeen = true;
            continue;
        }

        if (line.data[0] == '[')
        {
            if (line.length < 3u || line.data[line.length - 1u] != ']')
            {
                return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                             "malformed manifest section header");
            }
            section = SliceTrim((ByteSlice){line.data + 1u, line.length - 2u});
            if (section.length == 0u)
            {
                return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                             "empty manifest section name");
            }
            continue;
        }

        uint32_t equals = 0;
        while (equals < line.length && line.data[equals] != '=')
        {
            ++equals;
        }
        if (equals == 0u || equals == line.length)
        {
            return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                         "manifest field must use key = value syntax");
        }
        ByteSlice key = SliceTrim((ByteSlice){line.data, equals});
        ByteSlice value =
            SliceTrim((ByteSlice){line.data + equals + 1u, line.length - equals - 1u});
        if (key.length == 0u || value.length == 0u)
        {
            return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                         "manifest keys and values cannot be empty");
        }

        bool valid = section.length == 0u ? ParseRootField(&state, key, value)
                                          : !SliceEqualsAsciiIgnoreCase(section, "native") ||
                                                ParseNativeField(&state, key, value);
        if (!valid)
        {
            return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
                                         "duplicate or malformed manifest field");
        }
    }

    if (!headerSeen || !ManifestComplete(&state))
    {
        return LaiueModDiagnosticSet(
            diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID, 0,
            "manifest is missing id, version, engine, abi or a native entry");
    }
    if (!state.nameSeen)
    {
        uint32_t index = 0;
        while (outManifest->id[index] != '\0')
        {
            outManifest->displayName[index] = outManifest->id[index];
            ++index;
        }
        outManifest->displayName[index] = '\0';
    }
    return LAIUE_MOD_STATUS_OK;
}

LaiueModStatus LaiueModManifestSelectNativeEntry(const LaiueModManifest *manifest,
                                                 wchar_t *destination, uint32_t capacity,
                                                 LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (manifest == NULL || destination == NULL || capacity == 0u ||
        manifest->structSize < sizeof(*manifest))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "valid manifest and destination are required");
    }

    const wchar_t *selected = NULL;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    selected = manifest->entryWindowsX86_64;
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    selected = manifest->entryWindowsArm64;
#elif defined(__linux__) && defined(__x86_64__) && defined(LAIUE_LINUX_LIBC_MUSL)
    selected = manifest->entryLinuxX86_64Musl;
#elif defined(__linux__) && defined(__x86_64__)
    selected = manifest->entryLinuxX86_64Gnu;
#elif defined(__linux__) && defined(__aarch64__) && defined(LAIUE_LINUX_LIBC_MUSL)
    selected = manifest->entryLinuxArm64Musl;
#elif defined(__linux__) && defined(__aarch64__)
    selected = manifest->entryLinuxArm64Gnu;
#elif defined(__APPLE__) && defined(__x86_64__)
    selected = manifest->entryMacosX86_64;
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    selected = manifest->entryMacosArm64;
#endif
    if (selected == NULL || selected[0] == L'\0')
    {
        destination[0] = L'\0';
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_PLATFORM_UNSUPPORTED, 0,
                                     "manifest has no native artifact for the current platform");
    }
    if (!LaiueModWideCopy(destination, capacity, selected))
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_NATIVE_ARTIFACT_INVALID, 0,
                                     "native artifact name does not fit the destination");
    }
    return LAIUE_MOD_STATUS_OK;
}

static LaiueModStatus FinishPackInspection(LaiueModPackInfo *info, LaiueModDiagnostic *diagnostic,
                                           LaiueModStatus status, const char *message)
{
    LaiueModDiagnostic local;
    LaiueModDiagnosticSet(&local, status, 0, message);
    info->status = status;
    info->diagnostic = local;
    if (diagnostic != NULL)
    {
        *diagnostic = local;
    }
    return status;
}

static LaiueModStatus InspectPackEntry(const wchar_t *rootDirectory, const wchar_t *packName,
                                       LaiueModPackInfo *outInfo, LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (rootDirectory == NULL || rootDirectory[0] == L'\0' || outInfo == NULL)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "pack root and output are required");
    }
    memset(outInfo, 0, sizeof(*outInfo));
    if (!LaiueModPackNameIsSafe(packName) ||
        !LaiueModWideCopy(outInfo->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, packName))
    {
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_UNSAFE_PACK_NAME,
                                    "pack name must be one safe leaf ending in .lmp");
    }

    PackInspectionScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY,
                                    "could not allocate pack inspection scratch memory");
    }
    if (!LaiueModPathJoin(scratch->packPath, LAIUE_PLATFORM_PATH_CAPACITY, rootDirectory, packName,
                          NULL) ||
        !LaiueModPathJoin(scratch->manifestPath, LAIUE_PLATFORM_PATH_CAPACITY, rootDirectory,
                          packName, LAIUE_MOD_MANIFEST_FILE_NAME))
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_UNSAFE_PACK_NAME,
                                    "pack path exceeds the platform limit");
    }

    PlatformPathInformation information;
    if (!PlatformGetPathInformation(scratch->packPath, &information) || !information.exists)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_PACK_NOT_FOUND,
                                    "pack directory does not exist");
    }
    if (information.isSymbolicLink)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_PACK_IS_SYMBOLIC_LINK,
                                    "symbolic-link and reparse-point packs are rejected");
    }
    if (!information.isDirectory)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_PACK_NOT_DIRECTORY,
                                    "pack path is not a directory");
    }
    if (!PlatformGetPathInformation(scratch->manifestPath, &information) || !information.exists)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_MANIFEST_NOT_FOUND,
                                    "pack has no mod.lm manifest");
    }
    if (information.isDirectory || information.isSymbolicLink)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_MANIFEST_INVALID,
                                    "mod.lm must be a regular non-symbolic-link file");
    }
    if (information.size == 0u || information.size > LAIUE_MOD_MANIFEST_MAX_BYTES)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_MANIFEST_TOO_LARGE,
                                    "mod.lm must contain 1 to 65536 bytes");
    }

    uint8_t *bytes = NULL;
    uint64_t byteCount = 0;
    if (!PlatformReadEntireFile(scratch->manifestPath, LAIUE_MOD_MANIFEST_MAX_BYTES, &bytes,
                                &byteCount))
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_MANIFEST_NOT_FOUND,
                                    "mod.lm could not be read safely");
    }
    LaiueModDiagnostic parseDiagnostic;
    LaiueModStatus status =
        LaiueModManifestParse(bytes, (size_t)byteCount, &outInfo->manifest, &parseDiagnostic);
    PlatformFree(bytes);
    if (status != LAIUE_MOD_STATUS_OK)
    {
        PlatformFree(scratch);
        outInfo->status = status;
        outInfo->diagnostic = parseDiagnostic;
        if (diagnostic != NULL)
        {
            *diagnostic = parseDiagnostic;
        }
        return status;
    }

    LaiueModDiagnostic selectDiagnostic;
    status = LaiueModManifestSelectNativeEntry(&outInfo->manifest, scratch->nativeName,
                                               LAIUE_MOD_NATIVE_NAME_CAPACITY, &selectDiagnostic);
    if (status != LAIUE_MOD_STATUS_OK)
    {
        PlatformFree(scratch);
        outInfo->status = status;
        outInfo->diagnostic = selectDiagnostic;
        if (diagnostic != NULL)
        {
            *diagnostic = selectDiagnostic;
        }
        return status;
    }

    if (!LaiueModPathJoin(scratch->nativePath, LAIUE_PLATFORM_PATH_CAPACITY, rootDirectory,
                          packName, scratch->nativeName))
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_NATIVE_ARTIFACT_INVALID,
                                    "native artifact path exceeds the platform limit");
    }
    if (!PlatformGetPathInformation(scratch->nativePath, &information) || !information.exists)
    {
        PlatformFree(scratch);
        return FinishPackInspection(outInfo, diagnostic, LAIUE_MOD_STATUS_NATIVE_ARTIFACT_NOT_FOUND,
                                    "current-platform native artifact does not exist");
    }
    if (information.isSymbolicLink)
    {
        PlatformFree(scratch);
        return FinishPackInspection(
            outInfo, diagnostic, LAIUE_MOD_STATUS_NATIVE_ARTIFACT_IS_SYMBOLIC_LINK,
            "symbolic-link and reparse-point native artifacts are rejected");
    }
    if (information.isDirectory || information.size == 0u ||
        information.size > LAIUE_MOD_NATIVE_MAX_BYTES)
    {
        PlatformFree(scratch);
        return FinishPackInspection(
            outInfo, diagnostic, LAIUE_MOD_STATUS_NATIVE_ARTIFACT_INVALID,
            "native artifact must be a regular file no larger than 256 MiB");
    }

    PlatformFree(scratch);
    outInfo->status = LAIUE_MOD_STATUS_OK;
    LaiueModDiagnosticClear(&outInfo->diagnostic);
    return LAIUE_MOD_STATUS_OK;
}

static int ComparePackNames(const wchar_t *first, const wchar_t *second)
{
    char firstUtf8[LAIUE_MOD_NATIVE_NAME_CAPACITY * 4u];
    char secondUtf8[LAIUE_MOD_NATIVE_NAME_CAPACITY * 4u];
    uint32_t firstLength = 0;
    uint32_t secondLength = 0;
    bool firstValid = PlatformWideToUtf8(first, firstUtf8, sizeof(firstUtf8), &firstLength);
    bool secondValid = PlatformWideToUtf8(second, secondUtf8, sizeof(secondUtf8), &secondLength);
    if (!firstValid || !secondValid)
    {
        return firstValid ? -1 : secondValid ? 1 : 0;
    }
    uint32_t minimum = firstLength < secondLength ? firstLength : secondLength;
    for (uint32_t index = 0; index < minimum; ++index)
    {
        uint8_t firstByte = (uint8_t)firstUtf8[index];
        uint8_t secondByte = (uint8_t)secondUtf8[index];
        if (firstByte != secondByte)
        {
            return firstByte < secondByte ? -1 : 1;
        }
    }
    return firstLength < secondLength ? -1 : firstLength > secondLength ? 1 : 0;
}

static wchar_t FoldAsciiCase(wchar_t character)
{
    return character >= L'A' && character <= L'Z' ? character + (L'a' - L'A') : character;
}

static bool PackNamesEqualAsciiCaseInsensitive(const wchar_t *first, const wchar_t *second)
{
    uint32_t index = 0;
    while (first[index] != L'\0' && second[index] != L'\0')
    {
        if (FoldAsciiCase(first[index]) != FoldAsciiCase(second[index]))
        {
            return false;
        }
        ++index;
    }
    return first[index] == second[index];
}

static bool PackNamesEqual(const wchar_t *first, const wchar_t *second)
{
    uint32_t index = 0;
    while (first[index] != L'\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

static LaiueModStatus ValidateUniquePackRootEntry(const wchar_t *rootDirectory,
                                                  const wchar_t *packName)
{
    PlatformDirectoryIterator *iterator = PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry *entry = PlatformAllocate(sizeof(*entry), false);
    if (iterator == NULL || entry == NULL)
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return LAIUE_MOD_STATUS_OUT_OF_MEMORY;
    }
    if (!PlatformDirectoryOpen(iterator, rootDirectory))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return LAIUE_MOD_STATUS_PACK_NOT_FOUND;
    }

    uint32_t discoverableCount = 0u;
    uint32_t foldedMatchCount = 0u;
    bool exactMatch = false;
    LaiueModStatus status = LAIUE_MOD_STATUS_OK;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (!LaiueModPackNameIsSafe(entry->name))
        {
            continue;
        }
        if (++discoverableCount > LAIUE_MOD_PACK_ENUMERATION_LIMIT)
        {
            status = LAIUE_MOD_STATUS_CAPACITY_EXCEEDED;
            break;
        }
        if (!PackNamesEqualAsciiCaseInsensitive(entry->name, packName))
        {
            continue;
        }
        ++foldedMatchCount;
        exactMatch = exactMatch || PackNamesEqual(entry->name, packName);
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);

    if (status != LAIUE_MOD_STATUS_OK)
    {
        return status;
    }
    if (foldedMatchCount > 1u)
    {
        return LAIUE_MOD_STATUS_PACK_NAME_COLLISION;
    }
    return exactMatch ? LAIUE_MOD_STATUS_OK : LAIUE_MOD_STATUS_PACK_NOT_FOUND;
}

LaiueModStatus LaiueModPackInspect(const wchar_t *rootDirectory, const wchar_t *packName,
                                   LaiueModPackInfo *outInfo, LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (rootDirectory == NULL || rootDirectory[0] == L'\0' || outInfo == NULL)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "pack root and output are required");
    }
    if (!LaiueModPackNameIsSafe(packName))
    {
        return InspectPackEntry(rootDirectory, packName, outInfo, diagnostic);
    }

    LaiueModStatus status = ValidateUniquePackRootEntry(rootDirectory, packName);
    if (status == LAIUE_MOD_STATUS_OK)
    {
        return InspectPackEntry(rootDirectory, packName, outInfo, diagnostic);
    }

    memset(outInfo, 0, sizeof(*outInfo));
    LaiueModWideCopy(outInfo->packName, LAIUE_MOD_NATIVE_NAME_CAPACITY, packName);
    const char *message = status == LAIUE_MOD_STATUS_OUT_OF_MEMORY
                              ? "could not allocate pack root validation state"
                          : status == LAIUE_MOD_STATUS_CAPACITY_EXCEEDED
                              ? "pack root contains more than 4096 discoverable packs"
                          : status == LAIUE_MOD_STATUS_PACK_NAME_COLLISION
                              ? "pack root contains ASCII case-colliding names"
                              : "exact pack directory name does not exist";
    return FinishPackInspection(outInfo, diagnostic, status, message);
}

static void SortPackList(LaiueModPackList *list)
{
    for (uint32_t index = 1; index < list->count; ++index)
    {
        LaiueModPackInfo value = list->entries[index];
        uint32_t insertion = index;
        while (insertion > 0u &&
               ComparePackNames(value.packName, list->entries[insertion - 1u].packName) < 0)
        {
            list->entries[insertion] = list->entries[insertion - 1u];
            --insertion;
        }
        list->entries[insertion] = value;
    }
}

LaiueModStatus LaiueModPackEnumerate(const wchar_t *rootDirectory, LaiueModPackList *outList,
                                     LaiueModDiagnostic *diagnostic)
{
    LaiueModDiagnosticClear(diagnostic);
    if (outList == NULL || rootDirectory == NULL || rootDirectory[0] == L'\0')
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_INVALID_ARGUMENT, 0,
                                     "pack root and output list are required");
    }
    memset(outList, 0, sizeof(*outList));

    PlatformPathInformation information;
    if (!PlatformGetPathInformation(rootDirectory, &information) || !information.exists)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_PACK_NOT_FOUND, 0,
                                     "pack root directory does not exist");
    }
    if (!information.isDirectory)
    {
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_PACK_NOT_DIRECTORY, 0,
                                     "pack root is not a directory");
    }

    PlatformDirectoryIterator *iterator = PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry *entry = PlatformAllocate(sizeof(*entry), false);
    if (iterator == NULL || entry == NULL)
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY, 0,
                                     "could not allocate directory enumeration scratch memory");
    }
    if (!PlatformDirectoryOpen(iterator, rootDirectory))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_PACK_NOT_FOUND, 0,
                                     "pack root directory could not be enumerated");
    }

    uint32_t capacity = 0;
    LaiueModStatus status = LAIUE_MOD_STATUS_OK;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (!LaiueModPackNameIsSafe(entry->name))
        {
            continue;
        }
        bool nameCollision = false;
        for (uint32_t index = 0; index < outList->count; ++index)
        {
            if (PackNamesEqualAsciiCaseInsensitive(outList->entries[index].packName, entry->name))
            {
                nameCollision = true;
                break;
            }
        }
        if (nameCollision)
        {
            status = LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_PACK_NAME_COLLISION, 0,
                                           "pack root contains ASCII case-colliding names");
            break;
        }
        if (outList->count >= LAIUE_MOD_PACK_ENUMERATION_LIMIT)
        {
            status = LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_CAPACITY_EXCEEDED, 0,
                                           "pack root contains more than 4096 discoverable packs");
            break;
        }
        if (outList->count == capacity)
        {
            uint32_t nextCapacity = capacity == 0u ? 8u : capacity * 2u;
            if (nextCapacity > LAIUE_MOD_PACK_ENUMERATION_LIMIT)
            {
                nextCapacity = LAIUE_MOD_PACK_ENUMERATION_LIMIT;
            }
            void *memory = PlatformReallocate(
                outList->entries, (size_t)nextCapacity * sizeof(*outList->entries), false);
            if (memory == NULL)
            {
                status = LaiueModDiagnosticSet(diagnostic, LAIUE_MOD_STATUS_OUT_OF_MEMORY, 0,
                                               "could not allocate the discovered pack list");
                break;
            }
            outList->entries = memory;
            capacity = nextCapacity;
        }
        LaiueModPackInfo *pack = &outList->entries[outList->count++];
        InspectPackEntry(rootDirectory, entry->name, pack, NULL);
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);

    if (status != LAIUE_MOD_STATUS_OK)
    {
        LaiueModPackListRelease(outList);
        return status;
    }
    SortPackList(outList);
    return LAIUE_MOD_STATUS_OK;
}

void LaiueModPackListRelease(LaiueModPackList *list)
{
    if (list == NULL)
    {
        return;
    }
    PlatformFree(list->entries);
    memset(list, 0, sizeof(*list));
}
