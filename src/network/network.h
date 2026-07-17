#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_NETWORK_DEFAULT_PORT 27180U
#define LAIUE_NETWORK_MAX_PEERS 16U
#define LAIUE_NETWORK_MAX_MODS 32U
#define LAIUE_NETWORK_MOD_ID_CAPACITY 32U
#define LAIUE_NETWORK_MOD_VERSION_CAPACITY 16U
#define LAIUE_NETWORK_MOD_HASH_SIZE 32U
#define LAIUE_NETWORK_CONTENT_HASH_SIZE 32U
#define LAIUE_NETWORK_MAX_CONTENT_BYTES (256ULL * 1024ULL * 1024ULL)
#define LAIUE_NETWORK_INVENTORY_SLOTS 36U

typedef struct NetworkClient NetworkClient;
typedef struct NetworkServer NetworkServer;

typedef enum NetworkConnectionState
{
    NETWORK_CONNECTION_CONNECTING,
    NETWORK_CONNECTION_NEGOTIATING_MODS,
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

typedef enum NetworkRejectReason
{
    NETWORK_REJECT_NONE = 0,
    NETWORK_REJECT_MOD_MISMATCH = 1,
    NETWORK_REJECT_SERVER_POLICY = 2,
} NetworkRejectReason;

typedef struct NetworkModDescriptor
{
    char id[LAIUE_NETWORK_MOD_ID_CAPACITY];
    char version[LAIUE_NETWORK_MOD_VERSION_CAPACITY];
    uint8_t contentHash[LAIUE_NETWORK_MOD_HASH_SIZE];
} NetworkModDescriptor;

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

typedef struct NetworkBlockDrop
{
    uint32_t id;
    double position[3];
    uint8_t block;
} NetworkBlockDrop;

typedef struct NetworkInventorySlot
{
    uint8_t item;
    uint16_t count;
} NetworkInventorySlot;

typedef struct NetworkInventoryState
{
    uint8_t selectedHotbarSlot;
    NetworkInventorySlot slots[LAIUE_NETWORK_INVENTORY_SLOTS];
} NetworkInventoryState;

typedef enum NetworkClientEventType
{
    NETWORK_CLIENT_EVENT_READY,
    NETWORK_CLIENT_EVENT_SERVER_MODS,
    NETWORK_CLIENT_EVENT_CONTENT_READY,
    NETWORK_CLIENT_EVENT_REJECTED,
    NETWORK_CLIENT_EVENT_PLAYER_STATE,
    NETWORK_CLIENT_EVENT_BLOCK_DELTA,
    NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN,
    NETWORK_CLIENT_EVENT_BLOCK_DROP_REMOVE,
    NETWORK_CLIENT_EVENT_INVENTORY_STATE,
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
        NetworkBlockDrop blockDrop;
        uint32_t removedDropId;
        NetworkInventoryState inventory;
        struct
        {
            uint32_t count;
            bool downloadsAllowed;
        } serverMods;
        NetworkRejectReason rejectReason;
        NetworkDisconnectReason disconnectReason;
    } data;
} NetworkClientEvent;

typedef enum NetworkServerEventType
{
    NETWORK_SERVER_EVENT_CONNECTED,
    NETWORK_SERVER_EVENT_INPUT,
    NETWORK_SERVER_EVENT_EDIT_INTENT,
    NETWORK_SERVER_EVENT_SELECT_HOTBAR_SLOT,
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
            uint8_t placementBlock;
            float direction[3];
        } editIntent;
        uint8_t selectedHotbarSlot;
        NetworkDisconnectReason disconnectReason;
    } data;
} NetworkServerEvent;

typedef struct NetworkServerConfiguration
{
    uint16_t port;
    uint16_t maximumPeers;
    int64_t worldSeed;
    const NetworkModDescriptor *mods;
    uint32_t modCount;
    bool allowContentDownloads;
    const uint8_t *contentBundle;
    uint64_t contentBundleSize;
    uint8_t contentBundleSha256[LAIUE_NETWORK_CONTENT_HASH_SIZE];
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
LAIUE_NETWORK_API bool NetworkClientCopyServerMods(const NetworkClient *client,
                                                   NetworkModDescriptor *output,
                                                   uint32_t capacity,
                                                   uint32_t *outCount);
LAIUE_NETWORK_API bool NetworkClientSubmitMods(NetworkClient *client,
                                               const NetworkModDescriptor *mods,
                                               uint32_t count);
LAIUE_NETWORK_API bool NetworkClientRequestContent(NetworkClient *client);
// Передаёт владение буфером вызывающему; освобождать через HeapFree(GetProcessHeap()).
LAIUE_NETWORK_API bool NetworkClientTakeContent(NetworkClient *client,
                                                uint8_t **outBytes,
                                                uint64_t *outSize);
LAIUE_NETWORK_API bool NetworkClientSendInput(NetworkClient *client,
                                              const NetworkInputCommand *input);
LAIUE_NETWORK_API bool NetworkClientSendEditIntent(NetworkClient *client, bool breakBlock,
                                                   bool placeBlock, uint8_t placementBlock,
                                                   const float direction[3]);
LAIUE_NETWORK_API bool NetworkClientSendSelectedHotbarSlot(NetworkClient* client,
                                                           uint8_t slot);

LAIUE_NETWORK_API NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration);
LAIUE_NETWORK_API void NetworkServerDestroy(NetworkServer *server);
LAIUE_NETWORK_API void NetworkServerUpdate(NetworkServer *server);
LAIUE_NETWORK_API bool NetworkServerPollEvent(NetworkServer *server, NetworkServerEvent *outEvent);
LAIUE_NETWORK_API bool NetworkServerBroadcastPlayerState(NetworkServer *server,
                                                         const NetworkPlayerState *state);
LAIUE_NETWORK_API bool NetworkServerBroadcastBlockDelta(NetworkServer *server,
                                                        const NetworkBlockDelta *delta);
LAIUE_NETWORK_API bool NetworkServerBroadcastBlockDrop(NetworkServer* server,
                                                       const NetworkBlockDrop* drop);
LAIUE_NETWORK_API bool NetworkServerBroadcastDropRemove(NetworkServer* server,
                                                        uint32_t dropId);
LAIUE_NETWORK_API bool NetworkServerSendInventory(NetworkServer* server,
                                                  uint32_t peerId,
                                                  const NetworkInventoryState* inventory);
