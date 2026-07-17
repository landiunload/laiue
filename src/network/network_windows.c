#include "network/network.h"
#include "network/protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <stddef.h>
#include <string.h>

#define NETWORK_RECEIVE_CAPACITY 4096U
#define NETWORK_SEND_CAPACITY 65536U
#define NETWORK_CLIENT_EVENT_CAPACITY 128U
#define NETWORK_SERVER_EVENT_CAPACITY 256U
#define NETWORK_HANDSHAKE_TIMEOUT_MS 5000ULL
#define NETWORK_IDLE_TIMEOUT_MS 15000ULL
#define NETWORK_RATE_WINDOW_MS 1000ULL
#define NETWORK_MAX_FRAMES_PER_SECOND 160U
#define NETWORK_MAX_INPUTS_PER_SECOND 120U
#define NETWORK_MAX_EDITS_PER_SECOND 16U
#define NETWORK_KEEPALIVE_INTERVAL_MS 1000ULL

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
    bool allocated;
    bool ready;
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
    bool winsockAcquired;
};

static volatile LONG g_winsockReferences;

static bool WinsockAcquire(void)
{
    LONG references = InterlockedIncrement(&g_winsockReferences);
    if (references == 1)
    {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            InterlockedDecrement(&g_winsockReferences);
            return false;
        }
    }
    return true;
}

static void WinsockRelease(void)
{
    if (InterlockedDecrement(&g_winsockReferences) == 0)
    {
        WSACleanup();
    }
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
    uint8_t frame[LAIUE_PROTOCOL_MAX_FRAME_SIZE];
    uint32_t nextSequence = channel->sendSequence + 1U;
    if (nextSequence == 0)
    {
        return false;
    }
    uint32_t frameSize =
        LaiueProtocolWriteFrame(frame, sizeof(frame), type, nextSequence, payload, payloadSize);
    if (frameSize == 0 || !ChannelQueueBytes(channel, frame, frameSize))
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

static bool ClientBeginHandshake(NetworkClient *client)
{
    uint64_t nonce;
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
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
        uint64_t echoedNonce;
        if (frame->type != LAIUE_MESSAGE_SERVER_WELCOME ||
            !LaiueProtocolDecodeWelcome(frame->payload, frame->payloadSize,
                                        &event.data.ready.peerId, &event.data.ready.worldSeed,
                                        &echoedNonce) ||
            echoedNonce != client->clientNonce)
        {
            return false;
        }
        event.type = NETWORK_CLIENT_EVENT_READY;
        client->state = NETWORK_CONNECTION_READY;
        return ClientPushEvent(client, &event);
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
        uint64_t nonce;
        if (frame->type != LAIUE_MESSAGE_CLIENT_HELLO ||
            !LaiueProtocolDecodeHello(frame->payload, frame->payloadSize, &nonce))
        {
            return false;
        }
        uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
        uint32_t payloadSize = LaiueProtocolEncodeWelcome(payload, sizeof(payload), peer->peerId,
                                                          server->worldSeed, nonce);
        if (payloadSize == 0 || !ChannelQueuePayload(&peer->channel, LAIUE_MESSAGE_SERVER_WELCOME,
                                                     payload, payloadSize))
        {
            return false;
        }
        peer->ready = true;
        event.type = NETWORK_SERVER_EVENT_CONNECTED;
        return ServerPushEvent(server, &event);
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
                &event.data.editIntent.placeBlock, event.data.editIntent.direction))
        {
            return false;
        }
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
    if (client->state == NETWORK_CONNECTION_CONNECTING &&
        now - client->channel.connectedAtMs > NETWORK_HANDSHAKE_TIMEOUT_MS)
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
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
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

bool NetworkClientSendEditIntent(NetworkClient *client, bool breakBlock, bool placeBlock,
                                 const float direction[3])
{
    if (client == NULL || client->state != NETWORK_CONNECTION_READY)
    {
        return false;
    }
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    uint32_t size =
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), breakBlock, placeBlock, direction);
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

NetworkServer *NetworkServerCreateLoopback(const NetworkServerConfiguration *configuration)
{
    if (configuration == NULL || configuration->port == 0 || configuration->maximumPeers == 0 ||
        configuration->maximumPeers > LAIUE_NETWORK_MAX_PEERS || !WinsockAcquire())
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
        if ((!peer->ready && now - peer->channel.connectedAtMs > NETWORK_HANDSHAKE_TIMEOUT_MS) ||
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
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
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
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
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
