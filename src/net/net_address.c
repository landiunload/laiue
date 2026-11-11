#include "net/net.h"

#include <stddef.h>
#include <string.h>

/* Текстовая запись адресов. Разбор строгий: лишние символы, пустые группы и
 * числа с ведущими нулями отвергаются, чтобы одна строка не читалась двумя
 * способами. */

static bool ParseDecimal(const char *begin, const char *end, uint32_t limit, uint32_t *outValue)
{
    uint32_t value = 0u;
    if (begin == end || end - begin > 10 || (begin[0] == '0' && end - begin > 1))
        return false;
    for (const char *cursor = begin; cursor != end; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return false;
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (limit - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    *outValue = value;
    return true;
}

static const char *FindCharacter(const char *begin, const char *end, char wanted)
{
    while (begin != end && *begin != wanted)
        ++begin;
    return begin;
}

static bool ParseIpv4(const char *begin, const char *end, uint8_t output[4])
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const char *dot = FindCharacter(begin, end, '.');
        uint32_t octet = 0u;
        /* Точка после последнего октета недопустима, а до него обязательна. */
        if ((dot == end) != (index == 3u) || !ParseDecimal(begin, dot, 255u, &octet))
            return false;
        output[index] = (uint8_t)octet;
        begin = dot == end ? end : dot + 1;
    }
    return true;
}

static bool ParseGroup(const char *begin, const char *end, uint32_t *outValue)
{
    uint32_t value = 0u;
    if (begin == end || end - begin > 4)
        return false;
    for (const char *cursor = begin; cursor != end; ++cursor)
    {
        uint32_t digit = 0u;
        if (*cursor >= '0' && *cursor <= '9')
            digit = (uint32_t)(*cursor - '0');
        else if (*cursor >= 'a' && *cursor <= 'f')
            digit = (uint32_t)(*cursor - 'a') + 10u;
        else if (*cursor >= 'A' && *cursor <= 'F')
            digit = (uint32_t)(*cursor - 'A') + 10u;
        else
            return false;
        value = value * 16u + digit;
    }
    *outValue = value;
    return true;
}

/* Восемь групп по 16 бит; "::" заменяет одну или несколько нулевых групп;
 * последние две группы могут быть записаны как IPv4. */
static bool ParseIpv6(const char *begin, const char *end, uint8_t output[16])
{
    uint32_t groups[8] = {0u};
    uint32_t count = 0u;
    uint32_t gap = 8u;
    const char *cursor = begin;
    if (end - cursor >= 2 && cursor[0] == ':' && cursor[1] == ':')
    {
        gap = 0u;
        cursor += 2;
    }
    while (cursor != end)
    {
        const char *tokenEnd = FindCharacter(cursor, end, ':');
        if (FindCharacter(cursor, tokenEnd, '.') != tokenEnd)
        {
            uint8_t embedded[4];
            if (tokenEnd != end || count > 6u || !ParseIpv4(cursor, tokenEnd, embedded))
                return false;
            groups[count] = ((uint32_t)embedded[0] << 8u) | embedded[1];
            groups[count + 1u] = ((uint32_t)embedded[2] << 8u) | embedded[3];
            count += 2u;
            break;
        }
        if (count == 8u || !ParseGroup(cursor, tokenEnd, &groups[count]))
            return false;
        ++count;
        cursor = tokenEnd;
        if (cursor == end)
            break;
        if (end - cursor >= 2 && cursor[1] == ':')
        {
            if (gap != 8u)
                return false;
            gap = count;
            cursor += 2;
        }
        else
        {
            ++cursor;
            /* Одиночное двоеточие в конце ничего не разделяет. */
            if (cursor == end)
                return false;
        }
    }
    if (gap == 8u)
    {
        if (count != 8u)
            return false;
    }
    else
    {
        if (count == 8u)
            return false;
        /* Группы после "::" прижимаются к концу, середина остаётся нулевой. */
        const uint32_t tail = count - gap;
        for (uint32_t index = 0u; index < tail; ++index)
            groups[7u - index] = groups[count - 1u - index];
        for (uint32_t index = gap; index < 8u - tail; ++index)
            groups[index] = 0u;
    }
    for (size_t index = 0u; index < 8u; ++index)
    {
        output[index * 2u] = (uint8_t)(groups[index] >> 8u);
        output[index * 2u + 1u] = (uint8_t)groups[index];
    }
    return true;
}

