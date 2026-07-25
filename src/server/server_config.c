#include "server/server_config.h"
#include "platform/system.h"

#include <string.h>

#define SERVER_CONFIG_MAX_BYTES 16384U

static bool TextEquals(
    const char* text, uint32_t length, const char* expected)
{
    uint32_t i = 0;
    while (expected[i] != '\0' && i < length && text[i] == expected[i]) ++i;
    return i == length && expected[i] == '\0';
}

static uint32_t TextLength(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0') ++length;
    return length;
}

static bool ParseUnsigned(
    const char* text, uint32_t length, uint64_t* output)
{
    if (length == 0 || output == NULL) return false;
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

static bool ParseSigned(
    const char* text, uint32_t length, int64_t* output)
{
    if (length == 0 || output == NULL) return false;
    bool negative = text[0] == '-';
    uint32_t offset = negative ? 1U : 0U;
    uint64_t magnitude;
    if (!ParseUnsigned(text + offset, length - offset, &magnitude))
        return false;
    if ((!negative && magnitude > 0x7fffffffffffffffULL)
        || (negative && magnitude > 0x8000000000000000ULL))
        return false;
    *output = negative
        ? (magnitude == 0x8000000000000000ULL
            ? INT64_MIN : -(int64_t)magnitude)
        : (int64_t)magnitude;
    return true;
}

static bool CopyValue(char* destination, uint32_t capacity,
                      const char* value, uint32_t length)
{
    if (destination == NULL || value == NULL || length >= capacity)
        return false;
    memcpy(destination, value, length);
    destination[length] = '\0';
    return true;
}

static bool ParseAddressFamily(const char* value, uint32_t length,
                               ServerAddressFamily* output)
{
    if (TextEquals(value, length, "dual"))
        *output = SERVER_ADDRESS_FAMILY_DUAL;
    else if (TextEquals(value, length, "ipv4"))
        *output = SERVER_ADDRESS_FAMILY_IPV4;
    else if (TextEquals(value, length, "ipv6"))
        *output = SERVER_ADDRESS_FAMILY_IPV6;
    else
        return false;
    return true;
}

static void ApplyPair(ServerConfiguration* configuration,
    const char* key, uint32_t keyLength,
    const char* value, uint32_t valueLength)
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
            configuration->worldSeed = seed;
    }
    else if (TextEquals(key, keyLength, "allow_content_downloads"))
    {
        configuration->allowContentDownloads =
            TextEquals(value, valueLength, "true")
            || TextEquals(value, valueLength, "1");
    }
    else if (TextEquals(key, keyLength, "address_family"))
    {
        ParseAddressFamily(value, valueLength, &configuration->addressFamily);
    }
    else if (TextEquals(key, keyLength, "listen_address"))
    {
        CopyValue(configuration->listenAddress,
            SERVER_CONFIG_ADDRESS_CAPACITY, value, valueLength);
    }
    else if (TextEquals(key, keyLength, "certificate_file"))
    {
        CopyValue(configuration->certificateFile,
            SERVER_CONFIG_PATH_CAPACITY, value, valueLength);
    }
    else if (TextEquals(key, keyLength, "private_key_file"))
    {
        CopyValue(configuration->privateKeyFile,
            SERVER_CONFIG_PATH_CAPACITY, value, valueLength);
    }
    else if (TextEquals(key, keyLength, "certificate_thumbprint"))
    {
        CopyValue(configuration->certificateThumbprint,
            SERVER_CONFIG_THUMBPRINT_CAPACITY, value, valueLength);
    }
}

static void ApplyEnvironmentPair(ServerConfiguration* configuration,
    const char* environmentName, const char* configurationKey)
{
    char value[SERVER_CONFIG_PATH_CAPACITY];
    uint32_t length = PlatformGetEnvironmentUtf8(
        environmentName, value, sizeof(value));
    if (length != 0)
        ApplyPair(configuration, configurationKey,
            TextLength(configurationKey), value, length);
}

static void ApplyEnvironment(ServerConfiguration* configuration)
{
    ApplyEnvironmentPair(
        configuration, "LAIUE_SERVER_PORT", "port");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_ALLOW_CONTENT_DOWNLOADS", "allow_content_downloads");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_ADDRESS_FAMILY", "address_family");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_LISTEN_ADDRESS", "listen_address");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_CERTIFICATE_FILE", "certificate_file");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_PRIVATE_KEY_FILE", "private_key_file");
    ApplyEnvironmentPair(configuration,
        "LAIUE_SERVER_CERTIFICATE_THUMBPRINT", "certificate_thumbprint");
}

void ServerConfigurationLoad(ServerConfiguration* configuration)
{
    if (configuration == NULL) return;
    memset(configuration, 0, sizeof(*configuration));
    configuration->port = 27180U;
    configuration->maximumPeers = 16U;
    configuration->worldSeed = 42;
    configuration->addressFamily = SERVER_ADDRESS_FAMILY_DUAL;
    memcpy(configuration->listenAddress, "*", 2U);

    uint8_t* bytes = NULL;
    uint64_t size = 0;
    if (PlatformReadEntireFile(L"server.cfg",
            SERVER_CONFIG_MAX_BYTES, &bytes, &size))
    {
        uint32_t length = (uint32_t)size;
        uint32_t offset = length >= 3U
            && bytes[0] == 0xefU && bytes[1] == 0xbbU
            && bytes[2] == 0xbfU ? 3U : 0U;
        while (offset < length)
        {
            uint32_t end = offset;
            while (end < length && bytes[end] != '\n') ++end;
            uint32_t first = offset;
            while (first < end
                && (bytes[first] == ' ' || bytes[first] == '\t')) ++first;
            uint32_t last = end;
            while (last > first && (bytes[last - 1U] == ' '
                || bytes[last - 1U] == '\t'
                || bytes[last - 1U] == '\r')) --last;
            if (first < last && bytes[first] != '#')
            {
                uint32_t equals = first;
                while (equals < last && bytes[equals] != '=') ++equals;
                if (equals < last)
                {
                    uint32_t keyEnd = equals;
                    while (keyEnd > first
                        && (bytes[keyEnd - 1U] == ' '
                            || bytes[keyEnd - 1U] == '\t')) --keyEnd;
                    uint32_t valueStart = equals + 1U;
                    while (valueStart < last
                        && (bytes[valueStart] == ' '
                            || bytes[valueStart] == '\t')) ++valueStart;
                    ApplyPair(configuration,
                        (const char*)bytes + first, keyEnd - first,
                        (const char*)bytes + valueStart, last - valueStart);
                }
            }
            offset = end + 1U;
        }
    }
    PlatformFree(bytes);
    ApplyEnvironment(configuration);

    // В dual режиме конкретный адрес не может одновременно представлять оба
    // семейства. Fail-closed normalization оставляет dual только для wildcard.
    if (configuration->addressFamily == SERVER_ADDRESS_FAMILY_DUAL
        && !TextEquals(configuration->listenAddress,
            TextLength(configuration->listenAddress), "*"))
    {
        configuration->listenAddress[0] = '\0';
    }
}
