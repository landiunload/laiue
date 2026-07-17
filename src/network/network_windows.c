#include "network/network.h"
#include "network/protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>
#include <string.h>

#define NETWORK_RECEIVE_CAPACITY 4096U
#define NETWORK_SEND_CAPACITY 16384U
#define NETWORK_CLIENT_EVENT_CAPACITY 128U
#define NETWORK_SERVER_EVENT_CAPACITY 256U
#define NETWORK_HANDSHAKE_TIMEOUT_MS 60000ULL
#define NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS 120000ULL
#define NETWORK_IDLE_TIMEOUT_MS 15000ULL
#define NETWORK_RATE_WINDOW_MS 1000ULL
#define NETWORK_MAX_FRAMES_PER_SECOND 160U
#define NETWORK_MAX_INPUTS_PER_SECOND 120U
#define NETWORK_MAX_EDITS_PER_SECOND 16U
#define NETWORK_KEEPALIVE_INTERVAL_MS 1000ULL
#define NETWORK_CONTROL_PAYLOAD_CAPACITY 256U

typedef struct SocketChannel
{
    SOCKET socket;
    uint8_t receive[NETWORK_RECEIVE_CAPACITY];
    uint32_t receiveSize;
    uint8_t send[NETWORK_SEND_CAPACITY];
    uint32_t sendRead;
    uint32_t sendWrite;
    uint32_t sendCount;
    uint32_t receiveSequence;
    uint32_t sendSequence;
    uint64_t connectedAtMs;
    uint64_t lastReceiveAtMs;
    uint64_t lastQueueAtMs;
} SocketChannel;

typedef struct NetworkServerPeer
{
    SocketChannel channel;
    uint32_t peerId;
    uint64_t rateWindowStartMs;
    uint32_t framesInWindow;
    uint32_t inputsInWindow;
    uint32_t editsInWindow;
    NetworkModDescriptor mods[LAIUE_NETWORK_MAX_MODS];
    uint64_t clientNonce;
    uint32_t expectedModCount;
    uint32_t receivedModCount;
    uint64_t contentOffset;
    bool allocated;
    bool ready;
    bool helloReceived;
    bool modListReceived;
    bool contentTransferActive;
    bool rejected;
} NetworkServerPeer;

struct NetworkClient
{
    SocketChannel channel;
    NetworkClientEvent events[NETWORK_CLIENT_EVENT_CAPACITY];
    uint32_t eventRead;
    uint32_t eventWrite;
    uint32_t eventCount;
    NetworkConnectionState state;
    uint64_t clientNonce;
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
    bool transportConnected;
    bool disconnectNotified;
    bool winsockAcquired;
};

struct NetworkServer
{
    SOCKET listener;
    NetworkServerPeer peers[LAIUE_NETWORK_MAX_PEERS];
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
    bool allowContentDownloads;
    bool winsockAcquired;
};

_Static_assert((NETWORK_SEND_CAPACITY & (NETWORK_SEND_CAPACITY - 1U)) == 0,
               "Send ring capacity must remain a power of two");
_Static_assert(NETWORK_SEND_CAPACITY >= LAIUE_PROTOCOL_MAX_FRAME_SIZE,
               "Send ring must fit the largest protocol frame");
_Static_assert(sizeof(struct NetworkServer) <= 384U * 1024U,
               "Network server memory budget exceeded");
_Static_assert(sizeof(NetworkModDescriptor) == sizeof(LaiueProtocolMod),
               "Public and wire mod descriptors diverged");

static SRWLOCK g_winsockLock = SRWLOCK_INIT;
static uint32_t g_winsockReferences;

static bool WinsockAcquire(void)
{
    bool acquired = true;
    AcquireSRWLockExclusive(&g_winsockLock);
    if (g_winsockReferences == 0)
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            acquired = false;
        }
    }
    if (acquired)
    {
        ++g_winsockReferences;
    }
    ReleaseSRWLockExclusive(&g_winsockLock);
    return acquired;
}

static void WinsockRelease(void)
{
    AcquireSRWLockExclusive(&g_winsockLock);
    if (g_winsockReferences != 0 && --g_winsockReferences == 0)
    {
        WSACleanup();
    }
    ReleaseSRWLockExclusive(&g_winsockLock);
}

static bool SocketSetNonblocking(SOCKET socketHandle)
{
    u_long enabled = 1;
    return ioctlsocket(socketHandle, FIONBIO, &enabled) == 0;
}

static bool SocketSetLowLatency(SOCKET socketHandle)
{
    BOOL enabled = TRUE;
    return setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled,
                      sizeof(enabled)) == 0;
}

static void ChannelInitialize(SocketChannel *channel, SOCKET socketHandle)
{
    memset(channel, 0, sizeof(*channel));
    channel->socket = socketHandle;
    channel->connectedAtMs = GetTickCount64();
    channel->lastReceiveAtMs = channel->connectedAtMs;
    channel->lastQueueAtMs = channel->connectedAtMs;
}

static bool ChannelQueueBytes(SocketChannel *channel, const uint8_t *bytes, uint32_t size)
{
    if (size > NETWORK_SEND_CAPACITY - channel->sendCount)
    {
        return false;
    }

    uint32_t first = NETWORK_SEND_CAPACITY - channel->sendWrite;
    if (first > size)
    {
        first = size;
    }
    memcpy(channel->send + channel->sendWrite, bytes, first);
    if (size > first)
    {
        memcpy(channel->send, bytes + first, size - first);
    }
    channel->sendWrite = (channel->sendWrite + size) % NETWORK_SEND_CAPACITY;
    channel->sendCount += size;
    return true;
}

static bool ChannelQueuePayload(SocketChannel *channel, LaiueMessageType type,
                                const uint8_t *payload, uint32_t payloadSize)
{
    if (payloadSize > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE
        || (payloadSize != 0 && payload == NULL)
        || LAIUE_PROTOCOL_HEADER_SIZE + payloadSize
            > NETWORK_SEND_CAPACITY - channel->sendCount)
    {
        return false;
    }
    uint8_t header[LAIUE_PROTOCOL_HEADER_SIZE];
    uint32_t nextSequence = channel->sendSequence + 1U;
    if (nextSequence == 0)
    {
        return false;
    }
    if (LaiueProtocolWriteHeader(header, sizeof(header), type,
            nextSequence, payloadSize) == 0
        || !ChannelQueueBytes(channel, header, sizeof(header))
        || (payloadSize != 0
            && !ChannelQueueBytes(channel, payload, payloadSize)))
    {
        return false;
    }
    channel->sendSequence = nextSequence;
    channel->lastQueueAtMs = GetTickCount64();
    return true;
}

