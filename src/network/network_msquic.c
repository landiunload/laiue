#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "network/certificate_validation.h"
#include "network/network.h"
#include "network/protocol.h"
#include "platform/system.h"

#if !defined(LAIUE_HAS_MSQUIC)
#error "network_msquic.c requires LAIUE_HAS_MSQUIC"
#endif

#include <msquic.h>

#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#define NETWORK_RECEIVE_CAPACITY 16384U
#define NETWORK_CONTROL_RECEIVE_CAPACITY \
    (LAIUE_PROTOCOL_MAX_QUEUED_FRAMES * \
     LAIUE_PROTOCOL_MAX_FRAME_SIZE)
#define NETWORK_AUX_RECEIVE_CAPACITY \
    ((LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS + 2U) * \
     LAIUE_PROTOCOL_MAX_FRAME_SIZE)
#define NETWORK_CLIENT_EVENT_CAPACITY 128U
#define NETWORK_SERVER_EVENT_CAPACITY LAIUE_NETWORK_MAX_QUEUED_EVENTS
#define NETWORK_HANDSHAKE_TIMEOUT_MS 60000ULL
#define NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS 120000ULL
#define NETWORK_IDLE_TIMEOUT_MS 15000ULL
#define NETWORK_SHUTDOWN_DRAIN_TIMEOUT_MS 5000ULL
#define NETWORK_RATE_WINDOW_MS 1000ULL
#define NETWORK_MAX_FRAMES_PER_SECOND 160U
#define NETWORK_MAX_INPUTS_PER_SECOND 120U
#define NETWORK_MAX_EDITS_PER_SECOND 16U
#define NETWORK_MAX_RESYNCS_PER_SECOND 1U
#define NETWORK_CONTROL_PAYLOAD_CAPACITY LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE
#define NETWORK_AUX_MAX_OUTSTANDING_SENDS \
    (LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS + 2U)
#define NETWORK_APP_ERROR_PROTOCOL 0x100U
#define NETWORK_APP_ERROR_OVERFLOW 0x101U
#define NETWORK_APP_ERROR_SHUTDOWN 0x102U

typedef struct QuicChannel QuicChannel;

typedef struct QuicSend
{
    QuicChannel *channel;
    QUIC_BUFFER buffer;
    uint8_t bytes[1];
} QuicSend;

struct QuicChannel
{
    const QUIC_API_TABLE *api;
    PlatformRwLock *lock;
    HQUIC stream;
    uint8_t inlineReceive[NETWORK_RECEIVE_CAPACITY];
    uint8_t *receive;
    uint32_t receiveCapacity;
    uint32_t receiveSize;
    uint32_t receiveSequence;
    uint32_t sendSequence;
    uint32_t outstandingSends;
    uint64_t connectedAtMs;
    uint64_t lastReceiveAtMs;
    uint64_t lastSendAtMs;
    NetworkDisconnectReason callbackError;
    bool streamStarted;
    bool peerSendClosed;
    bool auxiliary;
    bool shutdownComplete;
};

typedef struct NetworkServerPeer NetworkServerPeer;

struct NetworkClient
{
    const QUIC_API_TABLE *api;
    HQUIC registration;
    HQUIC configuration;
    HQUIC connection;
    PlatformRwLock lock;
    QuicChannel channel;
    NetworkClientEvent events[NETWORK_CLIENT_EVENT_CAPACITY];
    uint32_t eventRead;
    uint32_t eventWrite;
    uint32_t eventCount;
    NetworkConnectionState state;
    NetworkDisconnectReason callbackReason;
    NetworkClientConfiguration connectionConfiguration;
    uint8_t controlReceive[NETWORK_CONTROL_RECEIVE_CAPACITY];
    uint8_t serverReceive[
        LAIUE_PROTOCOL_MAX_SERVER_STREAMS]
        [NETWORK_AUX_RECEIVE_CAPACITY];
    QuicChannel serverChannels[LAIUE_PROTOCOL_MAX_SERVER_STREAMS];
    uint64_t clientNonce;
    uint64_t snapshotId;
    uint64_t snapshotWorldRevision;
    int64_t resyncRequestChunk[3];
    uint64_t resyncExpectedRevision;
    uint32_t snapshotServerTick;
    uint32_t expectedSnapshotChunks;
    uint32_t receivedSnapshotChunks;
    int64_t snapshotPartChunk[3];
    uint64_t snapshotPartRevision;
    uint32_t snapshotLastEditIndex;
    uint16_t snapshotNextPart;
    uint16_t snapshotPartCount;
    uint32_t pendingPeerId;
    int64_t pendingWorldSeed;
    NetworkModDescriptor serverMods[LAIUE_NETWORK_MAX_MODS];
    uint32_t expectedServerModCount;
    uint32_t receivedServerModCount;
    uint8_t *contentBytes;
    uint64_t expectedContentSize;
    uint64_t receivedContentSize;
    uint8_t expectedContentHash[LAIUE_NETWORK_CONTENT_HASH_SIZE];
    bool serverDownloadsAllowed;
    bool modsSubmitted;
    bool contentRequested;
    bool contentVerified;
    bool transportConnected;
    bool transportConnectedHandled;
    bool certificateVerified;
    bool connectionStarted;
    bool stopping;
    bool shutdownComplete;
    bool shutdownRequested;
    bool disconnectNotified;
    bool snapshotStarted;
    bool snapshotEnded;
    bool snapshotRequiresReadyBarrier;
    bool syncBeginReceived;
    bool syncReadyReceived;
    bool syncAppliedSent;
    bool everReady;
    bool resyncRequestOutstanding;
    bool resyncResponseMatched;
};

struct NetworkServer
{
    const QUIC_API_TABLE *api;
    HQUIC registration;
    HQUIC configuration;
    HQUIC listener;
    PlatformRwLock lock;
    NetworkServerPeer *callbackPeer;
    NetworkServerEvent events[NETWORK_SERVER_EVENT_CAPACITY];
    uint32_t eventRead;
    uint32_t eventWrite;
    uint32_t eventCount;
    uint32_t nextPeerId;
    uint16_t maximumPeers;
    int64_t worldSeed;
    NetworkModDescriptor mods[LAIUE_NETWORK_MAX_MODS];
    uint32_t modCount;
    const uint8_t *contentBundle;
    uint64_t contentBundleSize;
    uint8_t contentBundleHash[LAIUE_NETWORK_CONTENT_HASH_SIZE];
    uint64_t nextBlockRevision;
    uint32_t handshakeTimeoutMs;
    uint32_t idleTimeoutMs;
    bool allowContentDownloads;
    bool stopping;
    NetworkServerPeer *peers;
};

struct NetworkServerPeer
{
    NetworkServer *server;
    HQUIC connection;
    QuicChannel channel;
    QuicChannel snapshotChannel;
    QuicChannel contentChannel;
    uint32_t peerId;
    uint64_t rateWindowStartMs;
    uint32_t framesInWindow;
    uint32_t inputsInWindow;
    uint32_t editsInWindow;
    uint32_t resyncsInWindow;
    NetworkModDescriptor mods[LAIUE_NETWORK_MAX_MODS];
    uint64_t clientNonce;
    uint32_t expectedModCount;
    uint32_t receivedModCount;
    uint64_t contentOffset;
    uint64_t snapshotId;
    uint64_t snapshotWorldRevision;
    int64_t resyncRequestChunk[3];
    uint64_t resyncExpectedRevision;
    uint32_t expectedSnapshotChunks;
    uint32_t sentSnapshotChunks;
    int64_t snapshotPartChunk[3];
    uint64_t snapshotPartRevision;
    uint32_t snapshotLastEditIndex;
    uint16_t snapshotNextPart;
    uint16_t snapshotPartCount;
    NetworkDisconnectReason callbackReason;
    NetworkDisconnectReason closeReason;
    bool allocated;
    bool listenerReady;
    bool secureConnected;
    bool shutdownComplete;
    bool closing;
    bool helloReceived;
    bool modListReceived;
    bool negotiated;
    bool ready;
    bool everReady;
    bool snapshotStarted;
    bool snapshotRequiresReadyBarrier;
    bool awaitingSyncApplied;
    bool resyncRequestOutstanding;
    bool resyncResponseMatched;
    bool contentTransferActive;
    bool rejected;
    bool connectedNotified;
};

_Static_assert(LAIUE_NETWORK_MAX_CHUNK_EDITS ==
                   LAIUE_PROTOCOL_MAX_CHUNK_EDITS,
               "Public and wire chunk edit limits diverged");
_Static_assert(LAIUE_NETWORK_MAX_SNAPSHOT_CHUNKS ==
                   LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS,
               "Public and wire snapshot limits diverged");
_Static_assert(LAIUE_NETWORK_MAX_CHUNK_PARTS ==
                   LAIUE_PROTOCOL_MAX_CHUNK_PARTS,
               "Public and wire chunk part limits diverged");
_Static_assert(sizeof(NetworkModDescriptor) == sizeof(LaiueProtocolMod),
               "Public and wire mod descriptors diverged");

static uint32_t AsciiLength(const char *text, uint32_t capacity)
{
    uint32_t length = 0;
    if (text == NULL)
    {
        return capacity;
    }
    while (length < capacity && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static bool AlpnMatches(uint8_t length, const uint8_t *bytes)
{
    static const uint8_t expected[] = LAIUE_PROTOCOL_ALPN;
    return length == sizeof(expected) - 1U && bytes != NULL &&
           memcmp(bytes, expected, sizeof(expected) - 1U) == 0;
}

static NetworkDisconnectReason TransportStatusReason(
    QUIC_STATUS status, bool handshakeComplete, bool dnsEndpoint)
{
    if (status == QUIC_STATUS_CONNECTION_TIMEOUT)
    {
        return NETWORK_DISCONNECT_TIMEOUT;
    }
    if (status == QUIC_STATUS_VER_NEG_ERROR ||
        status == QUIC_STATUS_ALPN_NEG_FAILURE)
    {
        return NETWORK_DISCONNECT_VERSION;
    }
    bool certificateStatus =
        status == QUIC_STATUS_BAD_CERTIFICATE ||
        status == QUIC_STATUS_UNSUPPORTED_CERTIFICATE ||
        status == QUIC_STATUS_REVOKED_CERTIFICATE ||
        status == QUIC_STATUS_EXPIRED_CERTIFICATE ||
        status == QUIC_STATUS_UNKNOWN_CERTIFICATE ||
        status == QUIC_STATUS_REQUIRED_CERTIFICATE ||
        status == QUIC_STATUS_CERT_EXPIRED ||
        status == QUIC_STATUS_CERT_UNTRUSTED_ROOT ||
        status == QUIC_STATUS_CERT_NO_CERT;
    if (certificateStatus)
    {
        return NETWORK_DISCONNECT_CERTIFICATE;
    }
    if (!handshakeComplete &&
        (status == QUIC_STATUS_HANDSHAKE_FAILURE ||
         status == QUIC_STATUS_TLS_ERROR))
    {
        return NETWORK_DISCONNECT_TLS;
    }
    if (!handshakeComplete && dnsEndpoint &&
        (status == QUIC_STATUS_UNREACHABLE ||
         status == QUIC_STATUS_NOT_FOUND))
    {
        return NETWORK_DISCONNECT_DNS;
    }
    return NETWORK_DISCONNECT_IO;
}

static void ChannelInitialize(
    QuicChannel *channel, const QUIC_API_TABLE *api,
    PlatformRwLock *lock)
{
    memset(channel, 0, sizeof(*channel));
    channel->api = api;
    channel->lock = lock;
    channel->receive = channel->inlineReceive;
    channel->receiveCapacity = NETWORK_RECEIVE_CAPACITY;
    channel->connectedAtMs = PlatformMonotonicMilliseconds();
    channel->lastReceiveAtMs = channel->connectedAtMs;
    channel->lastSendAtMs = channel->connectedAtMs;
}

static QUIC_STATUS QUIC_API ChannelStreamCallback(
    HQUIC stream, void *context, QUIC_STREAM_EVENT *event)
{
    QuicChannel *channel = context;
    if (channel == NULL || event == NULL)
    {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE)
    {
        QuicSend *send = event->SEND_COMPLETE.ClientContext;
        PlatformRwLockAcquireExclusive(channel->lock);
        if (channel->outstandingSends != 0)
        {
            --channel->outstandingSends;
        }
        channel->lastSendAtMs =
            PlatformMonotonicMilliseconds();
        PlatformRwLockReleaseExclusive(channel->lock);
        PlatformFree(send);
        return QUIC_STATUS_SUCCESS;
    }

    PlatformRwLockAcquireExclusive(channel->lock);
    switch (event->Type)
    {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            if (QUIC_FAILED(event->START_COMPLETE.Status))
            {
                channel->callbackError = NETWORK_DISCONNECT_IO;
            }
            else
            {
                channel->streamStarted = true;
            }
            break;

        case QUIC_STREAM_EVENT_RECEIVE:
        {
            uint64_t total = 0;
            bool valid =
                (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) == 0;
            for (uint32_t index = 0;
                 index < event->RECEIVE.BufferCount; ++index)
            {
                total += event->RECEIVE.Buffers[index].Length;
                if (total > UINT32_MAX)
                {
                    valid = false;
                }
            }
            if (!valid ||
                total >
                    channel->receiveCapacity - channel->receiveSize)
            {
                channel->callbackError =
                    valid ? NETWORK_DISCONNECT_OVERFLOW
                          : NETWORK_DISCONNECT_PROTOCOL;
            }
            else
            {
                uint32_t offset = channel->receiveSize;
                for (uint32_t index = 0;
                     index < event->RECEIVE.BufferCount; ++index)
                {
                    const QUIC_BUFFER *buffer =
                        &event->RECEIVE.Buffers[index];
                    memcpy(channel->receive + offset,
                           buffer->Buffer, buffer->Length);
                    offset += buffer->Length;
                }
                channel->receiveSize = offset;
                channel->lastReceiveAtMs =
                    PlatformMonotonicMilliseconds();
            }
            // Data is copied synchronously into a bounded queue.
            event->RECEIVE.TotalBufferLength = total;
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            channel->peerSendClosed = true;
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            if (channel->callbackError == NETWORK_DISCONNECT_NONE)
            {
                channel->callbackError = NETWORK_DISCONNECT_REMOTE;
            }
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            channel->shutdownComplete = true;
            if (!channel->auxiliary &&
                !event->SHUTDOWN_COMPLETE.AppCloseInProgress &&
                channel->callbackError == NETWORK_DISCONNECT_NONE)
            {
                channel->callbackError = NETWORK_DISCONNECT_REMOTE;
            }
            break;

        default:
            break;
    }
    PlatformRwLockReleaseExclusive(channel->lock);
    (void)stream;
    return QUIC_STATUS_SUCCESS;
}

#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
static void SetQuicStreamCallback(
    const QUIC_API_TABLE *api, HQUIC stream,
    QUIC_STREAM_CALLBACK_HANDLER callback, void *context)
{
    // MsQuic's generic setter intentionally represents callback functions
    // as void*. Keep the implementation-defined conversion at this single
    // audited ABI boundary.
    api->SetCallbackHandler(stream, (void *)callback, context);
}

static void SetQuicConnectionCallback(
    const QUIC_API_TABLE *api, HQUIC connection,
    QUIC_CONNECTION_CALLBACK_HANDLER callback, void *context)
{
    api->SetCallbackHandler(connection, (void *)callback, context);
}
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#pragma GCC diagnostic pop
#endif

static bool ChannelSendPayload(
    QuicChannel *channel, LaiueMessageType type,
    const uint8_t *payload, uint32_t payloadSize)
{
    if (channel == NULL || payloadSize > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE)
    {
        return false;
    }

    uint32_t frameSize = LAIUE_PROTOCOL_HEADER_SIZE + payloadSize;
    QuicSend *send = PlatformAllocate(
        offsetof(QuicSend, bytes) + frameSize, true);
    if (send == NULL)
    {
        return false;
    }

    HQUIC stream;
    PlatformRwLockAcquireExclusive(channel->lock);
    uint32_t sendLimit = channel->auxiliary
        ? NETWORK_AUX_MAX_OUTSTANDING_SENDS
        : LAIUE_NETWORK_MAX_OUTSTANDING_SENDS;
    if (channel->stream == NULL ||
        channel->callbackError != NETWORK_DISCONNECT_NONE ||
        channel->outstandingSends >= sendLimit)
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        PlatformFree(send);
        return false;
    }
    uint32_t sequence = ++channel->sendSequence;
    if (sequence == 0)
    {
        sequence = ++channel->sendSequence;
    }
    if (LaiueProtocolWriteFrame(
            send->bytes, frameSize, type, sequence,
            payload, payloadSize) != frameSize)
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        PlatformFree(send);
        return false;
    }
    send->channel = channel;
    send->buffer.Buffer = send->bytes;
    send->buffer.Length = frameSize;
    stream = channel->stream;
    ++channel->outstandingSends;
    channel->lastSendAtMs = PlatformMonotonicMilliseconds();
    PlatformRwLockReleaseExclusive(channel->lock);

    QUIC_STATUS status = channel->api->StreamSend(
        stream, &send->buffer, 1U, QUIC_SEND_FLAG_NONE, send);
    if (QUIC_FAILED(status))
    {
        PlatformRwLockAcquireExclusive(channel->lock);
        if (channel->outstandingSends != 0)
        {
            --channel->outstandingSends;
        }
        PlatformRwLockReleaseExclusive(channel->lock);
        PlatformFree(send);
        return false;
    }
    return true;
}

static bool ChannelPopFrame(
    QuicChannel *channel, LaiueProtocolFrame *outFrame,
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE],
    bool *outComplete)
{
    *outComplete = false;
    PlatformRwLockAcquireExclusive(channel->lock);
    if (channel->receiveSize < LAIUE_PROTOCOL_HEADER_SIZE)
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        return true;
    }

    LaiueProtocolFrame frame;
    if (!LaiueProtocolReadHeader(
            channel->receive, channel->receiveSize, &frame))
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        return false;
    }
    uint32_t frameSize =
        LAIUE_PROTOCOL_HEADER_SIZE + frame.payloadSize;
    if (channel->receiveSize < frameSize)
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        return true;
    }
    uint32_t expected = channel->receiveSequence + 1U;
    if (expected == 0)
    {
        expected = 1U;
    }
    if (frame.sequence != expected)
    {
        PlatformRwLockReleaseExclusive(channel->lock);
        return false;
    }
    channel->receiveSequence = expected;
    if (frame.payloadSize != 0)
    {
        memcpy(payload, frame.payload, frame.payloadSize);
    }
    *outFrame = frame;
    outFrame->payload = payload;
    channel->receiveSize -= frameSize;
    if (channel->receiveSize != 0)
    {
        // The Windows no-CRT runtime intentionally exposes memcpy but not
        // memmove. Copying towards the lower address is overlap-safe.
        for (uint32_t index = 0;
             index < channel->receiveSize; ++index)
        {
            channel->receive[index] =
                channel->receive[frameSize + index];
        }
    }
    *outComplete = true;
    PlatformRwLockReleaseExclusive(channel->lock);
    return true;
}

