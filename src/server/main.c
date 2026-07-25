#include "game/camera.h"
#include "content/content_bundle.h"
#include "content/content_catalog.h"
#include "gameplay/player_controller.h"
#include "gameplay/inventory.h"
#include "interaction/voxel_interaction.h"
#include "mod/mod_host.h"
#include "mod/mods.h"
#include "network/network.h"
#include "platform/system.h"
#include "server/server_config.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERVER_SPAWN_HEIGHT 100.0
#define SERVER_TICK_SECONDS (1.0 / 60.0)
#define SERVER_SNAPSHOT_TICKS 3U
#define SERVER_MAX_CATCH_UP_TICKS 8U
#define SERVER_INPUT_TIMEOUT_MS 250ULL
#define SERVER_EDIT_COOLDOWN_MS 125ULL
#define SERVER_BREAK_INTENT_TIMEOUT_MS 250ULL
#define SERVER_EDIT_REACH 8.0f
#define SERVER_DROP_CAPACITY 256U
#define SERVER_INTEREST_RADIUS_CHUNKS 5LL

typedef struct ServerBlockDrop
{
    uint32_t id;
    double position[3];
    float age;
    uint8_t block;
    bool active;
} ServerBlockDrop;

static const wchar_t g_serverWorldPath[] = L"saves\\default\\chunks.dat";

typedef struct ServerPlayer
{
    uint32_t peerId;
    Camera camera;
    PlayerController controller;
    PlayerControllerCommand command;
    uint64_t lastInputAtMs;
    uint64_t nextEditAtMs;
    uint64_t breakStartedAtMs;
    uint64_t lastBreakIntentAtMs;
    int64_t breakingBlock[3];
    bool breaking;
    Inventory inventory;
    int64_t snapshotCenter[3];
    bool hasSnapshotCenter;
    bool active;
} ServerPlayer;

typedef struct DedicatedServer
{
    NetworkServer *network;
    World *world;
    PlayerCollisionSource collision;
    ServerPlayer players[LAIUE_NETWORK_MAX_PEERS];
    ModsState mods;
    ModHost modHost;
    ModCompatibilityEntry compatibilityMods[MODS_MAX_ENTRIES];
    NetworkModDescriptor networkMods[LAIUE_NETWORK_MAX_MODS];
    uint32_t networkModCount;
    LaiueContentBundle downloadableContent;
    int64_t worldSeed;
    float timeOfDayHours;
    uint32_t tick;
    ServerBlockDrop drops[SERVER_DROP_CAPACITY];
    uint32_t nextDropSlot;
    uint32_t nextDropId;
    uint64_t nextSnapshotId;
} DedicatedServer;

static NetworkAddressFamily ToNetworkAddressFamily(
    ServerAddressFamily family)
{
    if (family == SERVER_ADDRESS_FAMILY_IPV4)
        return NETWORK_ADDRESS_FAMILY_IPV4;
    if (family == SERVER_ADDRESS_FAMILY_IPV6)
        return NETWORK_ADDRESS_FAMILY_IPV6;
    return NETWORK_ADDRESS_FAMILY_DUAL;
}

static bool ServerCredentialsAreUsable(
    const ServerConfiguration* configuration)
{
#if defined(_WIN32)
    return configuration->certificateThumbprint[0] != '\0';
#else
    uint32_t length = 0;
    while (configuration->privateKeyFile[length] != '\0') ++length;
    wchar_t privateKey[LAIUE_PLATFORM_PATH_CAPACITY];
    return configuration->certificateFile[0] != '\0' && length != 0
        && PlatformUtf8ToWide(configuration->privateKeyFile, length,
            privateKey, LAIUE_PLATFORM_PATH_CAPACITY, NULL)
        && PlatformValidatePrivateKeyFile(privateKey);
#endif
}

static bool CopyBundleSourceName(LaiueContentBundleSource* source,
                                 LaiueContentType type, const wchar_t* name)
{
    uint32_t length = 0;
    while (name[length] != L'\0' && length + 1U < 128U)
    {
        source->name[length] = name[length];
        ++length;
    }
    if (name[length] != L'\0') return false;
    source->name[length] = L'\0';
    source->type = type;
    return true;
}

static bool AppendCatalogSources(LaiueContentBundleSource* sources,
                                 uint32_t* count, LaiueContentType type)
{
    LaiueContentList list;
    if (!LaiueContentEnumerate(type, &list)) return false;
    bool succeeded = true;
    for (uint32_t i = 0; i < list.count && succeeded; ++i)
    {
        if (*count >= LAIUE_CONTENT_BUNDLE_MAX_SOURCES)
        {
            succeeded = false;
            break;
        }
        succeeded = CopyBundleSourceName(&sources[(*count)++],
            type, list.entries[i].name);
    }
    LaiueContentListRelease(&list);
    return succeeded;
}