static bool ChannelFlush(SocketChannel *channel)
{
    while (channel->sendCount != 0)
    {
        uint32_t contiguous = NETWORK_SEND_CAPACITY - channel->sendRead;
        if (contiguous > channel->sendCount)
        {
            contiguous = channel->sendCount;
        }
        int sent = send(channel->socket, (const char *)channel->send + channel->sendRead,
                        (int)contiguous, 0);
        if (sent == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            return error == WSAEWOULDBLOCK;
        }
        if (sent == 0)
        {
            return false;
        }
        channel->sendRead = (channel->sendRead + (uint32_t)sent) % NETWORK_SEND_CAPACITY;
        channel->sendCount -= (uint32_t)sent;
    }
    return true;
}

// 1 = bytes received, 0 = would block, -1 = closed/error, -2 = full buffer.
static int32_t ChannelReceive(SocketChannel *channel)
{
    if (channel->receiveSize == NETWORK_RECEIVE_CAPACITY)
    {
        return -2;
    }
    int received = recv(channel->socket, (char *)channel->receive + channel->receiveSize,
                        (int)(NETWORK_RECEIVE_CAPACITY - channel->receiveSize), 0);
    if (received > 0)
    {
        channel->receiveSize += (uint32_t)received;
        channel->lastReceiveAtMs = GetTickCount64();
        return 1;
    }
    if (received == 0)
    {
        return -1;
    }
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}

static void ChannelConsumeReceive(SocketChannel *channel, uint32_t size)
{
    uint32_t remaining = channel->receiveSize - size;
    for (uint32_t index = 0; index < remaining; ++index)
    {
        channel->receive[index] = channel->receive[size + index];
    }
    channel->receiveSize = remaining;
}

static bool ChannelNextFrame(SocketChannel *channel, LaiueProtocolFrame *outFrame,
                             bool *outComplete)
{
    *outComplete = false;
    if (channel->receiveSize < LAIUE_PROTOCOL_HEADER_SIZE)
    {
        return true;
    }
    if (!LaiueProtocolReadHeader(channel->receive, channel->receiveSize, outFrame))
    {
        return false;
    }
    uint32_t frameSize = LAIUE_PROTOCOL_HEADER_SIZE + outFrame->payloadSize;
    if (channel->receiveSize < frameSize)
    {
        return true;
    }
    uint32_t expected = channel->receiveSequence + 1U;
    if (expected == 0 || outFrame->sequence != expected)
    {
        return false;
    }
    channel->receiveSequence = expected;
    *outComplete = true;
    return true;
}

static void ChannelConsumeFrame(SocketChannel *channel, const LaiueProtocolFrame *frame)
{
    ChannelConsumeReceive(channel, LAIUE_PROTOCOL_HEADER_SIZE + frame->payloadSize);
}

static bool ClientPushEvent(NetworkClient *client, const NetworkClientEvent *event)
{
    if (client->eventCount == NETWORK_CLIENT_EVENT_CAPACITY)
    {
        return false;
    }
    client->events[client->eventWrite] = *event;
    client->eventWrite = (client->eventWrite + 1U) % NETWORK_CLIENT_EVENT_CAPACITY;
    client->eventCount++;
    return true;
}

static void ClientDisconnect(NetworkClient *client, NetworkDisconnectReason reason)
{
    if (client->state == NETWORK_CONNECTION_DISCONNECTED)
    {
        return;
    }
    if (client->channel.socket != INVALID_SOCKET)
    {
        closesocket(client->channel.socket);
        client->channel.socket = INVALID_SOCKET;
    }
    client->state = NETWORK_CONNECTION_DISCONNECTED;
    if (!client->disconnectNotified)
    {
        NetworkClientEvent event = {
            .type = NETWORK_CLIENT_EVENT_DISCONNECTED,
            .data.disconnectReason = reason,
        };
        ClientPushEvent(client, &event);
        client->disconnectNotified = true;
    }
}

static bool GenerateNonce(uint64_t *outNonce)
{
    *outNonce = 0;
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)outNonce, (ULONG)sizeof(*outNonce),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 && *outNonce != 0;
}

static bool ComputeSha256(const uint8_t *bytes, uint64_t size,
                          uint8_t output[LAIUE_NETWORK_CONTENT_HASH_SIZE])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool succeeded = BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0
        && BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    uint64_t offset = 0;
    while (succeeded && offset < size)
    {
        ULONG part = size - offset > 0xffffffffULL
            ? 0xffffffffU : (ULONG)(size - offset);
        succeeded = BCryptHashData(hash, (PUCHAR)(bytes + offset), part, 0) >= 0;
        offset += part;
    }
    if (succeeded)
    {
        succeeded = BCryptFinishHash(hash, output,
            LAIUE_NETWORK_CONTENT_HASH_SIZE, 0) >= 0;
    }
    if (hash != NULL) BCryptDestroyHash(hash);
    if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
    return succeeded;
}

static bool HashEquals(const uint8_t *left, const uint8_t *right,
                       uint32_t size)
{
    uint8_t difference = 0;
    for (uint32_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static void CopyModFromProtocol(NetworkModDescriptor *destination,
                                const LaiueProtocolMod *source)
{
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->id, source->id, sizeof(destination->id));
    memcpy(destination->version, source->version, sizeof(destination->version));
    memcpy(destination->contentHash, source->contentHash,
        LAIUE_NETWORK_MOD_HASH_SIZE);
}

static void CopyModToProtocol(LaiueProtocolMod *destination,
                              const NetworkModDescriptor *source)
{
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->id, source->id, sizeof(destination->id));
    memcpy(destination->version, source->version, sizeof(destination->version));
    memcpy(destination->contentHash, source->contentHash,
        LAIUE_NETWORK_MOD_HASH_SIZE);
}