static bool ChannelPeekFrameType(
    QuicChannel *channel, LaiueMessageType *outType,
    bool *outComplete)
{
    *outComplete = false;
    PlatformRwLockAcquireShared(channel->lock);
    if (channel->receiveSize < LAIUE_PROTOCOL_HEADER_SIZE)
    {
        PlatformRwLockReleaseShared(channel->lock);
        return true;
    }
    LaiueProtocolFrame frame;
    if (!LaiueProtocolReadHeader(
            channel->receive, channel->receiveSize, &frame))
    {
        PlatformRwLockReleaseShared(channel->lock);
        return false;
    }
    if (channel->receiveSize <
        LAIUE_PROTOCOL_HEADER_SIZE + frame.payloadSize)
    {
        PlatformRwLockReleaseShared(channel->lock);
        return true;
    }
    *outType = frame.type;
    *outComplete = true;
    PlatformRwLockReleaseShared(channel->lock);
    return true;
}

static NetworkDisconnectReason ChannelTakeError(QuicChannel *channel)
{
    PlatformRwLockAcquireExclusive(channel->lock);
    NetworkDisconnectReason reason = channel->callbackError;
    channel->callbackError = NETWORK_DISCONNECT_NONE;
    PlatformRwLockReleaseExclusive(channel->lock);
    return reason;
}

static uint32_t ChannelOutstandingSends(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    uint32_t count = channel->outstandingSends;
    PlatformRwLockReleaseShared(channel->lock);
    return count;
}

static uint64_t ChannelLastReceiveAt(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    uint64_t value = channel->lastReceiveAtMs;
    PlatformRwLockReleaseShared(channel->lock);
    return value;
}

static uint64_t ChannelLastSendAt(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    uint64_t value = channel->lastSendAtMs;
    PlatformRwLockReleaseShared(channel->lock);
    return value;
}

static uint64_t ChannelLastActivityAt(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    uint64_t value =
        channel->lastReceiveAtMs > channel->lastSendAtMs
            ? channel->lastReceiveAtMs
            : channel->lastSendAtMs;
    PlatformRwLockReleaseShared(channel->lock);
    return value;
}

static bool ChannelIsActive(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    bool active = channel->stream != NULL;
    PlatformRwLockReleaseShared(channel->lock);
    return active;
}

static bool ChannelShutdownGracefully(QuicChannel *channel)
{
    HQUIC stream = NULL;
    PlatformRwLockAcquireShared(channel->lock);
    stream = channel->stream;
    PlatformRwLockReleaseShared(channel->lock);
    return stream != NULL &&
           !QUIC_FAILED(channel->api->StreamShutdown(
               stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0));
}

static bool ChannelTryRetireAuxiliary(
    QuicChannel *channel, bool *outRetired)
{
    HQUIC stream = NULL;
    bool valid = true;
    *outRetired = false;

    PlatformRwLockAcquireExclusive(channel->lock);
    if (channel->stream != NULL && channel->shutdownComplete)
    {
        if (channel->receiveSize != 0 ||
            channel->outstandingSends != 0)
        {
            // The callback may observe FIN before the main thread has
            // drained all complete frames copied into the bounded buffer.
        }
        else if (!channel->auxiliary ||
                (channel->receiveSequence == 0 &&
                 channel->sendSequence == 0))
        {
            valid = false;
        }
        else
        {
            stream = channel->stream;
            channel->stream = NULL;
            *outRetired = true;
        }
    }
    PlatformRwLockReleaseExclusive(channel->lock);

    if (stream != NULL)
    {
        channel->api->StreamClose(stream);
    }
    return valid;
}

static bool ChannelReceiveEndedWithBytes(QuicChannel *channel)
{
    PlatformRwLockAcquireShared(channel->lock);
    bool truncated =
        (channel->peerSendClosed || channel->shutdownComplete) &&
        channel->receiveSize != 0;
    PlatformRwLockReleaseShared(channel->lock);
    return truncated;
}

static void CopyModFromProtocol(
    NetworkModDescriptor *destination,
    const LaiueProtocolMod *source)
{
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->id, source->id, sizeof(destination->id));
    memcpy(destination->version, source->version,
           sizeof(destination->version));
    memcpy(destination->contentHash, source->contentHash,
           LAIUE_NETWORK_MOD_HASH_SIZE);
}

static void CopyModToProtocol(
    LaiueProtocolMod *destination,
    const NetworkModDescriptor *source)
{
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->id, source->id, sizeof(destination->id));
    memcpy(destination->version, source->version,
           sizeof(destination->version));
    memcpy(destination->contentHash, source->contentHash,
           LAIUE_NETWORK_MOD_HASH_SIZE);
}

static bool ModDescriptorsEqual(
    const NetworkModDescriptor *left,
    const NetworkModDescriptor *right)
{
    if (!PlatformConstantTimeEqual(
            left->contentHash, right->contentHash,
            LAIUE_NETWORK_MOD_HASH_SIZE))
    {
        return false;
    }
    uint32_t index = 0;
    while (index < LAIUE_NETWORK_MOD_ID_CAPACITY &&
           left->id[index] != '\0' &&
           left->id[index] == right->id[index])
    {
        ++index;
    }
    if (index == LAIUE_NETWORK_MOD_ID_CAPACITY ||
        left->id[index] != right->id[index])
    {
        return false;
    }
    index = 0;
    while (index < LAIUE_NETWORK_MOD_VERSION_CAPACITY &&
           left->version[index] != '\0' &&
           left->version[index] == right->version[index])
    {
        ++index;
    }
    return index < LAIUE_NETWORK_MOD_VERSION_CAPACITY &&
           left->version[index] == right->version[index];
}

static bool ChannelSendModList(
    QuicChannel *channel, LaiueMessageType listType,
    LaiueMessageType entryType,
    const NetworkModDescriptor *mods,
    uint32_t count, uint8_t flags)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size =
        LaiueProtocolEncodeModList(payload, sizeof(payload), count, flags);
    if (size == 0 ||
        !ChannelSendPayload(channel, listType, payload, size))
    {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        LaiueProtocolMod wire;
        CopyModToProtocol(&wire, &mods[index]);
        size = LaiueProtocolEncodeMod(payload, sizeof(payload), &wire);
        if (size == 0 ||
            !ChannelSendPayload(channel, entryType, payload, size))
        {
            return false;
        }
    }
    return true;
}

static QUIC_ADDRESS_FAMILY QuicFamily(
    NetworkAddressFamily family)
{
    if (family == NETWORK_ADDRESS_FAMILY_IPV4)
    {
        return QUIC_ADDRESS_FAMILY_INET;
    }
    if (family == NETWORK_ADDRESS_FAMILY_IPV6)
    {
        return QUIC_ADDRESS_FAMILY_INET6;
    }
    return QUIC_ADDRESS_FAMILY_UNSPEC;
}