static bool BuildDownloadableContent(DedicatedServer* server)
{
    LaiueContentBundleSource* sources = PlatformAllocate(
        LAIUE_CONTENT_BUNDLE_MAX_SOURCES * sizeof(*sources), true);
    if (sources == NULL) return false;
    uint32_t count = 0;
    bool succeeded = true;
    for (uint32_t i = 0; i < server->mods.enabledCount && succeeded; ++i)
    {
        const ModEntry* entry =
            &server->mods.entries[server->mods.enabledOrder[i]];
        if (!entry->enabled || !entry->compatible
            || entry->side == MOD_SIDE_CLIENT) continue;
        if (count >= LAIUE_CONTENT_BUNDLE_MAX_SOURCES)
        {
            succeeded = false;
            break;
        }
        succeeded = CopyBundleSourceName(&sources[count++],
            LAIUE_CONTENT_MOD_PACK, entry->fileName);
    }
    if (succeeded)
    {
        succeeded = AppendCatalogSources(sources, &count,
            LAIUE_CONTENT_SHADER_PACK);
    }
    if (succeeded)
    {
        succeeded = AppendCatalogSources(sources, &count,
            LAIUE_CONTENT_TEXTURE_PACK);
    }
    if (succeeded)
    {
        succeeded = LaiueContentBundleBuild(sources, count,
            &server->downloadableContent);
    }
    PlatformFree(sources);
    return succeeded;
}

static double ServerTimeSeconds(void)
{
    return PlatformMonotonicSeconds();
}

static void WriteServerMessage(const wchar_t *message, uint32_t length)
{
    (void)length;
    char utf8[1024];
    if (PlatformWideToUtf8(message, utf8, sizeof(utf8), NULL))
        PlatformWriteConsoleUtf8(utf8);
}

static uint32_t AppendServerText(wchar_t* destination, uint32_t capacity,
                                 uint32_t length, const wchar_t* text)
{
    while (*text != L'\0' && length + 1U < capacity)
    {
        destination[length++] = *text++;
    }
    destination[length] = L'\0';
    return length;
}

static uint32_t AppendServerUnsigned(wchar_t* destination, uint32_t capacity,
                                     uint32_t length, uint32_t value)
{
    wchar_t reversed[10];
    uint32_t count = 0;
    do
    {
        reversed[count++] = (wchar_t)(L'0' + value % 10U);
        value /= 10U;
    }
    while (value != 0 && count < 10U);
    while (count != 0 && length + 1U < capacity)
    {
        destination[length++] = reversed[--count];
    }
    destination[length] = L'\0';
    return length;
}

static void QueryWorldBlockPhysics(void *context, int64_t x, int64_t y, int64_t z,
                                   VoxelBlockPhysics *outBlock)
{
    BlockProperties properties = BlockGetProperties(WorldGetBlock((World *)context, x, y, z));
    outBlock->flags = properties.solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0U;
    outBlock->friction = properties.friction;
}

static ServerPlayer *FindPlayer(DedicatedServer *server, uint32_t peerId)
{
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        if (server->players[index].active && server->players[index].peerId == peerId)
        {
            return &server->players[index];
        }
    }
    return NULL;
}

static ServerPlayer *AddPlayer(DedicatedServer *server, uint32_t peerId)
{
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer *player = &server->players[index];
        if (player->active)
        {
            continue;
        }
        memset(player, 0, sizeof(*player));
        player->active = true;
        player->peerId = peerId;
        player->camera.position[0] = 0.0;
        player->camera.position[1] = 0.0;
        player->camera.position[2] = SERVER_SPAWN_HEIGHT;
        player->camera.yaw = 0.0f;
        player->camera.pitch = -0.4f;
        player->lastInputAtMs = PlatformMonotonicMilliseconds();

        PlayerControllerConfig configuration;
        PlayerControllerGetDefaultConfig(&configuration);
        PlayerControllerInit(&player->controller, &configuration);
        PlayerControllerReset(&player->controller, &player->camera);
        PlayerControllerResolvePenetration(&player->controller, &server->collision,
                                           &player->camera);
        InventoryClear(&player->inventory);
        InventoryAdd(&player->inventory, BLOCK_EARTH, 16U);
        InventoryAdd(&player->inventory, BLOCK_GRASS, 16U);
        return player;
    }
    return NULL;
}

static void RemovePlayer(DedicatedServer *server, uint32_t peerId)
{
    ServerPlayer *player = FindPlayer(server, peerId);
    if (player != NULL)
    {
        memset(player, 0, sizeof(*player));
    }
}

