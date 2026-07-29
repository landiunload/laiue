#include "network/network_datagram.h"
#include "test_runtime.h"

#include <string.h>

// Keep the fixed-capacity queue out of the no-CRT Windows stack frame.
static LaiueNetworkDatagramQueue g_queue;

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t MakePlayerStateDatagram(uint8_t output[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE])
{
    LaiueProtocolPlayerState state;
    memset(&state, 0, sizeof(state));
    state.serverTick = 120U;
    state.peerId = 7U;
    state.lastProcessedInputSequence = 99U;

    uint8_t payload[LAIUE_PROTOCOL_PLAYER_STATE_PAYLOAD_SIZE];
    uint32_t payloadSize = LaiueProtocolEncodePlayerState(payload, sizeof(payload), &state);
    Expect(payloadSize == LAIUE_PROTOCOL_PLAYER_STATE_PAYLOAD_SIZE,
           "player-state payload setup failed");
    return LaiueProtocolWriteFrame(output, LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE,
                                   LAIUE_MESSAGE_PLAYER_STATE, 17U, payload, payloadSize);
}

static void TestEnvelope(void)
{
    uint8_t bytes[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE + 1U];
    uint32_t size = MakePlayerStateDatagram(bytes);
    Expect(size == LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE,
           "player-state datagram has an unexpected size");

    LaiueProtocolFrame frame;
    LaiueProtocolPlayerState decoded;
    Expect(LaiueNetworkDatagramReadPlayerStateFrame(bytes, size, &frame) && frame.sequence == 17U &&
               LaiueProtocolDecodePlayerState(frame.payload, frame.payloadSize, &decoded) &&
               decoded.serverTick == 120U && decoded.peerId == 7U &&
               decoded.lastProcessedInputSequence == 99U,
           "valid player-state datagram did not round-trip");
    Expect(!LaiueNetworkDatagramReadPlayerStateFrame(bytes, size - 1U, &frame),
           "truncated player-state datagram was accepted");

    bytes[size] = 0U;
    Expect(!LaiueNetworkDatagramReadPlayerStateFrame(bytes, size + 1U, &frame),
           "oversized player-state datagram was accepted");

    uint8_t originalTypeLow = bytes[6];
    bytes[6] = (uint8_t)LAIUE_MESSAGE_BLOCK_DELTA;
    Expect(!LaiueNetworkDatagramReadPlayerStateFrame(bytes, size, &frame),
           "non-player message was accepted as a datagram");
    bytes[6] = originalTypeLow;

    uint8_t originalMagic = bytes[0];
    bytes[0] ^= 0xffU;
    Expect(!LaiueNetworkDatagramReadPlayerStateFrame(bytes, size, &frame),
           "datagram with invalid protocol magic was accepted");
    bytes[0] = originalMagic;

    uint8_t originalFlag = bytes[size - 1U];
    bytes[size - 1U] = 0x80U;
    Expect(LaiueNetworkDatagramReadPlayerStateFrame(bytes, size, &frame) &&
               !LaiueProtocolDecodePlayerState(frame.payload, frame.payloadSize, &decoded),
           "malformed player-state payload passed main-thread decoding");
    bytes[size - 1U] = originalFlag;
}

static void TestBoundedQueue(void)
{
    uint8_t bytes[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE + 1U];
    uint32_t size = MakePlayerStateDatagram(bytes);
    memset(&g_queue, 0, sizeof(g_queue));

    Expect(LaiueNetworkDatagramQueuePush(&g_queue, bytes, size) == LAIUE_NETWORK_DATAGRAM_ACCEPTED,
           "bounded datagram queue rejected a valid frame");
    uint8_t output[LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE];
    uint32_t outputSize = 0;
    Expect(LaiueNetworkDatagramQueuePop(&g_queue, output, &outputSize) && outputSize == size &&
               memcmp(output, bytes, size) == 0,
           "bounded datagram queue corrupted a frame");
    Expect(!LaiueNetworkDatagramQueuePop(&g_queue, output, &outputSize),
           "empty datagram queue produced a frame");
    Expect(LaiueNetworkDatagramQueuePush(&g_queue, bytes, size + 1U) ==
               LAIUE_NETWORK_DATAGRAM_OVERSIZE,
           "oversized datagram did not fail before copying");
    Expect(LaiueNetworkDatagramQueuePush(&g_queue, bytes, 0U) == LAIUE_NETWORK_DATAGRAM_INVALID,
           "zero-length datagram was accepted");

    for (uint32_t index = 0; index < LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY; ++index)
    {
        bytes[12] = (uint8_t)(index + 1U);
        Expect(LaiueNetworkDatagramQueuePush(&g_queue, bytes, size) ==
                   LAIUE_NETWORK_DATAGRAM_ACCEPTED,
               "bounded datagram queue filled too early");
    }
    Expect(LaiueNetworkDatagramQueuePush(&g_queue, bytes, size) == LAIUE_NETWORK_DATAGRAM_OVERFLOW,
           "bounded datagram queue did not report overflow");

    for (uint32_t index = 0; index < LAIUE_NETWORK_DATAGRAM_QUEUE_CAPACITY; ++index)
    {
        Expect(LaiueNetworkDatagramQueuePop(&g_queue, output, &outputSize) &&
                   output[12] == (uint8_t)(index + 1U),
               "bounded datagram queue lost FIFO ordering");
    }
}

