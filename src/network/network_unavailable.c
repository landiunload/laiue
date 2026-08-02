#include "network/network.h"
#include "network/network_backend.h"

#include <stddef.h>

// Configurations without MsQuic remain linkable, but every runtime entry
// point fails closed on every platform. The legacy plaintext implementation
// is intentionally not selected by CMake.

NetworkSecureTransportStatus LaiueNetworkBackendGetStatus(void)
{
    return NETWORK_SECURE_TRANSPORT_UNAVAILABLE;
}

NetworkClient *LaiueNetworkBackendClientCreate(
    const NetworkClientConfiguration *configuration)
{
    (void)configuration;
    return NULL;
}

NetworkClient *LaiueNetworkBackendClientCreateLoopback(uint16_t port)
{
    (void)port;
    return NULL;
}

void LaiueNetworkBackendClientDestroy(NetworkClient *client)
{
    (void)client;
}

void LaiueNetworkBackendClientUpdate(NetworkClient *client)
{
    (void)client;
}

NetworkConnectionState LaiueNetworkBackendClientGetState(
    const NetworkClient *client)
{
    (void)client;
    return NETWORK_CONNECTION_DISCONNECTED;
}

bool LaiueNetworkBackendClientPollEvent(
    NetworkClient *client, NetworkClientEvent *outEvent)
{
    (void)client;
    (void)outEvent;
    return false;
}

bool LaiueNetworkBackendClientCopyServerMods(
    const NetworkClient *client, NetworkModDescriptor *output,
    uint32_t capacity, uint32_t *outCount)
{
    (void)client;
    (void)output;
    (void)capacity;
    if (outCount != NULL)
    {
        *outCount = 0;
    }
    return false;
}

bool LaiueNetworkBackendClientSubmitMods(
    NetworkClient *client, const NetworkModDescriptor *mods, uint32_t count)
{
    (void)client;
    (void)mods;
    (void)count;
    return false;
}

bool LaiueNetworkBackendClientRequestContent(NetworkClient *client)
{
    (void)client;
    return false;
}

bool LaiueNetworkBackendClientTakeContent(
    NetworkClient *client, uint8_t **outBytes, uint64_t *outSize)
{
    (void)client;
    if (outBytes != NULL)
    {
        *outBytes = NULL;
    }
    if (outSize != NULL)
    {
        *outSize = 0;
    }
    return false;
}

bool LaiueNetworkBackendClientSendInput(
    NetworkClient *client, const NetworkInputCommand *input)
{
    (void)client;
    (void)input;
    return false;
}

bool LaiueNetworkBackendClientSendEditIntent(
    NetworkClient *client, bool breakBlock, bool placeBlock,
    uint8_t placementBlock, const float direction[3])
{
    (void)client;
    (void)breakBlock;
    (void)placeBlock;
    (void)placementBlock;
    (void)direction;
    return false;
}

bool LaiueNetworkBackendClientSendSelectedHotbarSlot(
    NetworkClient *client, uint8_t slot)
{
    (void)client;
    (void)slot;
    return false;
}

bool LaiueNetworkBackendClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision)
{
    (void)client;
    (void)chunk;
    (void)expectedRevision;
    return false;
}

bool LaiueNetworkBackendClientAcknowledgeReady(NetworkClient *client)
{
    (void)client;
    return false;
}

NetworkServer *LaiueNetworkBackendServerCreate(
    const NetworkServerConfiguration *configuration)
{
    (void)configuration;
    return NULL;
}

NetworkServer *LaiueNetworkBackendServerCreateLoopback(
    const NetworkServerConfiguration *configuration)
{
    (void)configuration;
    return NULL;
}

void LaiueNetworkBackendServerDestroy(NetworkServer *server)
{
    (void)server;
}

void LaiueNetworkBackendServerUpdate(NetworkServer *server)
{
    (void)server;
}

bool LaiueNetworkBackendServerPollEvent(
    NetworkServer *server, NetworkServerEvent *outEvent)
{
    (void)server;
    (void)outEvent;
    return false;
}