static bool SendPlayerInventory(DedicatedServer* server,
    const ServerPlayer* player)
{
    NetworkInventoryState state;
    memset(&state, 0, sizeof(state));
    state.selectedHotbarSlot = player->inventory.selectedHotbarSlot;
    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT; ++i)
    {
        state.slots[i].item = (uint8_t)player->inventory.slots[i].item;
        state.slots[i].count = player->inventory.slots[i].count;
    }
    return NetworkServerSendInventory(
        server->network, player->peerId, &state);
}

static int64_t ServerPositionChunk(double position)
{
    double scaled = position / (double)CHUNK_SIZE;
    int64_t chunk = (int64_t)scaled;
    if ((double)chunk > scaled) --chunk;
    return chunk;
}

static int64_t ServerBlockChunk(int64_t block)
{
    int64_t chunk = block / CHUNK_SIZE;
    if (block < 0 && block % CHUNK_SIZE != 0) --chunk;
    return chunk;
}

static bool PlayerInterestedInChunk(
    const ServerPlayer* player, const int64_t chunk[3])
{
    if (!player->active || !player->hasSnapshotCenter) return false;
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        uint64_t distance = chunk[axis] >= player->snapshotCenter[axis]
            ? (uint64_t)chunk[axis]
                - (uint64_t)player->snapshotCenter[axis]
            : (uint64_t)player->snapshotCenter[axis]
                - (uint64_t)chunk[axis];
        if (distance > SERVER_INTEREST_RADIUS_CHUNKS) return false;
    }
    return true;
}

static uint64_t ServerWorldTimeMilliseconds(float hours)
{
    while (hours < 0.0f) hours += 24.0f;
    while (hours >= 24.0f) hours -= 24.0f;
    return (uint64_t)(hours * 3600000.0f + 0.5f);
}