static void TestReliableFallbackPolicy(void)
{
    const uint32_t size = LAIUE_NETWORK_PLAYER_STATE_DATAGRAM_SIZE;
    Expect(LaiueNetworkDatagramCanSendPlayerState(true, (uint16_t)size, 0U, size),
           "available datagram transport was not selected");
    Expect(!LaiueNetworkDatagramCanSendPlayerState(false, (uint16_t)size, 0U, size),
           "unavailable datagram transport did not fall back");
    Expect(!LaiueNetworkDatagramCanSendPlayerState(true, (uint16_t)(size - 1U), 0U, size),
           "insufficient peer datagram size did not fall back");
    Expect(!LaiueNetworkDatagramCanSendPlayerState(
               true, (uint16_t)size, LAIUE_NETWORK_MAX_OUTSTANDING_DATAGRAM_SENDS, size),
           "full datagram send queue did not fall back");
    Expect(!LaiueNetworkDatagramCanSendPlayerState(true, UINT16_MAX, 0U, size + 1U),
           "unexpected datagram layout bypassed reliable fallback");
}

static void TestFreshnessPolicy(void)
{
    LaiueNetworkPlayerStateFreshness freshness;
    memset(&freshness, 0, sizeof(freshness));
    Expect(!LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 100U),
           "state overtaking reliable roster activation was accepted");
    LaiueNetworkPlayerStateFreshnessActivate(&freshness, 7U);
    Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 100U),
           "first player state was rejected");
    Expect(!LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 100U) &&
               !LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 99U),
           "duplicate or out-of-order player state was accepted");
    Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 101U),
           "newer player state was rejected");

    memset(&freshness, 0, sizeof(freshness));
    LaiueNetworkPlayerStateFreshnessActivate(&freshness, 7U);
    Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, UINT32_MAX) &&
               LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 1U),
           "player-state freshness did not survive tick wrap");
    LaiueNetworkPlayerStateFreshnessRetire(&freshness, 7U);
    Expect(!LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 2U),
           "late state resurrected a retired peer");
    LaiueNetworkPlayerStateFreshnessActivate(&freshness, 7U);
    Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, 7U, 1U),
           "reactivated peer retained stale state");

    memset(&freshness, 0, sizeof(freshness));
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        LaiueNetworkPlayerStateFreshnessActivate(&freshness, index + 1U);
        Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, index + 1U, 1U),
               "freshness table filled before maximum peers");
    }
    Expect(!LaiueNetworkPlayerStateFreshnessAccept(&freshness, LAIUE_NETWORK_MAX_PEERS + 1U, 1U),
           "freshness table accepted more than maximum peers");
    LaiueNetworkPlayerStateFreshnessActivate(&freshness, LAIUE_NETWORK_MAX_PEERS + 1U);
    Expect(!LaiueNetworkPlayerStateFreshnessAccept(&freshness, LAIUE_NETWORK_MAX_PEERS + 1U, 1U) &&
               LaiueNetworkPlayerStateFreshnessAccept(&freshness, 1U, 2U),
           "activation evicted an active peer from a full table");
    LaiueNetworkPlayerStateFreshnessRetire(&freshness, 1U);
    LaiueNetworkPlayerStateFreshnessActivate(&freshness, LAIUE_NETWORK_MAX_PEERS + 1U);
    Expect(LaiueNetworkPlayerStateFreshnessAccept(&freshness, LAIUE_NETWORK_MAX_PEERS + 1U, 1U) &&
               !LaiueNetworkPlayerStateFreshnessAccept(&freshness, 1U, 2U),
           "retired freshness slot was not safely reused");
}

LAIUE_TEST_ENTRY(NetworkDatagramTestEntryPoint)
{
    TestEnvelope();
    TestBoundedQueue();
    TestReliableFallbackPolicy();
    TestFreshnessPolicy();
    LaiueTestRuntimeWrite("Network datagram policy: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