static bool ModDescriptorsEqual(const NetworkModDescriptor *left,
                                const NetworkModDescriptor *right)
{
    if (!HashEquals(left->contentHash, right->contentHash,
            LAIUE_NETWORK_MOD_HASH_SIZE)) return false;
    uint32_t index = 0;
    while (index < LAIUE_NETWORK_MOD_ID_CAPACITY
        && left->id[index] != '\0' && left->id[index] == right->id[index]) ++index;
    if (index == LAIUE_NETWORK_MOD_ID_CAPACITY
        || left->id[index] != right->id[index]) return false;
    index = 0;
    while (index < LAIUE_NETWORK_MOD_VERSION_CAPACITY
        && left->version[index] != '\0'
        && left->version[index] == right->version[index]) ++index;
    return index < LAIUE_NETWORK_MOD_VERSION_CAPACITY
        && left->version[index] == right->version[index];
}

static bool ChannelQueueModList(SocketChannel *channel,
                                LaiueMessageType listType,
                                LaiueMessageType entryType,
                                const NetworkModDescriptor *mods,
                                uint32_t count, uint8_t flags)
{
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeModList(payload, sizeof(payload), count, flags);
    if (size == 0 || !ChannelQueuePayload(channel, listType, payload, size)) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        LaiueProtocolMod wire;
        CopyModToProtocol(&wire, &mods[i]);
        size = LaiueProtocolEncodeMod(payload, sizeof(payload), &wire);
        if (size == 0 || !ChannelQueuePayload(channel, entryType, payload, size)) return false;
    }
    return true;
}

static bool ClientPushServerModsEvent(NetworkClient *client)
{
    NetworkClientEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NETWORK_CLIENT_EVENT_SERVER_MODS;
    event.data.serverMods.count = client->receivedServerModCount;
    event.data.serverMods.downloadsAllowed = client->serverDownloadsAllowed;
    return ClientPushEvent(client, &event);
}

static bool ClientBeginHandshake(NetworkClient *client)
{
    uint64_t nonce;
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    if (!GenerateNonce(&nonce))
    {
        return false;
    }
    client->clientNonce = nonce;
    uint32_t size = LaiueProtocolEncodeHello(payload, sizeof(payload), nonce);
    if (size == 0 ||
        !ChannelQueuePayload(&client->channel, LAIUE_MESSAGE_CLIENT_HELLO, payload, size))
    {
        return false;
    }
    client->transportConnected = true;
    return true;
}