static bool SendWorldSnapshot(DedicatedServer* server,
    ServerPlayer* player, const int64_t minimum[3], const int64_t maximum[3])
{
    WorldChunkSummary* summaries = PlatformAllocate(
        LAIUE_NETWORK_MAX_SNAPSHOT_CHUNKS * sizeof(*summaries), false);
    if (summaries == NULL) return false;
    bool truncated = false;
    uint64_t worldRevision = 0;
    uint32_t summaryCount = WorldCopyEditedChunkSummaries(
        server->world, minimum, maximum, summaries,
        LAIUE_NETWORK_MAX_SNAPSHOT_CHUNKS, &truncated, &worldRevision);
    uint32_t messageCount = 0;
    for (uint32_t i = 0; i < summaryCount; ++i)
    {
        uint32_t parts = summaries[i].deltaCount == 0 ? 1U
            : (summaries[i].deltaCount
                + LAIUE_NETWORK_MAX_CHUNK_EDITS - 1U)
                / LAIUE_NETWORK_MAX_CHUNK_EDITS;
        if (parts > LAIUE_NETWORK_MAX_SNAPSHOT_CHUNKS - messageCount)
        {
            truncated = true;
            break;
        }
        messageCount += parts;
    }
    if (truncated)
    {
        PlatformFree(summaries);
        return false;
    }

    uint64_t snapshotId = ++server->nextSnapshotId;
    if (snapshotId == 0) snapshotId = ++server->nextSnapshotId;
    NetworkSnapshotInfo snapshot = {
        .snapshotId = snapshotId,
        .worldRevision = worldRevision,
        .serverTick = server->tick,
        .chunkCount = summaryCount,
        .peerId = player->peerId,
        .worldSeed = server->worldSeed,
        .worldTime = ServerWorldTimeMilliseconds(server->timeOfDayHours),
    };
    bool succeeded = NetworkServerSendSnapshotBegin(
        server->network, player->peerId, &snapshot);
    for (uint32_t i = 0; i < summaryCount && succeeded; ++i)
    {
        uint32_t deltaCount = summaries[i].deltaCount;
        WorldChunkDelta* deltas = deltaCount == 0 ? NULL
            : PlatformAllocate(
                (size_t)deltaCount * sizeof(*deltas), false);
        uint32_t copied = 0;
        uint64_t chunkRevision = 0;
        succeeded = (deltaCount == 0 || deltas != NULL)
            && WorldCopyChunkDeltas(server->world, summaries[i].chunk,
                deltas, deltaCount, &copied, &chunkRevision)
            && copied == deltaCount;
        uint32_t partCount = deltaCount == 0 ? 1U
            : (deltaCount + LAIUE_NETWORK_MAX_CHUNK_EDITS - 1U)
                / LAIUE_NETWORK_MAX_CHUNK_EDITS;
        for (uint32_t partIndex = 0;
            partIndex < partCount && succeeded; ++partIndex)
        {
            uint32_t offset = partIndex * LAIUE_NETWORK_MAX_CHUNK_EDITS;
            uint32_t count = deltaCount - offset;
            if (count > LAIUE_NETWORK_MAX_CHUNK_EDITS)
                count = LAIUE_NETWORK_MAX_CHUNK_EDITS;
            NetworkChunkDelta chunk;
            memset(&chunk, 0, sizeof(chunk));
            memcpy(chunk.chunk, summaries[i].chunk, sizeof(chunk.chunk));
            chunk.revision = chunkRevision;
            chunk.partIndex = (uint16_t)partIndex;
            chunk.partCount = (uint16_t)partCount;
            chunk.editCount = (uint16_t)count;
            for (uint32_t d = 0; d < count; ++d)
            {
                uint32_t localIndex = deltas[offset + d].localIndex;
                chunk.edits[d].localX =
                    (uint8_t)(localIndex / (CHUNK_SIZE * CHUNK_SIZE));
                chunk.edits[d].localY =
                    (uint8_t)((localIndex / CHUNK_SIZE) % CHUNK_SIZE);
                chunk.edits[d].localZ =
                    (uint8_t)(localIndex % CHUNK_SIZE);
                chunk.edits[d].replacement = deltas[offset + d].block;
            }
            succeeded = NetworkServerSendSnapshotChunk(
                server->network, player->peerId, &chunk);
        }
        PlatformFree(deltas);
    }

    // Initial state belongs to the snapshot barrier. READY is emitted by the
    // transport only after SNAPSHOT_END has been received.
    if (succeeded)
        succeeded = NetworkServerSendWorldTime(server->network,
            player->peerId, snapshot.worldTime);
    if (succeeded)
    {
        succeeded = SendPlayerInventory(server, player);
        for (uint32_t i = 0; i < SERVER_DROP_CAPACITY && succeeded; ++i)
        {
            const ServerBlockDrop* drop = &server->drops[i];
            if (!drop->active) continue;
            NetworkBlockDrop networkDrop = {
                .id = drop->id,
                .position = { drop->position[0], drop->position[1],
                    drop->position[2] },
                .block = drop->block,
            };
            succeeded = NetworkServerSendBlockDrop(
                server->network, player->peerId, &networkDrop);
        }
    }
    for (uint32_t i = 0;
        i < LAIUE_NETWORK_MAX_PEERS && succeeded; ++i)
    {
        const ServerPlayer* existing = &server->players[i];
        if (!existing->active) continue;
        NetworkPlayerState state = {
            .serverTick = server->tick,
            .peerId = existing->peerId,
            .position = { existing->camera.position[0],
                existing->camera.position[1], existing->camera.position[2] },
            .yaw = existing->camera.yaw,
            .pitch = existing->camera.pitch,
            .grounded = PlayerControllerIsGrounded(&existing->controller),
        };
        succeeded = NetworkServerSendPlayerJoined(
                server->network, player->peerId, existing->peerId)
            && NetworkServerSendPlayerState(
                server->network, player->peerId, &state);
    }
    if (succeeded)
        succeeded = NetworkServerSendSnapshotEnd(server->network,
            player->peerId, snapshotId, worldRevision);
    PlatformFree(summaries);
    return succeeded;
}

static bool SendInitialWorldSnapshot(
    DedicatedServer* server, ServerPlayer* player)
{
    int64_t center[3] = {
        ServerPositionChunk(player->camera.position[0]),
        ServerPositionChunk(player->camera.position[1]),
        ServerPositionChunk(player->camera.position[2]),
    };
    int64_t minimum[3];
    int64_t maximum[3];
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        minimum[axis] = center[axis] - SERVER_INTEREST_RADIUS_CHUNKS;
        maximum[axis] = center[axis] + SERVER_INTEREST_RADIUS_CHUNKS;
        player->snapshotCenter[axis] = center[axis];
    }
    player->hasSnapshotCenter = true;
    return SendWorldSnapshot(server, player, minimum, maximum);
}

static void SpawnServerDrop(DedicatedServer* server, uint8_t block,
    const int64_t position[3])
{
    ServerBlockDrop* drop = &server->drops[
        server->nextDropSlot++ % SERVER_DROP_CAPACITY];
    if (drop->active)
        NetworkServerBroadcastDropRemove(server->network, drop->id);
    memset(drop, 0, sizeof(*drop));
    drop->active = true;
    drop->block = block;
    drop->id = ++server->nextDropId;
    if (drop->id == 0) drop->id = ++server->nextDropId;
    drop->position[0] = (double)position[0] + 0.5;
    drop->position[1] = (double)position[1] + 0.5;
    drop->position[2] = (double)position[2] + 0.65;
    NetworkBlockDrop networkDrop = {
        .id = drop->id,
        .position = { drop->position[0], drop->position[1],
            drop->position[2] },
        .block = block,
    };
    NetworkServerBroadcastBlockDrop(server->network, &networkDrop);
}

