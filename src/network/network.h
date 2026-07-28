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
#define LAIUE_NETWORK_HOST_CAPACITY 256U
#define LAIUE_NETWORK_CERTIFICATE_PIN_SIZE 32U
#define LAIUE_NETWORK_ALPN "laiue/5"
#define LAIUE_NETWORK_MAX_SNAPSHOT_CHUNKS 4096U
#define LAIUE_NETWORK_MAX_CHUNK_EDITS 128U
#define LAIUE_NETWORK_MAX_CHUNK_PARTS 2048U
#define LAIUE_NETWORK_MAX_CONTROL_STREAMS 1U
#define LAIUE_NETWORK_MAX_SERVER_STREAMS 2U
#define LAIUE_NETWORK_MAX_QUEUED_EVENTS 256U
#define LAIUE_NETWORK_MAX_OUTSTANDING_SENDS 256U

typedef struct NetworkClient NetworkClient;
typedef struct NetworkServer NetworkServer;

typedef enum NetworkAddressFamily
{
    // Client: resolve both families. Server: the zero/default value means
    // dual-stack, so a zero-initialized server configuration remains useful.
    NETWORK_ADDRESS_FAMILY_AUTO = 0,
    NETWORK_ADDRESS_FAMILY_IPV4 = 1,
    NETWORK_ADDRESS_FAMILY_IPV6 = 2,
    NETWORK_ADDRESS_FAMILY_DUAL = 3
} NetworkAddressFamily;

typedef enum NetworkEndpointKind
{
    NETWORK_ENDPOINT_DNS = 0,
    NETWORK_ENDPOINT_IPV4 = 1,
    NETWORK_ENDPOINT_IPV6 = 2,
    NETWORK_ENDPOINT_WILDCARD = 3
} NetworkEndpointKind;

typedef struct NetworkEndpoint
{
    char host[LAIUE_NETWORK_HOST_CAPACITY];
    uint16_t port;
    NetworkEndpointKind kind;
} NetworkEndpoint;

typedef enum NetworkEndpointParseResult
{
    NETWORK_ENDPOINT_PARSE_OK = 0,
    NETWORK_ENDPOINT_PARSE_NULL,
    NETWORK_ENDPOINT_PARSE_EMPTY,
    NETWORK_ENDPOINT_PARSE_TOO_LONG,
    NETWORK_ENDPOINT_PARSE_INVALID_HOST,
    NETWORK_ENDPOINT_PARSE_INVALID_IPV4,
    NETWORK_ENDPOINT_PARSE_INVALID_IPV6,
    NETWORK_ENDPOINT_PARSE_INVALID_PORT,
    NETWORK_ENDPOINT_PARSE_AMBIGUOUS,
    NETWORK_ENDPOINT_PARSE_FAMILY_MISMATCH
} NetworkEndpointParseResult;

typedef enum NetworkTrustMode
{
    NETWORK_TRUST_SYSTEM = 0,
    NETWORK_TRUST_SHA256 = 1
} NetworkTrustMode;

typedef enum NetworkSecureTransportStatus
{
    NETWORK_SECURE_TRANSPORT_UNAVAILABLE = 0,
    NETWORK_SECURE_TRANSPORT_AVAILABLE = 1,
    NETWORK_SECURE_TRANSPORT_INITIALIZATION_FAILED = 2
} NetworkSecureTransportStatus;

typedef enum NetworkConnectionState
{
    NETWORK_CONNECTION_CONNECTING = 0,
    NETWORK_CONNECTION_VERIFYING_SERVER = 1,
    NETWORK_CONNECTION_NEGOTIATING = 2,
    // Source-compatible name retained while callers migrate to the v5 state
    // name. Both values intentionally describe the same phase.
    NETWORK_CONNECTION_NEGOTIATING_MODS = NETWORK_CONNECTION_NEGOTIATING,
    NETWORK_CONNECTION_SYNCING_WORLD = 3,
    NETWORK_CONNECTION_READY = 4,
    NETWORK_CONNECTION_DISCONNECTED = 5
} NetworkConnectionState;

typedef enum NetworkDisconnectReason
{
    NETWORK_DISCONNECT_NONE,
    NETWORK_DISCONNECT_REMOTE,
    NETWORK_DISCONNECT_IO,
    NETWORK_DISCONNECT_PROTOCOL,
    NETWORK_DISCONNECT_OVERFLOW,
    NETWORK_DISCONNECT_TIMEOUT,
    NETWORK_DISCONNECT_DNS,
    NETWORK_DISCONNECT_TLS,
    NETWORK_DISCONNECT_CERTIFICATE,
    NETWORK_DISCONNECT_VERSION,
    NETWORK_DISCONNECT_CONFIGURATION
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
    uint64_t revision;
    int64_t block[3];
    uint8_t replacement;
} NetworkBlockDelta;