static bool ResolveDnsEndpoint(
    const NetworkClientConfiguration *configuration,
    QUIC_ADDR *outAddress)
{
    if (configuration == NULL || outAddress == NULL ||
        configuration->endpoint.kind != NETWORK_ENDPOINT_DNS)
    {
        return false;
    }

#if defined(_WIN32)
    ADDRINFOA hints;
    ADDRINFOA *addresses = NULL;
#else
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
#endif
    memset(&hints, 0, sizeof(hints));
    if (configuration->addressFamily ==
        NETWORK_ADDRESS_FAMILY_IPV4)
    {
        hints.ai_family = AF_INET;
    }
    else if (configuration->addressFamily ==
             NETWORK_ADDRESS_FAMILY_IPV6)
    {
        hints.ai_family = AF_INET6;
    }
    else
    {
        hints.ai_family = AF_UNSPEC;
    }
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

#if defined(_WIN32)
    int result = GetAddrInfoA(
        configuration->endpoint.host, NULL, &hints, &addresses);
#else
    int result = getaddrinfo(
        configuration->endpoint.host, NULL, &hints, &addresses);
#endif
    if (result != 0 || addresses == NULL)
    {
        return false;
    }

    bool found = false;
#if defined(_WIN32)
    for (const ADDRINFOA *candidate = addresses;
#else
    for (const struct addrinfo *candidate = addresses;
#endif
         candidate != NULL; candidate = candidate->ai_next)
    {
        if (candidate->ai_family == AF_INET &&
            candidate->ai_addr != NULL &&
            candidate->ai_addrlen >= sizeof(outAddress->Ipv4))
        {
            memset(outAddress, 0, sizeof(*outAddress));
            memcpy(
                &outAddress->Ipv4, candidate->ai_addr,
                sizeof(outAddress->Ipv4));
            found = true;
            break;
        }
        if (candidate->ai_family == AF_INET6 &&
            candidate->ai_addr != NULL &&
            candidate->ai_addrlen >= sizeof(outAddress->Ipv6))
        {
            memset(outAddress, 0, sizeof(*outAddress));
            memcpy(
                &outAddress->Ipv6, candidate->ai_addr,
                sizeof(outAddress->Ipv6));
            found = true;
            break;
        }
    }
#if defined(_WIN32)
    FreeAddrInfoA(addresses);
#else
    freeaddrinfo(addresses);
#endif
    return found;
}

static void FillSettings(
    QUIC_SETTINGS *settings, bool server,
    uint32_t handshakeTimeoutMs, uint32_t idleTimeoutMs)
{
    memset(settings, 0, sizeof(*settings));
    settings->HandshakeIdleTimeoutMs =
        handshakeTimeoutMs != 0
            ? handshakeTimeoutMs
            : (uint32_t)NETWORK_HANDSHAKE_TIMEOUT_MS;
    settings->IsSet.HandshakeIdleTimeoutMs = true;
    settings->IdleTimeoutMs =
        idleTimeoutMs != 0
            ? idleTimeoutMs
            : (uint32_t)NETWORK_IDLE_TIMEOUT_MS;
    settings->IsSet.IdleTimeoutMs = true;
    settings->KeepAliveIntervalMs = 5000U;
    settings->IsSet.KeepAliveIntervalMs = true;
    settings->PeerBidiStreamCount = server ? 1U : 0U;
    settings->IsSet.PeerBidiStreamCount = true;
    settings->PeerUnidiStreamCount =
        server ? 0U : LAIUE_PROTOCOL_MAX_SERVER_STREAMS;
    settings->IsSet.PeerUnidiStreamCount = true;
    settings->DatagramReceiveEnabled = false;
    settings->IsSet.DatagramReceiveEnabled = true;
    if (server)
    {
        settings->ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
        settings->IsSet.ServerResumptionLevel = true;
    }
}

static bool OpenApiAndRegistration(
    const QUIC_API_TABLE **outApi, HQUIC *outRegistration)
{
    *outApi = NULL;
    *outRegistration = NULL;
    if (QUIC_FAILED(MsQuicOpen2(outApi)) || *outApi == NULL)
    {
        return false;
    }
    QUIC_REGISTRATION_CONFIG registrationConfiguration = {
        "laiue",
        QUIC_EXECUTION_PROFILE_LOW_LATENCY,
    };
    if (QUIC_FAILED((*outApi)->RegistrationOpen(
            &registrationConfiguration, outRegistration)))
    {
        MsQuicClose(*outApi);
        *outApi = NULL;
        return false;
    }
    return true;
}

static bool OpenConfiguration(
    const QUIC_API_TABLE *api, HQUIC registration,
    bool server, uint32_t handshakeTimeoutMs,
    uint32_t idleTimeoutMs, HQUIC *outConfiguration)
{
    static uint8_t alpnBytes[] = LAIUE_PROTOCOL_ALPN;
    QUIC_BUFFER alpn = {
        (uint32_t)(sizeof(alpnBytes) - 1U),
        alpnBytes,
    };
    QUIC_SETTINGS settings;
    FillSettings(&settings, server, handshakeTimeoutMs, idleTimeoutMs);
    return !QUIC_FAILED(api->ConfigurationOpen(
        registration, &alpn, 1U, &settings, sizeof(settings),
        NULL, outConfiguration));
}

#if defined(_WIN32)
static bool ParseThumbprint(
    const char *text, QUIC_CERTIFICATE_HASH *outHash)
{
    if (text == NULL || outHash == NULL ||
        AsciiLength(text, 41U) != 40U)
    {
        return false;
    }
    memset(outHash, 0, sizeof(*outHash));
    for (uint32_t index = 0; index < 20U; ++index)
    {
        uint8_t values[2];
        for (uint32_t nibble = 0; nibble < 2U; ++nibble)
        {
            char value = text[index * 2U + nibble];
            if (value >= '0' && value <= '9')
            {
                values[nibble] = (uint8_t)(value - '0');
            }
            else if (value >= 'a' && value <= 'f')
            {
                values[nibble] = (uint8_t)(10 + value - 'a');
            }
            else if (value >= 'A' && value <= 'F')
            {
                values[nibble] = (uint8_t)(10 + value - 'A');
            }
            else
            {
                return false;
            }
        }
        outHash->ShaHash[index] =
            (uint8_t)((values[0] << 4U) | values[1]);
    }
    return true;
}
#endif

static bool ValidatePinnedCertificate(
    NetworkClient *client,
    const QUIC_CONNECTION_EVENT *event)
{
    const QUIC_BUFFER *certificate =
        (const QUIC_BUFFER *)
            event->PEER_CERTIFICATE_RECEIVED.Certificate;
    if (certificate == NULL || certificate->Buffer == NULL ||
        certificate->Length == 0)
    {
        return false;
    }
    uint8_t actual[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE];
    if (!PlatformSha256(
            certificate->Buffer, certificate->Length, actual) ||
        !PlatformConstantTimeEqual(
            actual,
            client->connectionConfiguration.certificateSha256,
            sizeof(actual)))
    {
        return false;
    }
    QUIC_STATUS deferred =
        event->PEER_CERTIFICATE_RECEIVED.DeferredStatus;
    // A pin may replace only the chain anchor. The DER leaf is parsed
    // independently so an untrusted-root status cannot mask expiration or
    // a DNS/IP SAN mismatch.
    bool acceptableChain =
        !QUIC_FAILED(deferred) ||
        deferred == QUIC_STATUS_CERT_UNTRUSTED_ROOT;
#if defined(_WIN32)
    acceptableChain =
        acceptableChain ||
        deferred == (QUIC_STATUS)CERT_E_CHAINING;
#endif
    return acceptableChain &&
           NetworkCertificateValidateLeafIdentity(
               &client->connectionConfiguration.endpoint,
               certificate->Buffer, certificate->Length);
}

static QUIC_STATUS QUIC_API ClientConnectionCallback(
    HQUIC connection, void *context, QUIC_CONNECTION_EVENT *event)
{
    NetworkClient *client = context;
    if (client == NULL || event == NULL)
    {
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    switch (event->Type)
    {
        case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
        {
            const QUIC_BUFFER *certificate =
                (const QUIC_BUFFER *)
                    event->PEER_CERTIFICATE_RECEIVED.Certificate;
            QUIC_STATUS deferred =
                event->PEER_CERTIFICATE_RECEIVED.DeferredStatus;
            bool valid =
                certificate != NULL &&
                certificate->Buffer != NULL &&
                certificate->Length != 0 &&
                (client->connectionConfiguration.trustMode ==
                         NETWORK_TRUST_SHA256
                     ? ValidatePinnedCertificate(client, event)
                     : !QUIC_FAILED(deferred) &&
                           NetworkCertificateValidateLeafIdentity(
                               &client->connectionConfiguration.endpoint,
                               certificate->Buffer,
                               certificate->Length));
            if (!valid)
            {
                PlatformRwLockAcquireExclusive(&client->lock);
                client->callbackReason =
                    NETWORK_DISCONNECT_CERTIFICATE;
                PlatformRwLockReleaseExclusive(&client->lock);
                return QUIC_STATUS_BAD_CERTIFICATE;
            }
            PlatformRwLockAcquireExclusive(&client->lock);
            client->certificateVerified = true;
            PlatformRwLockReleaseExclusive(&client->lock);
            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_CONNECTION_EVENT_CONNECTED:
            PlatformRwLockAcquireExclusive(&client->lock);
            if (event->CONNECTED.SessionResumed ||
                !AlpnMatches(
                    event->CONNECTED.NegotiatedAlpnLength,
                    event->CONNECTED.NegotiatedAlpn))
            {
                client->callbackReason =
                    NETWORK_DISCONNECT_VERSION;
            }
            else
            {
                client->transportConnected = true;
            }
            PlatformRwLockReleaseExclusive(&client->lock);
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            QuicChannel *accepted = NULL;
            bool stopping = false;
            PlatformRwLockAcquireExclusive(&client->lock);
            stopping = client->stopping;
            if (!stopping &&
                (event->PEER_STREAM_STARTED.Flags &
                    QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) != 0 &&
                (event->PEER_STREAM_STARTED.Flags &
                    QUIC_STREAM_OPEN_FLAG_0_RTT) == 0)
            {
                for (uint32_t index = 0;
                     index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS;
                     ++index)
                {
                    if (client->serverChannels[index].stream != NULL)
                    {
                        continue;
                    }
                    accepted = &client->serverChannels[index];
                    ChannelInitialize(
                        accepted, client->api, &client->lock);
                    accepted->receive =
                        client->serverReceive[index];
                    accepted->receiveCapacity =
                        NETWORK_AUX_RECEIVE_CAPACITY;
                    accepted->auxiliary = true;
                    accepted->stream =
                        event->PEER_STREAM_STARTED.Stream;
                    accepted->streamStarted = true;
                    break;
                }
            }
            if (accepted == NULL && !stopping)
            {
                client->callbackReason =
                    NETWORK_DISCONNECT_PROTOCOL;
            }
            PlatformRwLockReleaseExclusive(&client->lock);
            if (accepted != NULL)
            {
                SetQuicStreamCallback(
                    client->api,
                    event->PEER_STREAM_STARTED.Stream,
                    ChannelStreamCallback, accepted);
            }
            else
            {
                // A non-success return rejects a peer-created stream without
                // publishing a handle that teardown would have to race.
                return stopping
                           ? QUIC_STATUS_ABORTED
                           : QUIC_STATUS_NOT_SUPPORTED;
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            PlatformRwLockAcquireExclusive(&client->lock);
            if (client->callbackReason == NETWORK_DISCONNECT_NONE)
            {
                client->callbackReason = TransportStatusReason(
                    event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
                    client->transportConnected,
                    client->connectionConfiguration.endpoint.kind ==
                        NETWORK_ENDPOINT_DNS);
            }
            PlatformRwLockReleaseExclusive(&client->lock);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            PlatformRwLockAcquireExclusive(&client->lock);
            if (client->callbackReason == NETWORK_DISCONNECT_NONE)
            {
                client->callbackReason = NETWORK_DISCONNECT_REMOTE;
            }
            PlatformRwLockReleaseExclusive(&client->lock);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            PlatformRwLockAcquireExclusive(&client->lock);
            client->shutdownComplete = true;
            PlatformRwLockReleaseExclusive(&client->lock);
            break;

        case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
            // Tickets are deliberately neither persisted nor supplied to a
            // future connection in protocol v5.
            break;

        default:
            break;
    }
    (void)connection;
    return QUIC_STATUS_SUCCESS;
}

static bool ClientPushEvent(
    NetworkClient *client, const NetworkClientEvent *event)
{
    if (client->eventCount >= NETWORK_CLIENT_EVENT_CAPACITY)
    {
        return false;
    }
    client->events[client->eventWrite] = *event;
    client->eventWrite =
        (client->eventWrite + 1U) % NETWORK_CLIENT_EVENT_CAPACITY;
    ++client->eventCount;
    return true;
}

static void ClientDisconnect(
    NetworkClient *client, NetworkDisconnectReason reason)
{
    if (client == NULL ||
        client->state == NETWORK_CONNECTION_DISCONNECTED)
    {
        return;
    }
    client->state = NETWORK_CONNECTION_DISCONNECTED;
    if (!client->disconnectNotified)
    {
        NetworkClientEvent event;
        memset(&event, 0, sizeof(event));
        event.type = NETWORK_CLIENT_EVENT_DISCONNECTED;
        event.data.disconnectReason = reason;
        if (!ClientPushEvent(client, &event))
        {
            // A terminal event must never be lost behind non-terminal
            // state. Evict the oldest queued item as a last-resort bounded
            // shutdown path.
            client->eventRead =
                (client->eventRead + 1U) %
                NETWORK_CLIENT_EVENT_CAPACITY;
            --client->eventCount;
            (void)ClientPushEvent(client, &event);
        }
        client->disconnectNotified = true;
    }
    if (!client->shutdownRequested &&
        client->connectionStarted &&
        client->connection != NULL)
    {
        client->shutdownRequested = true;
        client->api->ConnectionShutdown(
            client->connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
            reason == NETWORK_DISCONNECT_OVERFLOW
                ? NETWORK_APP_ERROR_OVERFLOW
                : NETWORK_APP_ERROR_PROTOCOL);
    }
}

static bool ClientPushServerModsEvent(NetworkClient *client)
{
    NetworkClientEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NETWORK_CLIENT_EVENT_SERVER_MODS;
    event.data.serverMods.count = client->receivedServerModCount;
    event.data.serverMods.downloadsAllowed =
        client->serverDownloadsAllowed;
    return ClientPushEvent(client, &event);
}

static bool ClientBeginProtocol(NetworkClient *client)
{
    if (ChannelIsActive(&client->channel))
    {
        return false;
    }
    HQUIC stream = NULL;
    if (QUIC_FAILED(client->api->StreamOpen(
            client->connection, QUIC_STREAM_OPEN_FLAG_NONE,
            ChannelStreamCallback, &client->channel,
            &stream)))
    {
        return false;
    }
    PlatformRwLockAcquireExclusive(&client->lock);
    client->channel.stream = stream;
    PlatformRwLockReleaseExclusive(&client->lock);
    if (QUIC_FAILED(client->api->StreamStart(
            stream,
            QUIC_STREAM_START_FLAG_IMMEDIATE |
                QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL)))
    {
        return false;
    }

    uint64_t nonce = 0;
    if (!PlatformRandomBytes(&nonce, sizeof(nonce)) || nonce == 0)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size =
        LaiueProtocolEncodeHello(payload, sizeof(payload), nonce);
    if (size == 0 ||
        !ChannelSendPayload(
            &client->channel, LAIUE_MESSAGE_CLIENT_HELLO,
            payload, size))
    {
        return false;
    }
    client->clientNonce = nonce;
    client->state = NETWORK_CONNECTION_NEGOTIATING;
    NetworkClientEvent verified;
    memset(&verified, 0, sizeof(verified));
    verified.type = NETWORK_CLIENT_EVENT_SERVER_VERIFIED;
    return ClientPushEvent(client, &verified);
}

static uint32_t ChunkEditOrdinal(
    const LaiueProtocolChunkEdit *edit)
{
    return (uint32_t)edit->localX *
               LAIUE_PROTOCOL_CHUNK_SIZE *
               LAIUE_PROTOCOL_CHUNK_SIZE +
           (uint32_t)edit->localY *
               LAIUE_PROTOCOL_CHUNK_SIZE +
           edit->localZ;
}

static bool ChunkCoordinatesEqual(
    const int64_t left[3], const int64_t right[3])
{
    return left[0] == right[0] &&
           left[1] == right[1] &&
           left[2] == right[2];
}

static bool ClientHandleSnapshotChunk(
    NetworkClient *client, const LaiueProtocolFrame *frame,
    NetworkClientEvent *event)
{
    LaiueProtocolChunkDelta decoded;
    if (!client->snapshotStarted ||
        !LaiueProtocolDecodeSnapshotChunk(
            frame->payload, frame->payloadSize, &decoded))
    {
        return false;
    }

    if (decoded.partIndex == 0)
    {
        if (client->snapshotNextPart != 0 ||
            client->receivedSnapshotChunks >=
                client->expectedSnapshotChunks)
        {
            return false;
        }
        client->snapshotPartChunk[0] = decoded.chunk[0];
        client->snapshotPartChunk[1] = decoded.chunk[1];
        client->snapshotPartChunk[2] = decoded.chunk[2];
        client->snapshotPartRevision = decoded.revision;
        client->snapshotPartCount = decoded.partCount;
    }
    else if (decoded.partIndex != client->snapshotNextPart ||
             decoded.partCount != client->snapshotPartCount ||
             decoded.revision != client->snapshotPartRevision ||
             decoded.chunk[0] != client->snapshotPartChunk[0] ||
             decoded.chunk[1] != client->snapshotPartChunk[1] ||
             decoded.chunk[2] != client->snapshotPartChunk[2] ||
             ChunkEditOrdinal(&decoded.edits[0]) <=
                 client->snapshotLastEditIndex)
    {
        return false;
    }

    if (decoded.editCount != 0)
    {
        client->snapshotLastEditIndex =
            ChunkEditOrdinal(
                &decoded.edits[decoded.editCount - 1U]);
    }
    client->snapshotNextPart =
        (uint16_t)(decoded.partIndex + 1U);
    if (client->snapshotNextPart == decoded.partCount)
    {
        if (client->resyncRequestOutstanding &&
            ChunkCoordinatesEqual(
                client->resyncRequestChunk, decoded.chunk))
        {
            if (client->resyncResponseMatched ||
                decoded.revision <
                    client->resyncExpectedRevision)
            {
                return false;
            }
            client->resyncResponseMatched = true;
        }
        ++client->receivedSnapshotChunks;
        client->snapshotNextPart = 0;
        client->snapshotPartCount = 0;
        client->snapshotLastEditIndex = 0;
    }

    memset(event, 0, sizeof(*event));
    event->type = NETWORK_CLIENT_EVENT_SNAPSHOT_CHUNK;
    event->data.chunkDelta.chunk[0] = decoded.chunk[0];
    event->data.chunkDelta.chunk[1] = decoded.chunk[1];
    event->data.chunkDelta.chunk[2] = decoded.chunk[2];
    event->data.chunkDelta.revision = decoded.revision;
    event->data.chunkDelta.partIndex = decoded.partIndex;
    event->data.chunkDelta.partCount = decoded.partCount;
    event->data.chunkDelta.editCount = decoded.editCount;
    for (uint32_t index = 0; index < decoded.editCount; ++index)
    {
        event->data.chunkDelta.edits[index].localX =
            decoded.edits[index].localX;
        event->data.chunkDelta.edits[index].localY =
            decoded.edits[index].localY;
        event->data.chunkDelta.edits[index].localZ =
            decoded.edits[index].localZ;
        event->data.chunkDelta.edits[index].replacement =
            decoded.edits[index].replacement;
    }
    return ClientPushEvent(client, event);
}

static bool ClientCompleteSyncIfReady(NetworkClient *client)
{
    if (client->state != NETWORK_CONNECTION_SYNCING_WORLD ||
        !client->syncBeginReceived || !client->snapshotEnded ||
        !client->syncReadyReceived)
    {
        return true;
    }
    NetworkClientEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NETWORK_CLIENT_EVENT_READY;
    event.data.ready.peerId = client->pendingPeerId;
    event.data.ready.worldSeed = client->pendingWorldSeed;
    client->state = NETWORK_CONNECTION_READY;
    client->everReady = true;
    // Every barrier owns a new acknowledgement. Clear the receive-side
    // marker here so a later control-first SYNC_BEGIN is accepted.
    client->syncBeginReceived = false;
    client->syncAppliedSent = false;
    client->snapshotRequiresReadyBarrier = false;
    return ClientPushEvent(client, &event);
}

static bool ClientHandleFrame(
    NetworkClient *client, const LaiueProtocolFrame *frame)
{
    NetworkClientEvent event;
    memset(&event, 0, sizeof(event));

    if (client->state == NETWORK_CONNECTION_NEGOTIATING)
    {
        if (frame->type == LAIUE_MESSAGE_SERVER_MOD_LIST)
        {
            uint8_t flags = 0;
            if (client->expectedServerModCount != 0 ||
                !LaiueProtocolDecodeModList(
                    frame->payload, frame->payloadSize,
                    &client->expectedServerModCount, &flags))
            {
                return false;
            }
            client->serverDownloadsAllowed = (flags & 1U) != 0;
            return client->expectedServerModCount != 0 ||
                   ClientPushServerModsEvent(client);
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_MOD_ENTRY &&
            client->receivedServerModCount <
                client->expectedServerModCount)
        {
            LaiueProtocolMod wire;
            if (!LaiueProtocolDecodeMod(
                    frame->payload, frame->payloadSize, &wire))
            {
                return false;
            }
            CopyModFromProtocol(
                &client->serverMods[
                    client->receivedServerModCount++],
                &wire);
            return client->receivedServerModCount !=
                       client->expectedServerModCount ||
                   ClientPushServerModsEvent(client);
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_BEGIN &&
            client->contentRequested && client->contentBytes == NULL)
        {
            if (!LaiueProtocolDecodeContentBegin(
                    frame->payload, frame->payloadSize,
                    &client->expectedContentSize,
                    client->expectedContentHash) ||
                client->expectedContentSize == 0 ||
                client->expectedContentSize >
                    LAIUE_NETWORK_MAX_CONTENT_BYTES)
            {
                return false;
            }
            client->contentBytes = PlatformAllocate(
                (size_t)client->expectedContentSize, false);
            return client->contentBytes != NULL;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_CHUNK &&
            client->contentBytes != NULL &&
            frame->payloadSize != 0 &&
            frame->payloadSize <=
                client->expectedContentSize -
                    client->receivedContentSize)
        {
            memcpy(
                client->contentBytes + client->receivedContentSize,
                frame->payload, frame->payloadSize);
            client->receivedContentSize += frame->payloadSize;
            return true;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_END &&
            frame->payloadSize == 0 &&
            client->contentBytes != NULL &&
            client->receivedContentSize ==
                client->expectedContentSize)
        {
            uint8_t actual[LAIUE_NETWORK_CONTENT_HASH_SIZE];
            if (!PlatformSha256(
                    client->contentBytes,
                    client->receivedContentSize, actual) ||
                !PlatformConstantTimeEqual(
                    actual, client->expectedContentHash,
                    sizeof(actual)))
            {
                return false;
            }
            event.type = NETWORK_CLIENT_EVENT_CONTENT_READY;
            client->contentVerified = true;
            return ClientPushEvent(client, &event);
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_REJECT)
        {
            uint8_t reason = 0;
            if (!LaiueProtocolDecodeReject(
                    frame->payload, frame->payloadSize, &reason) ||
                reason > NETWORK_REJECT_SERVER_POLICY)
            {
                return false;
            }
            event.type = NETWORK_CLIENT_EVENT_REJECTED;
            event.data.rejectReason = (NetworkRejectReason)reason;
            if (!ClientPushEvent(client, &event))
            {
                return false;
            }
            ClientDisconnect(client, NETWORK_DISCONNECT_REMOTE);
            return true;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_WELCOME &&
            client->modsSubmitted)
        {
            uint64_t echoedNonce = 0;
            if (!LaiueProtocolDecodeWelcome(
                    frame->payload, frame->payloadSize,
                    &client->pendingPeerId,
                    &client->pendingWorldSeed, &echoedNonce) ||
                echoedNonce != client->clientNonce)
            {
                return false;
            }
            client->state = NETWORK_CONNECTION_SYNCING_WORLD;
            return true;
        }
        return false;
    }

    if (frame->type == LAIUE_MESSAGE_SYNC_BEGIN)
    {
        if (frame->payloadSize != 0 ||
            client->state != NETWORK_CONNECTION_SYNCING_WORLD ||
            client->everReady || client->syncBeginReceived)
        {
            return false;
        }
        client->syncBeginReceived = true;
        return ClientCompleteSyncIfReady(client);
    }

    if (frame->type == LAIUE_MESSAGE_SNAPSHOT_BEGIN &&
        !client->snapshotStarted)
    {
        LaiueProtocolSnapshotBegin snapshot;
        if (!LaiueProtocolDecodeSnapshotBegin(
                frame->payload, frame->payloadSize, &snapshot) ||
            snapshot.peerId != client->pendingPeerId ||
            snapshot.worldSeed != client->pendingWorldSeed)
        {
            return false;
        }
        bool validInitialBarrier =
            snapshot.requiresReadyBarrier &&
            !client->everReady &&
            client->state == NETWORK_CONNECTION_SYNCING_WORLD;
        bool validLiveSnapshot =
            !snapshot.requiresReadyBarrier &&
            client->everReady &&
            client->state == NETWORK_CONNECTION_READY &&
            !client->syncBeginReceived;
        if (!validInitialBarrier && !validLiveSnapshot)
        {
            return false;
        }
        client->snapshotId = snapshot.snapshotId;
        client->snapshotWorldRevision = snapshot.worldRevision;
        client->snapshotServerTick = snapshot.serverTick;
        client->expectedSnapshotChunks = snapshot.chunkCount;
        client->receivedSnapshotChunks = 0;
        client->resyncResponseMatched = false;
        client->snapshotStarted = true;
        client->snapshotEnded = false;
        client->snapshotRequiresReadyBarrier =
            snapshot.requiresReadyBarrier;
        event.type = NETWORK_CLIENT_EVENT_SNAPSHOT_BEGIN;
        event.data.snapshot.snapshotId = snapshot.snapshotId;
        event.data.snapshot.worldRevision = snapshot.worldRevision;
        event.data.snapshot.serverTick = snapshot.serverTick;
        event.data.snapshot.chunkCount = snapshot.chunkCount;
        event.data.snapshot.peerId = snapshot.peerId;
        event.data.snapshot.worldSeed = snapshot.worldSeed;
        event.data.snapshot.worldTime = snapshot.worldTime;
        event.data.snapshot.requiresReadyBarrier =
            snapshot.requiresReadyBarrier;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_SNAPSHOT_CHUNK)
    {
        return ClientHandleSnapshotChunk(client, frame, &event);
    }
    if (frame->type == LAIUE_MESSAGE_SNAPSHOT_END &&
        client->snapshotStarted)
    {
        uint64_t snapshotId = 0;
        uint64_t worldRevision = 0;
        if (!LaiueProtocolDecodeSnapshotEnd(
                frame->payload, frame->payloadSize,
                &snapshotId, &worldRevision) ||
            snapshotId != client->snapshotId ||
            worldRevision < client->snapshotWorldRevision ||
            client->snapshotNextPart != 0 ||
            client->receivedSnapshotChunks !=
                client->expectedSnapshotChunks)
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_SNAPSHOT_END;
        event.data.snapshot.snapshotId = snapshotId;
        event.data.snapshot.worldRevision = worldRevision;
        event.data.snapshot.requiresReadyBarrier =
            client->snapshotRequiresReadyBarrier;
        client->snapshotWorldRevision = worldRevision;
        if (!ClientPushEvent(client, &event))
        {
            return false;
        }
        if (client->resyncRequestOutstanding &&
            client->resyncResponseMatched)
        {
            client->resyncRequestOutstanding = false;
            client->resyncResponseMatched = false;
        }
        bool requiresReadyBarrier =
            client->snapshotRequiresReadyBarrier;
        client->snapshotStarted = false;
        if (requiresReadyBarrier)
        {
            client->snapshotEnded = true;
            return ClientCompleteSyncIfReady(client);
        }
        client->snapshotRequiresReadyBarrier = false;
        return client->state == NETWORK_CONNECTION_READY;
    }
    if (frame->type == LAIUE_MESSAGE_SYNC_READY &&
        frame->payloadSize == 0 &&
        client->state == NETWORK_CONNECTION_SYNCING_WORLD &&
        client->syncBeginReceived &&
        client->snapshotRequiresReadyBarrier)
    {
        client->syncReadyReceived = true;
        return ClientCompleteSyncIfReady(client);
    }

    if (client->state != NETWORK_CONNECTION_SYNCING_WORLD &&
        client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }

    if (frame->type ==
        LAIUE_MESSAGE_CHUNK_RESYNC_CANCELLED)
    {
        LaiueProtocolChunkResyncRequest cancelled;
        if (!LaiueProtocolDecodeChunkResyncCancelled(
                frame->payload, frame->payloadSize,
                &cancelled) ||
            !client->resyncRequestOutstanding ||
            !ChunkCoordinatesEqual(
                client->resyncRequestChunk,
                cancelled.chunk) ||
            client->resyncExpectedRevision !=
                cancelled.expectedRevision)
        {
            return false;
        }
        client->resyncRequestOutstanding = false;
        client->resyncResponseMatched = false;
        event.type =
            NETWORK_CLIENT_EVENT_CHUNK_RESYNC_CANCELLED;
        memcpy(event.data.chunkResyncCancelled.chunk,
            cancelled.chunk,
            sizeof(event.data.chunkResyncCancelled.chunk));
        event.data.chunkResyncCancelled.expectedRevision =
            cancelled.expectedRevision;
        return ClientPushEvent(client, &event);
    }

    if (frame->type == LAIUE_MESSAGE_PLAYER_STATE ||
        frame->type == LAIUE_MESSAGE_PLAYER_ROSTER_ENTRY)
    {
        LaiueProtocolPlayerState decoded;
        if (!LaiueProtocolDecodePlayerState(
                frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_PLAYER_STATE;
        event.data.playerState.serverTick = decoded.serverTick;
        event.data.playerState.peerId = decoded.peerId;
        event.data.playerState.lastProcessedInputSequence =
            decoded.lastProcessedInputSequence;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            event.data.playerState.position[axis] =
                decoded.position[axis];
        }
        event.data.playerState.yaw = decoded.yaw;
        event.data.playerState.pitch = decoded.pitch;
        event.data.playerState.locomotionVelocityX =
            decoded.locomotionVelocityX;
        event.data.playerState.locomotionVelocityY =
            decoded.locomotionVelocityY;
        event.data.playerState.verticalVelocity =
            decoded.verticalVelocity;
        event.data.playerState.externalVelocityX =
            decoded.externalVelocityX;
        event.data.playerState.externalVelocityY =
            decoded.externalVelocityY;
        event.data.playerState.jumpBufferRemaining =
            decoded.jumpBufferRemaining;
        event.data.playerState.coyoteTimeRemaining =
            decoded.coyoteTimeRemaining;
        event.data.playerState.colliderCrouchProgress =
            decoded.colliderCrouchProgress;
        event.data.playerState.eyeCrouchProgress =
            decoded.eyeCrouchProgress;
        event.data.playerState.airJumpsRemaining =
            decoded.airJumpsRemaining;
        event.data.playerState.crouchingRequested =
            decoded.crouchingRequested;
        event.data.playerState.grounded = decoded.grounded;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DELTA)
    {
        LaiueProtocolBlockDelta decoded;
        if (!LaiueProtocolDecodeBlockDelta(
                frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DELTA;
        event.data.blockDelta.serverTick = decoded.serverTick;
        event.data.blockDelta.revision = decoded.revision;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            event.data.blockDelta.block[axis] =
                decoded.block[axis];
        }
        event.data.blockDelta.replacement = decoded.replacement;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DROP_SPAWN)
    {
        LaiueProtocolBlockDrop decoded;
        if (!LaiueProtocolDecodeBlockDrop(
                frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN;
        event.data.blockDrop.id = decoded.id;
        event.data.blockDrop.block = decoded.block;
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            event.data.blockDrop.position[axis] =
                decoded.position[axis];
        }
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DROP_REMOVE)
    {
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DROP_REMOVE;
        if (!LaiueProtocolDecodeDropRemove(
                frame->payload, frame->payloadSize,
                &event.data.removedDropId))
        {
            return false;
        }
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_INVENTORY_STATE)
    {
        LaiueProtocolInventory decoded;
        if (!LaiueProtocolDecodeInventory(
                frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_INVENTORY_STATE;
        event.data.inventory.selectedHotbarSlot =
            decoded.selectedHotbarSlot;
        for (uint32_t index = 0;
             index < LAIUE_NETWORK_INVENTORY_SLOTS; ++index)
        {
            event.data.inventory.slots[index].item =
                decoded.slots[index].item;
            event.data.inventory.slots[index].count =
                decoded.slots[index].count;
        }
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_PLAYER_JOINED ||
        frame->type == LAIUE_MESSAGE_PLAYER_LEFT)
    {
        event.type =
            frame->type == LAIUE_MESSAGE_PLAYER_JOINED
                ? NETWORK_CLIENT_EVENT_PLAYER_JOINED
                : NETWORK_CLIENT_EVENT_PLAYER_LEFT;
        if (!LaiueProtocolDecodePeerId(
                frame->payload, frame->payloadSize,
                &event.data.peerId))
        {
            return false;
        }
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_WORLD_TIME)
    {
        event.type = NETWORK_CLIENT_EVENT_WORLD_TIME;
        if (!LaiueProtocolDecodeWorldTime(
                frame->payload, frame->payloadSize,
                &event.data.worldTime))
        {
            return false;
        }
        return ClientPushEvent(client, &event);
    }
    return frame->type == LAIUE_MESSAGE_PONG &&
           frame->payloadSize == 0;
}

static bool IsContentStreamMessage(LaiueMessageType type);
static bool IsSnapshotStreamMessage(LaiueMessageType type);

static bool ClientParseFrames(NetworkClient *client)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    for (uint32_t count = 0;
         count < LAIUE_PROTOCOL_MAX_QUEUED_FRAMES; ++count)
    {
        if (client->state ==
                NETWORK_CONNECTION_SYNCING_WORLD &&
            client->syncBeginReceived &&
            !client->snapshotEnded)
        {
            // Initial state and live gameplay follow SYNC_BEGIN on the
            // ordered control stream, while the snapshot uses an independent
            // stream. Hold all later control frames until SNAPSHOT_END so
            // the application always observes world creation first, then
            // inventory/drops/roster/deltas, and READY last.
            return true;
        }
        if (client->eventCount >=
            NETWORK_CLIENT_EVENT_CAPACITY - 1U)
        {
            return true;
        }
        LaiueProtocolFrame frame;
        bool complete = false;
        if (!ChannelPopFrame(
                &client->channel, &frame, payload, &complete))
        {
            return false;
        }
        if (!complete)
        {
            return true;
        }
        if (IsContentStreamMessage(frame.type) ||
            IsSnapshotStreamMessage(frame.type))
        {
            return false;
        }
        if (!ClientHandleFrame(client, &frame))
        {
            return false;
        }
    }
    return true;
}

static bool IsContentStreamMessage(LaiueMessageType type)
{
    return type == LAIUE_MESSAGE_SERVER_CONTENT_BEGIN ||
           type == LAIUE_MESSAGE_SERVER_CONTENT_CHUNK ||
           type == LAIUE_MESSAGE_SERVER_CONTENT_END;
}

static bool IsSnapshotStreamMessage(LaiueMessageType type)
{
    return type == LAIUE_MESSAGE_SNAPSHOT_BEGIN ||
           type == LAIUE_MESSAGE_SNAPSHOT_CHUNK ||
           type == LAIUE_MESSAGE_SNAPSHOT_END;
}

static bool ClientParseServerStream(
    NetworkClient *client, QuicChannel *channel)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    for (uint32_t count = 0;
         count < LAIUE_PROTOCOL_MAX_QUEUED_FRAMES; ++count)
    {
        if (client->eventCount >=
            NETWORK_CLIENT_EVENT_CAPACITY - 1U)
        {
            return true;
        }
        LaiueMessageType type = LAIUE_MESSAGE_COUNT;
        bool complete = false;
        if (!ChannelPeekFrameType(channel, &type, &complete))
        {
            return false;
        }
        if (!complete)
        {
            return !ChannelReceiveEndedWithBytes(channel);
        }
        if (IsSnapshotStreamMessage(type) &&
            client->state == NETWORK_CONNECTION_NEGOTIATING)
        {
            // Independent QUIC streams have no cross-stream ordering.
            // Keep the complete bounded frame queued until WELCOME on the
            // control stream moves the client into SYNCING_WORLD.
            return true;
        }
        if ((!IsContentStreamMessage(type) &&
             !IsSnapshotStreamMessage(type)) ||
            (IsContentStreamMessage(type) &&
             client->state != NETWORK_CONNECTION_NEGOTIATING) ||
            (IsSnapshotStreamMessage(type) &&
             client->state != NETWORK_CONNECTION_SYNCING_WORLD &&
             client->state != NETWORK_CONNECTION_READY))
        {
            return false;
        }
        LaiueProtocolFrame frame;
        if (!ChannelPopFrame(
                channel, &frame, payload, &complete) ||
            !complete || !ClientHandleFrame(client, &frame))
        {
            return false;
        }
    }
    return true;
}

static QUIC_STATUS QUIC_API ServerConnectionCallback(
    HQUIC connection, void *context, QUIC_CONNECTION_EVENT *event)
{
    NetworkServerPeer *peer = context;
    if (peer == NULL || peer->server == NULL || event == NULL)
    {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    NetworkServer *server = peer->server;

    switch (event->Type)
    {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            PlatformRwLockAcquireExclusive(&server->lock);
            if (event->CONNECTED.SessionResumed ||
                !AlpnMatches(
                    event->CONNECTED.NegotiatedAlpnLength,
                    event->CONNECTED.NegotiatedAlpn))
            {
                peer->callbackReason =
                    NETWORK_DISCONNECT_VERSION;
            }
            else
            {
                peer->secureConnected = true;
            }
            PlatformRwLockReleaseExclusive(&server->lock);
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            bool accept = false;
            PlatformRwLockAcquireExclusive(&server->lock);
            if (peer->allocated && peer->secureConnected &&
                peer->channel.stream == NULL &&
                (event->PEER_STREAM_STARTED.Flags &
                    (QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL |
                     QUIC_STREAM_OPEN_FLAG_0_RTT)) == 0)
            {
                peer->channel.stream =
                    event->PEER_STREAM_STARTED.Stream;
                peer->channel.streamStarted = true;
                accept = true;
            }
            else
            {
                peer->callbackReason =
                    NETWORK_DISCONNECT_PROTOCOL;
            }
            PlatformRwLockReleaseExclusive(&server->lock);
            if (accept)
            {
                SetQuicStreamCallback(
                    server->api,
                    event->PEER_STREAM_STARTED.Stream,
                    ChannelStreamCallback, &peer->channel);
            }
            else
            {
                // Returning failure lets MsQuic reject the unowned peer
                // stream; no callback context or handle is published.
                return QUIC_STATUS_NOT_SUPPORTED;
            }
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            PlatformRwLockAcquireExclusive(&server->lock);
            if (peer->callbackReason == NETWORK_DISCONNECT_NONE)
            {
                peer->callbackReason = TransportStatusReason(
                    event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
                    peer->secureConnected, false);
            }
            PlatformRwLockReleaseExclusive(&server->lock);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            PlatformRwLockAcquireExclusive(&server->lock);
            if (peer->callbackReason == NETWORK_DISCONNECT_NONE)
            {
                peer->callbackReason = NETWORK_DISCONNECT_REMOTE;
            }
            PlatformRwLockReleaseExclusive(&server->lock);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            PlatformRwLockAcquireExclusive(&server->lock);
            peer->shutdownComplete = true;
            PlatformRwLockReleaseExclusive(&server->lock);
            break;

        case QUIC_CONNECTION_EVENT_RESUMED:
            PlatformRwLockAcquireExclusive(&server->lock);
            peer->callbackReason = NETWORK_DISCONNECT_PROTOCOL;
            PlatformRwLockReleaseExclusive(&server->lock);
            return QUIC_STATUS_NOT_SUPPORTED;

        default:
            break;
    }
    (void)connection;
    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API ServerListenerCallback(
    HQUIC listener, void *context, QUIC_LISTENER_EVENT *event)
{
    NetworkServer *server = context;
    if (server == NULL || event == NULL)
    {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
    {
        return QUIC_STATUS_SUCCESS;
    }

    NetworkServerPeer *peer = NULL;
    PlatformRwLockAcquireExclusive(&server->lock);
    if (!server->stopping)
    {
        for (uint32_t index = 0;
             index < server->maximumPeers; ++index)
        {
            if (!server->peers[index].allocated)
            {
                peer = &server->peers[index];
                memset(peer, 0, sizeof(*peer));
                peer->server = server;
                peer->connection =
                    event->NEW_CONNECTION.Connection;
                peer->allocated = true;
                peer->rateWindowStartMs =
                    PlatformMonotonicMilliseconds();
                peer->peerId = ++server->nextPeerId;
                if (peer->peerId == 0)
                {
                    peer->peerId = ++server->nextPeerId;
                }
                ChannelInitialize(
                    &peer->channel, server->api, &server->lock);
                ChannelInitialize(
                    &peer->snapshotChannel,
                    server->api, &server->lock);
                peer->snapshotChannel.auxiliary = true;
                ChannelInitialize(
                    &peer->contentChannel,
                    server->api, &server->lock);
                peer->contentChannel.auxiliary = true;
                break;
            }
        }
    }
    PlatformRwLockReleaseExclusive(&server->lock);
    if (peer == NULL)
    {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    SetQuicConnectionCallback(
        server->api, peer->connection,
        ServerConnectionCallback, peer);
    QUIC_STATUS status = server->api->ConnectionSetConfiguration(
        peer->connection, server->configuration);
    PlatformRwLockAcquireExclusive(&server->lock);
    if (QUIC_FAILED(status))
    {
        memset(peer, 0, sizeof(*peer));
    }
    else
    {
        peer->listenerReady = true;
    }
    PlatformRwLockReleaseExclusive(&server->lock);
    (void)listener;
    return status;
}

static bool ServerPushEvent(
    NetworkServer *server, const NetworkServerEvent *event)
{
    if (server->eventCount >= NETWORK_SERVER_EVENT_CAPACITY)
    {
        return false;
    }
    server->events[server->eventWrite] = *event;
    server->eventWrite =
        (server->eventWrite + 1U) % NETWORK_SERVER_EVENT_CAPACITY;
    ++server->eventCount;
    return true;
}

typedef struct ServerPeerCallbackState
{
    NetworkDisconnectReason reason;
    bool allocated;
    bool secureConnected;
    bool shutdownComplete;
    bool peerSendClosed;
} ServerPeerCallbackState;

static ServerPeerCallbackState ServerTakePeerCallbackState(
    NetworkServer *server, NetworkServerPeer *peer)
{
    ServerPeerCallbackState state;
    memset(&state, 0, sizeof(state));
    PlatformRwLockAcquireExclusive(&server->lock);
    state.allocated =
        peer->allocated && peer->listenerReady;
    if (state.allocated)
    {
        state.reason = peer->callbackReason;
        peer->callbackReason = NETWORK_DISCONNECT_NONE;
        state.secureConnected = peer->secureConnected;
        state.shutdownComplete = peer->shutdownComplete;
        state.peerSendClosed = peer->channel.peerSendClosed;
    }
    PlatformRwLockReleaseExclusive(&server->lock);
    return state;
}

static NetworkServerPeer *ServerFindPeer(
    NetworkServer *server, uint32_t peerId)
{
    if (server == NULL || peerId == 0)
    {
        return NULL;
    }
    NetworkServerPeer *found = NULL;
    PlatformRwLockAcquireShared(&server->lock);
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (peer->allocated && peer->listenerReady &&
            peer->peerId == peerId)
        {
            found = peer;
            break;
        }
    }
    PlatformRwLockReleaseShared(&server->lock);
    return found;
}

static bool ServerDisconnectPeer(
    NetworkServer *server, NetworkServerPeer *peer,
    NetworkDisconnectReason reason)
{
    if (server == NULL || peer == NULL ||
        reason <= NETWORK_DISCONNECT_NONE ||
        reason > NETWORK_DISCONNECT_CONFIGURATION)
    {
        return false;
    }
    HQUIC connection = NULL;
    PlatformRwLockAcquireExclusive(&server->lock);
    if (!peer->allocated || !peer->listenerReady ||
        peer->closing)
    {
        PlatformRwLockReleaseExclusive(&server->lock);
        return false;
    }
    peer->closing = true;
    peer->closeReason = reason;
    connection = peer->connection;
    PlatformRwLockReleaseExclusive(&server->lock);
    if (connection != NULL)
    {
        server->api->ConnectionShutdown(
            connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
            reason == NETWORK_DISCONNECT_OVERFLOW
                ? NETWORK_APP_ERROR_OVERFLOW
                : NETWORK_APP_ERROR_PROTOCOL);
    }
    return true;
}

static void ServerFinalizePeer(
    NetworkServer *server, NetworkServerPeer *peer)
{
    uint32_t peerId = 0;
    NetworkDisconnectReason reason =
        NETWORK_DISCONNECT_REMOTE;
    bool notify = false;
    HQUIC controlStream = NULL;
    HQUIC snapshotStream = NULL;
    HQUIC contentStream = NULL;
    HQUIC connection = NULL;

    PlatformRwLockAcquireExclusive(&server->lock);
    if (!peer->allocated || !peer->listenerReady ||
        !peer->closing || !peer->shutdownComplete ||
        (peer->channel.stream != NULL &&
         !peer->channel.shutdownComplete) ||
        (peer->snapshotChannel.stream != NULL &&
         !peer->snapshotChannel.shutdownComplete) ||
        (peer->contentChannel.stream != NULL &&
         !peer->contentChannel.shutdownComplete))
    {
        PlatformRwLockReleaseExclusive(&server->lock);
        return;
    }
    peerId = peer->peerId;
    reason =
        peer->closeReason != NETWORK_DISCONNECT_NONE
            ? peer->closeReason
            : NETWORK_DISCONNECT_REMOTE;
    notify = peer->connectedNotified;
    if (notify &&
        server->eventCount >= NETWORK_SERVER_EVENT_CAPACITY)
    {
        // Keep the closed slot until the main thread drains room for the
        // mandatory terminal event. The next update retries finalization.
        PlatformRwLockReleaseExclusive(&server->lock);
        return;
    }
    controlStream = peer->channel.stream;
    snapshotStream = peer->snapshotChannel.stream;
    contentStream = peer->contentChannel.stream;
    connection = peer->connection;
    PlatformRwLockReleaseExclusive(&server->lock);

    if (controlStream != NULL)
    {
        server->api->StreamClose(controlStream);
    }
    if (snapshotStream != NULL)
    {
        server->api->StreamClose(snapshotStream);
    }
    if (contentStream != NULL)
    {
        server->api->StreamClose(contentStream);
    }
    if (connection != NULL)
    {
        server->api->ConnectionClose(connection);
    }

    // ConnectionClose/StreamClose synchronously retire their callback
    // handles. Only now may the slot be exposed for listener reuse.
    PlatformRwLockAcquireExclusive(&server->lock);
    memset(peer, 0, sizeof(*peer));
    PlatformRwLockReleaseExclusive(&server->lock);
    if (notify)
    {
        NetworkServerEvent event;
        memset(&event, 0, sizeof(event));
        event.type = NETWORK_SERVER_EVENT_DISCONNECTED;
        event.peerId = peerId;
        event.data.disconnectReason = reason;
        (void)ServerPushEvent(server, &event);
    }
}

static bool ServerCheckRate(
    NetworkServerPeer *peer, LaiueMessageType type)
{
    uint64_t now = PlatformMonotonicMilliseconds();
    if (now - peer->rateWindowStartMs >= NETWORK_RATE_WINDOW_MS)
    {
        peer->rateWindowStartMs = now;
        peer->framesInWindow = 0;
        peer->inputsInWindow = 0;
        peer->editsInWindow = 0;
        peer->resyncsInWindow = 0;
    }
    ++peer->framesInWindow;
    if (type == LAIUE_MESSAGE_PLAYER_INPUT)
    {
        ++peer->inputsInWindow;
    }
    else if (type == LAIUE_MESSAGE_EDIT_INTENT)
    {
        ++peer->editsInWindow;
    }
    else if (type == LAIUE_MESSAGE_CHUNK_RESYNC_REQUEST)
    {
        ++peer->resyncsInWindow;
    }
    return peer->framesInWindow <=
               NETWORK_MAX_FRAMES_PER_SECOND &&
           peer->inputsInWindow <=
               NETWORK_MAX_INPUTS_PER_SECOND &&
           peer->editsInWindow <=
               NETWORK_MAX_EDITS_PER_SECOND &&
           peer->resyncsInWindow <=
               NETWORK_MAX_RESYNCS_PER_SECOND;
}

static bool ServerFinishModNegotiation(
    NetworkServer *server, NetworkServerPeer *peer)
{
    bool matches =
        peer->receivedModCount == server->modCount;
    for (uint32_t index = 0;
         index < server->modCount && matches; ++index)
    {
        matches = ModDescriptorsEqual(
            &peer->mods[index], &server->mods[index]);
    }

    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    if (!matches)
    {
        uint32_t size = LaiueProtocolEncodeReject(
            payload, sizeof(payload),
            NETWORK_REJECT_MOD_MISMATCH);
        if (size == 0 ||
            !ChannelSendPayload(
                &peer->channel, LAIUE_MESSAGE_SERVER_REJECT,
                payload, size))
        {
            return false;
        }
        peer->rejected = true;
        return true;
    }

    uint32_t size = LaiueProtocolEncodeWelcome(
        payload, sizeof(payload), peer->peerId,
        server->worldSeed, peer->clientNonce);
    if (size == 0 ||
        !ChannelSendPayload(
            &peer->channel, LAIUE_MESSAGE_SERVER_WELCOME,
            payload, size))
    {
        return false;
    }
    peer->negotiated = true;
    peer->connectedNotified = true;
    NetworkServerEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NETWORK_SERVER_EVENT_CONNECTED;
    event.peerId = peer->peerId;
    return ServerPushEvent(server, &event);
}

static bool ServerOpenSendStream(
    NetworkServerPeer *peer, QuicChannel *channel)
{
    if (peer == NULL || channel == NULL)
    {
        return false;
    }
    if (ChannelIsActive(channel))
    {
        return false;
    }
    ChannelInitialize(
        channel, peer->server->api, &peer->server->lock);
    channel->auxiliary = true;
    HQUIC stream = NULL;
    if (QUIC_FAILED(peer->server->api->StreamOpen(
            peer->connection,
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            ChannelStreamCallback, channel, &stream)))
    {
        return false;
    }
    PlatformRwLockAcquireExclusive(&peer->server->lock);
    channel->stream = stream;
    PlatformRwLockReleaseExclusive(&peer->server->lock);
    if (QUIC_FAILED(peer->server->api->StreamStart(
            stream,
            QUIC_STREAM_START_FLAG_IMMEDIATE |
                QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL)))
    {
        peer->server->api->StreamClose(stream);
        PlatformRwLockAcquireExclusive(&peer->server->lock);
        channel->stream = NULL;
        PlatformRwLockReleaseExclusive(&peer->server->lock);
        return false;
    }
    return true;
}

static bool ServerBeginContentTransfer(
    NetworkServer *server, NetworkServerPeer *peer)
{
    if (!server->allowContentDownloads ||
        server->contentBundle == NULL ||
        server->contentBundleSize == 0 ||
        peer->contentTransferActive)
    {
        return false;
    }
    if (!ServerOpenSendStream(peer, &peer->contentChannel))
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeContentBegin(
        payload, sizeof(payload), server->contentBundleSize,
        server->contentBundleHash);
    if (size == 0 ||
        !ChannelSendPayload(
            &peer->contentChannel,
            LAIUE_MESSAGE_SERVER_CONTENT_BEGIN,
            payload, size))
    {
        return false;
    }
    peer->contentOffset = 0;
    peer->contentTransferActive = true;
    return true;
}

static bool ServerPumpContentTransfer(
    NetworkServer *server, NetworkServerPeer *peer)
{
    if (!peer->contentTransferActive)
    {
        return true;
    }
    for (uint32_t index = 0; index < 8U; ++index)
    {
        if (ChannelOutstandingSends(&peer->contentChannel) >=
            LAIUE_NETWORK_MAX_OUTSTANDING_SENDS - 8U)
        {
            return true;
        }
        if (peer->contentOffset == server->contentBundleSize)
        {
            if (!ChannelSendPayload(
                    &peer->contentChannel,
                    LAIUE_MESSAGE_SERVER_CONTENT_END,
                    NULL, 0))
            {
                return false;
            }
            peer->contentTransferActive = false;
            return ChannelShutdownGracefully(
                &peer->contentChannel);
        }
        uint64_t remaining =
            server->contentBundleSize - peer->contentOffset;
        uint32_t chunkSize =
            remaining > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE
                ? LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE
                : (uint32_t)remaining;
        if (!ChannelSendPayload(
                &peer->contentChannel,
                LAIUE_MESSAGE_SERVER_CONTENT_CHUNK,
                server->contentBundle + peer->contentOffset,
                chunkSize))
        {
            return false;
        }
        peer->contentOffset += chunkSize;
    }
    return true;
}

static bool ServerAcceptChunkResyncRequest(
    NetworkServer *server, NetworkServerPeer *peer,
    const LaiueProtocolFrame *frame)
{
    LaiueProtocolChunkResyncRequest request;
    if (peer->resyncRequestOutstanding ||
        !LaiueProtocolDecodeChunkResyncRequest(
            frame->payload, frame->payloadSize, &request))
    {
        return false;
    }

    NetworkServerEvent event;
    memset(&event, 0, sizeof(event));
    event.type =
        NETWORK_SERVER_EVENT_CHUNK_RESYNC_REQUEST;
    event.peerId = peer->peerId;
    memcpy(event.data.chunkResync.chunk, request.chunk,
        sizeof(event.data.chunkResync.chunk));
    event.data.chunkResync.expectedRevision =
        request.expectedRevision;
    if (!ServerPushEvent(server, &event))
    {
        return false;
    }

    memcpy(peer->resyncRequestChunk, request.chunk,
        sizeof(peer->resyncRequestChunk));
    peer->resyncExpectedRevision =
        request.expectedRevision;
    peer->resyncRequestOutstanding = true;
    peer->resyncResponseMatched = false;
    return true;
}

static bool ServerHandleFrame(
    NetworkServer *server, NetworkServerPeer *peer,
    const LaiueProtocolFrame *frame)
{
    if (!ServerCheckRate(peer, frame->type))
    {
        return false;
    }

    NetworkServerEvent event;
    memset(&event, 0, sizeof(event));
    event.peerId = peer->peerId;
    if (!peer->negotiated)
    {
        if (!peer->helloReceived)
        {
            if (frame->type != LAIUE_MESSAGE_CLIENT_HELLO ||
                !LaiueProtocolDecodeHello(
                    frame->payload, frame->payloadSize,
                    &peer->clientNonce))
            {
                return false;
            }
            peer->helloReceived = true;
            return ChannelSendModList(
                &peer->channel,
                LAIUE_MESSAGE_SERVER_MOD_LIST,
                LAIUE_MESSAGE_SERVER_MOD_ENTRY,
                server->mods, server->modCount,
                server->allowContentDownloads ? 1U : 0U);
        }
        if (!peer->modListReceived)
        {
            if (frame->type ==
                    LAIUE_MESSAGE_CLIENT_CONTENT_REQUEST &&
                frame->payloadSize == 0)
            {
                return ServerBeginContentTransfer(server, peer);
            }
            uint8_t flags = 0;
            if (frame->type != LAIUE_MESSAGE_CLIENT_MOD_LIST ||
                peer->contentTransferActive ||
                !LaiueProtocolDecodeModList(
                    frame->payload, frame->payloadSize,
                    &peer->expectedModCount, &flags) ||
                flags != 0)
            {
                return false;
            }
            peer->modListReceived = true;
            return peer->expectedModCount != 0 ||
                   ServerFinishModNegotiation(server, peer);
        }
        if (frame->type != LAIUE_MESSAGE_CLIENT_MOD_ENTRY ||
            peer->receivedModCount >= peer->expectedModCount)
        {
            return false;
        }
        LaiueProtocolMod wire;
        if (!LaiueProtocolDecodeMod(
                frame->payload, frame->payloadSize, &wire))
        {
            return false;
        }
        CopyModFromProtocol(
            &peer->mods[peer->receivedModCount++], &wire);
        return peer->receivedModCount != peer->expectedModCount ||
               ServerFinishModNegotiation(server, peer);
    }

    if (!peer->ready)
    {
        if (frame->type == LAIUE_MESSAGE_SYNC_APPLIED &&
            peer->awaitingSyncApplied &&
            !peer->snapshotStarted)
        {
            uint64_t snapshotId = 0;
            uint64_t worldRevision = 0;
            if (!LaiueProtocolDecodeSyncApplied(
                    frame->payload, frame->payloadSize,
                    &snapshotId, &worldRevision) ||
                snapshotId != peer->snapshotId ||
                worldRevision != peer->snapshotWorldRevision)
            {
                return false;
            }
            peer->awaitingSyncApplied = false;
            peer->ready = true;
            peer->everReady = true;
            return true;
        }
        if (frame->type == LAIUE_MESSAGE_PING &&
            frame->payloadSize == 0)
        {
            return ChannelSendPayload(
                &peer->channel, LAIUE_MESSAGE_PONG, NULL, 0);
        }
        if (!peer->everReady)
        {
            return false;
        }

        // SYNC_BEGIN is ordered on the control stream, but gameplay frames
        // already sent by a previously-ready client can still be in flight.
        // Validate and discard only those well-formed stale commands.
        if (frame->type == LAIUE_MESSAGE_PLAYER_INPUT)
        {
            LaiueProtocolInput ignored;
            return LaiueProtocolDecodeInput(
                frame->payload, frame->payloadSize, &ignored);
        }
        if (frame->type == LAIUE_MESSAGE_EDIT_INTENT)
        {
            bool breakBlock = false;
            bool placeBlock = false;
            uint8_t placementBlock = 0;
            float direction[3];
            return LaiueProtocolDecodeEditIntent(
                frame->payload, frame->payloadSize,
                &breakBlock, &placeBlock, &placementBlock,
                direction);
        }
        if (frame->type == LAIUE_MESSAGE_SELECT_HOTBAR_SLOT)
        {
            return frame->payloadSize == 1U &&
                   frame->payload[0] < 9U;
        }
        if (frame->type == LAIUE_MESSAGE_CHUNK_RESYNC_REQUEST)
        {
            return ServerAcceptChunkResyncRequest(
                server, peer, frame);
        }
        return false;
    }
    if (frame->type == LAIUE_MESSAGE_PLAYER_INPUT)
    {
        LaiueProtocolInput decoded;
        if (!LaiueProtocolDecodeInput(
                frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_SERVER_EVENT_INPUT;
        event.data.input.sequence = decoded.sequence;
        event.data.input.movementX = decoded.movementX;
        event.data.input.movementY = decoded.movementY;
        event.data.input.yaw = decoded.yaw;
        event.data.input.pitch = decoded.pitch;
        event.data.input.jumpPressed = decoded.jumpPressed;
        event.data.input.jumpHeld = decoded.jumpHeld;
        event.data.input.sprintHeld = decoded.sprintHeld;
        event.data.input.crouchHeld = decoded.crouchHeld;
        return ServerPushEvent(server, &event);
    }
    if (frame->type == LAIUE_MESSAGE_EDIT_INTENT)
    {
        event.type = NETWORK_SERVER_EVENT_EDIT_INTENT;
        if (!LaiueProtocolDecodeEditIntent(
                frame->payload, frame->payloadSize,
                &event.data.editIntent.breakBlock,
                &event.data.editIntent.placeBlock,
                &event.data.editIntent.placementBlock,
                event.data.editIntent.direction))
        {
            return false;
        }
        return ServerPushEvent(server, &event);
    }
    if (frame->type == LAIUE_MESSAGE_SELECT_HOTBAR_SLOT &&
        frame->payloadSize == 1U && frame->payload[0] < 9U)
    {
        event.type = NETWORK_SERVER_EVENT_SELECT_HOTBAR_SLOT;
        event.data.selectedHotbarSlot = frame->payload[0];
        return ServerPushEvent(server, &event);
    }
    if (frame->type == LAIUE_MESSAGE_CHUNK_RESYNC_REQUEST)
    {
        return ServerAcceptChunkResyncRequest(
            server, peer, frame);
    }
    return frame->type == LAIUE_MESSAGE_PING &&
                   frame->payloadSize == 0
               ? ChannelSendPayload(
                     &peer->channel, LAIUE_MESSAGE_PONG,
                     NULL, 0)
               : false;
}

static bool ServerParseFrames(
    NetworkServer *server, NetworkServerPeer *peer)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    for (uint32_t count = 0;
         count < LAIUE_PROTOCOL_MAX_QUEUED_FRAMES; ++count)
    {
        if (server->eventCount >=
            NETWORK_SERVER_EVENT_CAPACITY - 1U)
        {
            return true;
        }
        LaiueProtocolFrame frame;
        bool complete = false;
        if (!ChannelPopFrame(
                &peer->channel, &frame, payload, &complete))
        {
            return false;
        }
        if (!complete)
        {
            return true;
        }
        if (!ServerHandleFrame(server, peer, &frame))
        {
            return false;
        }
    }
    return true;
}

NetworkSecureTransportStatus NetworkGetSecureTransportStatus(void)
{
    const QUIC_API_TABLE *api = NULL;
    HQUIC registration = NULL;
    if (!OpenApiAndRegistration(&api, &registration))
    {
        return NETWORK_SECURE_TRANSPORT_INITIALIZATION_FAILED;
    }
    api->RegistrationClose(registration);
    MsQuicClose(api);
    return NETWORK_SECURE_TRANSPORT_AVAILABLE;
}

static bool LoadClientCredentials(NetworkClient *client)
{
    QUIC_CREDENTIAL_CONFIG credentials;
    memset(&credentials, 0, sizeof(credentials));
    credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credentials.Flags =
        QUIC_CREDENTIAL_FLAG_CLIENT |
        QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
        QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES |
        QUIC_CREDENTIAL_FLAG_DEFER_CERTIFICATE_VALIDATION;
#if !defined(_WIN32)
    credentials.Flags |=
        QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION;
#endif
    // Keep the TLS backend's chain result available to the callback. System
    // trust requires a successful chain; pin trust may replace only an
    // untrusted root and still requires exact DER, validity and SAN checks.
    // NO_CERTIFICATE_VALIDATION remains deliberately unused.
    return !QUIC_FAILED(client->api->ConfigurationLoadCredential(
        client->configuration, &credentials));
}

NetworkClient *NetworkClientCreate(
    const NetworkClientConfiguration *configuration)
{
    if (!NetworkClientConfigurationIsValid(configuration))
    {
        return NULL;
    }

    NetworkClient *client =
        PlatformAllocate(sizeof(*client), true);
    if (client == NULL ||
        !PlatformRwLockInitialize(&client->lock))
    {
        PlatformFree(client);
        return NULL;
    }
    client->connectionConfiguration = *configuration;
    client->state = NETWORK_CONNECTION_CONNECTING;
    if (!OpenApiAndRegistration(
            &client->api, &client->registration))
    {
        NetworkClientDestroy(client);
        return NULL;
    }
    ChannelInitialize(
        &client->channel, client->api, &client->lock);
    client->channel.receive = client->controlReceive;
    client->channel.receiveCapacity =
        NETWORK_CONTROL_RECEIVE_CAPACITY;
    for (uint32_t index = 0;
         index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS; ++index)
    {
        ChannelInitialize(
            &client->serverChannels[index],
            client->api, &client->lock);
        client->serverChannels[index].receive =
            client->serverReceive[index];
        client->serverChannels[index].receiveCapacity =
            NETWORK_AUX_RECEIVE_CAPACITY;
        client->serverChannels[index].auxiliary = true;
    }
    if (!OpenConfiguration(
            client->api, client->registration, false,
            configuration->handshakeTimeoutMs,
            configuration->idleTimeoutMs,
            &client->configuration) ||
        !LoadClientCredentials(client) ||
        QUIC_FAILED(client->api->ConnectionOpen(
            client->registration, ClientConnectionCallback,
            client, &client->connection)))
    {
        NetworkClientDestroy(client);
        return NULL;
    }
    if (configuration->endpoint.kind == NETWORK_ENDPOINT_DNS)
    {
        QUIC_ADDR remoteAddress;
        if (!ResolveDnsEndpoint(configuration, &remoteAddress))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_DNS);
            return client;
        }
        if (QUIC_FAILED(client->api->SetParam(
                client->connection,
                QUIC_PARAM_CONN_REMOTE_ADDRESS,
                (uint32_t)sizeof(remoteAddress),
                &remoteAddress)))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
            return client;
        }
    }
    QUIC_STATUS status = client->api->ConnectionStart(
        client->connection, client->configuration,
        QuicFamily(configuration->addressFamily),
        configuration->endpoint.host,
        configuration->endpoint.port);
    if (QUIC_FAILED(status))
    {
        ClientDisconnect(
            client,
            TransportStatusReason(
                status, false,
                configuration->endpoint.kind ==
                    NETWORK_ENDPOINT_DNS));
        return client;
    }
    client->connectionStarted = true;
    client->state = NETWORK_CONNECTION_VERIFYING_SERVER;
    return client;
}

NetworkClient *NetworkClientCreateLoopback(uint16_t port)
{
    NetworkClientConfiguration configuration;
    NetworkClientConfigurationInitialize(&configuration);
    if (NetworkEndpointParse(
            "localhost",
            port != 0 ? port : LAIUE_NETWORK_DEFAULT_PORT,
            &configuration.endpoint) != NETWORK_ENDPOINT_PARSE_OK)
    {
        return NULL;
    }
    return NetworkClientCreate(&configuration);
}

static bool ClientTakeShutdownStreams(
    NetworkClient *client, HQUIC *outControlStream,
    HQUIC outServerStreams[LAIUE_PROTOCOL_MAX_SERVER_STREAMS])
{
    bool ready = false;
    PlatformRwLockAcquireExclusive(&client->lock);
    ready = client->shutdownComplete &&
            (client->channel.stream == NULL ||
             client->channel.shutdownComplete);
    for (uint32_t index = 0;
         index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS && ready; ++index)
    {
        ready =
            client->serverChannels[index].stream == NULL ||
            client->serverChannels[index].shutdownComplete;
    }
    if (ready)
    {
        *outControlStream = client->channel.stream;
        client->channel.stream = NULL;
        for (uint32_t index = 0;
             index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS; ++index)
        {
            outServerStreams[index] =
                client->serverChannels[index].stream;
            client->serverChannels[index].stream = NULL;
        }
    }
    PlatformRwLockReleaseExclusive(&client->lock);
    return ready;
}

void NetworkClientDestroy(NetworkClient *client)
{
    if (client == NULL)
    {
        return;
    }
    HQUIC connection = NULL;
    bool connectionStarted = false;
    PlatformRwLockAcquireExclusive(&client->lock);
    client->stopping = true;
    connection = client->connection;
    connectionStarted = client->connectionStarted;
    PlatformRwLockReleaseExclusive(&client->lock);
    if (connection != NULL)
    {
        HQUIC controlStream = NULL;
        HQUIC serverStreams[LAIUE_PROTOCOL_MAX_SERVER_STREAMS] = {0};
        bool shutdownDrained = !connectionStarted;
        if (connectionStarted)
        {
            client->api->ConnectionShutdown(
                connection,
                QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
                NETWORK_APP_ERROR_SHUTDOWN);
            uint64_t deadline = PlatformMonotonicMilliseconds() +
                                NETWORK_SHUTDOWN_DRAIN_TIMEOUT_MS;
            do
            {
                shutdownDrained = ClientTakeShutdownStreams(
                    client, &controlStream, serverStreams);
                if (shutdownDrained ||
                    PlatformMonotonicMilliseconds() >= deadline)
                {
                    break;
                }
                PlatformSleepMilliseconds(1U);
            } while (true);
        }
        if (shutdownDrained)
        {
            if (controlStream != NULL)
            {
                client->api->StreamClose(controlStream);
            }
            for (uint32_t index = 0;
                 index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS; ++index)
            {
                if (serverStreams[index] != NULL)
                {
                    client->api->StreamClose(serverStreams[index]);
                }
            }
        }
        // ConnectionClose is synchronous. Normally all child handles were
        // closed above only after their SHUTDOWN_COMPLETE callbacks.
        client->api->ConnectionClose(connection);
        client->connection = NULL;
    }
    if (client->configuration != NULL)
    {
        client->api->ConfigurationClose(client->configuration);
    }
    if (client->registration != NULL)
    {
        client->api->RegistrationClose(client->registration);
    }
    if (client->api != NULL)
    {
        MsQuicClose(client->api);
    }
    PlatformFree(client->contentBytes);
    PlatformRwLockDestroy(&client->lock);
    PlatformFree(client);
}

void NetworkClientUpdate(NetworkClient *client)
{
    if (client == NULL ||
        client->state == NETWORK_CONNECTION_DISCONNECTED)
    {
        return;
    }

    NetworkDisconnectReason reason;
    bool connected;
    bool verified;
    PlatformRwLockAcquireExclusive(&client->lock);
    reason = client->callbackReason;
    client->callbackReason = NETWORK_DISCONNECT_NONE;
    connected = client->transportConnected;
    verified = client->certificateVerified;
    PlatformRwLockReleaseExclusive(&client->lock);
    NetworkDisconnectReason channelReason =
        ChannelTakeError(&client->channel);
    if (reason == NETWORK_DISCONNECT_NONE)
    {
        reason = channelReason;
    }
    for (uint32_t index = 0;
         index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS &&
         reason == NETWORK_DISCONNECT_NONE; ++index)
    {
        if (ChannelIsActive(&client->serverChannels[index]))
        {
            reason = ChannelTakeError(
                &client->serverChannels[index]);
        }
    }
    if (reason != NETWORK_DISCONNECT_NONE)
    {
        ClientDisconnect(client, reason);
        return;
    }

    if (connected && verified &&
        !client->transportConnectedHandled)
    {
        client->transportConnectedHandled = true;
        if (!ClientBeginProtocol(client))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
            return;
        }
    }
    if (client->transportConnectedHandled &&
        !ClientParseFrames(client))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_PROTOCOL);
        return;
    }
    for (uint32_t index = 0;
         index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS; ++index)
    {
        if (!ChannelIsActive(&client->serverChannels[index]))
        {
            continue;
        }
        if (!ClientParseServerStream(
                client, &client->serverChannels[index]))
        {
            ClientDisconnect(
                client, NETWORK_DISCONNECT_PROTOCOL);
            return;
        }
        bool retired = false;
        if (!ChannelTryRetireAuxiliary(
                &client->serverChannels[index], &retired))
        {
            ClientDisconnect(
                client, NETWORK_DISCONNECT_PROTOCOL);
            return;
        }
    }

    uint64_t now = PlatformMonotonicMilliseconds();
    uint32_t handshakeTimeout =
        client->connectionConfiguration.handshakeTimeoutMs != 0
            ? client->connectionConfiguration.handshakeTimeoutMs
            : (uint32_t)NETWORK_HANDSHAKE_TIMEOUT_MS;
    uint32_t idleTimeout =
        client->connectionConfiguration.idleTimeoutMs != 0
            ? client->connectionConfiguration.idleTimeoutMs
            : (uint32_t)NETWORK_IDLE_TIMEOUT_MS;
    if (client->state == NETWORK_CONNECTION_READY)
    {
        uint32_t keepAliveInterval = idleTimeout / 3U;
        if (keepAliveInterval < 1000U)
        {
            keepAliveInterval = 1000U;
        }
        if (now - ChannelLastSendAt(&client->channel) >=
                keepAliveInterval &&
            !ChannelSendPayload(
                &client->channel, LAIUE_MESSAGE_PING, NULL, 0))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
            return;
        }
    }
    uint64_t lastActivity =
        ChannelLastActivityAt(&client->channel);
    for (uint32_t index = 0;
         index < LAIUE_PROTOCOL_MAX_SERVER_STREAMS; ++index)
    {
        if (!ChannelIsActive(&client->serverChannels[index]))
        {
            continue;
        }
        uint64_t channelActivity =
            ChannelLastActivityAt(&client->serverChannels[index]);
        if (channelActivity > lastActivity)
        {
            lastActivity = channelActivity;
        }
    }
    uint64_t deadline =
        client->state == NETWORK_CONNECTION_VERIFYING_SERVER
            ? client->channel.connectedAtMs + handshakeTimeout
            : lastActivity +
                  (client->state == NETWORK_CONNECTION_NEGOTIATING ||
                           client->state ==
                               NETWORK_CONNECTION_SYNCING_WORLD
                       ? NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS
                       : idleTimeout);
    if (now > deadline)
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_TIMEOUT);
    }
}

NetworkConnectionState NetworkClientGetState(
    const NetworkClient *client)
{
    return client != NULL
               ? client->state
               : NETWORK_CONNECTION_DISCONNECTED;
}

bool NetworkClientPollEvent(
    NetworkClient *client, NetworkClientEvent *outEvent)
{
    if (client == NULL || outEvent == NULL ||
        client->eventCount == 0)
    {
        return false;
    }
    *outEvent = client->events[client->eventRead];
    client->eventRead =
        (client->eventRead + 1U) % NETWORK_CLIENT_EVENT_CAPACITY;
    --client->eventCount;
    return true;
}

bool NetworkClientCopyServerMods(
    const NetworkClient *client, NetworkModDescriptor *output,
    uint32_t capacity, uint32_t *outCount)
{
    if (client == NULL || outCount == NULL ||
        (client->receivedServerModCount != 0 &&
         (output == NULL ||
          capacity < client->receivedServerModCount)))
    {
        return false;
    }
    if (client->receivedServerModCount != 0)
    {
        memcpy(
            output, client->serverMods,
            sizeof(*output) * client->receivedServerModCount);
    }
    *outCount = client->receivedServerModCount;
    return true;
}

bool NetworkClientSubmitMods(
    NetworkClient *client, const NetworkModDescriptor *mods,
    uint32_t count)
{
    if (client == NULL ||
        client->state != NETWORK_CONNECTION_NEGOTIATING ||
        client->modsSubmitted || count > LAIUE_NETWORK_MAX_MODS ||
        (count != 0 && mods == NULL) ||
        (client->contentRequested && !client->contentVerified))
    {
        return false;
    }
    if (!ChannelSendModList(
            &client->channel,
            LAIUE_MESSAGE_CLIENT_MOD_LIST,
            LAIUE_MESSAGE_CLIENT_MOD_ENTRY,
            mods, count, 0))
    {
        return false;
    }
    client->modsSubmitted = true;
    return true;
}

bool NetworkClientRequestContent(NetworkClient *client)
{
    if (client == NULL ||
        client->state != NETWORK_CONNECTION_NEGOTIATING ||
        !client->serverDownloadsAllowed ||
        client->contentRequested)
    {
        return false;
    }
    if (!ChannelSendPayload(
            &client->channel,
            LAIUE_MESSAGE_CLIENT_CONTENT_REQUEST, NULL, 0))
    {
        return false;
    }
    client->contentRequested = true;
    return true;
}

bool NetworkClientTakeContent(
    NetworkClient *client, uint8_t **outBytes, uint64_t *outSize)
{
    if (client == NULL || outBytes == NULL || outSize == NULL ||
        client->contentBytes == NULL ||
        client->receivedContentSize != client->expectedContentSize)
    {
        return false;
    }
    *outBytes = client->contentBytes;
    *outSize = client->receivedContentSize;
    client->contentBytes = NULL;
    client->expectedContentSize = 0;
    client->receivedContentSize = 0;
    return true;
}

bool NetworkClientSendInput(
    NetworkClient *client, const NetworkInputCommand *input)
{
    if (client == NULL || input == NULL ||
        client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    LaiueProtocolInput wire = {
        .sequence = input->sequence,
        .movementX = input->movementX,
        .movementY = input->movementY,
        .yaw = input->yaw,
        .pitch = input->pitch,
        .jumpPressed = input->jumpPressed,
        .jumpHeld = input->jumpHeld,
        .sprintHeld = input->sprintHeld,
        .crouchHeld = input->crouchHeld,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size =
        LaiueProtocolEncodeInput(payload, sizeof(payload), &wire);
    if (size == 0)
    {
        return false;
    }
    if (!ChannelSendPayload(
            &client->channel, LAIUE_MESSAGE_PLAYER_INPUT,
            payload, size))
    {
        // Input is the only client send performed continuously at 60 Hz.
        // Once its bounded QUIC queue is exhausted, keeping the connection
        // nominally READY would freeze prediction and repeatedly retry the
        // same tick. Fail the offending connection just like the loopback
        // backend, while other peers and the server keep running.
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return true;
}

bool NetworkClientSendEditIntent(
    NetworkClient *client, bool breakBlock, bool placeBlock,
    uint8_t placementBlock, const float direction[3])
{
    if (client == NULL || direction == NULL ||
        client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeEditIntent(
        payload, sizeof(payload), breakBlock, placeBlock,
        placementBlock, direction);
    return size != 0 &&
           ChannelSendPayload(
               &client->channel, LAIUE_MESSAGE_EDIT_INTENT,
               payload, size);
}

bool NetworkClientSendSelectedHotbarSlot(
    NetworkClient *client, uint8_t slot)
{
    return client != NULL && slot < 9U &&
           client->state == NETWORK_CONNECTION_READY &&
           ChannelSendPayload(
               &client->channel,
               LAIUE_MESSAGE_SELECT_HOTBAR_SLOT,
               &slot, 1U);
}

bool NetworkClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision)
{
    if (client == NULL || chunk == NULL ||
        client->state != NETWORK_CONNECTION_READY ||
        client->resyncRequestOutstanding)
    {
        return false;
    }
    LaiueProtocolChunkResyncRequest request = {
        { chunk[0], chunk[1], chunk[2] },
        expectedRevision,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeChunkResyncRequest(
        payload, sizeof(payload), &request);
    if (size == 0 ||
        !ChannelSendPayload(
            &client->channel,
            LAIUE_MESSAGE_CHUNK_RESYNC_REQUEST,
            payload, size))
    {
        return false;
    }
    memcpy(client->resyncRequestChunk, chunk,
        sizeof(client->resyncRequestChunk));
    client->resyncExpectedRevision = expectedRevision;
    client->resyncRequestOutstanding = true;
    client->resyncResponseMatched = false;
    return true;
}

bool NetworkClientAcknowledgeReady(NetworkClient *client)
{
    if (client == NULL ||
        client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    if (client->syncAppliedSent)
    {
        return true;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeSyncApplied(
        payload, sizeof(payload), client->snapshotId,
        client->snapshotWorldRevision);
    if (size == 0 ||
        !ChannelSendPayload(
            &client->channel,
            LAIUE_MESSAGE_SYNC_APPLIED, payload, size))
    {
        return false;
    }
    client->syncAppliedSent = true;
    return true;
}

#if !defined(_WIN32)
static bool ValidateServerPrivateKey(const char *path)
{
    uint32_t length =
        AsciiLength(path, LAIUE_PLATFORM_PATH_CAPACITY * 4U);
    wchar_t wide[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t wideLength = 0;
    return length != 0 &&
           length < LAIUE_PLATFORM_PATH_CAPACITY * 4U &&
           PlatformUtf8ToWide(
               path, length, wide,
               LAIUE_PLATFORM_PATH_CAPACITY, &wideLength) &&
           wideLength != 0 &&
           PlatformValidatePrivateKeyFile(wide);
}
#endif

static bool LoadServerCredentials(
    NetworkServer *server,
    const NetworkServerConfiguration *configuration)
{
    QUIC_CREDENTIAL_CONFIG credentials;
    memset(&credentials, 0, sizeof(credentials));
#if defined(_WIN32)
    QUIC_CERTIFICATE_HASH certificateHash;
    if (!ParseThumbprint(
            configuration->certificateStoreThumbprint,
            &certificateHash))
    {
        return false;
    }
    credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
    credentials.CertificateHash = &certificateHash;
#else
    if (!ValidateServerPrivateKey(configuration->privateKeyFile))
    {
        return false;
    }
    QUIC_CERTIFICATE_FILE certificateFile = {
        configuration->privateKeyFile,
        configuration->certificateFile,
    };
    credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    credentials.CertificateFile = &certificateFile;
#endif
    credentials.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    return !QUIC_FAILED(server->api->ConfigurationLoadCredential(
        server->configuration, &credentials));
}

static bool BuildListenAddress(
    const NetworkServerConfiguration *configuration,
    QUIC_ADDR *outAddress)
{
    NetworkEndpoint endpoint;
    if (NetworkListenEndpointParse(
            configuration->listenAddress,
            configuration->addressFamily, configuration->port,
            &endpoint) != NETWORK_ENDPOINT_PARSE_OK)
    {
        return false;
    }
    memset(outAddress, 0, sizeof(*outAddress));
    if (endpoint.kind == NETWORK_ENDPOINT_WILDCARD)
    {
        NetworkAddressFamily family =
            configuration->addressFamily ==
                    NETWORK_ADDRESS_FAMILY_AUTO
                ? NETWORK_ADDRESS_FAMILY_DUAL
                : configuration->addressFamily;
        QuicAddrSetFamily(outAddress, QuicFamily(family));
        QuicAddrSetPort(outAddress, endpoint.port);
        return true;
    }
    return QuicAddrFromString(
               endpoint.host, endpoint.port, outAddress) != FALSE;
}

NetworkServer *NetworkServerCreate(
    const NetworkServerConfiguration *configuration)
{
    if (!NetworkServerConfigurationIsValid(configuration))
    {
        return NULL;
    }

    NetworkServer *server =
        PlatformAllocate(sizeof(*server), true);
    if (server == NULL ||
        !PlatformRwLockInitialize(&server->lock))
    {
        PlatformFree(server);
        return NULL;
    }
    server->maximumPeers = configuration->maximumPeers;
    server->worldSeed = configuration->worldSeed;
    server->modCount = configuration->modCount;
    server->allowContentDownloads =
        configuration->allowContentDownloads;
    server->contentBundle = configuration->contentBundle;
    server->contentBundleSize =
        configuration->contentBundleSize;
    server->nextBlockRevision = 1U;
    server->handshakeTimeoutMs =
        configuration->handshakeTimeoutMs != 0
            ? configuration->handshakeTimeoutMs
            : (uint32_t)NETWORK_HANDSHAKE_TIMEOUT_MS;
    server->idleTimeoutMs =
        configuration->idleTimeoutMs != 0
            ? configuration->idleTimeoutMs
            : (uint32_t)NETWORK_IDLE_TIMEOUT_MS;
    if (server->modCount != 0)
    {
        memcpy(
            server->mods, configuration->mods,
            sizeof(server->mods[0]) * server->modCount);
    }
    if (server->allowContentDownloads)
    {
        uint8_t actual[LAIUE_NETWORK_CONTENT_HASH_SIZE];
        if (!PlatformSha256(
                server->contentBundle, server->contentBundleSize,
                actual) ||
            !PlatformConstantTimeEqual(
                actual, configuration->contentBundleSha256,
                sizeof(actual)))
        {
            NetworkServerDestroy(server);
            return NULL;
        }
        memcpy(
            server->contentBundleHash, actual, sizeof(actual));
    }
    server->peers = PlatformAllocate(
        sizeof(*server->peers) * server->maximumPeers, true);
    if (server->peers == NULL ||
        !OpenApiAndRegistration(
            &server->api, &server->registration) ||
        !OpenConfiguration(
            server->api, server->registration, true,
            configuration->handshakeTimeoutMs,
            configuration->idleTimeoutMs,
            &server->configuration) ||
        !LoadServerCredentials(server, configuration) ||
        QUIC_FAILED(server->api->ListenerOpen(
            server->registration, ServerListenerCallback,
            server, &server->listener)))
    {
        NetworkServerDestroy(server);
        return NULL;
    }

    static uint8_t alpnBytes[] = LAIUE_PROTOCOL_ALPN;
    QUIC_BUFFER alpn = {
        (uint32_t)(sizeof(alpnBytes) - 1U),
        alpnBytes,
    };
    QUIC_ADDR address;
    if (!BuildListenAddress(configuration, &address) ||
        QUIC_FAILED(server->api->ListenerStart(
            server->listener, &alpn, 1U, &address)))
    {
        NetworkServerDestroy(server);
        return NULL;
    }
    return server;
}

NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration)
{
    if (configuration == NULL)
    {
        return NULL;
    }
    NetworkServerConfiguration loopback = *configuration;
    loopback.addressFamily = NETWORK_ADDRESS_FAMILY_DUAL;
    loopback.listenAddress = "*";
    return NetworkServerCreate(&loopback);
}

typedef struct ServerShutdownHandles
{
    HQUIC connection;
    HQUIC controlStream;
    HQUIC snapshotStream;
    HQUIC contentStream;
} ServerShutdownHandles;

static bool ServerShutdownIsDrained(NetworkServer *server)
{
    bool drained = true;
    PlatformRwLockAcquireShared(&server->lock);
    for (uint32_t index = 0;
         index < server->maximumPeers && drained; ++index)
    {
        const NetworkServerPeer *peer = &server->peers[index];
        if (!peer->allocated)
        {
            continue;
        }
        drained =
            peer->listenerReady && peer->shutdownComplete &&
            (peer->channel.stream == NULL ||
             peer->channel.shutdownComplete) &&
            (peer->snapshotChannel.stream == NULL ||
             peer->snapshotChannel.shutdownComplete) &&
            (peer->contentChannel.stream == NULL ||
             peer->contentChannel.shutdownComplete);
    }
    PlatformRwLockReleaseShared(&server->lock);
    return drained;
}

static void ServerTakeShutdownHandles(
    NetworkServer *server,
    ServerShutdownHandles handles[LAIUE_NETWORK_MAX_PEERS])
{
    PlatformRwLockAcquireExclusive(&server->lock);
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (!peer->allocated)
        {
            continue;
        }
        handles[index].connection = peer->connection;
        handles[index].controlStream = peer->channel.stream;
        handles[index].snapshotStream =
            peer->snapshotChannel.stream;
        handles[index].contentStream =
            peer->contentChannel.stream;
        peer->connection = NULL;
        peer->channel.stream = NULL;
        peer->snapshotChannel.stream = NULL;
        peer->contentChannel.stream = NULL;
    }
    PlatformRwLockReleaseExclusive(&server->lock);
}

void NetworkServerDestroy(NetworkServer *server)
{
    if (server == NULL)
    {
        return;
    }
    PlatformRwLockAcquireExclusive(&server->lock);
    server->stopping = true;
    PlatformRwLockReleaseExclusive(&server->lock);
    if (server->listener != NULL)
    {
        server->api->ListenerStop(server->listener);
        server->api->ListenerClose(server->listener);
        server->listener = NULL;
    }
    if (server->registration != NULL)
    {
        server->api->RegistrationShutdown(
            server->registration,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT,
            NETWORK_APP_ERROR_SHUTDOWN);
    }
    if (server->peers != NULL && server->api != NULL)
    {
        uint64_t deadline = PlatformMonotonicMilliseconds() +
                            NETWORK_SHUTDOWN_DRAIN_TIMEOUT_MS;
        while (!ServerShutdownIsDrained(server) &&
               PlatformMonotonicMilliseconds() < deadline)
        {
            PlatformSleepMilliseconds(1U);
        }
        ServerShutdownHandles
            handles[LAIUE_NETWORK_MAX_PEERS] = {0};
        bool shutdownDrained = ServerShutdownIsDrained(server);
        if (shutdownDrained)
        {
            ServerTakeShutdownHandles(server, handles);
        }
        for (uint32_t index = 0;
             index < server->maximumPeers; ++index)
        {
            if (handles[index].controlStream != NULL)
            {
                server->api->StreamClose(
                    handles[index].controlStream);
            }
            if (handles[index].snapshotStream != NULL)
            {
                server->api->StreamClose(
                    handles[index].snapshotStream);
            }
            if (handles[index].contentStream != NULL)
            {
                server->api->StreamClose(
                    handles[index].contentStream);
            }
            if (handles[index].connection != NULL)
            {
                server->api->ConnectionClose(
                    handles[index].connection);
            }
        }
        if (!shutdownDrained)
        {
            // This is a fail-safe for a broken transport callback contract.
            // RegistrationClose below remains the last-resort owner; never
            // invoke StreamClose before SHUTDOWN_COMPLETE.
            PlatformWriteConsoleUtf8(
                "MsQuic shutdown callbacks did not drain in time\n");
        }
    }
    if (server->configuration != NULL)
    {
        server->api->ConfigurationClose(server->configuration);
    }
    if (server->registration != NULL)
    {
        server->api->RegistrationClose(server->registration);
    }
    if (server->api != NULL)
    {
        MsQuicClose(server->api);
    }
    PlatformFree(server->peers);
    PlatformRwLockDestroy(&server->lock);
    PlatformFree(server);
}

void NetworkServerUpdate(NetworkServer *server)
{
    if (server == NULL || server->stopping)
    {
        return;
    }
    uint64_t now = PlatformMonotonicMilliseconds();
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        ServerPeerCallbackState callbackState =
            ServerTakePeerCallbackState(server, peer);
        if (!callbackState.allocated)
        {
            continue;
        }

        NetworkDisconnectReason reason = callbackState.reason;
        NetworkDisconnectReason channelReason =
            ChannelTakeError(&peer->channel);
        if (reason == NETWORK_DISCONNECT_NONE)
        {
            reason = channelReason;
        }
        if (reason == NETWORK_DISCONNECT_NONE &&
            ChannelIsActive(&peer->snapshotChannel))
        {
            reason = ChannelTakeError(
                &peer->snapshotChannel);
        }
        if (reason == NETWORK_DISCONNECT_NONE &&
            ChannelIsActive(&peer->contentChannel))
        {
            reason = ChannelTakeError(&peer->contentChannel);
        }
        bool retired = false;
        if (reason == NETWORK_DISCONNECT_NONE &&
            !ChannelTryRetireAuxiliary(
                &peer->snapshotChannel, &retired))
        {
            reason = NETWORK_DISCONNECT_PROTOCOL;
        }
        if (reason == NETWORK_DISCONNECT_NONE &&
            !ChannelTryRetireAuxiliary(
                &peer->contentChannel, &retired))
        {
            reason = NETWORK_DISCONNECT_PROTOCOL;
        }
        if (callbackState.peerSendClosed &&
            reason == NETWORK_DISCONNECT_NONE)
        {
            reason = NETWORK_DISCONNECT_REMOTE;
        }
        if (reason != NETWORK_DISCONNECT_NONE)
        {
            ServerDisconnectPeer(server, peer, reason);
        }
        if (!peer->closing &&
            (!ServerParseFrames(server, peer) ||
             !ServerPumpContentTransfer(server, peer)))
        {
            ServerDisconnectPeer(
                server, peer, NETWORK_DISCONNECT_PROTOCOL);
        }
        if (!peer->closing)
        {
            uint64_t deadline;
            if (!callbackState.secureConnected)
            {
                deadline = peer->channel.connectedAtMs +
                           server->handshakeTimeoutMs;
            }
            else
            {
                if (peer->ready)
                {
                    deadline =
                        ChannelLastReceiveAt(&peer->channel) +
                        server->idleTimeoutMs;
                }
                else
                {
                    uint64_t lastActivity =
                        ChannelLastActivityAt(&peer->channel);
                    if (ChannelIsActive(&peer->snapshotChannel))
                    {
                        uint64_t snapshotActivity =
                            ChannelLastActivityAt(
                                &peer->snapshotChannel);
                        if (snapshotActivity > lastActivity)
                            lastActivity = snapshotActivity;
                    }
                    if (ChannelIsActive(&peer->contentChannel))
                    {
                        uint64_t contentActivity =
                            ChannelLastActivityAt(
                                &peer->contentChannel);
                        if (contentActivity > lastActivity)
                            lastActivity = contentActivity;
                    }
                    deadline = lastActivity +
                        NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS;
                }
            }
            if (now > deadline)
            {
                ServerDisconnectPeer(
                    server, peer, NETWORK_DISCONNECT_TIMEOUT);
            }
            else if (peer->rejected &&
                     ChannelOutstandingSends(&peer->channel) == 0)
            {
                ServerDisconnectPeer(
                    server, peer, NETWORK_DISCONNECT_REMOTE);
            }
        }
        if (peer->closing && callbackState.shutdownComplete)
        {
            ServerFinalizePeer(server, peer);
        }
    }
}

bool NetworkServerPollEvent(
    NetworkServer *server, NetworkServerEvent *outEvent)
{
    if (server == NULL || outEvent == NULL ||
        server->eventCount == 0)
    {
        return false;
    }
    *outEvent = server->events[server->eventRead];
    server->eventRead =
        (server->eventRead + 1U) % NETWORK_SERVER_EVENT_CAPACITY;
    --server->eventCount;
    return true;
}

bool NetworkServerDisconnect(
    NetworkServer *server, uint32_t peerId,
    NetworkDisconnectReason reason)
{
    return ServerDisconnectPeer(
        server, ServerFindPeer(server, peerId), reason);
}

static bool ServerPeerCanSync(
    NetworkServer *server, const NetworkServerPeer *peer)
{
    if (server == NULL || peer == NULL)
    {
        return false;
    }
    PlatformRwLockAcquireShared(&server->lock);
    bool canSync =
        peer->allocated && peer->listenerReady &&
        peer->negotiated && !peer->closing &&
        peer->channel.stream != NULL;
    PlatformRwLockReleaseShared(&server->lock);
    return canSync;
}

static bool ServerPeerCanBeginSnapshot(
    NetworkServer *server, NetworkServerPeer *peer,
    bool requiresReadyBarrier)
{
    if (!ServerPeerCanSync(server, peer) ||
        peer->snapshotStarted ||
        ChannelIsActive(&peer->snapshotChannel))
    {
        return false;
    }
    if (requiresReadyBarrier)
    {
        return !peer->everReady && !peer->ready &&
            !peer->awaitingSyncApplied;
    }
    return peer->everReady && peer->ready &&
        !peer->awaitingSyncApplied;
}

static bool ServerSendToPeer(
    NetworkServer *server, NetworkServerPeer *peer,
    LaiueMessageType type,
    const uint8_t *payload, uint32_t size, bool requireReady)
{
    if (!ServerPeerCanSync(server, peer) ||
        (requireReady && !peer->ready))
    {
        return false;
    }
    if (!ChannelSendPayload(&peer->channel, type, payload, size))
    {
        ServerDisconnectPeer(
            server, peer, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return true;
}

static bool EncodePlayerState(
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY],
    const NetworkPlayerState *state, uint32_t *outSize)
{
    if (state == NULL)
    {
        return false;
    }
    LaiueProtocolPlayerState wire;
    memset(&wire, 0, sizeof(wire));
    wire.serverTick = state->serverTick;
    wire.peerId = state->peerId;
    wire.lastProcessedInputSequence =
        state->lastProcessedInputSequence;
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        wire.position[axis] = state->position[axis];
    }
    wire.yaw = state->yaw;
    wire.pitch = state->pitch;
    wire.locomotionVelocityX = state->locomotionVelocityX;
    wire.locomotionVelocityY = state->locomotionVelocityY;
    wire.verticalVelocity = state->verticalVelocity;
    wire.externalVelocityX = state->externalVelocityX;
    wire.externalVelocityY = state->externalVelocityY;
    wire.jumpBufferRemaining = state->jumpBufferRemaining;
    wire.coyoteTimeRemaining = state->coyoteTimeRemaining;
    wire.colliderCrouchProgress =
        state->colliderCrouchProgress;
    wire.eyeCrouchProgress = state->eyeCrouchProgress;
    wire.airJumpsRemaining = state->airJumpsRemaining;
    wire.crouchingRequested = state->crouchingRequested;
    wire.grounded = state->grounded;
    *outSize = LaiueProtocolEncodePlayerState(
        payload, NETWORK_CONTROL_PAYLOAD_CAPACITY, &wire);
    return *outSize != 0;
}

bool NetworkServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    if (!EncodePlayerState(payload, state, &size) ||
        !ServerPeerCanSync(server, peer))
    {
        return false;
    }
    return ServerSendToPeer(
        server, peer,
        peer->ready ? LAIUE_MESSAGE_PLAYER_STATE
                    : LAIUE_MESSAGE_PLAYER_ROSTER_ENTRY,
        payload, size, false);
}

bool NetworkServerBroadcastPlayerState(
    NetworkServer *server, const NetworkPlayerState *state)
{
    if (server == NULL)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    if (!EncodePlayerState(payload, state, &size))
    {
        return false;
    }
    bool succeeded = true;
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (ServerPeerCanSync(server, peer) && peer->ready)
        {
            succeeded =
                ServerSendToPeer(
                    server, peer, LAIUE_MESSAGE_PLAYER_STATE,
                    payload, size, false) &&
                succeeded;
        }
    }
    return succeeded;
}

static bool EncodeBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta,
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY],
    uint32_t *outSize)
{
    if (server == NULL || delta == NULL)
    {
        return false;
    }
    LaiueProtocolBlockDelta wire;
    memset(&wire, 0, sizeof(wire));
    wire.serverTick = delta->serverTick;
    wire.revision =
        delta->revision != 0
            ? delta->revision
            : server->nextBlockRevision;
    if (wire.revision >= server->nextBlockRevision)
    {
        server->nextBlockRevision = wire.revision + 1U;
        if (server->nextBlockRevision == 0)
        {
            return false;
        }
    }
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        wire.block[axis] = delta->block[axis];
    }
    wire.replacement = delta->replacement;
    *outSize = LaiueProtocolEncodeBlockDelta(
        payload, NETWORK_CONTROL_PAYLOAD_CAPACITY, &wire);
    return *outSize != 0;
}

bool NetworkServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    return EncodeBlockDelta(server, delta, payload, &size) &&
           ServerSendToPeer(
               server, ServerFindPeer(server, peerId),
               LAIUE_MESSAGE_BLOCK_DELTA,
               payload, size, false);
}