static bool ClientHandleFrame(NetworkClient *client, const LaiueProtocolFrame *frame)
{
    NetworkClientEvent event;
    memset(&event, 0, sizeof(event));
    if (client->state == NETWORK_CONNECTION_CONNECTING)
    {
        uint8_t flags;
        if (frame->type != LAIUE_MESSAGE_SERVER_MOD_LIST
            || !LaiueProtocolDecodeModList(frame->payload, frame->payloadSize,
                &client->expectedServerModCount, &flags))
        {
            return false;
        }
        client->serverDownloadsAllowed = (flags & 1U) != 0;
        client->state = NETWORK_CONNECTION_NEGOTIATING_MODS;
        return client->expectedServerModCount != 0
            || ClientPushServerModsEvent(client);
    }

    if (client->state == NETWORK_CONNECTION_NEGOTIATING_MODS)
    {
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_BEGIN
            && client->contentRequested && client->contentBytes == NULL)
        {
            if (!LaiueProtocolDecodeContentBegin(frame->payload,
                    frame->payloadSize, &client->expectedContentSize,
                    client->expectedContentHash)
                || client->expectedContentSize > LAIUE_NETWORK_MAX_CONTENT_BYTES)
            {
                return false;
            }
            client->contentBytes = HeapAlloc(GetProcessHeap(), 0,
                (size_t)client->expectedContentSize);
            return client->contentBytes != NULL;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_CHUNK
            && client->contentBytes != NULL && frame->payloadSize != 0
            && frame->payloadSize <= client->expectedContentSize
                - client->receivedContentSize)
        {
            memcpy(client->contentBytes + client->receivedContentSize,
                frame->payload, frame->payloadSize);
            client->receivedContentSize += frame->payloadSize;
            return true;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_CONTENT_END
            && frame->payloadSize == 0 && client->contentBytes != NULL
            && client->receivedContentSize == client->expectedContentSize)
        {
            uint8_t actualHash[LAIUE_NETWORK_CONTENT_HASH_SIZE];
            if (!ComputeSha256(client->contentBytes,
                    client->receivedContentSize, actualHash)
                || !HashEquals(actualHash, client->expectedContentHash,
                    LAIUE_NETWORK_CONTENT_HASH_SIZE)) return false;
            event.type = NETWORK_CLIENT_EVENT_CONTENT_READY;
            return ClientPushEvent(client, &event);
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_MOD_ENTRY
            && client->receivedServerModCount < client->expectedServerModCount)
        {
            LaiueProtocolMod wire;
            if (!LaiueProtocolDecodeMod(frame->payload, frame->payloadSize, &wire))
            {
                return false;
            }
            CopyModFromProtocol(
                &client->serverMods[client->receivedServerModCount++], &wire);
            return client->receivedServerModCount != client->expectedServerModCount
                || ClientPushServerModsEvent(client);
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_REJECT)
        {
            uint8_t reason;
            if (!LaiueProtocolDecodeReject(frame->payload, frame->payloadSize, &reason)
                || reason > NETWORK_REJECT_SERVER_POLICY) return false;
            event.type = NETWORK_CLIENT_EVENT_REJECTED;
            event.data.rejectReason = (NetworkRejectReason)reason;
            if (!ClientPushEvent(client, &event)) return false;
            client->disconnectNotified = true;
            if (client->channel.socket != INVALID_SOCKET)
            {
                closesocket(client->channel.socket);
                client->channel.socket = INVALID_SOCKET;
            }
            client->state = NETWORK_CONNECTION_DISCONNECTED;
            return true;
        }
        if (frame->type == LAIUE_MESSAGE_SERVER_WELCOME && client->modsSubmitted)
        {
            uint64_t echoedNonce;
            if (!LaiueProtocolDecodeWelcome(frame->payload, frame->payloadSize,
                    &event.data.ready.peerId, &event.data.ready.worldSeed,
                    &echoedNonce) || echoedNonce != client->clientNonce) return false;
            event.type = NETWORK_CLIENT_EVENT_READY;
            client->state = NETWORK_CONNECTION_READY;
            return ClientPushEvent(client, &event);
        }
        return false;
    }

    if (client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    if (frame->type == LAIUE_MESSAGE_PLAYER_STATE)
    {
        LaiueProtocolPlayerState decoded;
        if (!LaiueProtocolDecodePlayerState(frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_PLAYER_STATE;
        event.data.playerState.serverTick = decoded.serverTick;
        event.data.playerState.peerId = decoded.peerId;
        event.data.playerState.position[0] = decoded.position[0];
        event.data.playerState.position[1] = decoded.position[1];
        event.data.playerState.position[2] = decoded.position[2];
        event.data.playerState.yaw = decoded.yaw;
        event.data.playerState.pitch = decoded.pitch;
        event.data.playerState.grounded = decoded.grounded;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DELTA)
    {
        LaiueProtocolBlockDelta decoded;
        if (!LaiueProtocolDecodeBlockDelta(frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DELTA;
        event.data.blockDelta.serverTick = decoded.serverTick;
        event.data.blockDelta.block[0] = decoded.block[0];
        event.data.blockDelta.block[1] = decoded.block[1];
        event.data.blockDelta.block[2] = decoded.block[2];
        event.data.blockDelta.replacement = decoded.replacement;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DROP_SPAWN)
    {
        LaiueProtocolBlockDrop decoded;
        if (!LaiueProtocolDecodeBlockDrop(frame->payload,
                frame->payloadSize, &decoded)) return false;
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN;
        event.data.blockDrop.id = decoded.id;
        event.data.blockDrop.block = decoded.block;
        event.data.blockDrop.position[0] = decoded.position[0];
        event.data.blockDrop.position[1] = decoded.position[1];
        event.data.blockDrop.position[2] = decoded.position[2];
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_BLOCK_DROP_REMOVE)
    {
        event.type = NETWORK_CLIENT_EVENT_BLOCK_DROP_REMOVE;
        if (!LaiueProtocolDecodeDropRemove(frame->payload,
                frame->payloadSize, &event.data.removedDropId)) return false;
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_INVENTORY_STATE)
    {
        LaiueProtocolInventory decoded;
        if (!LaiueProtocolDecodeInventory(frame->payload,
                frame->payloadSize, &decoded)) return false;
        event.type = NETWORK_CLIENT_EVENT_INVENTORY_STATE;
        event.data.inventory.selectedHotbarSlot =
            decoded.selectedHotbarSlot;
        for (uint32_t i = 0; i < LAIUE_NETWORK_INVENTORY_SLOTS; ++i)
        {
            event.data.inventory.slots[i].item = decoded.slots[i].item;
            event.data.inventory.slots[i].count = decoded.slots[i].count;
        }
        return ClientPushEvent(client, &event);
    }
    if (frame->type == LAIUE_MESSAGE_PONG && frame->payloadSize == 0)
    {
        return true;
    }
    return false;
}

static bool ClientParseFrames(NetworkClient *client)
{
    for (;;)
    {
        LaiueProtocolFrame frame;
        bool complete;
        if (!ChannelNextFrame(&client->channel, &frame, &complete))
        {
            return false;
        }
        if (!complete)
        {
            return true;
        }
        if (!ClientHandleFrame(client, &frame))
        {
            return false;
        }
        ChannelConsumeFrame(&client->channel, &frame);
    }
}

static bool ServerPushEvent(NetworkServer *server, const NetworkServerEvent *event)
{
    if (server->eventCount == NETWORK_SERVER_EVENT_CAPACITY)
    {
        return false;
    }
    server->events[server->eventWrite] = *event;
    server->eventWrite = (server->eventWrite + 1U) % NETWORK_SERVER_EVENT_CAPACITY;
    server->eventCount++;
    return true;
}

static void ServerDisconnectPeer(NetworkServer *server, uint32_t peerIndex,
                                 NetworkDisconnectReason reason)
{
    NetworkServerPeer *peer = &server->peers[peerIndex];
    bool notify = peer->allocated && peer->ready;
    uint32_t peerId = peer->peerId;
    if (peer->allocated && peer->channel.socket != INVALID_SOCKET)
    {
        closesocket(peer->channel.socket);
    }
    memset(peer, 0, sizeof(*peer));
    peer->channel.socket = INVALID_SOCKET;
    if (notify)
    {
        NetworkServerEvent event = {
            .type = NETWORK_SERVER_EVENT_DISCONNECTED,
            .peerId = peerId,
            .data.disconnectReason = reason,
        };
        ServerPushEvent(server, &event);
    }
}

static bool ServerCheckRate(NetworkServerPeer *peer, LaiueMessageType type)
{
    uint64_t now = GetTickCount64();
    if (now - peer->rateWindowStartMs >= NETWORK_RATE_WINDOW_MS)
    {
        peer->rateWindowStartMs = now;
        peer->framesInWindow = 0;
        peer->inputsInWindow = 0;
        peer->editsInWindow = 0;
    }
    peer->framesInWindow++;
    if (type == LAIUE_MESSAGE_PLAYER_INPUT)
    {
        peer->inputsInWindow++;
    }
    else if (type == LAIUE_MESSAGE_EDIT_INTENT)
    {
        peer->editsInWindow++;
    }
    return peer->framesInWindow <= NETWORK_MAX_FRAMES_PER_SECOND &&
           peer->inputsInWindow <= NETWORK_MAX_INPUTS_PER_SECOND &&
           peer->editsInWindow <= NETWORK_MAX_EDITS_PER_SECOND;
}

static bool ServerFinishModNegotiation(NetworkServer *server,
                                       NetworkServerPeer *peer)
{
    bool matches = peer->receivedModCount == server->modCount;
    for (uint32_t i = 0; i < server->modCount && matches; ++i)
    {
        matches = ModDescriptorsEqual(&peer->mods[i], &server->mods[i]);
    }

    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    if (!matches)
    {
        uint32_t size = LaiueProtocolEncodeReject(payload, sizeof(payload),
            NETWORK_REJECT_MOD_MISMATCH);
        if (size == 0 || !ChannelQueuePayload(&peer->channel,
                LAIUE_MESSAGE_SERVER_REJECT, payload, size)) return false;
        peer->rejected = true;
        return true;
    }

    uint32_t size = LaiueProtocolEncodeWelcome(payload, sizeof(payload),
        peer->peerId, server->worldSeed, peer->clientNonce);
    if (size == 0 || !ChannelQueuePayload(&peer->channel,
            LAIUE_MESSAGE_SERVER_WELCOME, payload, size)) return false;
    peer->ready = true;
    NetworkServerEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NETWORK_SERVER_EVENT_CONNECTED;
    event.peerId = peer->peerId;
    return ServerPushEvent(server, &event);
}

static bool ServerBeginContentTransfer(NetworkServer *server,
                                       NetworkServerPeer *peer)
{
    if (!server->allowContentDownloads || server->contentBundle == NULL
        || server->contentBundleSize == 0 || peer->contentTransferActive)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeContentBegin(payload, sizeof(payload),
        server->contentBundleSize, server->contentBundleHash);
    if (size == 0 || !ChannelQueuePayload(&peer->channel,
            LAIUE_MESSAGE_SERVER_CONTENT_BEGIN, payload, size)) return false;
    peer->contentOffset = 0;
    peer->contentTransferActive = true;
    return true;
}

static bool ServerPumpContentTransfer(NetworkServer *server,
                                      NetworkServerPeer *peer)
{
    if (!peer->contentTransferActive) return true;
    for (uint32_t chunkIndex = 0; chunkIndex < 8U; ++chunkIndex)
    {
        uint32_t available = NETWORK_SEND_CAPACITY - peer->channel.sendCount;
        if (peer->contentOffset == server->contentBundleSize)
        {
            if (available < LAIUE_PROTOCOL_HEADER_SIZE) return true;
            if (!ChannelQueuePayload(&peer->channel,
                    LAIUE_MESSAGE_SERVER_CONTENT_END, NULL, 0)) return false;
            peer->contentTransferActive = false;
            return true;
        }
        if (available <= LAIUE_PROTOCOL_HEADER_SIZE) return true;
        uint64_t remaining = server->contentBundleSize - peer->contentOffset;
        uint32_t chunkSize = remaining > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE
            ? LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE : (uint32_t)remaining;
        uint32_t maximumForRing = available - LAIUE_PROTOCOL_HEADER_SIZE;
        if (chunkSize > maximumForRing) chunkSize = maximumForRing;
        if (chunkSize == 0) return true;
        if (!ChannelQueuePayload(&peer->channel,
                LAIUE_MESSAGE_SERVER_CONTENT_CHUNK,
                server->contentBundle + peer->contentOffset,
                chunkSize)) return false;
        peer->contentOffset += chunkSize;
    }
    return true;
}

static bool ServerHandleFrame(NetworkServer *server, NetworkServerPeer *peer,
                              const LaiueProtocolFrame *frame)
{
    if (!ServerCheckRate(peer, frame->type))
    {
        return false;
    }

    NetworkServerEvent event;
    memset(&event, 0, sizeof(event));
    event.peerId = peer->peerId;
    if (!peer->ready)
    {
        if (!peer->helloReceived)
        {
            if (frame->type != LAIUE_MESSAGE_CLIENT_HELLO
                || !LaiueProtocolDecodeHello(frame->payload,
                    frame->payloadSize, &peer->clientNonce)) return false;
            peer->helloReceived = true;
            return ChannelQueueModList(&peer->channel,
                LAIUE_MESSAGE_SERVER_MOD_LIST,
                LAIUE_MESSAGE_SERVER_MOD_ENTRY,
                server->mods, server->modCount,
                server->allowContentDownloads ? 1U : 0U);
        }
        if (!peer->modListReceived)
        {
            if (frame->type == LAIUE_MESSAGE_CLIENT_CONTENT_REQUEST
                && frame->payloadSize == 0)
            {
                return ServerBeginContentTransfer(server, peer);
            }
            uint8_t flags;
            if (frame->type != LAIUE_MESSAGE_CLIENT_MOD_LIST
                || !LaiueProtocolDecodeModList(frame->payload,
                    frame->payloadSize, &peer->expectedModCount, &flags)
                || flags != 0) return false;
            peer->modListReceived = true;
            return peer->expectedModCount != 0
                || ServerFinishModNegotiation(server, peer);
        }
        if (frame->type != LAIUE_MESSAGE_CLIENT_MOD_ENTRY
            || peer->receivedModCount >= peer->expectedModCount) return false;
        LaiueProtocolMod wire;
        if (!LaiueProtocolDecodeMod(frame->payload, frame->payloadSize, &wire)) return false;
        CopyModFromProtocol(&peer->mods[peer->receivedModCount++], &wire);
        return peer->receivedModCount != peer->expectedModCount
            || ServerFinishModNegotiation(server, peer);
    }

    if (frame->type == LAIUE_MESSAGE_PLAYER_INPUT)
    {
        LaiueProtocolInput decoded;
        if (!LaiueProtocolDecodeInput(frame->payload, frame->payloadSize, &decoded))
        {
            return false;
        }
        event.type = NETWORK_SERVER_EVENT_INPUT;
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
                frame->payload, frame->payloadSize, &event.data.editIntent.breakBlock,
                &event.data.editIntent.placeBlock,
                &event.data.editIntent.placementBlock,
                event.data.editIntent.direction))
        {
            return false;
        }
        return ServerPushEvent(server, &event);
    }
    if (frame->type == LAIUE_MESSAGE_SELECT_HOTBAR_SLOT
        && frame->payloadSize == 1U && frame->payload[0] < 9U)
    {
        event.type = NETWORK_SERVER_EVENT_SELECT_HOTBAR_SLOT;
        event.data.selectedHotbarSlot = frame->payload[0];
        return ServerPushEvent(server, &event);
    }
    if (frame->type == LAIUE_MESSAGE_PING && frame->payloadSize == 0)
    {
        return ChannelQueuePayload(&peer->channel, LAIUE_MESSAGE_PONG, NULL, 0);
    }
    return false;
}

static bool ServerParseFrames(NetworkServer *server, NetworkServerPeer *peer)
{
    for (;;)
    {
        LaiueProtocolFrame frame;
        bool complete;
        if (!ChannelNextFrame(&peer->channel, &frame, &complete))
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
        ChannelConsumeFrame(&peer->channel, &frame);
    }
}

NetworkClient *NetworkClientCreateLoopback(uint16_t port)
{
    if (port == 0 || !WinsockAcquire())
    {
        return NULL;
    }

    NetworkClient *client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*client));
    if (client == NULL)
    {
        WinsockRelease();
        return NULL;
    }
    client->winsockAcquired = true;
    client->state = NETWORK_CONNECTION_CONNECTING;
    client->channel.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->channel.socket == INVALID_SOCKET || !SocketSetNonblocking(client->channel.socket) ||
        !SocketSetLowLatency(client->channel.socket))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_IO);
        return client;
    }
    SOCKET socketHandle = client->channel.socket;
    ChannelInitialize(&client->channel, socketHandle);

    SOCKADDR_IN address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(client->channel.socket, (const SOCKADDR *)&address, sizeof(address));
    if (result == 0)
    {
        if (!ClientBeginHandshake(client))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
        }
    }
    else
    {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
        }
    }
    return client;
}

