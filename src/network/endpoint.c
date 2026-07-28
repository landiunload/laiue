#include "network/network.h"

#include <stddef.h>
#include <string.h>

#define NETWORK_ENDPOINT_TEXT_LIMIT (LAIUE_NETWORK_HOST_CAPACITY + 8U)
#define NETWORK_DNS_NAME_LIMIT 253U
#define NETWORK_DNS_LABEL_LIMIT 63U
#define NETWORK_MIN_TIMEOUT_MS 1000U
#define NETWORK_MAX_HANDSHAKE_TIMEOUT_MS 120000U
#define NETWORK_MAX_IDLE_TIMEOUT_MS 600000U

static uint32_t BoundedLength(const char *text, uint32_t limit)
{
    uint32_t length = 0;
    if (text == NULL)
    {
        return limit;
    }
    while (length < limit && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static bool IsAsciiDigit(char value)
{
    return value >= '0' && value <= '9';
}

static bool IsAsciiLetter(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

static bool IsAsciiHex(char value)
{
    return IsAsciiDigit(value) ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static uint8_t HexValue(char value)
{
    if (value >= '0' && value <= '9')
    {
        return (uint8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return (uint8_t)(10 + value - 'a');
    }
    return (uint8_t)(10 + value - 'A');
}

static char AsciiLower(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return (char)(value + ('a' - 'A'));
    }
    return value;
}

static bool ParsePort(const char *text, uint32_t length, uint16_t *outPort)
{
    uint32_t value = 0;
    if (text == NULL || outPort == NULL || length == 0 || length > 5U)
    {
        return false;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        if (!IsAsciiDigit(text[index]))
        {
            return false;
        }
        value = value * 10U + (uint32_t)(text[index] - '0');
        if (value > 65535U)
        {
            return false;
        }
    }
    if (value == 0)
    {
        return false;
    }
    *outPort = (uint16_t)value;
    return true;
}

static bool ParseIpv4(const char *text, uint32_t length)
{
    uint32_t octets = 0;
    uint32_t offset = 0;
    while (offset < length)
    {
        uint32_t start = offset;
        uint32_t value = 0;
        while (offset < length && text[offset] != '.')
        {
            if (!IsAsciiDigit(text[offset]) || offset - start >= 3U)
            {
                return false;
            }
            value = value * 10U + (uint32_t)(text[offset] - '0');
            ++offset;
        }
        uint32_t digits = offset - start;
        if (digits == 0 || value > 255U ||
            (digits > 1U && text[start] == '0'))
        {
            return false;
        }
        ++octets;
        if (offset == length)
        {
            break;
        }
        ++offset;
        if (offset == length)
        {
            return false;
        }
    }
    return octets == 4U;
}

static bool LooksLikeIpv4(const char *text, uint32_t length)
{
    bool hasDot = false;
    for (uint32_t index = 0; index < length; ++index)
    {
        if (text[index] == '.')
        {
            hasDot = true;
        }
        else if (!IsAsciiDigit(text[index]))
        {
            return false;
        }
    }
    return hasDot;
}

static bool ParseIpv6(const char *text, uint32_t length)
{
    uint32_t offset = 0;
    uint32_t groups = 0;
    bool compressed = false;

    if (length == 0)
    {
        return false;
    }
    if (text[0] == ':')
    {
        if (length < 2U || text[1] != ':')
        {
            return false;
        }
        compressed = true;
        offset = 2U;
        if (offset == length)
        {
            return true;
        }
    }

    while (offset < length)
    {
        uint32_t start = offset;
        bool hasDot = false;
        while (offset < length && text[offset] != ':')
        {
            hasDot = hasDot || text[offset] == '.';
            ++offset;
        }

        uint32_t tokenLength = offset - start;
        if (tokenLength == 0)
        {
            return false;
        }
        if (hasDot)
        {
            if (offset != length || groups > 6U ||
                !ParseIpv4(text + start, tokenLength))
            {
                return false;
            }
            groups += 2U;
            break;
        }
        if (tokenLength > 4U)
        {
            return false;
        }
        for (uint32_t index = start; index < offset; ++index)
        {
            if (!IsAsciiHex(text[index]))
            {
                return false;
            }
        }
        ++groups;
        if (groups > 8U)
        {
            return false;
        }

        if (offset == length)
        {
            break;
        }
        ++offset;
        if (offset == length)
        {
            return false;
        }
        if (text[offset] == ':')
        {
            if (compressed)
            {
                return false;
            }
            compressed = true;
            ++offset;
            if (offset == length)
            {
                break;
            }
        }
    }

    return compressed ? groups < 8U : groups == 8U;
}

static bool ParseDnsName(const char *text, uint32_t length)
{
    uint32_t effectiveLength = length;
    if (effectiveLength != 0 && text[effectiveLength - 1U] == '.')
    {
        --effectiveLength;
    }
    if (effectiveLength == 0 || effectiveLength > NETWORK_DNS_NAME_LIMIT)
    {
        return false;
    }

    uint32_t labelLength = 0;
    bool labelStartsWithHyphen = false;
    for (uint32_t index = 0; index <= effectiveLength; ++index)
    {
        char value = index == effectiveLength ? '.' : text[index];
        if (value == '.')
        {
            if (labelLength == 0 || labelLength > NETWORK_DNS_LABEL_LIMIT ||
                labelStartsWithHyphen || text[index - 1U] == '-')
            {
                return false;
            }
            labelLength = 0;
            labelStartsWithHyphen = false;
            continue;
        }
        if (!IsAsciiLetter(value) && !IsAsciiDigit(value) && value != '-')
        {
            return false;
        }
        if (labelLength == 0)
        {
            labelStartsWithHyphen = value == '-';
        }
        ++labelLength;
    }
    return true;
}

static void CopyCanonicalHost(char destination[LAIUE_NETWORK_HOST_CAPACITY],
                              const char *source, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index)
    {
        destination[index] = AsciiLower(source[index]);
    }
    destination[length] = '\0';
}

NetworkEndpointParseResult NetworkEndpointParse(
    const char *text, uint16_t defaultPort, NetworkEndpoint *outEndpoint)
{
    if (text == NULL || outEndpoint == NULL)
    {
        return NETWORK_ENDPOINT_PARSE_NULL;
    }
    if (defaultPort == 0)
    {
        return NETWORK_ENDPOINT_PARSE_INVALID_PORT;
    }

    uint32_t length = BoundedLength(text, NETWORK_ENDPOINT_TEXT_LIMIT);
    if (length == 0)
    {
        return NETWORK_ENDPOINT_PARSE_EMPTY;
    }
    if (length == NETWORK_ENDPOINT_TEXT_LIMIT)
    {
        return NETWORK_ENDPOINT_PARSE_TOO_LONG;
    }

    const char *host = text;
    uint32_t hostLength = length;
    uint16_t port = defaultPort;
    bool bracketed = text[0] == '[';

    if (bracketed)
    {
        uint32_t close = 1U;
        while (close < length && text[close] != ']')
        {
            ++close;
        }
        if (close == length || close == 1U)
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_IPV6;
        }
        host = text + 1;
        hostLength = close - 1U;
        if (close + 1U < length)
        {
            if (text[close + 1U] != ':' ||
                !ParsePort(text + close + 2U, length - close - 2U, &port))
            {
                return NETWORK_ENDPOINT_PARSE_INVALID_PORT;
            }
        }
        else if (close + 1U != length)
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_HOST;
        }
    }
    else
    {
        uint32_t colonCount = 0;
        uint32_t colonOffset = 0;
        for (uint32_t index = 0; index < length; ++index)
        {
            if (text[index] == ':')
            {
                ++colonCount;
                colonOffset = index;
            }
            else if ((uint8_t)text[index] <= 0x20U ||
                     (uint8_t)text[index] >= 0x7fU ||
                     text[index] == '/' || text[index] == '\\' ||
                     text[index] == '@' || text[index] == '#' ||
                     text[index] == '?' || text[index] == '[' ||
                     text[index] == ']')
            {
                return NETWORK_ENDPOINT_PARSE_INVALID_HOST;
            }
        }
        if (colonCount == 1U)
        {
            if (colonOffset == 0 ||
                !ParsePort(text + colonOffset + 1U,
                           length - colonOffset - 1U, &port))
            {
                return NETWORK_ENDPOINT_PARSE_INVALID_PORT;
            }
            hostLength = colonOffset;
        }
    }

    if (hostLength == 0 || hostLength >= LAIUE_NETWORK_HOST_CAPACITY)
    {
        return hostLength == 0 ? NETWORK_ENDPOINT_PARSE_EMPTY
                               : NETWORK_ENDPOINT_PARSE_TOO_LONG;
    }

    NetworkEndpointKind kind;
    if (ParseIpv4(host, hostLength))
    {
        if (bracketed)
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_IPV6;
        }
        kind = NETWORK_ENDPOINT_IPV4;
    }
    else if (ParseIpv6(host, hostLength))
    {
        kind = NETWORK_ENDPOINT_IPV6;
    }
    else
    {
        bool hasColon = false;
        for (uint32_t index = 0; index < hostLength; ++index)
        {
            hasColon = hasColon || host[index] == ':';
        }
        if (bracketed || hasColon)
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_IPV6;
        }
        if (LooksLikeIpv4(host, hostLength))
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_IPV4;
        }
        if (!ParseDnsName(host, hostLength))
        {
            return NETWORK_ENDPOINT_PARSE_INVALID_HOST;
        }
        kind = NETWORK_ENDPOINT_DNS;
    }

    memset(outEndpoint, 0, sizeof(*outEndpoint));
    CopyCanonicalHost(outEndpoint->host, host, hostLength);
    outEndpoint->port = port;
    outEndpoint->kind = kind;
    return NETWORK_ENDPOINT_PARSE_OK;
}