bool LaiueNetworkBackendServerDisconnect(
    NetworkServer *server, uint32_t peerId,
    NetworkDisconnectReason reason)
{
    (void)server;
    (void)peerId;
    (void)reason;
    return false;
}

bool LaiueNetworkBackendServerBroadcastPlayerState(
    NetworkServer *server, const NetworkPlayerState *state)
{
    (void)server;
    (void)state;
    return false;
}

bool LaiueNetworkBackendServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state)
{
    (void)server;
    (void)peerId;
    (void)state;
    return false;
}

bool LaiueNetworkBackendServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta)
{
    (void)server;
    (void)peerId;
    (void)delta;
    return false;
}

bool LaiueNetworkBackendServerBroadcastBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta)
{
    (void)server;
    (void)delta;
    return false;
}

bool LaiueNetworkBackendServerCanBeginSnapshot(
    NetworkServer *server, uint32_t peerId,
    bool requiresReadyBarrier)
{
    (void)server;
    (void)peerId;
    (void)requiresReadyBarrier;
    return false;
}

bool LaiueNetworkBackendServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot)
{
    (void)server;
    (void)peerId;
    (void)snapshot;
    return false;
}

bool LaiueNetworkBackendServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk)
{
    (void)server;
    (void)peerId;
    (void)chunk;
    return false;
}

bool LaiueNetworkBackendServerSendConstructReset(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructReset *reset)
{
    (void)server;
    (void)peerId;
    (void)reset;
    return false;
}

bool LaiueNetworkBackendServerSendConstructBody(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructBody *body)
{
    (void)server;
    (void)peerId;
    (void)body;
    return false;
}

bool LaiueNetworkBackendServerSendConstructBlocks(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructBlockBatch *blocks)
{
    (void)server;
    (void)peerId;
    (void)blocks;
    return false;
}

bool LaiueNetworkBackendServerSendConstructState(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructState *state)
{
    (void)server;
    (void)peerId;
    (void)state;
    return false;
}

bool LaiueNetworkBackendServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision)
{
    (void)server;
    (void)peerId;
    (void)snapshotId;
    (void)worldRevision;
    return false;
}

bool LaiueNetworkBackendServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision)
{
    (void)server;
    (void)peerId;
    (void)chunk;
    (void)expectedRevision;
    return false;
}

bool LaiueNetworkBackendServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId,
    uint32_t joinedPeerId)
{
    (void)server;
    (void)peerId;
    (void)joinedPeerId;
    return false;
}

bool LaiueNetworkBackendServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId)
{
    (void)server;
    (void)joinedPeerId;
    return false;
}

bool LaiueNetworkBackendServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId,
    uint32_t leftPeerId)
{
    (void)server;
    (void)peerId;
    (void)leftPeerId;
    return false;
}

bool LaiueNetworkBackendServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId)
{
    (void)server;
    (void)leftPeerId;
    return false;
}

bool LaiueNetworkBackendServerSendWorldTime(
    NetworkServer *server, uint32_t peerId,
    uint64_t worldTime)
{
    (void)server;
    (void)peerId;
    (void)worldTime;
    return false;
}

bool LaiueNetworkBackendServerBroadcastBlockDrop(
    NetworkServer *server, const NetworkBlockDrop *drop)
{
    (void)server;
    (void)drop;
    return false;
}

bool LaiueNetworkBackendServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop)
{
    (void)server;
    (void)peerId;
    (void)drop;
    return false;
}

bool LaiueNetworkBackendServerBroadcastDropRemove(
    NetworkServer *server, uint32_t dropId)
{
    (void)server;
    (void)dropId;
    return false;
}

bool LaiueNetworkBackendServerSendInventory(
    NetworkServer *server, uint32_t peerId,
    const NetworkInventoryState *inventory)
{
    (void)server;
    (void)peerId;
    (void)inventory;
    return false;
}