void NetworkClientDestroy(NetworkClient *client)
{
    if (client == NULL)
    {
        return;
    }
    if (client->channel.socket != INVALID_SOCKET)
    {
        closesocket(client->channel.socket);
    }
    if (client->contentBytes != NULL)
    {
        HeapFree(GetProcessHeap(), 0, client->contentBytes);
    }
    bool releaseWinsock = client->winsockAcquired;
    HeapFree(GetProcessHeap(), 0, client);
    if (releaseWinsock)
    {
        WinsockRelease();
    }
}

void NetworkClientUpdate(NetworkClient *client)
{
    if (client == NULL || client->state == NETWORK_CONNECTION_DISCONNECTED)
    {
        return;
    }

    if (!client->transportConnected)
    {
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(client->channel.socket, &writable);
        FD_SET(client->channel.socket, &failed);
        TIMEVAL timeout = {0, 0};
        int selected = select(0, NULL, &writable, &failed, &timeout);
        if (selected == SOCKET_ERROR || FD_ISSET(client->channel.socket, &failed))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_IO);
            return;
        }
        if (FD_ISSET(client->channel.socket, &writable))
        {
            int socketError = 0;
            int length = sizeof(socketError);
            if (getsockopt(client->channel.socket, SOL_SOCKET, SO_ERROR, (char *)&socketError,
                           &length) != 0 ||
                socketError != 0 || !ClientBeginHandshake(client))
            {
                ClientDisconnect(client, NETWORK_DISCONNECT_IO);
                return;
            }
        }
    }

    if (client->transportConnected && !ChannelFlush(&client->channel))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_IO);
        return;
    }
    for (uint32_t receivePass = 0; receivePass < 64U; ++receivePass)
    {
        int32_t receiveResult = ChannelReceive(&client->channel);
        if (receiveResult == 0)
        {
            break;
        }
        if (receiveResult < 0)
        {
            ClientDisconnect(client, receiveResult == -2 ? NETWORK_DISCONNECT_OVERFLOW
                                                         : NETWORK_DISCONNECT_REMOTE);
            return;
        }
        if (!ClientParseFrames(client))
        {
            ClientDisconnect(client, NETWORK_DISCONNECT_PROTOCOL);
            return;
        }
    }

    uint64_t now = GetTickCount64();
    if (client->state == NETWORK_CONNECTION_CONNECTING
        && now - client->channel.connectedAtMs > NETWORK_HANDSHAKE_TIMEOUT_MS)
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_TIMEOUT);
    }
    else if (client->state == NETWORK_CONNECTION_NEGOTIATING_MODS
        && now - client->channel.lastReceiveAtMs
            > NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS)
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_TIMEOUT);
    }
    else if (client->state == NETWORK_CONNECTION_READY &&
             now - client->channel.lastReceiveAtMs > NETWORK_IDLE_TIMEOUT_MS)
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_TIMEOUT);
    }
    else if (client->state == NETWORK_CONNECTION_READY &&
             now - client->channel.lastQueueAtMs >= NETWORK_KEEPALIVE_INTERVAL_MS &&
             !ChannelQueuePayload(&client->channel, LAIUE_MESSAGE_PING, NULL, 0))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
    }
}

