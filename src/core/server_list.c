#include "core/server_list.h"

#include <windows.h>
#include <string.h>

#define SERVER_LIST_FILE_MAX_BYTES 16384U

static bool BuildServersPath(wchar_t* output, uint32_t capacity)
{
    DWORD length = GetModuleFileNameW(NULL, output, capacity);
    if (length == 0 || length >= capacity) return false;
    while (length != 0 && output[length - 1U] != L'\\'
        && output[length - 1U] != L'/') --length;
    static const wchar_t name[] = L"servers.txt";
    if (length + (uint32_t)(sizeof(name) / sizeof(name[0])) > capacity)
    {
        return false;
    }
    memcpy(output + length, name, sizeof(name));
    return true;
}

static uint32_t NarrowLength(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0') ++length;
    return length;
}

static bool ParsePort(const char* bytes, uint32_t length, uint16_t* output)
{
    if (length == 0) return false;
    uint32_t value = 0;
    for (uint32_t i = 0; i < length; ++i)
    {
        if (bytes[i] < '0' || bytes[i] > '9') return false;
        value = value * 10U + (uint32_t)(bytes[i] - '0');
        if (value > 65535U) return false;
    }
    if (value == 0) return false;
    *output = (uint16_t)value;
    return true;
}

static bool CopyUtf8(const char* bytes, uint32_t length,
    wchar_t* output, uint32_t capacity)
{
    int32_t written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes, (int32_t)length, output, (int32_t)capacity - 1);
    if (written <= 0) return false;
    output[written] = L'\0';
    return true;
}

static bool ParseLine(const char* line, uint32_t length,
    ServerListEntry* entry)
{
    uint32_t first = 0;
    while (first < length && line[first] != '|') ++first;
    if (first == 0 || first == length) return false;
    uint32_t second = first + 1U;
    while (second < length && line[second] != '|') ++second;
    if (second == first + 1U || second == length) return false;
    uint32_t endpointLength = second - first - 1U;
    const char* endpointText = line + first + 1U;
    uint32_t thirdLength = length - second - 1U;
    if (endpointLength >= LAIUE_NETWORK_HOST_CAPACITY
        || thirdLength == 0 || thirdLength >= 80U) return false;
    char endpointUtf8[LAIUE_NETWORK_HOST_CAPACITY];
    memcpy(endpointUtf8, endpointText, endpointLength);
    endpointUtf8[endpointLength] = '\0';
    memset(entry, 0, sizeof(*entry));
    uint16_t legacyPort = 0;
    bool legacy = ParsePort(
        line + second + 1U, thirdLength, &legacyPort);
    NetworkEndpointParseResult endpointResult = NetworkEndpointParse(
        endpointUtf8,
        legacy ? legacyPort : LAIUE_NETWORK_DEFAULT_PORT,
        &entry->endpoint);
    if (endpointResult != NETWORK_ENDPOINT_PARSE_OK) return false;
    if (legacy)
    {
        entry->trustMode = NETWORK_TRUST_SYSTEM;
        entry->endpoint.port = legacyPort;
    }
    else
    {
        char trust[80];
        memcpy(trust, line + second + 1U, thirdLength);
        trust[thirdLength] = '\0';
        if (!NetworkTrustParse(trust, &entry->trustMode,
                entry->certificateSha256))
            return false;
    }
    entry->port = entry->endpoint.port;
    return CopyUtf8(line, first, entry->name, SERVER_LIST_TEXT_CAPACITY)
        && CopyUtf8(entry->endpoint.host,
            NarrowLength(entry->endpoint.host), entry->address,
            SERVER_LIST_TEXT_CAPACITY)
        && CopyUtf8(endpointText, endpointLength, entry->endpointText,
            SERVER_LIST_TEXT_CAPACITY);
}

bool ServerListLoad(ServerList* list)
{
    if (list == NULL) return false;
    memset(list, 0, sizeof(*list));
    wchar_t path[MAX_PATH];
    HANDLE file = BuildServersPath(path, MAX_PATH)
        ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0
            && size.QuadPart <= SERVER_LIST_FILE_MAX_BYTES)
        {
            uint32_t length = (uint32_t)size.QuadPart;
            char* bytes = HeapAlloc(GetProcessHeap(), 0, length);
            DWORD read = 0;
            if (bytes != NULL && ReadFile(file, bytes, length, &read, NULL)
                && read == length)
            {
                uint32_t offset = length >= 3U
                    && (uint8_t)bytes[0] == 0xefU
                    && (uint8_t)bytes[1] == 0xbbU
                    && (uint8_t)bytes[2] == 0xbfU ? 3U : 0U;
                while (offset < length && list->count < SERVER_LIST_MAX_ENTRIES)
                {
                    uint32_t end = offset;
                    while (end < length && bytes[end] != '\n') ++end;
                    uint32_t last = end;
                    while (last > offset && (bytes[last - 1U] == '\r'
                        || bytes[last - 1U] == ' ' || bytes[last - 1U] == '\t')) --last;
                    if (last > offset && bytes[offset] != '#'
                        && ParseLine(bytes + offset, last - offset,
                            &list->entries[list->count])) ++list->count;
                    offset = end + 1U;
                }
            }
            if (bytes != NULL) HeapFree(GetProcessHeap(), 0, bytes);
        }
        CloseHandle(file);
    }

    if (list->count == 0)
    {
        ServerListEntry* entry = &list->entries[0];
        memcpy(entry->name, L"Локальный сервер", sizeof(L"Локальный сервер"));
        memcpy(entry->address, L"localhost", sizeof(L"localhost"));
        memcpy(entry->endpointText, L"localhost", sizeof(L"localhost"));
        NetworkEndpointParse("localhost", LAIUE_NETWORK_DEFAULT_PORT,
            &entry->endpoint);
        entry->port = entry->endpoint.port;
        entry->trustMode = NETWORK_TRUST_SYSTEM;
        list->count = 1;
    }
    return true;
}
