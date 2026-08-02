#pragma once

#include "network/network.h"

// Internal transport contract. Exactly one backend provides these symbols in
// each build; network_secure.c is the sole owner of the public Network* ABI.
// Keeping the opaque client/server handles unchanged makes backend selection a
// compile-time implementation detail without adding allocations or indirection.

NetworkSecureTransportStatus LaiueNetworkBackendGetStatus(void);

NetworkClient *LaiueNetworkBackendClientCreate(
    const NetworkClientConfiguration *configuration);
NetworkClient *LaiueNetworkBackendClientCreateLoopback(uint16_t port);
void LaiueNetworkBackendClientDestroy(NetworkClient *client);
void LaiueNetworkBackendClientUpdate(NetworkClient *client);
NetworkConnectionState LaiueNetworkBackendClientGetState(
    const NetworkClient *client);
bool LaiueNetworkBackendClientPollEvent(
    NetworkClient *client, NetworkClientEvent *outEvent);
bool LaiueNetworkBackendClientCopyServerMods(
    const NetworkClient *client, NetworkModDescriptor *output,
    uint32_t capacity, uint32_t *outCount);
bool LaiueNetworkBackendClientSubmitMods(
    NetworkClient *client, const NetworkModDescriptor *mods,
    uint32_t count);
bool LaiueNetworkBackendClientRequestContent(NetworkClient *client);
bool LaiueNetworkBackendClientTakeContent(
    NetworkClient *client, uint8_t **outBytes, uint64_t *outSize);
bool LaiueNetworkBackendClientSendInput(
    NetworkClient *client, const NetworkInputCommand *input);
bool LaiueNetworkBackendClientSendEditIntent(
    NetworkClient *client, bool breakBlock, bool placeBlock,
    uint8_t placementBlock, const float direction[3]);
bool LaiueNetworkBackendClientSendSelectedHotbarSlot(
    NetworkClient *client, uint8_t slot);
bool LaiueNetworkBackendClientRequestChunkResync(
    NetworkClient *client, const int64_t chunk[3],
    uint64_t expectedRevision);
bool LaiueNetworkBackendClientAcknowledgeReady(NetworkClient *client);

NetworkServer *LaiueNetworkBackendServerCreate(
    const NetworkServerConfiguration *configuration);
NetworkServer *LaiueNetworkBackendServerCreateLoopback(
    const NetworkServerConfiguration *configuration);
void LaiueNetworkBackendServerDestroy(NetworkServer *server);
void LaiueNetworkBackendServerUpdate(NetworkServer *server);
bool LaiueNetworkBackendServerPollEvent(
    NetworkServer *server, NetworkServerEvent *outEvent);
bool LaiueNetworkBackendServerDisconnect(
    NetworkServer *server, uint32_t peerId,
    NetworkDisconnectReason reason);
bool LaiueNetworkBackendServerBroadcastPlayerState(
    NetworkServer *server, const NetworkPlayerState *state);
bool LaiueNetworkBackendServerSendPlayerState(
    NetworkServer *server, uint32_t peerId,
    const NetworkPlayerState *state);
bool LaiueNetworkBackendServerSendBlockDelta(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDelta *delta);
bool LaiueNetworkBackendServerBroadcastBlockDelta(
    NetworkServer *server, const NetworkBlockDelta *delta);
bool LaiueNetworkBackendServerCanBeginSnapshot(
    NetworkServer *server, uint32_t peerId,
    bool requiresReadyBarrier);
bool LaiueNetworkBackendServerSendSnapshotBegin(
    NetworkServer *server, uint32_t peerId,
    const NetworkSnapshotInfo *snapshot);
bool LaiueNetworkBackendServerSendSnapshotChunk(
    NetworkServer *server, uint32_t peerId,
    const NetworkChunkDelta *chunk);
bool LaiueNetworkBackendServerSendConstructReset(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructReset *reset);
bool LaiueNetworkBackendServerSendConstructBody(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructBody *body);
bool LaiueNetworkBackendServerSendConstructBlocks(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructBlockBatch *blocks);
bool LaiueNetworkBackendServerSendConstructState(
    NetworkServer *server, uint32_t peerId,
    const NetworkConstructState *state);
bool LaiueNetworkBackendServerSendSnapshotEnd(
    NetworkServer *server, uint32_t peerId,
    uint64_t snapshotId, uint64_t worldRevision);
bool LaiueNetworkBackendServerSendChunkResyncCancelled(
    NetworkServer *server, uint32_t peerId,
    const int64_t chunk[3], uint64_t expectedRevision);
bool LaiueNetworkBackendServerSendPlayerJoined(
    NetworkServer *server, uint32_t peerId, uint32_t joinedPeerId);
bool LaiueNetworkBackendServerBroadcastPlayerJoined(
    NetworkServer *server, uint32_t joinedPeerId);
bool LaiueNetworkBackendServerSendPlayerLeft(
    NetworkServer *server, uint32_t peerId, uint32_t leftPeerId);
bool LaiueNetworkBackendServerBroadcastPlayerLeft(
    NetworkServer *server, uint32_t leftPeerId);
bool LaiueNetworkBackendServerSendWorldTime(
    NetworkServer *server, uint32_t peerId, uint64_t worldTime);
bool LaiueNetworkBackendServerBroadcastBlockDrop(
    NetworkServer *server, const NetworkBlockDrop *drop);
bool LaiueNetworkBackendServerSendBlockDrop(
    NetworkServer *server, uint32_t peerId,
    const NetworkBlockDrop *drop);
bool LaiueNetworkBackendServerBroadcastDropRemove(
    NetworkServer *server, uint32_t dropId);
bool LaiueNetworkBackendServerSendInventory(
    NetworkServer *server, uint32_t peerId,
    const NetworkInventoryState *inventory);