NetworkConnectionState NetworkClientGetState(const NetworkClient *client)
{
    return client != NULL ? client->state : NETWORK_CONNECTION_DISCONNECTED;
}

bool NetworkClientPollEvent(NetworkClient *client, NetworkClientEvent *outEvent)
{
    if (client == NULL || outEvent == NULL || client->eventCount == 0)
    {
        return false;
    }
    *outEvent = client->events[client->eventRead];
    client->eventRead = (client->eventRead + 1U) % NETWORK_CLIENT_EVENT_CAPACITY;
    client->eventCount--;
    return true;
}

bool NetworkClientCopyServerMods(const NetworkClient *client,
                                 NetworkModDescriptor *output,
                                 uint32_t capacity, uint32_t *outCount)
{
    if (client == NULL || outCount == NULL) return false;
    *outCount = client->receivedServerModCount;
    if (capacity < client->receivedServerModCount
        || (client->receivedServerModCount != 0 && output == NULL)) return false;
    if (client->receivedServerModCount != 0)
    {
        memcpy(output, client->serverMods,
            (size_t)client->receivedServerModCount * sizeof(output[0]));
    }
    return true;
}

bool NetworkClientSubmitMods(NetworkClient *client,
                             const NetworkModDescriptor *mods,
                             uint32_t count)
{
    if (client == NULL
        || client->state != NETWORK_CONNECTION_NEGOTIATING_MODS
        || client->receivedServerModCount != client->expectedServerModCount
        || client->modsSubmitted || count > LAIUE_NETWORK_MAX_MODS
        || (count != 0 && mods == NULL)) return false;
    if (!ChannelQueueModList(&client->channel,
            LAIUE_MESSAGE_CLIENT_MOD_LIST,
            LAIUE_MESSAGE_CLIENT_MOD_ENTRY, mods, count, 0))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    client->modsSubmitted = true;
    return true;
}

bool NetworkClientRequestContent(NetworkClient *client)
{
    if (client == NULL
        || client->state != NETWORK_CONNECTION_NEGOTIATING_MODS
        || !client->serverDownloadsAllowed || client->contentRequested
        || client->modsSubmitted) return false;
    if (!ChannelQueuePayload(&client->channel,
            LAIUE_MESSAGE_CLIENT_CONTENT_REQUEST, NULL, 0))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    client->contentRequested = true;
    return true;
}