NetworkEndpointParseResult NetworkListenEndpointParse(
    const char *address, NetworkAddressFamily family, uint16_t port,
    NetworkEndpoint *outEndpoint)
{
    if (outEndpoint == NULL)
    {
        return NETWORK_ENDPOINT_PARSE_NULL;
    }
    if (port == 0)
    {
        return NETWORK_ENDPOINT_PARSE_INVALID_PORT;
    }
    if (family == NETWORK_ADDRESS_FAMILY_AUTO)
    {
        family = NETWORK_ADDRESS_FAMILY_DUAL;
    }
    if (family != NETWORK_ADDRESS_FAMILY_IPV4 &&
        family != NETWORK_ADDRESS_FAMILY_IPV6 &&
        family != NETWORK_ADDRESS_FAMILY_DUAL)
    {
        return NETWORK_ENDPOINT_PARSE_FAMILY_MISMATCH;
    }

    if (address == NULL || (address[0] == '*' && address[1] == '\0'))
    {
        memset(outEndpoint, 0, sizeof(*outEndpoint));
        outEndpoint->kind = NETWORK_ENDPOINT_WILDCARD;
        outEndpoint->port = port;
        if (family == NETWORK_ADDRESS_FAMILY_IPV4)
        {
            CopyCanonicalHost(outEndpoint->host, "0.0.0.0", 7U);
        }
        else
        {
            CopyCanonicalHost(outEndpoint->host, "::", 2U);
        }
        return NETWORK_ENDPOINT_PARSE_OK;
    }

    uint32_t addressLength =
        BoundedLength(address, NETWORK_ENDPOINT_TEXT_LIMIT);
    if (addressLength == NETWORK_ENDPOINT_TEXT_LIMIT)
    {
        return NETWORK_ENDPOINT_PARSE_TOO_LONG;
    }
    uint32_t colonCount = 0;
    for (uint32_t index = 0; index < addressLength; ++index)
    {
        colonCount += address[index] == ':' ? 1U : 0U;
        if (address[index] == '[' || address[index] == ']')
        {
            return NETWORK_ENDPOINT_PARSE_AMBIGUOUS;
        }
    }
    // listenAddress and port are separate configuration fields. A single
    // colon can only be host:port here (a complete IPv6 literal needs at
    // least two); reject it instead of silently overriding either value.
    if (colonCount == 1U)
    {
        return NETWORK_ENDPOINT_PARSE_AMBIGUOUS;
    }

    NetworkEndpoint parsed;
    NetworkEndpointParseResult result =
        NetworkEndpointParse(address, port, &parsed);
    if (result != NETWORK_ENDPOINT_PARSE_OK)
    {
        return result;
    }
    if (parsed.port != port)
    {
        return NETWORK_ENDPOINT_PARSE_AMBIGUOUS;
    }
    if (parsed.kind == NETWORK_ENDPOINT_DNS)
    {
        return NETWORK_ENDPOINT_PARSE_INVALID_HOST;
    }
    if ((parsed.kind == NETWORK_ENDPOINT_IPV4 &&
         family != NETWORK_ADDRESS_FAMILY_IPV4) ||
        (parsed.kind == NETWORK_ENDPOINT_IPV6 &&
         family != NETWORK_ADDRESS_FAMILY_IPV6))
    {
        return NETWORK_ENDPOINT_PARSE_FAMILY_MISMATCH;
    }
    *outEndpoint = parsed;
    return NETWORK_ENDPOINT_PARSE_OK;
}