typedef struct NetworkChunkEdit
{
    uint8_t localX;
    uint8_t localY;
    uint8_t localZ;
    uint8_t replacement;
} NetworkChunkEdit;

typedef struct NetworkChunkDelta
{
    int64_t chunk[3];
    uint64_t revision;
    uint16_t partIndex;
    uint16_t partCount;
    uint16_t editCount;
    NetworkChunkEdit edits[LAIUE_NETWORK_MAX_CHUNK_EDITS];
} NetworkChunkDelta;

typedef struct NetworkSnapshotInfo
{
    uint64_t snapshotId;
    uint64_t worldRevision;
    uint32_t serverTick;
    uint32_t chunkCount;
    uint32_t peerId;
    int64_t worldSeed;
    uint64_t worldTime;
} NetworkSnapshotInfo;

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
    NETWORK_CLIENT_EVENT_SERVER_VERIFIED,
    NETWORK_CLIENT_EVENT_READY,
    NETWORK_CLIENT_EVENT_SERVER_MODS,
    NETWORK_CLIENT_EVENT_CONTENT_READY,
    NETWORK_CLIENT_EVENT_REJECTED,
    NETWORK_CLIENT_EVENT_PLAYER_STATE,
    NETWORK_CLIENT_EVENT_BLOCK_DELTA,
    NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN,
    NETWORK_CLIENT_EVENT_BLOCK_DROP_REMOVE,
    NETWORK_CLIENT_EVENT_INVENTORY_STATE,
    NETWORK_CLIENT_EVENT_SNAPSHOT_BEGIN,
    NETWORK_CLIENT_EVENT_SNAPSHOT_CHUNK,
    NETWORK_CLIENT_EVENT_SNAPSHOT_END,
    NETWORK_CLIENT_EVENT_PLAYER_JOINED,
    NETWORK_CLIENT_EVENT_PLAYER_LEFT,
    NETWORK_CLIENT_EVENT_WORLD_TIME,
    NETWORK_CLIENT_EVENT_CHUNK_RESYNC_CANCELLED,
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
        NetworkSnapshotInfo snapshot;
        NetworkChunkDelta chunkDelta;
        uint32_t peerId;
        uint64_t worldTime;
        struct
        {
            int64_t chunk[3];
            uint64_t expectedRevision;
        } chunkResyncCancelled;
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
    NETWORK_SERVER_EVENT_CHUNK_RESYNC_REQUEST,
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
        struct
        {
            int64_t chunk[3];
            uint64_t expectedRevision;
        } chunkResync;
        NetworkDisconnectReason disconnectReason;
    } data;
} NetworkServerEvent;

typedef struct NetworkClientConfiguration
{
    uint32_t structureSize;
    NetworkEndpoint endpoint;
    NetworkAddressFamily addressFamily;
    NetworkTrustMode trustMode;
    uint8_t certificateSha256[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE];
    uint32_t handshakeTimeoutMs;
    uint32_t idleTimeoutMs;
} NetworkClientConfiguration;

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

    // Secure remote transport fields. NETWORK_ADDRESS_FAMILY_AUTO is treated
    // as DUAL for a server. NULL/"*" listens on the wildcard address.
    uint32_t structureSize;
    NetworkAddressFamily addressFamily;
    const char *listenAddress;
    const char *certificateFile;
    const char *privateKeyFile;
    const char *certificateStoreThumbprint;
    uint32_t handshakeTimeoutMs;
    uint32_t idleTimeoutMs;
} NetworkServerConfiguration;

// Parses host names, IPv4 and IPv6 without performing DNS or network I/O.
// IPv6 with an explicit port must be bracketed. Bare IPv6 uses defaultPort.
LAIUE_NETWORK_API NetworkEndpointParseResult NetworkEndpointParse(
    const char *text, uint16_t defaultPort, NetworkEndpoint *outEndpoint);
// Resolves the server bind policy. address may be NULL or "*" for wildcard.
// AUTO is normalized to DUAL. DNS names and explicit ports are rejected.
LAIUE_NETWORK_API NetworkEndpointParseResult NetworkListenEndpointParse(
    const char *address, NetworkAddressFamily family, uint16_t port,
    NetworkEndpoint *outEndpoint);
LAIUE_NETWORK_API bool NetworkTrustParse(
    const char *text, NetworkTrustMode *outMode,
    uint8_t outSha256[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE]);