bool NetworkClientTakeContent(NetworkClient *client,
                              uint8_t **outBytes, uint64_t *outSize)
{
    if (client == NULL || outBytes == NULL || outSize == NULL
        || client->contentBytes == NULL
        || client->receivedContentSize != client->expectedContentSize)
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

bool NetworkClientSendInput(NetworkClient *client, const NetworkInputCommand *input)
{
    if (client == NULL || input == NULL || client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    LaiueProtocolInput protocolInput = {
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
    uint32_t size = LaiueProtocolEncodeInput(payload, sizeof(payload), &protocolInput);
    if (size == 0)
    {
        return false;
    }
    if (!ChannelQueuePayload(&client->channel, LAIUE_MESSAGE_PLAYER_INPUT, payload, size))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return true;
}

bool NetworkClientSendEditIntent(NetworkClient *client, bool breakBlock,
                                 bool placeBlock, uint8_t placementBlock,
                                 const float direction[3])
{
    if (client == NULL || client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size =
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), breakBlock,
            placeBlock, placementBlock, direction);
    if (size == 0)
    {
        return false;
    }
    if (!ChannelQueuePayload(&client->channel, LAIUE_MESSAGE_EDIT_INTENT, payload, size))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return true;
}

bool NetworkClientSendSelectedHotbarSlot(NetworkClient* client, uint8_t slot)
{
    if (client == NULL || client->state != NETWORK_CONNECTION_READY
        || slot >= 9U) return false;
    if (!ChannelQueuePayload(&client->channel,
            LAIUE_MESSAGE_SELECT_HOTBAR_SLOT, &slot, 1U))
    {
        ClientDisconnect(client, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return true;
}

NetworkServer *NetworkServerCreateLoopback(const NetworkServerConfiguration *configuration)
{
    if (configuration == NULL || configuration->port == 0 || configuration->maximumPeers == 0 ||
        configuration->maximumPeers > LAIUE_NETWORK_MAX_PEERS
        || configuration->modCount > LAIUE_NETWORK_MAX_MODS
        || (configuration->modCount != 0 && configuration->mods == NULL)
        || (configuration->allowContentDownloads
            && (configuration->contentBundle == NULL
                || configuration->contentBundleSize == 0
                || configuration->contentBundleSize > LAIUE_NETWORK_MAX_CONTENT_BYTES))
        || !WinsockAcquire())
    {
        return NULL;
    }
    NetworkServer *server = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*server));
    if (server == NULL)
    {
        WinsockRelease();
        return NULL;
    }
    server->winsockAcquired = true;
    server->listener = INVALID_SOCKET;
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        server->peers[index].channel.socket = INVALID_SOCKET;
    }
    server->maximumPeers = configuration->maximumPeers;
    server->worldSeed = configuration->worldSeed;
    server->modCount = configuration->modCount;
    server->allowContentDownloads = configuration->allowContentDownloads;
    server->contentBundle = configuration->contentBundle;
    server->contentBundleSize = configuration->contentBundleSize;
    memcpy(server->contentBundleHash, configuration->contentBundleSha256,
        LAIUE_NETWORK_CONTENT_HASH_SIZE);
    if (server->modCount != 0)
    {
        memcpy(server->mods, configuration->mods,
            (size_t)server->modCount * sizeof(server->mods[0]));
    }
    server->nextPeerId = 1;

    server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listener == INVALID_SOCKET || !SocketSetNonblocking(server->listener))
    {
        NetworkServerDestroy(server);
        return NULL;
    }
    BOOL exclusive = TRUE;
    if (setsockopt(server->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive,
                   sizeof(exclusive)) != 0)
    {
        NetworkServerDestroy(server);
        return NULL;
    }

    SOCKADDR_IN address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(configuration->port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener, (const SOCKADDR *)&address, sizeof(address)) != 0 ||
        listen(server->listener, (int)server->maximumPeers) != 0)
    {
        NetworkServerDestroy(server);
        return NULL;
    }
    return server;
}

void NetworkServerDestroy(NetworkServer *server)
{
    if (server == NULL)
    {
        return;
    }
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        if (server->peers[index].allocated && server->peers[index].channel.socket != INVALID_SOCKET)
        {
            closesocket(server->peers[index].channel.socket);
        }
    }
    if (server->listener != INVALID_SOCKET)
    {
        closesocket(server->listener);
    }
    bool releaseWinsock = server->winsockAcquired;
    HeapFree(GetProcessHeap(), 0, server);
    if (releaseWinsock)
    {
        WinsockRelease();
    }
}

static void ServerAcceptPeers(NetworkServer *server)
{
    for (;;)
    {
        SOCKET accepted = accept(server->listener, NULL, NULL);
        if (accepted == INVALID_SOCKET)
        {
            return;
        }
        uint32_t freeIndex = LAIUE_NETWORK_MAX_PEERS;
        for (uint32_t index = 0; index < server->maximumPeers; ++index)
        {
            if (!server->peers[index].allocated)
            {
                freeIndex = index;
                break;
            }
        }
        if (freeIndex == LAIUE_NETWORK_MAX_PEERS || !SocketSetNonblocking(accepted) ||
            !SocketSetLowLatency(accepted))
        {
            closesocket(accepted);
            continue;
        }

        NetworkServerPeer *peer = &server->peers[freeIndex];
        memset(peer, 0, sizeof(*peer));
        ChannelInitialize(&peer->channel, accepted);
        peer->allocated = true;
        peer->peerId = server->nextPeerId++;
        if (server->nextPeerId == 0)
        {
            server->nextPeerId = 1;
        }
        peer->rateWindowStartMs = GetTickCount64();
    }
}

