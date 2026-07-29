#include "network/network.h"
#include "network/network_backend.h"

#include <stddef.h>

NetworkSecureTransportStatus NetworkGetSecureTransportStatus(void)
{
    return LaiueNetworkBackendGetStatus();
}

NetworkClient *NetworkClientCreate(
    const NetworkClientConfiguration *configuration)
{
    if (!NetworkClientConfigurationIsValid(configuration))
    {
        return NULL;
    }
    return LaiueNetworkBackendClientCreate(configuration);
}

NetworkClient *NetworkClientCreateLoopback(uint16_t port)
{
    return LaiueNetworkBackendClientCreateLoopback(port);
}

void NetworkClientDestroy(NetworkClient *client)
{
    LaiueNetworkBackendClientDestroy(client);
}

void NetworkClientUpdate(NetworkClient *client)
{
    LaiueNetworkBackendClientUpdate(client);
}

NetworkConnectionState NetworkClientGetState(
    const NetworkClient *client)
{
    return LaiueNetworkBackendClientGetState(client);
}

bool NetworkClientPollEvent(
    NetworkClient *client, NetworkClientEvent *outEvent)
{
    return LaiueNetworkBackendClientPollEvent(client, outEvent);
}

bool NetworkClientCopyServerMods(
    const NetworkClient *client, NetworkModDescriptor *output,
    uint32_t capacity, uint32_t *outCount)
{
    return LaiueNetworkBackendClientCopyServerMods(
        client, output, capacity, outCount);
}

bool NetworkClientSubmitMods(
    NetworkClient *client, const NetworkModDescriptor *mods,
    uint32_t count)
{
    return LaiueNetworkBackendClientSubmitMods(client, mods, count);
}

bool NetworkClientRequestContent(NetworkClient *client)
{
    return LaiueNetworkBackendClientRequestContent(client);
}

bool NetworkClientTakeContent(
    NetworkClient *client, uint8_t **outBytes, uint64_t *outSize)
{
    return LaiueNetworkBackendClientTakeContent(
        client, outBytes, outSize);
}

bool NetworkClientSendInput(
    NetworkClient *client, const NetworkInputCommand *input)
{
    return LaiueNetworkBackendClientSendInput(client, input);
}

bool NetworkClientSendEditIntent(
    NetworkClient *client, bool breakBlock, bool placeBlock,
    uint8_t placementBlock, const float direction[3])
{
    return LaiueNetworkBackendClientSendEditIntent(
        client, breakBlock, placeBlock, placementBlock, direction);
}

bool NetworkClientSendSelectedHotbarSlot(
    NetworkClient *client, uint8_t slot)
{
    return LaiueNetworkBackendClientSendSelectedHotbarSlot(
        client, slot);
}

bool NetworkClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision)
{
    return LaiueNetworkBackendClientRequestChunkResync(
        client, chunk, expectedRevision);
}

bool NetworkClientAcknowledgeReady(NetworkClient *client)
{
    return LaiueNetworkBackendClientAcknowledgeReady(client);
}

NetworkServer *NetworkServerCreate(
    const NetworkServerConfiguration *configuration)
{
    if (!NetworkServerConfigurationIsValid(configuration))
    {
        return NULL;
    }
    return LaiueNetworkBackendServerCreate(configuration);
}

NetworkServer *NetworkServerCreateLoopback(
    const NetworkServerConfiguration *configuration)
{
    return LaiueNetworkBackendServerCreateLoopback(configuration);
}

void NetworkServerDestroy(NetworkServer *server)
{
    LaiueNetworkBackendServerDestroy(server);
}

void NetworkServerUpdate(NetworkServer *server)
{
    LaiueNetworkBackendServerUpdate(server);
}

bool NetworkServerPollEvent(
    NetworkServer *server, NetworkServerEvent *outEvent)
{
    return LaiueNetworkBackendServerPollEvent(server, outEvent);
}

bool NetworkServerDisconnect(
    NetworkServer *server, uint32_t peerId,
    NetworkDisconnectReason reason)
{
    return LaiueNetworkBackendServerDisconnect(
        server, peerId, reason);
}

bool NetworkServerBroadcastPlayerState(
    NetworkServer *server, const NetworkPlayerState *state)
{
    return LaiueNetworkBackendServerBroadcastPlayerState(
        server, state);
}

bool NetworkServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state)
{
    return LaiueNetworkBackendServerSendPlayerState(
        server, peerId, state);
}

bool NetworkServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta)
{
    return LaiueNetworkBackendServerSendBlockDelta(
        server, peerId, delta);
}

bool NetworkServerBroadcastBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta)
{
    return LaiueNetworkBackendServerBroadcastBlockDelta(
        server, delta);
}

bool NetworkServerCanBeginSnapshot(
    NetworkServer *server, uint32_t peerId,
    bool requiresReadyBarrier)
{
    return LaiueNetworkBackendServerCanBeginSnapshot(
        server, peerId, requiresReadyBarrier);
}

bool NetworkServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot)
{
    return LaiueNetworkBackendServerSendSnapshotBegin(
        server, peerId, snapshot);
}

bool NetworkServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk)
{
    return LaiueNetworkBackendServerSendSnapshotChunk(
        server, peerId, chunk);
}

bool NetworkServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision)
{
    return LaiueNetworkBackendServerSendSnapshotEnd(
        server, peerId, snapshotId, worldRevision);
}

bool NetworkServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision)
{
    return LaiueNetworkBackendServerSendChunkResyncCancelled(
        server, peerId, chunk, expectedRevision);
}

bool NetworkServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId, uint32_t joinedPeerId)
{
    return LaiueNetworkBackendServerSendPlayerJoined(
        server, peerId, joinedPeerId);
}

bool NetworkServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId)
{
    return LaiueNetworkBackendServerBroadcastPlayerJoined(
        server, joinedPeerId);
}

bool NetworkServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId, uint32_t leftPeerId)
{
    return LaiueNetworkBackendServerSendPlayerLeft(
        server, peerId, leftPeerId);
}

bool NetworkServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId)
{
    return LaiueNetworkBackendServerBroadcastPlayerLeft(
        server, leftPeerId);
}

bool NetworkServerSendWorldTime(
    NetworkServer *server, uint32_t peerId, uint64_t worldTime)
{
    return LaiueNetworkBackendServerSendWorldTime(
        server, peerId, worldTime);
}

bool NetworkServerBroadcastBlockDrop(
    NetworkServer *server, const NetworkBlockDrop *drop)
{
    return LaiueNetworkBackendServerBroadcastBlockDrop(
        server, drop);
}

bool NetworkServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop)
{
    return LaiueNetworkBackendServerSendBlockDrop(
        server, peerId, drop);
}

bool NetworkServerBroadcastDropRemove(
    NetworkServer *server, uint32_t dropId)
{
    return LaiueNetworkBackendServerBroadcastDropRemove(
        server, dropId);
}

bool NetworkServerSendInventory(
    NetworkServer *server, uint32_t peerId,
    const NetworkInventoryState *inventory)
{
    return LaiueNetworkBackendServerSendInventory(
        server, peerId, inventory);
}