LaiueNetStatus LaiueNetAddressParse(const char *text, LaiueNetAddress *outAddress)
{
    if (text == NULL || outAddress == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    LaiueNetAddress address;
    memset(&address, 0, sizeof(address));
    const char *begin = text;
    const char *end = text;
    uint32_t colons = 0u;
    while (*end != '\0')
    {
        if (*end == ':')
            ++colons;
        ++end;
        if (end - text > 128)
            return LAIUE_NET_INVALID_ARGUMENT;
    }
    const char *hostEnd = end;
    const char *portBegin = end;
    bool ipv6 = colons >= 2u;
    if (*begin == '[')
    {
        /* Скобки отделяют адрес IPv6 от порта. */
        const char *bracket = FindCharacter(begin, end, ']');
        if (bracket == end || (bracket + 1 != end && bracket[1] != ':'))
            return LAIUE_NET_INVALID_ARGUMENT;
        ++begin;
        hostEnd = bracket;
        portBegin = bracket + 1 != end ? bracket + 2 : end;
        if (bracket + 1 != end && portBegin == end)
            return LAIUE_NET_INVALID_ARGUMENT;
        ipv6 = true;
    }
    else if (colons == 1u)
    {
        hostEnd = FindCharacter(begin, end, ':');
        portBegin = hostEnd + 1;
        if (portBegin == end)
            return LAIUE_NET_INVALID_ARGUMENT;
    }
    if (portBegin != end)
    {
        uint32_t port = 0u;
        if (!ParseDecimal(portBegin, end, 65535u, &port))
            return LAIUE_NET_INVALID_ARGUMENT;
        address.port = (uint16_t)port;
    }
    if (ipv6)
    {
        const char *zone = FindCharacter(begin, hostEnd, '%');
        if (zone != hostEnd && !ParseDecimal(zone + 1, hostEnd, UINT32_MAX, &address.scope))
            return LAIUE_NET_INVALID_ARGUMENT;
        if (!ParseIpv6(begin, zone, address.ip))
            return LAIUE_NET_INVALID_ARGUMENT;
        address.family = LAIUE_NET_FAMILY_IPV6;
    }
    else
    {
        if (!ParseIpv4(begin, hostEnd, address.ip))
            return LAIUE_NET_INVALID_ARGUMENT;
        address.family = LAIUE_NET_FAMILY_IPV4;
    }
    *outAddress = address;
    return LAIUE_NET_OK;
}

typedef struct TextWriter
{
    char *output;
    uint32_t capacity;
    uint32_t length;
    bool overflow;
} TextWriter;

static void WriteCharacter(TextWriter *writer, char character)
{
    /* Последняя ячейка — под завершающий ноль. */
    if (writer->length + 1u >= writer->capacity)
    {
        writer->overflow = true;
        return;
    }
    writer->output[writer->length] = character;
    ++writer->length;
}

static void WriteNumber(TextWriter *writer, uint32_t value, uint32_t base)
{
    static const char digits[] = "0123456789abcdef";
    char reversed[10];
    uint32_t count = 0u;
    do
    {
        reversed[count] = digits[value % base];
        value /= base;
        ++count;
    } while (value != 0u);
    while (count != 0u)
    {
        --count;
        WriteCharacter(writer, reversed[count]);
    }
}

static void WriteIpv6(TextWriter *writer, const uint8_t ip[16])
{
    uint32_t groups[8];
    /* Самая длинная цепочка нулевых групп, не короче двух, сжимается в "::";
     * из равных берётся левая. */
    uint32_t bestStart = 8u;
    uint32_t bestLength = 1u;
    uint32_t runStart = 0u;
    uint32_t runLength = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        groups[index] = ((uint32_t)ip[(size_t)index * 2u] << 8u) | ip[(size_t)index * 2u + 1u];
        if (groups[index] != 0u)
        {
            runLength = 0u;
            continue;
        }
        if (runLength == 0u)
            runStart = index;
        ++runLength;
        if (runLength > bestLength)
        {
            bestStart = runStart;
            bestLength = runLength;
        }
    }
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        if (index == bestStart)
        {
            WriteCharacter(writer, ':');
            WriteCharacter(writer, ':');
            index += bestLength - 1u;
            continue;
        }
        if (index != 0u && index != bestStart + bestLength)
            WriteCharacter(writer, ':');
        WriteNumber(writer, groups[index], 16u);
    }
}

uint32_t LaiueNetAddressFormat(const LaiueNetAddress *address, char *output, uint32_t capacity)
{
    if (address == NULL || output == NULL || capacity == 0u)
        return 0u;
    TextWriter writer = {.output = output, .capacity = capacity};
    output[0] = '\0';
    if (address->family == LAIUE_NET_FAMILY_IPV4)
    {
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            if (index != 0u)
                WriteCharacter(&writer, '.');
            WriteNumber(&writer, address->ip[index], 10u);
        }
    }
    else if (address->family == LAIUE_NET_FAMILY_IPV6)
    {
        if (address->port != 0u)
            WriteCharacter(&writer, '[');
        WriteIpv6(&writer, address->ip);
        if (address->scope != 0u)
        {
            WriteCharacter(&writer, '%');
            WriteNumber(&writer, address->scope, 10u);
        }
        if (address->port != 0u)
            WriteCharacter(&writer, ']');
    }
    else
    {
        return 0u;
    }
    if (address->port != 0u)
    {
        WriteCharacter(&writer, ':');
        WriteNumber(&writer, address->port, 10u);
    }
    if (writer.overflow)
    {
        output[0] = '\0';
        return 0u;
    }
    output[writer.length] = '\0';
    return writer.length;
}

bool LaiueNetAddressEqual(const LaiueNetAddress *first, const LaiueNetAddress *second)
{
    if (first == NULL || second == NULL || first->family != second->family ||
        first->port != second->port)
        return false;
    if (first->family == LAIUE_NET_FAMILY_IPV4)
        return memcmp(first->ip, second->ip, 4u) == 0;
    return first->scope == second->scope && memcmp(first->ip, second->ip, sizeof(first->ip)) == 0;
}
