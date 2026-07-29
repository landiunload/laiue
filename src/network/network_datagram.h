#pragma once

#include "network/network.h"
#include "network/protocol.h"

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY 64U
#define LAIUE_NETWORK_MAX_OUTSTANDING_DATAGRAM_SENDS 64U
#define LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE                                                   \
    (LAIUE_PROTOCOL_HEADER_SIZE + LAIUE_PROTOCOL_PLAYER_STATE_PAYLOAD_SIZE)

typedef enum LaiueNetworkDatagramPushResult
{
    LAIUE_NETWORK_DATAGRAM_ACCEPTED = 0,
    LAIUE_NETWORK_DATAGRAM_INVALID,
    LAIUE_NETWORK_DATAGRAM_OVERSIZE,
    LAIUE_NETWORK_DATAGRAM_OVERFLOW
} LaiueNetworkDatagramPushResult;

typedef struct LaiueNetworkDatagram
{
    uint16_t size;
    uint8_t bytes[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE];
} LaiueNetworkDatagram;

typedef struct LaiueNetworkDatagramQueue
{
    LaiueNetworkDatagram items[LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY];
    uint32_t read;
    uint32_t write;
    uint32_t count;
} LaiueNetworkDatagramQueue;

typedef struct LaiueNetworkPlayerStateFreshnessEntry
{
    uint32_t peerId;
    uint32_t serverTick;
    bool hasServerTick;
    bool retired;
} LaiueNetworkPlayerStateFreshnessEntry;

typedef struct LaiueNetworkPlayerStateFreshness
{
    LaiueNetworkPlayerStateFreshnessEntry entries[LAIUE_NETWORK_MAX_PEERS];
} LaiueNetworkPlayerStateFreshness;

// The caller serializes access. This keeps the transport callback in control
// of its existing per-connection lock and makes the bounded queue testable
// without creating a second lock or allocator.
LaiueNetworkDatagramPushResult LaiueNetworkDatagramQueuePush(LaiueNetworkDatagramQueue *queue,
                                                             const uint8_t *bytes, uint32_t size);
bool LaiueNetworkDatagramQueuePop(LaiueNetworkDatagramQueue *queue,
                                  uint8_t output[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE],
                                  uint32_t *outSize);

// Validates the complete datagram envelope. Player-state payload validation is
// still performed by LaiueProtocolDecodePlayerState on the main thread.
bool LaiueNetworkDatagramReadPlayerStateFrame(const uint8_t *bytes, uint32_t size,
                                              LaiueProtocolFrame *outFrame);

// A false result means that the caller must preserve compatibility by using
// the reliable control stream.
bool LaiueNetworkDatagramCanSendPlayerState(bool sendEnabled, uint16_t maximumSendLength,
                                            uint32_t outstandingSends, uint32_t frameSize);

// Returns false for an unknown peer, duplicate, stale or out-of-order state.
// Such transient state is dropped rather than treated as a protocol error.
bool LaiueNetworkPlayerStateFreshnessAccept(LaiueNetworkPlayerStateFreshness *freshness,
                                            uint32_t peerId, uint32_t serverTick);
void LaiueNetworkPlayerStateFreshnessActivate(LaiueNetworkPlayerStateFreshness *freshness,
                                              uint32_t peerId);
void LaiueNetworkPlayerStateFreshnessRetire(LaiueNetworkPlayerStateFreshness *freshness,
                                            uint32_t peerId);
