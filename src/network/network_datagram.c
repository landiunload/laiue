#include "network/network_datagram.h"

#include <string.h>

LaiueNetworkDatagramPushResult LaiueNetworkDatagramQueuePush(LaiueNetworkDatagramQueue *queue,
                                                             const uint8_t *bytes, uint32_t size)
{
    if (queue == NULL || bytes == NULL || size == 0)
    {
        return LAIUE_NETWORK_DATAGRAM_INVALID;
    }
    if (size > LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE)
    {
        return LAIUE_NETWORK_DATAGRAM_OVERSIZE;
    }
    if (queue->count >= LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY)
    {
        return LAIUE_NETWORK_DATAGRAM_OVERFLOW;
    }

    LaiueNetworkDatagram *datagram = &queue->items[queue->write];
    memcpy(datagram->bytes, bytes, size);
    datagram->size = (uint16_t)size;
    queue->write = (queue->write + 1U) % LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY;
    ++queue->count;
    return LAIUE_NETWORK_DATAGRAM_ACCEPTED;
}

bool LaiueNetworkDatagramQueuePop(LaiueNetworkDatagramQueue *queue,
                                  uint8_t output[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE],
                                  uint32_t *outSize)
{
    if (queue == NULL || output == NULL || outSize == NULL || queue->count == 0)
    {
        return false;
    }

    const LaiueNetworkDatagram *datagram = &queue->items[queue->read];
    memcpy(output, datagram->bytes, datagram->size);
    *outSize = datagram->size;
    queue->read = (queue->read + 1U) % LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY;
    --queue->count;
    return true;
}

bool LaiueNetworkDatagramReadPlayerStateFrame(const uint8_t *bytes, uint32_t size,
                                              LaiueProtocolFrame *outFrame)
{
    if (bytes == NULL || outFrame == NULL || size != LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE ||
        !LaiueProtocolReadHeader(bytes, size, outFrame) ||
        outFrame->type != LAIUE_MESSAGE_PLAYER_STATE ||
        outFrame->payloadSize != LAIUE_PROTOCOL_PLAYER_STATE_PAYLOAD_SIZE ||
        LAIUE_PROTOCOL_HEADER_SIZE + outFrame->payloadSize != size)
    {
        return false;
    }
    return true;
}

bool LaiueNetworkDatagramCanSendPlayerState(bool sendEnabled, uint16_t maximumSendLength,
                                            uint32_t outstandingSends, uint32_t frameSize)
{
    return sendEnabled && frameSize == LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE &&
           frameSize <= maximumSendLength &&
           outstandingSends < LAIUE_NETWORK_MAX_OUTSTANDING_DATAGRAM_SENDS;
}

static bool SequenceIsNewer(uint32_t candidate, uint32_t reference)
{
    uint32_t distance = candidate - reference;
    return distance != 0 && distance < 0x80000000U;
}

bool LaiueNetworkPlayerStateFreshnessAccept(LaiueNetworkPlayerStateFreshness *freshness,
                                            uint32_t peerId, uint32_t serverTick)
{
    if (freshness == NULL || peerId == 0)
    {
        return false;
    }

    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        LaiueNetworkPlayerStateFreshnessEntry *entry = &freshness->entries[index];
        if (entry->peerId == peerId)
        {
            if (entry->retired)
            {
                return false;
            }
            if (!entry->hasServerTick)
            {
                entry->serverTick = serverTick;
                entry->hasServerTick = true;
                return true;
            }
            if (!SequenceIsNewer(serverTick, entry->serverTick))
            {
                return false;
            }
            entry->serverTick = serverTick;
            return true;
        }
    }
    // DATAGRAM can overtake the reliable PLAYER_JOINED/roster record.
    // Unknown peers are dropped until that control record activates them.
    return false;
}

void LaiueNetworkPlayerStateFreshnessActivate(LaiueNetworkPlayerStateFreshness *freshness,
                                              uint32_t peerId)
{
    if (freshness == NULL || peerId == 0)
    {
        return;
    }
    uint32_t reusableIndex = LAIUE_NETWORK_MAX_PEERS;
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        LaiueNetworkPlayerStateFreshnessEntry *entry = &freshness->entries[index];
        if (entry->peerId == peerId)
        {
            if (entry->retired)
            {
                memset(entry, 0, sizeof(*entry));
                entry->peerId = peerId;
            }
            return;
        }
        if ((entry->peerId == 0 || entry->retired) && reusableIndex == LAIUE_NETWORK_MAX_PEERS)
        {
            reusableIndex = index;
        }
    }
    if (reusableIndex != LAIUE_NETWORK_MAX_PEERS)
    {
        LaiueNetworkPlayerStateFreshnessEntry *entry = &freshness->entries[reusableIndex];
        memset(entry, 0, sizeof(*entry));
        entry->peerId = peerId;
    }
}

void LaiueNetworkPlayerStateFreshnessRetire(LaiueNetworkPlayerStateFreshness *freshness,
                                            uint32_t peerId)
{
    if (freshness == NULL || peerId == 0)
    {
        return;
    }
    uint32_t reusableIndex = LAIUE_NETWORK_MAX_PEERS;
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        LaiueNetworkPlayerStateFreshnessEntry *entry = &freshness->entries[index];
        if (entry->peerId == peerId)
        {
            entry->retired = true;
            return;
        }
        if ((entry->peerId == 0 || entry->retired) && reusableIndex == LAIUE_NETWORK_MAX_PEERS)
        {
            reusableIndex = index;
        }
    }
    if (reusableIndex != LAIUE_NETWORK_MAX_PEERS)
    {
        LaiueNetworkPlayerStateFreshnessEntry *entry = &freshness->entries[reusableIndex];
        memset(entry, 0, sizeof(*entry));
        entry->peerId = peerId;
        entry->retired = true;
    }
}