static void HandleEditIntent(DedicatedServer *server, ServerPlayer *player,
                             const NetworkServerEvent *event)
{
    uint64_t now = PlatformMonotonicMilliseconds();
    VoxelBodyShape bodyShape;
    PlayerControllerGetBodyShape(&player->controller, &bodyShape);
    VoxelEdit edit;
    if (!VoxelInteractionTryCreateEdit(server->world, player->camera.position,
                                       event->data.editIntent.direction, player->camera.position,
                                       &bodyShape, event->data.editIntent.breakBlock,
                                       event->data.editIntent.placeBlock,
                                       event->data.editIntent.placementBlock,
                                       SERVER_EDIT_REACH, &edit))
    {
        player->breaking = false;
        return;
    }

    uint8_t previous = (uint8_t)WorldGetBlock(server->world,
        edit.block[0], edit.block[1], edit.block[2]);
    if (edit.type == VOXEL_EDIT_BREAK)
    {
        bool sameTarget = player->breaking
            && player->breakingBlock[0] == edit.block[0]
            && player->breakingBlock[1] == edit.block[1]
            && player->breakingBlock[2] == edit.block[2]
            && now - player->lastBreakIntentAtMs
                <= SERVER_BREAK_INTENT_TIMEOUT_MS;
        if (!sameTarget)
        {
            player->breaking = true;
            player->breakingBlock[0] = edit.block[0];
            player->breakingBlock[1] = edit.block[1];
            player->breakingBlock[2] = edit.block[2];
            player->breakStartedAtMs = now;
            player->lastBreakIntentAtMs = now;
            return;
        }
        player->lastBreakIntentAtMs = now;
        BlockProperties properties = BlockGetProperties(previous);
        uint64_t requiredMs = (uint64_t)(properties.breakSeconds
            * 1000.0f + 0.5f);
        if (now - player->breakStartedAtMs < requiredMs) return;
        player->breaking = false;
    }
    else
    {
        player->breaking = false;
        if (now < player->nextEditAtMs) return;
        const InventorySlot* selected =
            InventorySelectedSlot(&player->inventory);
        if (selected == NULL || selected->count == 0
            || selected->item != event->data.editIntent.placementBlock)
            return;
        player->nextEditAtMs = now + SERVER_EDIT_COOLDOWN_MS;
        if (!InventoryConsumeSelected(&player->inventory, 1U, NULL))
            return;
    }

    WorldSetBlock(server->world, edit.block[0], edit.block[1], edit.block[2], edit.replacement);
    ModHostDispatchBlockEdit(&server->modHost,
        edit.block[0], edit.block[1], edit.block[2],
        previous, (uint8_t)edit.replacement);
    int64_t changedChunk[3] = {
        ServerBlockChunk(edit.block[0]),
        ServerBlockChunk(edit.block[1]),
        ServerBlockChunk(edit.block[2]),
    };
    NetworkBlockDelta delta = {
        .serverTick = server->tick,
        .revision = WorldGetChunkRevision(server->world, changedChunk),
        .block = {edit.block[0], edit.block[1], edit.block[2]},
        .replacement = edit.replacement,
    };
    for (uint32_t i = 0; i < LAIUE_NETWORK_MAX_PEERS; ++i)
    {
        if (PlayerInterestedInChunk(&server->players[i], changedChunk))
        {
            NetworkServerSendBlockDelta(server->network,
                server->players[i].peerId, &delta);
        }
    }
    if (edit.type == VOXEL_EDIT_BREAK)
        SpawnServerDrop(server, previous, edit.block);
    else
        SendPlayerInventory(server, player);
}