LAIUE_NETWORK_API void NetworkClientConfigurationInitialize(
    NetworkClientConfiguration *configuration);
LAIUE_NETWORK_API void NetworkServerConfigurationInitialize(
    NetworkServerConfiguration *configuration);
LAIUE_NETWORK_API bool NetworkClientConfigurationIsValid(
    const NetworkClientConfiguration *configuration);
LAIUE_NETWORK_API bool NetworkServerConfigurationIsValid(
    const NetworkServerConfiguration *configuration);
LAIUE_NETWORK_API NetworkSecureTransportStatus NetworkGetSecureTransportStatus(void);

// Production entry points are authenticated QUIC/TLS 1.3 only. They fail
// closed when MsQuic is not compiled in or credentials cannot be validated.
// A client-side resolution/start failure can return a valid client already in
// DISCONNECTED state so callers can poll its precise terminal reason.
LAIUE_NETWORK_API NetworkClient *NetworkClientCreate(
    const NetworkClientConfiguration *configuration);
LAIUE_NETWORK_API NetworkServer *NetworkServerCreate(
    const NetworkServerConfiguration *configuration);

// Temporary compatibility wrappers for the pre-v5 call sites and local
// diagnostics. They are loopback-only and must never accept a remote address.
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
// Transfers ownership to the caller. Release the returned buffer only with
// NetworkContentRelease; the allocator is private to the network module.
LAIUE_NETWORK_API bool NetworkClientTakeContent(NetworkClient *client,
                                                uint8_t **outBytes,
                                                uint64_t *outSize);
// Releases a buffer returned by NetworkClientTakeContent using the allocator
// owned by the network module. NULL is accepted.
LAIUE_NETWORK_API void NetworkContentRelease(void *bytes);
LAIUE_NETWORK_API bool NetworkClientSendInput(NetworkClient *client,
                                              const NetworkInputCommand *input);
LAIUE_NETWORK_API bool NetworkClientSendEditIntent(NetworkClient *client, bool breakBlock,
                                                   bool placeBlock, uint8_t placementBlock,
                                                   const float direction[3]);
LAIUE_NETWORK_API bool NetworkClientSendSelectedHotbarSlot(NetworkClient* client,
                                                           uint8_t slot);
LAIUE_NETWORK_API bool NetworkClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision);
// Confirms that the main thread has applied the complete world/state barrier.
// Input/edit calls remain server-gated until this ordered acknowledgement.
LAIUE_NETWORK_API bool NetworkClientAcknowledgeReady(
    NetworkClient *client);

LAIUE_NETWORK_API NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration);
LAIUE_NETWORK_API void NetworkServerDestroy(NetworkServer *server);
LAIUE_NETWORK_API void NetworkServerUpdate(NetworkServer *server);
LAIUE_NETWORK_API bool NetworkServerPollEvent(NetworkServer *server, NetworkServerEvent *outEvent);
LAIUE_NETWORK_API bool NetworkServerBroadcastPlayerState(NetworkServer *server,
                                                         const NetworkPlayerState *state);
LAIUE_NETWORK_API bool NetworkServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state);
LAIUE_NETWORK_API bool NetworkServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta);
LAIUE_NETWORK_API bool NetworkServerBroadcastBlockDelta(NetworkServer *server,
                                                        const NetworkBlockDelta *delta);
LAIUE_NETWORK_API bool NetworkServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot);
LAIUE_NETWORK_API bool NetworkServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk);
LAIUE_NETWORK_API bool NetworkServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision);
LAIUE_NETWORK_API bool NetworkServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision);
LAIUE_NETWORK_API bool NetworkServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId, uint32_t joinedPeerId);
LAIUE_NETWORK_API bool NetworkServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId);
LAIUE_NETWORK_API bool NetworkServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId, uint32_t leftPeerId);
LAIUE_NETWORK_API bool NetworkServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId);
LAIUE_NETWORK_API bool NetworkServerSendWorldTime(
    NetworkServer *server, uint32_t peerId, uint64_t worldTime);
LAIUE_NETWORK_API bool NetworkServerBroadcastBlockDrop(NetworkServer* server,
                                                       const NetworkBlockDrop* drop);
LAIUE_NETWORK_API bool NetworkServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop);
LAIUE_NETWORK_API bool NetworkServerBroadcastDropRemove(NetworkServer* server,
                                                        uint32_t dropId);
LAIUE_NETWORK_API bool NetworkServerSendInventory(NetworkServer* server,
                                                  uint32_t peerId,
                                                  const NetworkInventoryState* inventory);