void NetworkServerUpdate(NetworkServer *server)
{
    if (server == NULL)
    {
        return;
    }
    ServerAcceptPeers(server);
    uint64_t now = GetTickCount64();
    for (uint32_t index = 0; index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (!peer->allocated)
        {
            continue;
        }
        if (!ChannelFlush(&peer->channel))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_IO);
            continue;
        }
        if (peer->rejected && peer->channel.sendCount == 0)
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_PROTOCOL);
            continue;
        }
        bool disconnected = false;
        for (uint32_t receivePass = 0; receivePass < 64U; ++receivePass)
        {
            int32_t receiveResult = ChannelReceive(&peer->channel);
            if (receiveResult == 0)
            {
                break;
            }
            if (receiveResult < 0)
            {
                ServerDisconnectPeer(server, index,
                                     receiveResult == -2 ? NETWORK_DISCONNECT_OVERFLOW
                                                         : NETWORK_DISCONNECT_REMOTE);
                disconnected = true;
                break;
            }
            if (!ServerParseFrames(server, peer))
            {
                ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_PROTOCOL);
                disconnected = true;
                break;
            }
        }
        if (disconnected || !peer->allocated)
        {
            continue;
        }
        if (!ServerPumpContentTransfer(server, peer))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_OVERFLOW);
            continue;
        }
        if ((!peer->helloReceived
                && now - peer->channel.connectedAtMs > NETWORK_HANDSHAKE_TIMEOUT_MS)
            || (!peer->ready && peer->helloReceived
                && now - peer->channel.lastReceiveAtMs
                    > NETWORK_NEGOTIATION_IDLE_TIMEOUT_MS) ||
            (peer->ready && now - peer->channel.lastReceiveAtMs > NETWORK_IDLE_TIMEOUT_MS))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_TIMEOUT);
        }
        else if (!ChannelFlush(&peer->channel))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_IO);
        }
    }
}

bool NetworkServerPollEvent(NetworkServer *server, NetworkServerEvent *outEvent)
{
    if (server == NULL || outEvent == NULL || server->eventCount == 0)
    {
        return false;
    }
    *outEvent = server->events[server->eventRead];
    server->eventRead = (server->eventRead + 1U) % NETWORK_SERVER_EVENT_CAPACITY;
    server->eventCount--;
    return true;
}

bool NetworkServerBroadcastPlayerState(NetworkServer *server, const NetworkPlayerState *state)
{
    if (server == NULL || state == NULL)
    {
        return false;
    }
    LaiueProtocolPlayerState encodedState = {
        .serverTick = state->serverTick,
        .peerId = state->peerId,
        .position = {state->position[0], state->position[1], state->position[2]},
        .yaw = state->yaw,
        .pitch = state->pitch,
        .grounded = state->grounded,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodePlayerState(payload, sizeof(payload), &encodedState);
    if (size == 0)
    {
        return false;
    }
    bool result = true;
    for (uint32_t index = 0; index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (peer->allocated && peer->ready &&
            !ChannelQueuePayload(&peer->channel, LAIUE_MESSAGE_PLAYER_STATE, payload, size))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_OVERFLOW);
            result = false;
        }
    }
    return result;
}

bool NetworkServerBroadcastBlockDelta(NetworkServer *server, const NetworkBlockDelta *delta)
{
    if (server == NULL || delta == NULL)
    {
        return false;
    }
    LaiueProtocolBlockDelta encodedDelta = {
        .serverTick = delta->serverTick,
        .block = {delta->block[0], delta->block[1], delta->block[2]},
        .replacement = delta->replacement,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeBlockDelta(payload, sizeof(payload), &encodedDelta);
    if (size == 0)
    {
        return false;
    }
    bool result = true;
    for (uint32_t index = 0; index < server->maximumPeers; ++index)
    {
        NetworkServerPeer *peer = &server->peers[index];
        if (peer->allocated && peer->ready &&
            !ChannelQueuePayload(&peer->channel, LAIUE_MESSAGE_BLOCK_DELTA, payload, size))
        {
            ServerDisconnectPeer(server, index, NETWORK_DISCONNECT_OVERFLOW);
            result = false;
        }
    }
    return result;
}

bool NetworkServerBroadcastBlockDrop(NetworkServer* server,
    const NetworkBlockDrop* drop)
{
    if (server == NULL || drop == NULL) return false;
    LaiueProtocolBlockDrop encoded = {
        .id = drop->id,
        .position = { drop->position[0], drop->position[1], drop->position[2] },
        .block = drop->block,
    };
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeBlockDrop(payload,
        sizeof(payload), &encoded);
    if (size == 0) return false;
    bool result = true;
    for (uint32_t i = 0; i < server->maximumPeers; ++i)
    {
        NetworkServerPeer* peer = &server->peers[i];
        if (peer->allocated && peer->ready
            && !ChannelQueuePayload(&peer->channel,
                LAIUE_MESSAGE_BLOCK_DROP_SPAWN, payload, size))
        {
            ServerDisconnectPeer(server, i, NETWORK_DISCONNECT_OVERFLOW);
            result = false;
        }
    }
    return result;
}

bool NetworkServerBroadcastDropRemove(NetworkServer* server, uint32_t dropId)
{
    if (server == NULL) return false;
    uint8_t payload[4];
    uint32_t size = LaiueProtocolEncodeDropRemove(payload,
        sizeof(payload), dropId);
    if (size == 0) return false;
    bool result = true;
    for (uint32_t i = 0; i < server->maximumPeers; ++i)
    {
        NetworkServerPeer* peer = &server->peers[i];
        if (peer->allocated && peer->ready
            && !ChannelQueuePayload(&peer->channel,
                LAIUE_MESSAGE_BLOCK_DROP_REMOVE, payload, size))
        {
            ServerDisconnectPeer(server, i, NETWORK_DISCONNECT_OVERFLOW);
            result = false;
        }
    }
    return result;
}

bool NetworkServerSendInventory(NetworkServer* server, uint32_t peerId,
    const NetworkInventoryState* inventory)
{
    if (server == NULL || peerId == 0 || inventory == NULL) return false;
    LaiueProtocolInventory encoded;
    memset(&encoded, 0, sizeof(encoded));
    encoded.selectedHotbarSlot = inventory->selectedHotbarSlot;
    for (uint32_t i = 0; i < LAIUE_NETWORK_INVENTORY_SLOTS; ++i)
    {
        encoded.slots[i].item = inventory->slots[i].item;
        encoded.slots[i].count = inventory->slots[i].count;
    }
    uint8_t payload[NETWORK_CONTROL_PAYLOAD_CAPACITY];
    uint32_t size = LaiueProtocolEncodeInventory(payload,
        sizeof(payload), &encoded);
    if (size == 0) return false;
    for (uint32_t i = 0; i < server->maximumPeers; ++i)
    {
        NetworkServerPeer* peer = &server->peers[i];
        if (!peer->allocated || !peer->ready || peer->peerId != peerId)
            continue;
        if (ChannelQueuePayload(&peer->channel,
                LAIUE_MESSAGE_INVENTORY_STATE, payload, size)) return true;
        ServerDisconnectPeer(server, i, NETWORK_DISCONNECT_OVERFLOW);
        return false;
    }
    return false;
}
