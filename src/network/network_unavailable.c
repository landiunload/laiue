#include "network/network.h"

#include <stddef.h>

// Configurations without MsQuic remain linkable, but every runtime entry
// point fails closed on every platform. The legacy plaintext implementation
// is intentionally not selected by CMake.

NetworkClient *NetworkClientCreateLoopback(uint16_t port)
{
    (void)port;
    return NULL;
}

void NetworkClientDestroy(NetworkClient *client)
{
    (void)client;
}

void NetworkClientUpdate(NetworkClient *client)
{
    (void)client;
}

NetworkConnectionState NetworkClientGetState(const NetworkClient *client)
{
    (void)client;
    return NETWORK_CONNECTION_DISCONNECTED;
}

bool NetworkClientPollEvent(
    NetworkClient *client, NetworkClientEvent *outEvent)
{
    (void)client;
    (void)outEvent;
    return false;
}

bool NetworkClientCopyServerMods(
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

bool NetworkClientSubmitMods(
    NetworkClient *client, const NetworkModDescriptor *mods, uint32_t count)
{
    (void)client;
    (void)mods;
    (void)count;
    return false;
}

bool NetworkClientRequestContent(NetworkClient *client)
{
    (void)client;
    return false;
}

bool NetworkClientTakeContent(
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

bool NetworkClientSendInput(
    NetworkClient *client, const NetworkInputCommand *input)
{
    (void)client;
    (void)input;
    return false;
}

bool NetworkClientSendEditIntent(
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

bool NetworkClientSendSelectedHotbarSlot(
    NetworkClient *client, uint8_t slot)
{
    (void)client;
    (void)slot;
    return false;
}

bool NetworkClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision)
{
    (void)client;
    (void)chunk;
    (void)expectedRevision;
    return false;
}

bool NetworkClientAcknowledgeReady(NetworkClient *client)
{
    (void)client;
    return false;
}

NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration)
{
    (void)configuration;
    return NULL;
}

void NetworkServerDestroy(NetworkServer *server)
{
    (void)server;
}

void NetworkServerUpdate(NetworkServer *server)
{
    (void)server;
}

bool NetworkServerPollEvent(
    NetworkServer *server, NetworkServerEvent *outEvent)
{
    (void)server;
    (void)outEvent;
    return false;
}

bool NetworkServerBroadcastPlayerState(
    NetworkServer *server, const NetworkPlayerState *state)
{
    (void)server;
    (void)state;
    return false;
}

bool NetworkServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state)
{
    (void)server;
    (void)peerId;
    (void)state;
    return false;
}

bool NetworkServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta)
{
    (void)server;
    (void)peerId;
    (void)delta;
    return false;
}

bool NetworkServerBroadcastBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta)
{
    (void)server;
    (void)delta;
    return false;
}

bool NetworkServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot)
{
    (void)server;
    (void)peerId;
    (void)snapshot;
    return false;
}

bool NetworkServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk)
{
    (void)server;
    (void)peerId;
    (void)chunk;
    return false;
}

bool NetworkServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision)
{
    (void)server;
    (void)peerId;
    (void)snapshotId;
    (void)worldRevision;
    return false;
}

bool NetworkServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision)
{
    (void)server;
    (void)peerId;
    (void)chunk;
    (void)expectedRevision;
    return false;
}

bool NetworkServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId,
    uint32_t joinedPeerId)
{
    (void)server;
    (void)peerId;
    (void)joinedPeerId;
    return false;
}

bool NetworkServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId)
{
    (void)server;
    (void)joinedPeerId;
    return false;
}

bool NetworkServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId,
    uint32_t leftPeerId)
{
    (void)server;
    (void)peerId;
    (void)leftPeerId;
    return false;
}

bool NetworkServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId)
{
    (void)server;
    (void)leftPeerId;
    return false;
}

bool NetworkServerSendWorldTime(
    NetworkServer *server, uint32_t peerId,
    uint64_t worldTime)
{
    (void)server;
    (void)peerId;
    (void)worldTime;
    return false;
}

bool NetworkServerBroadcastBlockDrop(
    NetworkServer *server, const NetworkBlockDrop *drop)
{
    (void)server;
    (void)drop;
    return false;
}

bool NetworkServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop)
{
    (void)server;
    (void)peerId;
    (void)drop;
    return false;
}

bool NetworkServerBroadcastDropRemove(
    NetworkServer *server, uint32_t dropId)
{
    (void)server;
    (void)dropId;
    return false;
}

bool NetworkServerSendInventory(
    NetworkServer *server, uint32_t peerId,
    const NetworkInventoryState *inventory)
{
    (void)server;
    (void)peerId;
    (void)inventory;
    return false;
}
