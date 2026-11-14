#include "net/net_internal.h"

uint32_t NetVarintSize(uint64_t value)
{
    uint32_t size = 1u;
    while (value >= 0x80u)
    {
        value >>= 7u;
        ++size;
    }
    return size;
}

uint32_t NetVarintWrite(uint8_t *output, uint64_t value)
{
    uint32_t size = 0u;
    while (value >= 0x80u)
    {
        output[size] = (uint8_t)(value | 0x80u);
        value >>= 7u;
        ++size;
    }
    output[size] = (uint8_t)value;
    return size + 1u;
}

bool NetVarintRead(const uint8_t **cursor, const uint8_t *end, uint64_t *outValue)
{
    const uint8_t *position = *cursor;
    uint64_t value = 0u;
    for (uint32_t shift = 0u; shift < 64u; shift += 7u)
    {
        if (position == end)
            return false;
        const uint8_t byte = *position;
        ++position;
        /* Десятый байт несёт один бит: остальное в 64 бита не помещается. */
        if (shift == 63u && byte > 1u)
            return false;
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u)
        {
            *cursor = position;
            *outValue = value;
            return true;
        }
    }
    return false;
}

void NetParametersWrite(const NetParameters *parameters, uint8_t output[NET_PARAMETERS_SIZE])
{
    NetStore16(output, parameters->datagramBytes);
    output[2] = (uint8_t)parameters->channelCount;
    output[3] = parameters->ackClock ? NET_PARAMETERS_ACK_CLOCK : 0u;
    NetStore32(output + 4, parameters->maxMessageBytes);
    NetStore32(output + 8, parameters->windowBytes);
    NetStore32(output + 12, parameters->idleMilliseconds);
}

bool NetParametersRead(const uint8_t input[NET_PARAMETERS_SIZE], NetParameters *outParameters)
{
    outParameters->datagramBytes = NetLoad16(input);
    outParameters->channelCount = input[2];
    outParameters->ackClock = (input[3] & NET_PARAMETERS_ACK_CLOCK) != 0u;
    outParameters->maxMessageBytes = NetLoad32(input + 4);
    outParameters->windowBytes = NetLoad32(input + 8);
    outParameters->idleMilliseconds = NetLoad32(input + 12);
    return outParameters->datagramBytes >= NET_DATAGRAM_MINIMUM &&
           outParameters->datagramBytes <= NET_DATAGRAM_MAXIMUM &&
           outParameters->channelCount >= 1u &&
           outParameters->channelCount <= LAIUE_NET_MAX_CHANNELS &&
           outParameters->maxMessageBytes <= NET_MESSAGE_LIMIT &&
           outParameters->windowBytes >= NET_WINDOW_MINIMUM &&
           outParameters->windowBytes <= NET_WINDOW_LIMIT &&
           outParameters->idleMilliseconds >= 1000u;
}