static void HandleNetworkEvents(DedicatedServer *server)
{
    NetworkServerEvent event;
    while (NetworkServerPollEvent(server->network, &event))
    {
        if (event.type == NETWORK_SERVER_EVENT_CONNECTED)
        {
            ServerPlayer* connected = AddPlayer(server, event.peerId);
            if (connected != NULL)
            {
                for (uint32_t i = 0; i < LAIUE_NETWORK_MAX_PEERS; ++i)
                {
                    const ServerPlayer* existing = &server->players[i];
                    if (existing->active
                        && existing->peerId != connected->peerId)
                    {
                        NetworkServerSendPlayerJoined(server->network,
                            existing->peerId, connected->peerId);
                    }
                }
                SendInitialWorldSnapshot(server, connected);
            }
            continue;
        }
        if (event.type == NETWORK_SERVER_EVENT_DISCONNECTED)
        {
            NetworkServerBroadcastPlayerLeft(server->network, event.peerId);
            RemovePlayer(server, event.peerId);
            continue;
        }

        ServerPlayer *player = FindPlayer(server, event.peerId);
        if (player == NULL)
        {
            continue;
        }
        if (event.type == NETWORK_SERVER_EVENT_INPUT)
        {
            player->command.movementX = event.data.input.movementX;
            player->command.movementY = event.data.input.movementY;
            player->command.jumpPressed = event.data.input.jumpPressed;
            player->command.jumpHeld = event.data.input.jumpHeld;
            player->command.sprintHeld = event.data.input.sprintHeld;
            player->command.crouchHeld = event.data.input.crouchHeld;
            player->camera.yaw = event.data.input.yaw;
            player->camera.pitch = event.data.input.pitch;
            player->lastInputAtMs = PlatformMonotonicMilliseconds();
        }
        else if (event.type == NETWORK_SERVER_EVENT_EDIT_INTENT)
        {
            HandleEditIntent(server, player, &event);
        }
        else if (event.type == NETWORK_SERVER_EVENT_SELECT_HOTBAR_SLOT)
        {
            InventorySelectHotbar(&player->inventory,
                event.data.selectedHotbarSlot);
            SendPlayerInventory(server, player);
        }
        else if (event.type == NETWORK_SERVER_EVENT_CHUNK_RESYNC_REQUEST)
        {
            int64_t minimum[3];
            int64_t maximum[3];
            memcpy(minimum, event.data.chunkResync.chunk, sizeof(minimum));
            memcpy(maximum, minimum, sizeof(maximum));
            SendWorldSnapshot(server, player, minimum, maximum);
        }
    }
}

static void SimulateTick(DedicatedServer *server)
{
    uint64_t now = PlatformMonotonicMilliseconds();
    server->tick++;
    ModHostDispatchFrame(&server->modHost, (float)SERVER_TICK_SECONDS);
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer *player = &server->players[index];
        if (!player->active)
        {
            continue;
        }
        if (now - player->lastInputAtMs > SERVER_INPUT_TIMEOUT_MS)
        {
            memset(&player->command, 0, sizeof(player->command));
        }
        PlayerControllerUpdate(&player->controller, &server->collision, &player->camera,
                               &player->command, (float)SERVER_TICK_SECONDS);
        player->command.jumpPressed = false;
        if (player->breaking
            && now - player->lastBreakIntentAtMs
                > SERVER_BREAK_INTENT_TIMEOUT_MS)
            player->breaking = false;
    }

    for (uint32_t dropIndex = 0; dropIndex < SERVER_DROP_CAPACITY;
        ++dropIndex)
    {
        ServerBlockDrop* drop = &server->drops[dropIndex];
        if (!drop->active) continue;
        drop->age += (float)SERVER_TICK_SECONDS;
        if (drop->age < 0.30f) continue;
        for (uint32_t playerIndex = 0;
            playerIndex < LAIUE_NETWORK_MAX_PEERS; ++playerIndex)
        {
            ServerPlayer* player = &server->players[playerIndex];
            if (!player->active) continue;
            double dx = player->camera.position[0] - drop->position[0];
            double dy = player->camera.position[1] - drop->position[1];
            double dz = player->camera.position[2] - 0.8
                - drop->position[2];
            if (dx * dx + dy * dy + dz * dz >= 1.7 * 1.7) continue;
            if (InventoryAdd(&player->inventory, drop->block, 1U) != 0)
                continue;
            drop->active = false;
            NetworkServerBroadcastDropRemove(server->network, drop->id);
            SendPlayerInventory(server, player);
            break;
        }
    }

    if (server->tick % SERVER_SNAPSHOT_TICKS != 0)
    {
        return;
    }
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        const ServerPlayer *player = &server->players[index];
        if (!player->active)
        {
            continue;
        }
        NetworkPlayerState state = {
            .serverTick = server->tick,
            .peerId = player->peerId,
            .position = {player->camera.position[0], player->camera.position[1],
                         player->camera.position[2]},
            .yaw = player->camera.yaw,
            .pitch = player->camera.pitch,
            .grounded = PlayerControllerIsGrounded(&player->controller),
        };
        NetworkServerBroadcastPlayerState(server->network, &state);
    }
}