static bool EqualAsciiInsensitive(const char *left, const char *right)
{
    uint32_t index = 0;
    if (left == NULL || right == NULL)
    {
        return false;
    }
    while (left[index] != '\0' && right[index] != '\0')
    {
        if (AsciiLower(left[index]) != AsciiLower(right[index]))
        {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

static bool BytesAreZero(const uint8_t *bytes, uint32_t size);

bool NetworkTrustParse(
    const char *text, NetworkTrustMode *outMode,
    uint8_t outSha256[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE])
{
    if (text == NULL || outMode == NULL || outSha256 == NULL)
    {
        return false;
    }
    memset(outSha256, 0, LAIUE_NETWORK_CERTIFICATE_PIN_SIZE);
    if (EqualAsciiInsensitive(text, "system"))
    {
        *outMode = NETWORK_TRUST_SYSTEM;
        return true;
    }

    static const char prefix[] = "sha256:";
    const uint32_t expectedLength =
        (uint32_t)(sizeof(prefix) - 1U) +
        LAIUE_NETWORK_CERTIFICATE_PIN_SIZE * 2U;
    if (BoundedLength(text, expectedLength + 1U) != expectedLength)
    {
        return false;
    }
    for (uint32_t index = 0; index < sizeof(prefix) - 1U; ++index)
    {
        if (AsciiLower(text[index]) != prefix[index])
        {
            return false;
        }
    }
    const char *hex = text + sizeof(prefix) - 1U;
    for (uint32_t index = 0; index < LAIUE_NETWORK_CERTIFICATE_PIN_SIZE; ++index)
    {
        char high = hex[index * 2U];
        char low = hex[index * 2U + 1U];
        if (!IsAsciiHex(high) || !IsAsciiHex(low))
        {
            memset(outSha256, 0, LAIUE_NETWORK_CERTIFICATE_PIN_SIZE);
            return false;
        }
        outSha256[index] = (uint8_t)((HexValue(high) << 4) | HexValue(low));
    }
    if (hex[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE * 2U] != '\0')
    {
        memset(outSha256, 0, LAIUE_NETWORK_CERTIFICATE_PIN_SIZE);
        return false;
    }
    if (BytesAreZero(outSha256, LAIUE_NETWORK_CERTIFICATE_PIN_SIZE))
    {
        return false;
    }
    *outMode = NETWORK_TRUST_SHA256;
    return true;
}

void NetworkClientConfigurationInitialize(
    NetworkClientConfiguration *configuration)
{
    if (configuration == NULL)
    {
        return;
    }
    memset(configuration, 0, sizeof(*configuration));
    configuration->structureSize = sizeof(*configuration);
    configuration->addressFamily = NETWORK_ADDRESS_FAMILY_AUTO;
    configuration->trustMode = NETWORK_TRUST_SYSTEM;
    configuration->handshakeTimeoutMs = 60000U;
    configuration->idleTimeoutMs = 15000U;
}

void NetworkServerConfigurationInitialize(
    NetworkServerConfiguration *configuration)
{
    if (configuration == NULL)
    {
        return;
    }
    memset(configuration, 0, sizeof(*configuration));
    configuration->structureSize = sizeof(*configuration);
    configuration->port = (uint16_t)LAIUE_NETWORK_DEFAULT_PORT;
    configuration->maximumPeers = (uint16_t)LAIUE_NETWORK_MAX_PEERS;
    configuration->addressFamily = NETWORK_ADDRESS_FAMILY_DUAL;
    configuration->listenAddress = "*";
    configuration->handshakeTimeoutMs = 60000U;
    configuration->idleTimeoutMs = 15000U;
}

static bool BytesAreZero(const uint8_t *bytes, uint32_t size)
{
    uint8_t combined = 0;
    for (uint32_t index = 0; index < size; ++index)
    {
        combined |= bytes[index];
    }
    return combined == 0;
}

static bool ValidTimeout(uint32_t value, uint32_t maximum)
{
    return value == 0 ||
           (value >= NETWORK_MIN_TIMEOUT_MS && value <= maximum);
}

bool NetworkClientConfigurationIsValid(
    const NetworkClientConfiguration *configuration)
{
    if (configuration == NULL ||
        configuration->structureSize != sizeof(*configuration) ||
        configuration->endpoint.port == 0 ||
        configuration->endpoint.host[0] == '\0' ||
        BoundedLength(configuration->endpoint.host,
                      LAIUE_NETWORK_HOST_CAPACITY) ==
            LAIUE_NETWORK_HOST_CAPACITY ||
        (configuration->endpoint.kind != NETWORK_ENDPOINT_DNS &&
         configuration->endpoint.kind != NETWORK_ENDPOINT_IPV4 &&
         configuration->endpoint.kind != NETWORK_ENDPOINT_IPV6) ||
        (configuration->addressFamily != NETWORK_ADDRESS_FAMILY_AUTO &&
         configuration->addressFamily != NETWORK_ADDRESS_FAMILY_IPV4 &&
         configuration->addressFamily != NETWORK_ADDRESS_FAMILY_IPV6) ||
        (configuration->trustMode != NETWORK_TRUST_SYSTEM &&
         configuration->trustMode != NETWORK_TRUST_SHA256) ||
        !ValidTimeout(configuration->handshakeTimeoutMs,
                      NETWORK_MAX_HANDSHAKE_TIMEOUT_MS) ||
        !ValidTimeout(configuration->idleTimeoutMs,
                      NETWORK_MAX_IDLE_TIMEOUT_MS))
    {
        return false;
    }
    if ((configuration->endpoint.kind == NETWORK_ENDPOINT_IPV4 &&
         configuration->addressFamily == NETWORK_ADDRESS_FAMILY_IPV6) ||
        (configuration->endpoint.kind == NETWORK_ENDPOINT_IPV6 &&
         configuration->addressFamily == NETWORK_ADDRESS_FAMILY_IPV4) ||
        configuration->endpoint.kind == NETWORK_ENDPOINT_WILDCARD)
    {
        return false;
    }
    bool pinIsZero = BytesAreZero(configuration->certificateSha256,
                                 LAIUE_NETWORK_CERTIFICATE_PIN_SIZE);
    return configuration->trustMode == NETWORK_TRUST_SHA256
               ? !pinIsZero
               : pinIsZero;
}

static bool NonemptyString(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static bool ValidCertificateThumbprint(const char *text)
{
    if (text == NULL)
    {
        return false;
    }
    // MsQuic's Schannel certificate-store selector is a SHA-1 thumbprint.
    // Configuration files use the canonical 40-hex representation without
    // whitespace, separators or a "sha1:" prefix.
    for (uint32_t index = 0; index < 40U; ++index)
    {
        if (!IsAsciiHex(text[index]))
        {
            return false;
        }
    }
    return text[40] == '\0';
}

bool NetworkServerConfigurationIsValid(
    const NetworkServerConfiguration *configuration)
{
    if (configuration == NULL ||
        configuration->structureSize != sizeof(*configuration) ||
        configuration->port == 0 ||
        configuration->maximumPeers == 0 ||
        configuration->maximumPeers > LAIUE_NETWORK_MAX_PEERS ||
        configuration->modCount > LAIUE_NETWORK_MAX_MODS ||
        (configuration->modCount != 0 && configuration->mods == NULL) ||
        (configuration->allowContentDownloads &&
         (configuration->contentBundle == NULL ||
          configuration->contentBundleSize == 0 ||
          configuration->contentBundleSize >
              LAIUE_NETWORK_MAX_CONTENT_BYTES)) ||
        !ValidTimeout(configuration->handshakeTimeoutMs,
                      NETWORK_MAX_HANDSHAKE_TIMEOUT_MS) ||
        !ValidTimeout(configuration->idleTimeoutMs,
                      NETWORK_MAX_IDLE_TIMEOUT_MS))
    {
        return false;
    }

    NetworkEndpoint listenEndpoint;
    if (NetworkListenEndpointParse(configuration->listenAddress,
                                   configuration->addressFamily,
                                   configuration->port,
                                   &listenEndpoint) !=
        NETWORK_ENDPOINT_PARSE_OK)
    {
        return false;
    }

    bool hasPemCertificate = NonemptyString(configuration->certificateFile);
    bool hasPemKey = NonemptyString(configuration->privateKeyFile);
    bool hasThumbprint =
        NonemptyString(configuration->certificateStoreThumbprint);
    if (hasPemCertificate != hasPemKey ||
        (hasThumbprint && hasPemCertificate))
    {
        return false;
    }
#if defined(_WIN32)
    return hasThumbprint &&
           ValidCertificateThumbprint(
               configuration->certificateStoreThumbprint);
#else
    (void)ValidCertificateThumbprint;
    return hasPemCertificate;
#endif
}
