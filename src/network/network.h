#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_NETWORK_DEFAULT_PORT 27180U
#define LAIUE_NETWORK_MAX_PEERS 16U

typedef struct NetworkClient NetworkClient;
typedef struct NetworkServer NetworkServer;

typedef enum NetworkConnectionState
{
    NETWORK_CONNECTION_CONNECTING,
    NETWORK_CONNECTION_READY,
    NETWORK_CONNECTION_DISCONNECTED
} NetworkConnectionState;

typedef enum NetworkDisconnectReason
{
    NETWORK_DISCONNECT_NONE,
    NETWORK_DISCONNECT_REMOTE,
    NETWORK_DISCONNECT_IO,
    NETWORK_DISCONNECT_PROTOCOL,
    NETWORK_DISCONNECT_OVERFLOW,
    NETWORK_DISCONNECT_TIMEOUT
} NetworkDisconnectReason;

typedef struct NetworkInputCommand
{
    float movementX;
    float movementY;
    float yaw;
    float pitch;
    bool jumpPressed;
    bool jumpHeld;
    bool sprintHeld;
    bool crouchHeld;
} NetworkInputCommand;

typedef struct NetworkPlayerState
{
    uint32_t serverTick;
    uint32_t peerId;
    double position[3];
    float yaw;
    float pitch;
    bool grounded;
} NetworkPlayerState;

typedef struct NetworkBlockDelta
{
    uint32_t serverTick;
    int64_t block[3];
    uint8_t replacement;
} NetworkBlockDelta;

typedef enum NetworkClientEventType
{
    NETWORK_CLIENT_EVENT_READY,
    NETWORK_CLIENT_EVENT_PLAYER_STATE,
    NETWORK_CLIENT_EVENT_BLOCK_DELTA,
    NETWORK_CLIENT_EVENT_DISCONNECTED
} NetworkClientEventType;

typedef struct NetworkClientEvent
{
    NetworkClientEventType type;
    union
    {
        struct
        {
            uint32_t peerId;
            int64_t worldSeed;
        } ready;
        NetworkPlayerState playerState;
        NetworkBlockDelta blockDelta;
        NetworkDisconnectReason disconnectReason;
    } data;
} NetworkClientEvent;

typedef enum NetworkServerEventType
{
    NETWORK_SERVER_EVENT_CONNECTED,
    NETWORK_SERVER_EVENT_INPUT,
    NETWORK_SERVER_EVENT_EDIT_INTENT,
    NETWORK_SERVER_EVENT_DISCONNECTED
} NetworkServerEventType;

typedef struct NetworkServerEvent
{
    NetworkServerEventType type;
    uint32_t peerId;
    union
    {
        NetworkInputCommand input;
        struct
        {
            bool breakBlock;
            bool placeBlock;
            float direction[3];
        } editIntent;
        NetworkDisconnectReason disconnectReason;
    } data;
} NetworkServerEvent;

typedef struct NetworkServerConfiguration
{
    uint16_t port;
    uint16_t maximumPeers;
    int64_t worldSeed;
} NetworkServerConfiguration;

// Phase-one transport is deliberately loopback-only. It is useful for the
// split client/server runtime and cannot accidentally expose plaintext TCP to
// a LAN or the Internet. A remote transport must preserve this API and use
// authenticated TLS 1.3 (the planned implementation is MsQuic).
LAIUE_NETWORK_API NetworkClient *NetworkClientCreateLoopback(uint16_t port);
LAIUE_NETWORK_API void NetworkClientDestroy(NetworkClient *client);
LAIUE_NETWORK_API void NetworkClientUpdate(NetworkClient *client);
LAIUE_NETWORK_API NetworkConnectionState NetworkClientGetState(const NetworkClient *client);
LAIUE_NETWORK_API bool NetworkClientPollEvent(NetworkClient *client, NetworkClientEvent *outEvent);
LAIUE_NETWORK_API bool NetworkClientSendInput(NetworkClient *client,
                                              const NetworkInputCommand *input);
LAIUE_NETWORK_API bool NetworkClientSendEditIntent(NetworkClient *client, bool breakBlock,
                                                   bool placeBlock, const float direction[3]);

LAIUE_NETWORK_API NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration);
LAIUE_NETWORK_API void NetworkServerDestroy(NetworkServer *server);
LAIUE_NETWORK_API void NetworkServerUpdate(NetworkServer *server);
LAIUE_NETWORK_API bool NetworkServerPollEvent(NetworkServer *server, NetworkServerEvent *outEvent);
LAIUE_NETWORK_API bool NetworkServerBroadcastPlayerState(NetworkServer *server,
                                                         const NetworkPlayerState *state);
LAIUE_NETWORK_API bool NetworkServerBroadcastBlockDelta(NetworkServer *server,
                                                        const NetworkBlockDelta *delta);