bool NetworkServerBroadcastBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    if (!EncodeBlockDelta(server, delta, payload, &size))
    {
        return false;
    }
    bool succeeded = true;
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (ServerPeerCanSync(server, peer))
        {
            succeeded =
                ServerSendToPeer(
                    server, peer, LAIUE_MESSAGE_BLOCK_DELTA,
                    payload, size, false) &&
                succeeded;
        }
    }
    return succeeded;
}

bool NetworkServerCanBeginSnapshot(
    NetworkServer *server, uint32_t peerId,
    bool requiresReadyBarrier)
{
    return ServerPeerCanBeginSnapshot(
        server, ServerFindPeer(server, peerId),
        requiresReadyBarrier);
}

bool NetworkServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    if (snapshot == NULL ||
        !ServerPeerCanBeginSnapshot(
            server, peer, snapshot->requiresReadyBarrier) ||
        snapshot->peerId != peerId ||
        snapshot->worldSeed != server->worldSeed)
    {
        return false;
    }
    LaiueProtocolSnapshotBegin wire = {
        .snapshotId = snapshot->snapshotId,
        .worldRevision = snapshot->worldRevision,
        .serverTick = snapshot->serverTick,
        .chunkCount = snapshot->chunkCount,
        .peerId = snapshot->peerId,
        .worldSeed = snapshot->worldSeed,
        .worldTime = snapshot->worldTime,
        .requiresReadyBarrier =
            snapshot->requiresReadyBarrier,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeSnapshotBegin(
        payload, sizeof(payload), &wire);
    if (size == 0 ||
        !ServerOpenSendStream(peer, &peer->snapshotChannel))
    {
        return false;
    }
    if (snapshot->requiresReadyBarrier &&
        !ChannelSendPayload(
            &peer->channel, LAIUE_MESSAGE_SYNC_BEGIN, NULL, 0))
    {
        ServerDisconnectPeer(
            server, peer, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    if (snapshot->requiresReadyBarrier)
    {
        peer->ready = false;
        peer->awaitingSyncApplied = false;
    }
    if (!ChannelSendPayload(
            &peer->snapshotChannel,
            LAIUE_MESSAGE_SNAPSHOT_BEGIN, payload, size))
    {
        ServerDisconnectPeer(
            server, peer, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    peer->snapshotId = snapshot->snapshotId;
    peer->expectedSnapshotChunks = snapshot->chunkCount;
    peer->sentSnapshotChunks = 0;
    peer->snapshotNextPart = 0;
    peer->snapshotPartCount = 0;
    peer->snapshotStarted = true;
    peer->snapshotRequiresReadyBarrier =
        snapshot->requiresReadyBarrier;
    return true;
}

bool NetworkServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    if (!ServerPeerCanSync(server, peer) ||
        !peer->snapshotStarted ||
        (peer->snapshotRequiresReadyBarrier
             ? peer->ready
             : !peer->ready) ||
        chunk == NULL ||
        peer->sentSnapshotChunks >=
            peer->expectedSnapshotChunks)
    {
        return false;
    }

    LaiueProtocolChunkDelta wire;
    memset(&wire, 0, sizeof(wire));
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        wire.chunk[axis] = chunk->chunk[axis];
    }
    wire.revision = chunk->revision;
    wire.partIndex = chunk->partIndex;
    wire.partCount = chunk->partCount;
    wire.editCount = chunk->editCount;
    for (uint32_t index = 0;
         index < chunk->editCount &&
         index < LAIUE_NETWORK_MAX_CHUNK_EDITS; ++index)
    {
        wire.edits[index].localX = chunk->edits[index].localX;
        wire.edits[index].localY = chunk->edits[index].localY;
        wire.edits[index].localZ = chunk->edits[index].localZ;
        wire.edits[index].replacement =
            chunk->edits[index].replacement;
    }
    bool resyncChunk =
        peer->resyncRequestOutstanding &&
        ChunkCoordinatesEqual(
            peer->resyncRequestChunk, wire.chunk);
    if (resyncChunk &&
        wire.revision < peer->resyncExpectedRevision)
    {
        return false;
    }

    if (wire.partIndex == 0)
    {
        if (peer->snapshotNextPart != 0)
        {
            return false;
        }
    }
    else if (wire.partIndex != peer->snapshotNextPart ||
             wire.partCount != peer->snapshotPartCount ||
             wire.revision != peer->snapshotPartRevision ||
             wire.chunk[0] != peer->snapshotPartChunk[0] ||
             wire.chunk[1] != peer->snapshotPartChunk[1] ||
             wire.chunk[2] != peer->snapshotPartChunk[2] ||
             wire.editCount == 0 ||
             ChunkEditOrdinal(&wire.edits[0]) <=
                 peer->snapshotLastEditIndex)
    {
        return false;
    }

    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeSnapshotChunk(
        payload, sizeof(payload), &wire);
    if (size == 0 ||
        !ChannelSendPayload(
            &peer->snapshotChannel,
            LAIUE_MESSAGE_SNAPSHOT_CHUNK, payload, size))
    {
        if (size != 0)
        {
            ServerDisconnectPeer(
                server, peer, NETWORK_DISCONNECT_OVERFLOW);
        }
        return false;
    }
    if (wire.partIndex == 0)
    {
        peer->snapshotPartChunk[0] = wire.chunk[0];
        peer->snapshotPartChunk[1] = wire.chunk[1];
        peer->snapshotPartChunk[2] = wire.chunk[2];
        peer->snapshotPartRevision = wire.revision;
        peer->snapshotPartCount = wire.partCount;
    }
    if (wire.editCount != 0)
    {
        peer->snapshotLastEditIndex =
            ChunkEditOrdinal(&wire.edits[wire.editCount - 1U]);
    }
    peer->snapshotNextPart =
        (uint16_t)(wire.partIndex + 1U);
    if (peer->snapshotNextPart == wire.partCount)
    {
        if (resyncChunk)
        {
            if (peer->resyncResponseMatched)
            {
                return false;
            }
            peer->resyncResponseMatched = true;
        }
        ++peer->sentSnapshotChunks;
        peer->snapshotNextPart = 0;
        peer->snapshotPartCount = 0;
        peer->snapshotLastEditIndex = 0;
    }
    return true;
}

bool NetworkServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    if (!ServerPeerCanSync(server, peer) ||
        !peer->snapshotStarted ||
        (peer->snapshotRequiresReadyBarrier
             ? peer->ready
             : !peer->ready) ||
        peer->snapshotId != snapshotId ||
        peer->snapshotNextPart != 0 ||
        (peer->resyncRequestOutstanding &&
         !peer->resyncResponseMatched) ||
        peer->sentSnapshotChunks !=
            peer->expectedSnapshotChunks)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeSnapshotEnd(
        payload, sizeof(payload), snapshotId, worldRevision);
    if (size == 0 ||
        !ChannelSendPayload(
            &peer->snapshotChannel,
            LAIUE_MESSAGE_SNAPSHOT_END, payload, size))
    {
        if (size != 0)
        {
            ServerDisconnectPeer(
                server, peer, NETWORK_DISCONNECT_OVERFLOW);
        }
        return false;
    }
    if (peer->snapshotRequiresReadyBarrier &&
        !ChannelSendPayload(
            &peer->channel, LAIUE_MESSAGE_SYNC_READY, NULL, 0))
    {
        ServerDisconnectPeer(
            server, peer, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    if (!ChannelShutdownGracefully(&peer->snapshotChannel))
    {
        ServerDisconnectPeer(
            server, peer, NETWORK_DISCONNECT_IO);
        return false;
    }
    bool requiresReadyBarrier =
        peer->snapshotRequiresReadyBarrier;
    peer->snapshotStarted = false;
    peer->snapshotRequiresReadyBarrier = false;
    peer->snapshotWorldRevision = worldRevision;
    peer->awaitingSyncApplied = requiresReadyBarrier;
    if (peer->resyncRequestOutstanding)
    {
        peer->resyncRequestOutstanding = false;
        peer->resyncResponseMatched = false;
    }
    return true;
}

bool NetworkServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    if (!ServerPeerCanSync(server, peer) || chunk == NULL ||
        !peer->resyncRequestOutstanding ||
        !ChunkCoordinatesEqual(
            peer->resyncRequestChunk, chunk) ||
        peer->resyncExpectedRevision != expectedRevision)
    {
        return false;
    }
    LaiueProtocolChunkResyncRequest cancelled = {
        { chunk[0], chunk[1], chunk[2] },
        expectedRevision,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size =
        LaiueProtocolEncodeChunkResyncCancelled(
            payload, sizeof(payload), &cancelled);
    if (size == 0 ||
        !ServerSendToPeer(
            server, peer,
            LAIUE_MESSAGE_CHUNK_RESYNC_CANCELLED,
            payload, size, false))
    {
        return false;
    }
    peer->resyncRequestOutstanding = false;
    peer->resyncResponseMatched = false;
    return true;
}

static bool ServerSendPeerId(
    NetworkServer *server, uint32_t peerId,
    uint32_t value, LaiueMessageType type)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodePeerId(
        payload, sizeof(payload), value);
    return size != 0 &&
           ServerSendToPeer(
               server, peer, type, payload, size, false);
}

static bool ServerBroadcastPeerId(
    NetworkServer *server, uint32_t value,
    LaiueMessageType type)
{
    if (server == NULL)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodePeerId(
        payload, sizeof(payload), value);
    if (size == 0)
    {
        return false;
    }
    bool succeeded = true;
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (ServerPeerCanSync(server, peer))
        {
            succeeded =
                ServerSendToPeer(
                    server, peer, type, payload, size, false) &&
                succeeded;
        }
    }
    return succeeded;
}

bool NetworkServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId,
    uint32_t joinedPeerId)
{
    return ServerSendPeerId(
        server, peerId, joinedPeerId,
        LAIUE_MESSAGE_PLAYER_JOINED);
}