static void RefreshPlayerInterest(DedicatedServer* server)
{
    for (uint32_t i = 0; i < LAIUE_NETWORK_MAX_PEERS; ++i)
    {
        ServerPlayer* player = &server->players[i];
        if (!player->active || !player->hasSnapshotCenter) continue;
        int64_t center[3] = {
            ServerPositionChunk(player->camera.position[0]),
            ServerPositionChunk(player->camera.position[1]),
            ServerPositionChunk(player->camera.position[2]),
        };
        bool changed = false;
        for (uint32_t axis = 0; axis < 3U; ++axis)
            changed = changed
                || center[axis] != player->snapshotCenter[axis];
        if (!changed) continue;
        int64_t minimum[3];
        int64_t maximum[3];
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            minimum[axis] = center[axis] - SERVER_INTEREST_RADIUS_CHUNKS;
            maximum[axis] = center[axis] + SERVER_INTEREST_RADIUS_CHUNKS;
        }
        if (SendWorldSnapshot(server, player, minimum, maximum))
        {
            memcpy(player->snapshotCenter, center,
                sizeof(player->snapshotCenter));
        }
    }
}

static uint32_t RunServer(void)
{
    ServerConfiguration* configuration =
        PlatformAllocate(sizeof(*configuration), false);
    if (configuration == NULL) return 1U;
    ServerConfigurationLoad(configuration);

    World *world = WorldCreate(configuration->worldSeed);
    if (world == NULL)
    {
        PlatformFree(configuration);
        return 1U;
    }
    PlatformCreateDirectory(L"saves");
    PlatformCreateDirectory(L"saves\\default");
    // Для локального split-runtime это тот же мир, который клиент уже
    // загрузил до handshake. После подключения клиент больше его не пишет.
    WorldLoadDeltas(world, g_serverWorldPath);

    DedicatedServer *server = PlatformAllocate(sizeof(*server), true);
    if (server == NULL)
    {
        WorldDestroy(world);
        PlatformFree(configuration);
        return 2U;
    }
    server->world = world;
    server->worldSeed = configuration->worldSeed;
    server->timeOfDayHours = 12.0f;
    server->collision.context = world;
    server->collision.queryBlockPhysics = QueryWorldBlockPhysics;

    ModsInit(&server->mods, L"server_enabled.txt");
    ModsRefresh(&server->mods);
    ModHostBindings modBindings = {
        .world = world,
        .timeOfDayHours = &server->timeOfDayHours,
        .runtimeSide = MOD_SIDE_SERVER,
        .modDataDirectory = L"saves\\default\\moddata",
    };
    PlatformCreateDirectory(L"saves\\default\\moddata");
    if (ModHostInit(&server->modHost, &modBindings))
    {
        ModHostSync(&server->modHost, &server->mods);
    }

    uint32_t compatibilityCount = 0;
    if (!ModsBuildCompatibilitySet(&server->mods,
            server->compatibilityMods, MODS_MAX_ENTRIES,
            &compatibilityCount))
    {
        ModHostShutdown(&server->modHost);
        PlatformFree(server);
        WorldDestroy(world);
        PlatformFree(configuration);
        return 3U;
    }
    server->networkModCount = compatibilityCount;
    for (uint32_t i = 0; i < compatibilityCount; ++i)
    {
        memcpy(server->networkMods[i].id, server->compatibilityMods[i].id,
            sizeof(server->networkMods[i].id));
        memcpy(server->networkMods[i].version,
            server->compatibilityMods[i].version,
            sizeof(server->networkMods[i].version));
        memcpy(server->networkMods[i].contentHash,
            server->compatibilityMods[i].contentHash,
            LAIUE_NETWORK_MOD_HASH_SIZE);
    }
    bool downloadsReady = !configuration->allowContentDownloads
        || BuildDownloadableContent(server);
    if (!downloadsReady)
    {
        ModHostShutdown(&server->modHost);
        PlatformFree(server);
        WorldDestroy(world);
        PlatformFree(configuration);
        return 4U;
    }
    NetworkServerConfiguration networkConfiguration;
    NetworkServerConfigurationInitialize(&networkConfiguration);
    networkConfiguration.port = configuration->port;
    networkConfiguration.maximumPeers = configuration->maximumPeers;
    networkConfiguration.worldSeed = configuration->worldSeed;
    networkConfiguration.mods = server->networkMods;
    networkConfiguration.modCount = server->networkModCount;
    networkConfiguration.allowContentDownloads =
        configuration->allowContentDownloads;
    networkConfiguration.contentBundle = server->downloadableContent.bytes;
    networkConfiguration.contentBundleSize =
        server->downloadableContent.size;
    networkConfiguration.addressFamily =
        ToNetworkAddressFamily(configuration->addressFamily);
    networkConfiguration.listenAddress = configuration->listenAddress;
    networkConfiguration.certificateFile = configuration->certificateFile;
    networkConfiguration.privateKeyFile = configuration->privateKeyFile;
    networkConfiguration.certificateStoreThumbprint =
        configuration->certificateThumbprint;
    memcpy(networkConfiguration.contentBundleSha256,
        server->downloadableContent.sha256,
        LAIUE_NETWORK_CONTENT_HASH_SIZE);
    NetworkServer *network = ServerCredentialsAreUsable(configuration)
        ? NetworkServerCreate(&networkConfiguration) : NULL;
    if (network == NULL)
    {
        LaiueContentBundleRelease(&server->downloadableContent);
        ModHostShutdown(&server->modHost);
        PlatformFree(server);
        WorldDestroy(world);
        PlatformFree(configuration);
        return 5U;
    }
    server->network = network;

    wchar_t bindAddress[SERVER_CONFIG_ADDRESS_CAPACITY + 3U];
    uint32_t bindLength = 0;
    bool wildcard = configuration->listenAddress[0] == '*'
        && configuration->listenAddress[1] == '\0';
    const char* shownAddress = wildcard
        ? (configuration->addressFamily == SERVER_ADDRESS_FAMILY_IPV4
            ? "0.0.0.0" : "::")
        : configuration->listenAddress;
    uint32_t shownLength = 0;
    while (shownAddress[shownLength] != '\0') ++shownLength;
    uint32_t convertedLength = 0;
    if (!PlatformUtf8ToWide(shownAddress, shownLength,
            bindAddress + bindLength,
            (uint32_t)(sizeof(bindAddress) / sizeof(bindAddress[0]))
                - bindLength,
            &convertedLength))
    {
        bindAddress[0] = L'?';
        bindAddress[1] = L'\0';
        bindLength = 1U;
    }
    else
    {
        bindLength += convertedLength;
        if (configuration->addressFamily != SERVER_ADDRESS_FAMILY_IPV4)
        {
            for (uint32_t i = bindLength; i > 0; --i)
                bindAddress[i] = bindAddress[i - 1U];
            bindAddress[0] = L'[';
            ++bindLength;
            bindAddress[bindLength++] = L']';
            bindAddress[bindLength] = L'\0';
        }
    }

    wchar_t readyMessage[384];
    uint32_t readyLength = AppendServerText(readyMessage, 384U, 0,
        L"laiue dedicated server: ");
    readyLength = AppendServerText(
        readyMessage, 384U, readyLength, bindAddress);
    readyLength = AppendServerText(
        readyMessage, 384U, readyLength, L":");
    readyLength = AppendServerUnsigned(readyMessage, 384U,
        readyLength, configuration->port);
    readyLength = AppendServerText(readyMessage, 384U, readyLength,
        configuration->addressFamily == SERVER_ADDRESS_FAMILY_DUAL
            ? L" (UDP, IPv4 + IPv6)\r\ncontent downloads: "
            : configuration->addressFamily == SERVER_ADDRESS_FAMILY_IPV4
                ? L" (UDP, IPv4)\r\ncontent downloads: "
                : L" (UDP, IPv6)\r\ncontent downloads: ");
    readyLength = AppendServerText(readyMessage, 384U, readyLength,
        configuration->allowContentDownloads ? L"enabled" : L"disabled");
    readyLength = AppendServerText(readyMessage, 384U, readyLength,
        L"\r\nCtrl+C to stop.\r\n");
    WriteServerMessage(readyMessage, readyLength);

    PlatformInstallTerminationHandler();
    double previousTime = ServerTimeSeconds();
    double accumulator = 0.0;
    while (!PlatformTerminationRequested())
    {
        double currentTime = ServerTimeSeconds();
        double elapsed = currentTime - previousTime;
        previousTime = currentTime;
        if (elapsed < 0.0)
        {
            elapsed = 0.0;
        }
        else if (elapsed > 0.25)
        {
            elapsed = 0.25;
        }
        accumulator += elapsed;

        NetworkServerUpdate(network);
        HandleNetworkEvents(server);

        uint32_t steps = 0;
        while (accumulator >= SERVER_TICK_SECONDS && steps < SERVER_MAX_CATCH_UP_TICKS)
        {
            SimulateTick(server);
            accumulator -= SERVER_TICK_SECONDS;
            steps++;
        }
        if (steps == SERVER_MAX_CATCH_UP_TICKS)
        {
            accumulator = 0.0;
        }
        if (steps != 0) RefreshPlayerInterest(server);
        PlatformSleepMilliseconds(1U);
    }

    PlatformRemoveTerminationHandler();
    NetworkServerDestroy(network);
    LaiueContentBundleRelease(&server->downloadableContent);
    ModHostShutdown(&server->modHost);
    WorldSaveDeltas(world, g_serverWorldPath);
    WorldDestroy(world);
    PlatformFree(server);
    PlatformFree(configuration);
    return 0U;
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
void __stdcall ServerEntryPoint(void)
{
    ExitProcess(RunServer());
}
#else
int main(void)
{
    return (int)RunServer();
}
#endif
