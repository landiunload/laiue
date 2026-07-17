#include "server/server_config.h"

#include <windows.h>
#include <string.h>

#define SERVER_CONFIG_MAX_BYTES 16384U

static bool TextEquals(const char* text, uint32_t length, const char* expected)
{
    uint32_t i = 0;
    while (expected[i] != '\0' && i < length && text[i] == expected[i]) ++i;
    return i == length && expected[i] == '\0';
}

static bool ParseUnsigned(const char* text, uint32_t length, uint64_t* output)
{
    if (length == 0) return false;
    uint64_t value = 0;
    for (uint32_t i = 0; i < length; ++i)
    {
        if (text[i] < '0' || text[i] > '9') return false;
        uint64_t next = value * 10U + (uint64_t)(text[i] - '0');
        if (next < value) return false;
        value = next;
    }
    *output = value;
    return true;
}

static bool ParseSigned(const char* text, uint32_t length, int64_t* output)
{
    if (length == 0) return false;
    bool negative = text[0] == '-';
    uint32_t offset = negative ? 1U : 0U;
    uint64_t magnitude;
    if (!ParseUnsigned(text + offset, length - offset, &magnitude)) return false;
    if ((!negative && magnitude > 0x7fffffffffffffffULL)
        || (negative && magnitude > 0x8000000000000000ULL)) return false;
    *output = negative
        ? (magnitude == 0x8000000000000000ULL
            ? (int64_t)0x8000000000000000ULL
            : -(int64_t)magnitude)
        : (int64_t)magnitude;
    return true;
}

static void ApplyPair(ServerConfiguration* configuration,
    const char* key, uint32_t keyLength, const char* value, uint32_t valueLength)
{
    uint64_t number;
    if (TextEquals(key, keyLength, "port")
        && ParseUnsigned(value, valueLength, &number)
        && number > 0 && number <= 65535U)
    {
        configuration->port = (uint16_t)number;
    }
    else if (TextEquals(key, keyLength, "maximum_peers")
        && ParseUnsigned(value, valueLength, &number)
        && number > 0 && number <= 16U)
    {
        configuration->maximumPeers = (uint16_t)number;
    }
    else if (TextEquals(key, keyLength, "world_seed"))
    {
        int64_t seed;
        if (ParseSigned(value, valueLength, &seed))
        {
            configuration->worldSeed = seed;
        }
    }
    else if (TextEquals(key, keyLength, "allow_content_downloads"))
    {
        configuration->allowContentDownloads =
            TextEquals(value, valueLength, "true")
            || TextEquals(value, valueLength, "1");
    }
}

static void ApplyEnvironment(ServerConfiguration* configuration)
{
    char value[64];
    DWORD valueLength = GetEnvironmentVariableA(
        "LAIUE_SERVER_PORT", value, sizeof(value));
    uint64_t number;
    if (valueLength > 0 && valueLength < sizeof(value)
        && ParseUnsigned(value, valueLength, &number)
        && number > 0 && number <= 65535U)
    {
        configuration->port = (uint16_t)number;
    }
    valueLength = GetEnvironmentVariableA(
        "LAIUE_SERVER_ALLOW_CONTENT_DOWNLOADS", value, sizeof(value));
    if (valueLength > 0 && valueLength < sizeof(value))
    {
        configuration->allowContentDownloads =
            TextEquals(value, valueLength, "true")
            || TextEquals(value, valueLength, "1");
    }
}

void ServerConfigurationLoad(ServerConfiguration* configuration)
{
    configuration->port = 27180U;
    configuration->maximumPeers = 16U;
    configuration->worldSeed = 42;
    configuration->allowContentDownloads = false;

    HANDLE file = CreateFileW(L"server.cfg", GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        ApplyEnvironment(configuration);
        return;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0
        || size.QuadPart > SERVER_CONFIG_MAX_BYTES)
    {
        CloseHandle(file);
        ApplyEnvironment(configuration);
        return;
    }
    uint32_t length = (uint32_t)size.QuadPart;
    char* bytes = HeapAlloc(GetProcessHeap(), 0, length);
    DWORD read = 0;
    bool loaded = bytes != NULL
        && ReadFile(file, bytes, length, &read, NULL) && read == length;
    CloseHandle(file);
    if (!loaded)
    {
        if (bytes != NULL) HeapFree(GetProcessHeap(), 0, bytes);
        ApplyEnvironment(configuration);
        return;
    }

    uint32_t offset = 0;
    while (offset < length)
    {
        uint32_t end = offset;
        while (end < length && bytes[end] != '\n') ++end;
        uint32_t first = offset;
        while (first < end && (bytes[first] == ' ' || bytes[first] == '\t')) ++first;
        uint32_t last = end;
        while (last > first && (bytes[last - 1] == ' ' || bytes[last - 1] == '\t'
            || bytes[last - 1] == '\r')) --last;
        if (first < last && bytes[first] != '#')
        {
            uint32_t equals = first;
            while (equals < last && bytes[equals] != '=') ++equals;
            if (equals < last)
            {
                uint32_t keyEnd = equals;
                while (keyEnd > first && bytes[keyEnd - 1] == ' ') --keyEnd;
                uint32_t valueStart = equals + 1U;
                while (valueStart < last && bytes[valueStart] == ' ') ++valueStart;
                ApplyPair(configuration, bytes + first, keyEnd - first,
                    bytes + valueStart, last - valueStart);
            }
        }
        offset = end + 1U;
    }
    HeapFree(GetProcessHeap(), 0, bytes);
    // Удобно для контейнера/нескольких локальных инстансов; файл остаётся
    // основным источником конфигурации, переменные имеют последний приоритет.
    ApplyEnvironment(configuration);
}