bool NetworkServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId)
{
    return ServerBroadcastPeerId(
        server, joinedPeerId, LAIUE_MESSAGE_PLAYER_JOINED);
}

bool NetworkServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId,
    uint32_t leftPeerId)
{
    return ServerSendPeerId(
        server, peerId, leftPeerId,
        LAIUE_MESSAGE_PLAYER_LEFT);
}

bool NetworkServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId)
{
    return ServerBroadcastPeerId(
        server, leftPeerId, LAIUE_MESSAGE_PLAYER_LEFT);
}

bool NetworkServerSendWorldTime(
    NetworkServer *server, uint32_t peerId, uint64_t worldTime)
{
    NetworkServerPeer *peer = ServerFindPeer(server, peerId);
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeWorldTime(
        payload, sizeof(payload), worldTime);
    return size != 0 &&
           ServerSendToPeer(
               server, peer, LAIUE_MESSAGE_WORLD_TIME,
               payload, size, false);
}

static bool EncodeBlockDrop(
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY],
    const NetworkBlockDrop *drop, uint32_t *outSize)
{
    if (drop == NULL)
    {
        return false;
    }
    LaiueProtocolBlockDrop wire;
    memset(&wire, 0, sizeof(wire));
    wire.id = drop->id;
    wire.block = drop->block;
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        wire.position[axis] = drop->position[axis];
    }
    *outSize = LaiueProtocolEncodeBlockDrop(
        payload, NETWORK_CONTROL_PAYLOAD_CAPACITY, &wire);
    return *outSize != 0;
}

bool NetworkServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    return EncodeBlockDrop(payload, drop, &size) &&
           ServerSendToPeer(
               server, ServerFindPeer(server, peerId),
               LAIUE_MESSAGE_BLOCK_DROP_SPAWN,
               payload, size, false);
}

bool NetworkServerBroadcastBlockDrop(
    NetworkServer *server, const NetworkBlockDrop *drop)
{
    if (server == NULL)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = 0;
    if (!EncodeBlockDrop(payload, drop, &size))
    {
        return false;
    }
    bool succeeded = true;
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (ServerPeerCanSync(server, peer))
        {
            succeeded =
                ServerSendToPeer(
                    server, peer,
                    LAIUE_MESSAGE_BLOCK_DROP_SPAWN,
                    payload, size, false) &&
                succeeded;
        }
    }
    return succeeded;
}

bool NetworkServerBroadcastDropRemove(
    NetworkServer *server, uint32_t dropId)
{
    if (server == NULL)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeDropRemove(
        payload, sizeof(payload), dropId);
    if (size == 0)
    {
        return false;
    }
    bool succeeded = true;
    for (uint32_t index = 0;
         index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (ServerPeerCanSync(server, peer))
        {
            succeeded =
                ServerSendToPeer(
                    server, peer,
                    LAIUE_MESSAGE_BLOCK_DROP_REMOVE,
                    payload, size, false) &&
                succeeded;
        }
    }
    return succeeded;
}

bool NetworkServerSendInventory(
    NetworkServer *server, uint32_t peerId,
    const NetworkInventoryState *inventory)
{
    if (inventory == NULL)
    {
        return false;
    }
    LaiueProtocolInventory wire;
    memset(&wire, 0, sizeof(wire));
    wire.selectedHotbarSlot = inventory->selectedHotbarSlot;
    for (uint32_t index = 0;
         index < LAIUE_NETWORK_INVENTORY_SLOTS; ++index)
    {
        wire.slots[index].item = inventory->slots[index].item;
        wire.slots[index].count = inventory->slots[index].count;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeInventory(
        payload, sizeof(payload), &wire);
    return size != 0 &&
           ServerSendToPeer(
               server, ServerFindPeer(server, peerId),
               LAIUE_MESSAGE_INVENTORY_STATE,
               payload, size, false);
}
