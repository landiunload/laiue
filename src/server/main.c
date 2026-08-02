#include "game/camera.h"
#include "content/content_bundle.h"
#include "content/content_catalog.h"
#include "construct/physical_construct.h"
#include "construct/physical_construct_persistence.h"
#include "gameplay/player_controller.h"
#include "gameplay/inventory.h"
#include "gameplay/view_direction.h"
#include "interaction/voxel_interaction.h"
#include "mod/mod_host.h"
#include "mod/mods.h"
#include "network/network.h"
#include "network/protocol.h"
#include "platform/system.h"
#include "server/player_input_queue.h"
#include "server/server_config.h"
#include "server/server_tick_clock.h"
#include "server/snapshot_scheduler.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERVER_SPAWN_HEIGHT 100.0
#define SERVER_TICK_RATE 60U
#define SERVER_TICK_SECONDS (1.0 / (double)SERVER_TICK_RATE)
#define SERVER_PHYSICS_SUBSTEPS 4U
#define SERVER_SNAPSHOT_TICKS 3U
#define SERVER_TIME_SYNC_TICKS 60U
#define SERVER_DAY_LENGTH_SECONDS 3600.0
#define SERVER_MAX_CATCH_UP_TICKS 8U
#define SERVER_MAX_BACKLOG_SECONDS \
    ((double)SERVER_PLAYER_INPUT_QUEUE_CAPACITY / (double)SERVER_TICK_RATE)
#define SERVER_EDIT_COOLDOWN_MS 125ULL
#define SERVER_BREAK_INTENT_TIMEOUT_MS 250ULL
#define SERVER_EDIT_REACH 8.0f
#define SERVER_DROP_CAPACITY 128U
#define SERVER_INTEREST_RADIUS_CHUNKS 5LL
#define SERVER_RESYNC_QUEUE_CAPACITY 1U

_Static_assert(LAIUE_PROTOCOL_MAX_INVENTORY_ITEM ==
        INVENTORY_ITEM_PHYSICS_LEVER,
    "protocol v6 and gameplay inventory item ranges must match");
_Static_assert(LAIUE_PROTOCOL_INVENTORY_SLOTS == INVENTORY_SLOT_COUNT,
    "protocol and gameplay inventory slot counts must match");

typedef struct ServerBlockDrop
{
    uint32_t id;
    double position[3];
    float age;
    uint8_t block;
    bool active;
} ServerBlockDrop;

typedef struct ServerResyncRequest
{
    int64_t chunk[3];
    uint64_t expectedRevision;
} ServerResyncRequest;

static const wchar_t g_serverWorldPath[] = L"saves\\default\\chunks.dat";
static const wchar_t g_serverConstructPath[] =
    L"saves/default/constructs.dat";
static const wchar_t g_serverCommitPath[] =
    L"saves/default/state.commit";

typedef struct ServerPlayer
{
    uint32_t peerId;
    Camera camera;
    PlayerController controller;
    PlayerControllerCommand command;
    ServerPlayerInputQueue inputQueue;
    uint32_t lastProcessedInputSequence;
    uint32_t lastInputServerTick;
    uint64_t nextEditAtMs;
    uint64_t breakStartedAtMs;
    uint64_t lastBreakIntentAtMs;
    int64_t breakingBlock[3];
    bool breaking;
    VoxelEditSpace breakingSpace;
    uint64_t breakingBodyId;
    int32_t breakingLocalBlock[3];
    uint64_t grabbedConstructBodyId;
    double grabbedConstructDistance;
    bool constructGrabHeld;
    Inventory inventory;
    int64_t snapshotCenter[3];
    ServerResyncRequest resyncQueue[
        SERVER_RESYNC_QUEUE_CAPACITY];
    uint32_t resyncQueueCount;
    ServerSnapshotScheduler snapshotScheduler;
    bool hasSnapshotCenter;
    bool fullSnapshotPending;
    bool topologySnapshotPending;
    bool active;
} ServerPlayer;

typedef struct DedicatedServer
{
    NetworkServer *network;
    World *world;
    PhysicalConstructSystem *constructs;
    PlayerCollisionSource collision;
    PhysicalConstructCollider constructColliderScratch[
        VOXEL_DYNAMIC_COLLIDER_CAPACITY];
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
    ServerSnapshotWorkBudget snapshotWorkBudget;
    uint32_t nextSnapshotPlayerIndex;
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

// Отказ старта обязан объяснить себя оператору. Под systemd молчаливый
// выход виден только как Restart=on-failure в цикле: журнал пуст, а
// причина (нет сертификата, нет памяти, битый набор модов) неотличима.
static void WriteServerStartupFailure(const wchar_t* reason, uint32_t exitCode)
{
    wchar_t message[256];
    uint32_t length = AppendServerText(message, 256U, 0,
        L"laiue dedicated server: startup failed: ");
    length = AppendServerText(message, 256U, length, reason);
    length = AppendServerText(message, 256U, length, L" (exit ");
    length = AppendServerUnsigned(message, 256U, length, exitCode);
    length = AppendServerText(message, 256U, length, L")\r\n");
    WriteServerMessage(message, length);
}

static void QueryWorldBlockPhysics(void *context, int64_t x, int64_t y, int64_t z,
                                   VoxelBlockPhysics *outBlock)
{
    DedicatedServer* server = context;
    BlockProperties properties = BlockGetProperties(
        WorldGetBlock(server->world, x, y, z));
    outBlock->flags = properties.solid
        ? VOXEL_BLOCK_PHYSICS_SOLID : 0U;
    outBlock->friction = properties.friction;
}

static bool QueryConstructColliders(void* context,
    const VoxelBodyBounds* queryBounds,
    VoxelDynamicCollider* outColliders, uint32_t colliderCapacity,
    uint32_t* outColliderCount)
{
    DedicatedServer* server = context;
    *outColliderCount = 0U;
    if (server->constructs == NULL)
        return true;
    if (queryBounds == NULL || outColliders == NULL ||
        colliderCapacity > VOXEL_DYNAMIC_COLLIDER_CAPACITY)
        return false;

    PhysicalConstructAabb query;
    memcpy(query.minimum, queryBounds->minimum,
        sizeof(query.minimum));
    memcpy(query.maximum, queryBounds->maximum,
        sizeof(query.maximum));
    bool truncated = false;
    uint32_t count = PhysicalConstructCopyCollidersInAabb(
        server->constructs, &query,
        server->constructColliderScratch,
        colliderCapacity, &truncated);
    if (truncated)
        return false;

    for (uint32_t index = 0U; index < count; ++index)
    {
        const PhysicalConstructCollider* source =
            &server->constructColliderScratch[index];
        VoxelDynamicCollider* destination = &outColliders[index];
        memcpy(destination->bounds.minimum, source->bounds.minimum,
            sizeof(destination->bounds.minimum));
        memcpy(destination->bounds.maximum, source->bounds.maximum,
            sizeof(destination->bounds.maximum));
        memcpy(destination->velocity, source->velocity,
            sizeof(destination->velocity));
        destination->friction = 1.0f;
        destination->stableId = (uint64_t)index + 1U;
    }
    *outColliderCount = count;
    return true;
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
        ServerPlayerInputQueueInitialize(&player->inputQueue);
        ServerSnapshotSchedulerInitialize(
            &player->snapshotScheduler, server->tick);
        player->lastInputServerTick = server->tick;

        PlayerControllerConfig configuration;
        PlayerControllerGetDefaultConfig(&configuration);
        PlayerControllerInit(&player->controller, &configuration);
        PlayerControllerReset(&player->controller, &player->camera);
        PlayerControllerResolvePenetration(&player->controller, &server->collision,
                                           &player->camera);
        InventoryClear(&player->inventory);
        InventoryAdd(&player->inventory, BLOCK_EARTH, 16U);
        InventoryAdd(&player->inventory, BLOCK_GRASS, 16U);
        InventoryAdd(&player->inventory,
            INVENTORY_ITEM_PHYSICS_LEVER, 8U);
        return player;
    }
    return NULL;
}

static void RemovePlayer(DedicatedServer *server, uint32_t peerId)
{
    ServerPlayer *player = FindPlayer(server, peerId);
    if (player != NULL)
    {
        PhysicalConstructReleaseOwner(
            server->constructs, peerId);
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

static void BuildNetworkPlayerState(
    const DedicatedServer* server, const ServerPlayer* player,
    NetworkPlayerState* outState)
{
    PlayerControllerState physics;
    PlayerControllerCaptureState(
        &player->controller, &player->camera, &physics);

    memset(outState, 0, sizeof(*outState));
    outState->serverTick = server->tick;
    outState->peerId = player->peerId;
    outState->lastProcessedInputSequence =
        player->lastProcessedInputSequence;
    memcpy(outState->position, physics.position,
        sizeof(outState->position));
    outState->yaw = player->camera.yaw;
    outState->pitch = player->camera.pitch;
    outState->locomotionVelocityX =
        physics.locomotionVelocityX;
    outState->locomotionVelocityY =
        physics.locomotionVelocityY;
    outState->verticalVelocity = physics.verticalVelocity;
    outState->externalVelocityX = physics.externalVelocityX;
    outState->externalVelocityY = physics.externalVelocityY;
    outState->jumpBufferRemaining =
        physics.jumpBufferRemaining;
    outState->coyoteTimeRemaining =
        physics.coyoteTimeRemaining;
    outState->colliderCrouchProgress =
        physics.colliderCrouchProgress;
    outState->eyeCrouchProgress =
        physics.eyeCrouchProgress;
    outState->airJumpsRemaining = physics.airJumpsRemaining;
    outState->crouchingRequested =
        physics.crouchingRequested;
    outState->grounded = physics.grounded;
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

static bool MutateServerModBlock(
    void* context, int64_t x, int64_t y, int64_t z,
    uint8_t block)
{
    DedicatedServer* server = context;
    if (server == NULL || server->world == NULL ||
        block > BLOCK_GRASS)
    {
        return false;
    }

    BlockType requested = (BlockType)block;
    BlockType previous = WorldGetBlock(
        server->world, x, y, z);
    if (previous == requested)
    {
        return true;
    }

    // A mod uses the same authoritative collision rule as a player edit:
    // static world data must never be written through a moving construct.
    if (requested != BLOCK_AIR && server->constructs != NULL &&
        PhysicalConstructWorldCellOccupied(
            server->constructs, x, y, z))
    {
        return false;
    }

    if (!WorldTrySetBlock(server->world, x, y, z, requested))
    {
        return false;
    }

    // Во время ModHostSync transport ещё может не существовать. Такая
    // startup-правка остаётся в authoritative world и попадёт в первый
    // snapshot. После старта сети каждая правка получает текущую revision и
    // рассылается только peers, чей interest window содержит этот chunk.
    if (server->network == NULL)
    {
        return true;
    }

    int64_t changedChunk[3] = {
        ServerBlockChunk(x),
        ServerBlockChunk(y),
        ServerBlockChunk(z),
    };
    NetworkBlockDelta delta = {
        .serverTick = server->tick,
        .revision = WorldGetChunkRevision(
            server->world, changedChunk),
        .block = {x, y, z},
        .replacement = requested,
    };
    for (uint32_t index = 0;
         index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer* player = &server->players[index];
        if (PlayerInterestedInChunk(player, changedChunk))
        {
            // Ошибка отправки изолирует/закрывает конкретного peer внутри
            // transport; уже принятую authoritative mutation не откатываем.
            (void)NetworkServerSendBlockDelta(
                server->network, player->peerId, &delta);
        }
    }
    return true;
}

static uint64_t ServerWorldTimeMilliseconds(float hours)
{
    while (hours < 0.0f) hours += 24.0f;
    while (hours >= 24.0f) hours -= 24.0f;
    return (uint64_t)(hours * 3600000.0f + 0.5f);
}

typedef struct ServerConstructSnapshotScratch
{
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    PhysicalConstructBlock blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
} ServerConstructSnapshotScratch;

static bool ServerConstructLocalFitsWire(const int32_t local[3])
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (local[axis] < INT16_MIN || local[axis] > INT16_MAX)
            return false;
    }
    return true;
}

static bool ServerConstructBlockLess(
    const PhysicalConstructBlock* left,
    const PhysicalConstructBlock* right)
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (left->local[axis] != right->local[axis])
            return left->local[axis] < right->local[axis];
    }
    if (left->kind != right->kind)
        return left->kind < right->kind;
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (left->mountNormal[axis] != right->mountNormal[axis])
            return left->mountNormal[axis] < right->mountNormal[axis];
    }
    return left->material < right->material;
}

static void ServerSortConstructBlocks(
    PhysicalConstructBlock* blocks, uint32_t count)
{
    for (uint32_t index = 1U; index < count; ++index)
    {
        PhysicalConstructBlock value = blocks[index];
        uint32_t insert = index;
        while (insert != 0U &&
            ServerConstructBlockLess(&value, &blocks[insert - 1U]))
        {
            blocks[insert] = blocks[insert - 1U];
            --insert;
        }
        blocks[insert] = value;
    }
}

static bool SendConstructTopologySnapshot(
    DedicatedServer* server, ServerPlayer* player)
{
    ServerConstructSnapshotScratch* scratch = PlatformAllocate(
        sizeof(*scratch), false);
    if (scratch == NULL) return false;

    bool truncated = false;
    uint32_t bodyCount = PhysicalConstructCopyBodyStates(
        server->constructs, scratch->states,
        PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    NetworkConstructReset reset = {
        .bodyCount = (uint16_t)bodyCount,
    };
    bool succeeded = !truncated &&
        bodyCount <= LAIUE_NETWORK_MAX_CONSTRUCT_BODIES &&
        NetworkServerSendConstructReset(
            server->network, player->peerId, &reset);

    for (uint32_t bodyIndex = 0;
        bodyIndex < bodyCount && succeeded; ++bodyIndex)
    {
        const PhysicalConstructBodyState* state =
            &scratch->states[bodyIndex];
        if (state->blockCount == 0U ||
            state->blockCount > PHYSICAL_CONSTRUCT_MAX_BLOCKS ||
            state->blockCount > UINT16_MAX)
        {
            succeeded = false;
            break;
        }
        uint32_t copied = 0U;
        succeeded = PhysicalConstructCopyBlocks(server->constructs,
            state->id, scratch->blocks,
            PHYSICAL_CONSTRUCT_MAX_BLOCKS, &copied) &&
            copied == state->blockCount;
        if (succeeded)
            ServerSortConstructBlocks(scratch->blocks, copied);
        for (uint32_t block = 0; block < copied && succeeded; ++block)
        {
            succeeded = ServerConstructLocalFitsWire(
                scratch->blocks[block].local);
        }
        if (!succeeded) break;

        NetworkConstructBody body = {
            .id = state->id,
            .revision = state->topologyRevision,
            .origin = { state->origin[0], state->origin[1],
                state->origin[2] },
            .velocity = { state->velocity[0], state->velocity[1],
                state->velocity[2] },
            .blockCount = (uint16_t)copied,
        };
        succeeded = NetworkServerSendConstructBody(
            server->network, player->peerId, &body);

        for (uint32_t first = 0U;
            first < copied && succeeded;
            first += LAIUE_NETWORK_MAX_CONSTRUCT_BLOCKS_PER_BATCH)
        {
            uint32_t count = copied - first;
            if (count > LAIUE_NETWORK_MAX_CONSTRUCT_BLOCKS_PER_BATCH)
                count = LAIUE_NETWORK_MAX_CONSTRUCT_BLOCKS_PER_BATCH;
            NetworkConstructBlockBatch batch;
            memset(&batch, 0, sizeof(batch));
            batch.id = state->id;
            batch.revision = state->topologyRevision;
            batch.firstBlock = (uint16_t)first;
            batch.blockCount = (uint16_t)count;
            for (uint32_t index = 0; index < count; ++index)
            {
                const PhysicalConstructBlock* source =
                    &scratch->blocks[first + index];
                NetworkConstructBlock* destination =
                    &batch.blocks[index];
                for (uint32_t axis = 0; axis < 3U; ++axis)
                {
                    destination->local[axis] =
                        (int16_t)source->local[axis];
                    destination->mountNormal[axis] =
                        source->mountNormal[axis];
                }
                destination->material = source->material;
                destination->kind = source->kind;
            }
            succeeded = NetworkServerSendConstructBlocks(
                server->network, player->peerId, &batch);
        }
    }

    PlatformFree(scratch);
    return succeeded;
}

static bool SendWorldSnapshot(DedicatedServer* server,
    ServerPlayer* player, const int64_t minimum[3],
    const int64_t maximum[3], bool includeConstructTopology)
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
    uint32_t maximumDeltaCount = 0;
    for (uint32_t i = 0; i < summaryCount; ++i)
    {
        if (summaries[i].deltaCount > maximumDeltaCount)
            maximumDeltaCount = summaries[i].deltaCount;
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
    WorldChunkDelta* deltas = maximumDeltaCount == 0 ? NULL
        : PlatformAllocate(
            (size_t)maximumDeltaCount * sizeof(*deltas), false);
    if (maximumDeltaCount != 0 && deltas == NULL)
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
        .requiresReadyBarrier = !player->hasSnapshotCenter,
    };
    bool succeeded = NetworkServerSendSnapshotBegin(
        server->network, player->peerId, &snapshot);
    for (uint32_t i = 0; i < summaryCount && succeeded; ++i)
    {
        uint32_t deltaCount = summaries[i].deltaCount;
        uint32_t copied = 0;
        uint64_t chunkRevision = 0;
        succeeded = WorldCopyChunkDeltas(
                server->world, summaries[i].chunk,
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
    }

    // Initial state belongs to the snapshot barrier. READY is emitted by the
    // transport only after SNAPSHOT_END has been received.
    if (succeeded && includeConstructTopology)
        succeeded = SendConstructTopologySnapshot(server, player);
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
        NetworkPlayerState state;
        BuildNetworkPlayerState(server, existing, &state);
        succeeded = NetworkServerSendPlayerJoined(
                server->network, player->peerId, existing->peerId)
            && NetworkServerSendPlayerState(
                server->network, player->peerId, &state);
    }
    if (succeeded)
        succeeded = NetworkServerSendSnapshotEnd(server->network,
            player->peerId, snapshotId, worldRevision);
    PlatformFree(deltas);
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
        for (uint32_t request = 0;
            request < player->resyncQueueCount; ++request)
        {
            int64_t requested =
                player->resyncQueue[request].chunk[axis];
            if (requested < minimum[axis]) minimum[axis] = requested;
            if (requested > maximum[axis]) maximum[axis] = requested;
        }
    }
    bool includeConstructTopology = !player->hasSnapshotCenter ||
        player->topologySnapshotPending;
    if (!SendWorldSnapshot(server, player, minimum, maximum,
            includeConstructTopology))
        return false;
    memcpy(player->snapshotCenter, center,
        sizeof(player->snapshotCenter));
    player->hasSnapshotCenter = true;
    player->resyncQueueCount = 0;
    player->fullSnapshotPending = false;
    if (includeConstructTopology)
        player->topologySnapshotPending = false;
    return true;
}

static bool SendTopologyOnlySnapshot(
    DedicatedServer* server, ServerPlayer* player)
{
    uint64_t snapshotId = ++server->nextSnapshotId;
    if (snapshotId == 0U) snapshotId = ++server->nextSnapshotId;
    uint64_t worldRevision = WorldGetRevision(server->world);
    NetworkSnapshotInfo snapshot = {
        .snapshotId = snapshotId,
        .worldRevision = worldRevision,
        .serverTick = server->tick,
        .chunkCount = 0U,
        .peerId = player->peerId,
        .worldSeed = server->worldSeed,
        .worldTime = ServerWorldTimeMilliseconds(server->timeOfDayHours),
        .requiresReadyBarrier = false,
    };
    return NetworkServerSendSnapshotBegin(
               server->network, player->peerId, &snapshot) &&
           SendConstructTopologySnapshot(server, player) &&
           NetworkServerSendSnapshotEnd(server->network,
               player->peerId, snapshotId, worldRevision);
}

static bool ServerChunkEquals(
    const int64_t left[3], const int64_t right[3])
{
    return left[0] == right[0] &&
           left[1] == right[1] &&
           left[2] == right[2];
}

static void QueueChunkResync(
    ServerPlayer* player, const NetworkServerEvent* event)
{
    for (uint32_t index = 0;
        index < player->resyncQueueCount; ++index)
    {
        ServerResyncRequest* queued =
            &player->resyncQueue[index];
        if (!ServerChunkEquals(
                queued->chunk,
                event->data.chunkResync.chunk))
            continue;
        if (event->data.chunkResync.expectedRevision
            < queued->expectedRevision)
        {
            queued->expectedRevision =
                event->data.chunkResync.expectedRevision;
        }
        ServerSnapshotSchedulerRequest(
            &player->snapshotScheduler);
        return;
    }
    if (player->resyncQueueCount >=
        SERVER_RESYNC_QUEUE_CAPACITY)
    {
        player->fullSnapshotPending = true;
        ServerSnapshotSchedulerRequest(
            &player->snapshotScheduler);
        return;
    }
    ServerResyncRequest* queued =
        &player->resyncQueue[player->resyncQueueCount++];
    memcpy(queued->chunk, event->data.chunkResync.chunk,
        sizeof(queued->chunk));
    queued->expectedRevision =
        event->data.chunkResync.expectedRevision;
    ServerSnapshotSchedulerRequest(
        &player->snapshotScheduler);
}

static void ProcessPendingResyncs(DedicatedServer* server)
{
    if (!ServerSnapshotWorkBudgetAvailable(
            &server->snapshotWorkBudget, server->tick))
    {
        return;
    }

    for (uint32_t offset = 0;
        offset < LAIUE_NETWORK_MAX_PEERS; ++offset)
    {
        uint32_t playerIndex =
            (server->nextSnapshotPlayerIndex + offset) %
            LAIUE_NETWORK_MAX_PEERS;
        ServerPlayer* player = &server->players[playerIndex];
        if (!player->active) continue;
        if (!player->fullSnapshotPending &&
            !player->topologySnapshotPending &&
            player->resyncQueueCount == 0)
        {
            continue;
        }

        bool requiresReadyBarrier =
            !player->hasSnapshotCenter;
        bool transportCanBegin =
            NetworkServerCanBeginSnapshot(
                server->network, player->peerId,
                requiresReadyBarrier);
        if (!ServerSnapshotSchedulerTryBegin(
                &player->snapshotScheduler, server->tick,
                transportCanBegin))
        {
            continue;
        }

        ServerSnapshotWorkBudgetConsume(
            &server->snapshotWorkBudget);
        server->nextSnapshotPlayerIndex =
            (playerIndex + 1U) % LAIUE_NETWORK_MAX_PEERS;
        bool succeeded = false;
        if (player->fullSnapshotPending)
        {
            succeeded =
                SendInitialWorldSnapshot(server, player);
            ServerSnapshotSchedulerFinish(
                &player->snapshotScheduler, server->tick,
                succeeded);
            return;
        }

        if (player->topologySnapshotPending)
        {
            succeeded = SendTopologyOnlySnapshot(server, player);
            if (succeeded)
                player->topologySnapshotPending = false;
            ServerSnapshotSchedulerFinish(
                &player->snapshotScheduler, server->tick, succeeded);
            return;
        }

        int64_t minimum[3];
        int64_t maximum[3];
        memcpy(minimum, player->resyncQueue[0].chunk,
            sizeof(minimum));
        memcpy(maximum, minimum, sizeof(maximum));
        succeeded = SendWorldSnapshot(
            server, player, minimum, maximum, false);
        if (!succeeded)
        {
            ServerSnapshotSchedulerFinish(
                &player->snapshotScheduler, server->tick,
                false);
            return;
        }
        for (uint32_t index = 1;
            index < player->resyncQueueCount; ++index)
        {
            player->resyncQueue[index - 1U] =
                player->resyncQueue[index];
        }
        --player->resyncQueueCount;
        ServerSnapshotSchedulerFinish(
            &player->snapshotScheduler, server->tick, true);
        if (player->resyncQueueCount != 0)
        {
            ServerSnapshotSchedulerRequest(
                &player->snapshotScheduler);
        }
        return;
    }
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

static uint32_t BuildServerPlayerBlockers(
    DedicatedServer* server,
    PhysicalConstructAabb output[LAIUE_NETWORK_MAX_PEERS])
{
    uint32_t count = 0U;
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer* player = &server->players[index];
        if (!player->active) continue;
        VoxelBodyShape shape;
        VoxelBodyBounds bounds;
        PlayerControllerGetBodyShape(&player->controller, &shape);
        VoxelBodyCalculateBounds(player->camera.position, &shape, &bounds);
        memcpy(output[count].minimum, bounds.minimum,
            sizeof(output[count].minimum));
        memcpy(output[count].maximum, bounds.maximum,
            sizeof(output[count].maximum));
        ++count;
    }
    return count;
}

static uint32_t BuildServerSimulationBlockers(
    DedicatedServer* server,
    PhysicalConstructDynamicBlocker
        output[LAIUE_NETWORK_MAX_PEERS])
{
    uint32_t count = 0U;
    for (uint32_t index = 0U;
        index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer* player = &server->players[index];
        if (!player->active)
            continue;
        VoxelBodyShape shape;
        VoxelBodyBounds bounds;
        PlayerControllerGetBodyShape(&player->controller, &shape);
        VoxelBodyCalculateBounds(
            player->camera.position, &shape, &bounds);
        memcpy(output[count].bounds.minimum, bounds.minimum,
            sizeof(output[count].bounds.minimum));
        memcpy(output[count].bounds.maximum, bounds.maximum,
            sizeof(output[count].bounds.maximum));
        output[count].ignoredBodyId = 0U;
        ++count;
    }
    return count;
}

static bool ServerFindConstructState(
    DedicatedServer* server, uint64_t bodyId,
    PhysicalConstructBodyState* outState)
{
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    bool truncated = false;
    uint32_t count = PhysicalConstructCopyBodyStates(
        server->constructs, states, PHYSICAL_CONSTRUCT_MAX_BODIES,
        &truncated);
    if (truncated) return false;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (states[index].id == bodyId)
        {
            *outState = states[index];
            return true;
        }
    }
    return false;
}

static bool SameServerBreakTarget(
    const ServerPlayer* player, const VoxelEdit* edit)
{
    if (!player->breaking || player->breakingSpace != edit->space)
        return false;
    if (edit->space == VOXEL_EDIT_SPACE_CONSTRUCT)
    {
        return player->breakingBodyId == edit->bodyId &&
            player->breakingLocalBlock[0] == edit->localBlock[0] &&
            player->breakingLocalBlock[1] == edit->localBlock[1] &&
            player->breakingLocalBlock[2] == edit->localBlock[2];
    }
    return player->breakingBlock[0] == edit->block[0] &&
        player->breakingBlock[1] == edit->block[1] &&
        player->breakingBlock[2] == edit->block[2];
}

static void RequestConstructTopologySnapshots(DedicatedServer* server)
{
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer* player = &server->players[index];
        if (!player->active) continue;
        player->topologySnapshotPending = true;
        ServerSnapshotSchedulerRequest(&player->snapshotScheduler);
    }
}

static void RequestConstructActivationSnapshots(DedicatedServer* server)
{
    // Activation changes two authoritative domains atomically: captured
    // voxels disappear from World and reappear as construct topology. A
    // topology-only snapshot would leave remote clients with duplicate
    // static blocks, so schedule one live interest snapshot containing both.
    for (uint32_t index = 0U;
        index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer* player = &server->players[index];
        if (!player->active)
            continue;
        player->fullSnapshotPending = true;
        player->topologySnapshotPending = true;
        ServerSnapshotSchedulerRequest(&player->snapshotScheduler);
    }
}

static void HandleEditIntent(DedicatedServer *server, ServerPlayer *player,
                             const NetworkServerEvent *event)
{
    uint64_t now = PlatformMonotonicMilliseconds();
    PhysicalConstructAabb blockers[LAIUE_NETWORK_MAX_PEERS];
    uint32_t blockerCount = BuildServerPlayerBlockers(server, blockers);
    VoxelEdit edit;
    if (!VoxelInteractionTryCreateEdit(server->world, server->constructs,
            player->camera.position, event->data.editIntent.direction,
            blockers, blockerCount,
            event->data.editIntent.breakBlock,
            event->data.editIntent.placeBlock,
            event->data.editIntent.placementBlock,
            SERVER_EDIT_REACH, &edit))
    {
        player->breaking = false;
        return;
    }

    uint8_t previous = edit.original;
    if (edit.type == VOXEL_EDIT_BREAK)
    {
        bool sameTarget = SameServerBreakTarget(player, &edit) &&
            now - player->lastBreakIntentAtMs <=
                SERVER_BREAK_INTENT_TIMEOUT_MS;
        if (!sameTarget)
        {
            player->breaking = true;
            player->breakingSpace = edit.space;
            player->breakingBlock[0] = edit.block[0];
            player->breakingBlock[1] = edit.block[1];
            player->breakingBlock[2] = edit.block[2];
            player->breakingBodyId = edit.bodyId;
            memcpy(player->breakingLocalBlock, edit.localBlock,
                sizeof(player->breakingLocalBlock));
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
    }

    if (edit.type == VOXEL_EDIT_ACTIVATE_LEVER)
    {
        uint64_t bodyId = 0;
        PhysicalConstructResult result = PhysicalConstructActivateLever(
            server->constructs, edit.block, edit.mountNormal,
            blockers, blockerCount, &bodyId);
        if (result != PHYSICAL_CONSTRUCT_OK) return;
        (void)InventoryConsumeSelected(&player->inventory, 1U, NULL);
        SendPlayerInventory(server, player);
        RequestConstructActivationSnapshots(server);
        return;
    }

    if (edit.space == VOXEL_EDIT_SPACE_CONSTRUCT)
    {
        if (edit.type == VOXEL_EDIT_PLACE)
        {
            PhysicalConstructResult result = PhysicalConstructPlaceBlock(
                server->constructs, edit.bodyId, edit.localBlock,
                edit.replacement, blockers, blockerCount);
            if (result != PHYSICAL_CONSTRUCT_OK) return;
            (void)InventoryConsumeSelected(&player->inventory, 1U, NULL);
            SendPlayerInventory(server, player);
        }
        else
        {
            PhysicalConstructBodyState before;
            if (!ServerFindConstructState(server, edit.bodyId, &before))
                return;
            PhysicalConstructBlock removed;
            uint64_t bodyIds[PHYSICAL_CONSTRUCT_MAX_BODIES];
            uint32_t bodyIdCount = 0;
            PhysicalConstructResult result = PhysicalConstructBreakBlock(
                server->constructs, edit.bodyId, edit.localBlock,
                &removed, bodyIds, PHYSICAL_CONSTRUCT_MAX_BODIES,
                &bodyIdCount);
            if (result != PHYSICAL_CONSTRUCT_OK) return;
            if (removed.kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
            {
                (void)InventoryAdd(&player->inventory,
                    INVENTORY_ITEM_PHYSICS_LEVER, 1U);
                SendPlayerInventory(server, player);
            }
            else
            {
                int64_t dropBlock[3] = {
                    (int64_t)(before.origin[0] + removed.local[0]),
                    (int64_t)(before.origin[1] + removed.local[1]),
                    (int64_t)(before.origin[2] + removed.local[2]),
                };
                SpawnServerDrop(server, removed.material, dropBlock);
            }
        }
        RequestConstructTopologySnapshots(server);
        return;
    }

    if (!WorldTrySetBlock(server->world,
            edit.block[0], edit.block[1], edit.block[2],
            edit.replacement))
        return;
    if (edit.type == VOXEL_EDIT_PLACE)
    {
        (void)InventoryConsumeSelected(&player->inventory, 1U, NULL);
    }
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
                // Snapshot construction is scheduled after the fixed ticks;
                // accepting a peer must not stall every existing player.
                connected->fullSnapshotPending = true;
                ServerSnapshotSchedulerRequest(
                    &connected->snapshotScheduler);
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
            ServerPlayerInputPushResult pushed =
                ServerPlayerInputQueuePush(
                    &player->inputQueue, &event.data.input);
            if (pushed == SERVER_PLAYER_INPUT_OVERFLOW)
            {
                NetworkServerDisconnect(server->network, player->peerId,
                    NETWORK_DISCONNECT_OVERFLOW);
            }
            else if (pushed == SERVER_PLAYER_INPUT_GAP)
            {
                NetworkServerDisconnect(server->network, player->peerId,
                    NETWORK_DISCONNECT_PROTOCOL);
            }
            else if (pushed == SERVER_PLAYER_INPUT_ACCEPTED)
            {
                player->lastInputServerTick = server->tick;
            }
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
            uint64_t authoritativeRevision =
                WorldGetChunkRevision(
                    server->world,
                    event.data.chunkResync.chunk);
            if (!PlayerInterestedInChunk(
                    player, event.data.chunkResync.chunk) ||
                authoritativeRevision <=
                    event.data.chunkResync.expectedRevision)
            {
                NetworkServerSendChunkResyncCancelled(
                    server->network, player->peerId,
                    event.data.chunkResync.chunk,
                    event.data.chunkResync.expectedRevision);
            }
            else
            {
                QueueChunkResync(player, &event);
            }
        }
    }
}

static void ReleaseServerConstructGrab(
    DedicatedServer* server, ServerPlayer* player)
{
    if (player->grabbedConstructBodyId != 0U)
    {
        (void)PhysicalConstructEndGrab(server->constructs,
            player->grabbedConstructBodyId, player->peerId);
    }
    player->grabbedConstructBodyId = 0U;
    player->grabbedConstructDistance = 0.0;
    player->constructGrabHeld = false;
}

static void UpdateServerConstructGrab(
    DedicatedServer* server, ServerPlayer* player)
{
    if (!player->constructGrabHeld)
    {
        ReleaseServerConstructGrab(server, player);
        return;
    }

    float direction[3];
    GameplayViewForward(
        player->camera.yaw, player->camera.pitch, direction);
    if (player->grabbedConstructBodyId == 0U)
    {
        VoxelSceneHit hit;
        if (!VoxelInteractionRaycastScene(server->world,
                server->constructs, player->camera.position,
                direction, SERVER_EDIT_REACH, &hit) ||
            hit.kind != VOXEL_SCENE_HIT_LEVER_HANDLE)
        {
            return;
        }
        player->grabbedConstructDistance = hit.distance;
        double target[3];
        for (uint32_t axis = 0; axis < 3U; ++axis)
        {
            target[axis] = player->camera.position[axis]
                + (double)direction[axis] * hit.distance;
        }
        if (PhysicalConstructBeginGrab(server->constructs,
                hit.bodyId, player->peerId, target) !=
            PHYSICAL_CONSTRUCT_OK)
        {
            player->grabbedConstructDistance = 0.0;
            return;
        }
        player->grabbedConstructBodyId = hit.bodyId;
        return;
    }

    double target[3];
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        target[axis] = player->camera.position[axis]
            + (double)direction[axis]
                * player->grabbedConstructDistance;
    }
    if (!PhysicalConstructUpdateGrab(server->constructs,
            player->grabbedConstructBodyId, player->peerId, target))
    {
        player->grabbedConstructBodyId = 0U;
        player->grabbedConstructDistance = 0.0;
        player->constructGrabHeld = false;
    }
}

static void SendConstructMotionStates(DedicatedServer* server)
{
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    bool truncated = false;
    uint32_t bodyCount = PhysicalConstructCopyBodyStates(
        server->constructs, states, PHYSICAL_CONSTRUCT_MAX_BODIES,
        &truncated);
    if (truncated) return;

    for (uint32_t body = 0; body < bodyCount; ++body)
    {
        NetworkConstructState state = {
            .serverTick = server->tick,
            .id = states[body].id,
            .revision = states[body].topologyRevision,
            .origin = { states[body].origin[0], states[body].origin[1],
                states[body].origin[2] },
            .velocity = { states[body].velocity[0],
                states[body].velocity[1], states[body].velocity[2] },
            .heldBy = states[body].grabbed
                ? states[body].grabOwner : 0U,
        };
        for (uint32_t peer = 0;
            peer < LAIUE_NETWORK_MAX_PEERS; ++peer)
        {
            const ServerPlayer* player = &server->players[peer];
            if (!player->active || !player->hasSnapshotCenter ||
                player->fullSnapshotPending)
            {
                continue;
            }
            (void)NetworkServerSendConstructState(
                server->network, player->peerId, &state);
        }
    }
}

static void SimulateTick(DedicatedServer *server)
{
    uint64_t now = PlatformMonotonicMilliseconds();
    server->tick++;
    server->timeOfDayHours +=
        (float)(SERVER_TICK_SECONDS *
                (24.0 / SERVER_DAY_LENGTH_SECONDS));
    if (server->timeOfDayHours >= 24.0f)
        server->timeOfDayHours -= 24.0f;
    ModHostDispatchFrame(&server->modHost, (float)SERVER_TICK_SECONDS);
    for (uint32_t index = 0; index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer *player = &server->players[index];
        if (!player->active)
        {
            continue;
        }
        NetworkInputCommand input;
        bool consumedInput =
            ServerPlayerInputQueuePop(&player->inputQueue, &input);
        if (consumedInput)
        {
            player->command.movementX = input.movementX;
            player->command.movementY = input.movementY;
            player->command.jumpPressed = input.jumpPressed;
            player->command.jumpHeld = input.jumpHeld;
            player->command.sprintHeld = input.sprintHeld;
            player->command.crouchHeld = input.crouchHeld;
            player->constructGrabHeld = input.useHeld;
            player->camera.yaw = input.yaw;
            player->camera.pitch = input.pitch;
            player->lastProcessedInputSequence = input.sequence;
        }
        else
        {
            // Held state may span ticks, but a press edge is consumed once.
            player->command.jumpPressed = false;
        }
        if (!consumedInput && ServerPlayerInputTimedOut(
                server->tick, player->lastInputServerTick))
        {
            memset(&player->command, 0, sizeof(player->command));
            ReleaseServerConstructGrab(server, player);
        }
        UpdateServerConstructGrab(server, player);
    }

    // Both solvers use the same 240 Hz phase ordering. Each construct step
    // sees current player AABBs; then every player sees the exact new
    // fractional construct pose. This avoids a four-substep platform lead.
    for (uint32_t substep = 0U;
        substep < SERVER_PHYSICS_SUBSTEPS; ++substep)
    {
        PhysicalConstructDynamicBlocker
            blockers[LAIUE_NETWORK_MAX_PEERS];
        uint32_t blockerCount = BuildServerSimulationBlockers(
            server, blockers);
        (void)PhysicalConstructStepWithBlockers(
            server->constructs, blockers, blockerCount);

        for (uint32_t index = 0U;
            index < LAIUE_NETWORK_MAX_PEERS; ++index)
        {
            ServerPlayer *player = &server->players[index];
            if (!player->active)
                continue;
            PlayerControllerCommand command = player->command;
            command.jumpPressed = substep == 0U &&
                player->command.jumpPressed;
            (void)PlayerControllerSimulateFixedSteps(
                &player->controller, &server->collision,
                &player->camera, &command, 1U);
        }
    }

    for (uint32_t index = 0U;
        index < LAIUE_NETWORK_MAX_PEERS; ++index)
    {
        ServerPlayer *player = &server->players[index];
        if (!player->active)
            continue;
        player->command.jumpPressed = false;
        if (player->breaking &&
            now - player->lastBreakIntentAtMs >
                SERVER_BREAK_INTENT_TIMEOUT_MS)
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

    // Construct motion is a 60 Hz authoritative stream. Clients render from
    // a short tick-indexed interpolation buffer, while topology remains on
    // the bounded snapshot stream.
    SendConstructMotionStates(server);

    if (server->tick % SERVER_TIME_SYNC_TICKS == 0)
    {
        uint64_t worldTime =
            ServerWorldTimeMilliseconds(server->timeOfDayHours);
        for (uint32_t index = 0;
            index < LAIUE_NETWORK_MAX_PEERS; ++index)
        {
            const ServerPlayer* player = &server->players[index];
            if (player->active)
                NetworkServerSendWorldTime(
                    server->network, player->peerId, worldTime);
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
        NetworkPlayerState state;
        BuildNetworkPlayerState(server, player, &state);
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
        // The next snapshot is a live interest update (hasSnapshotCenter is
        // already true), so the transport keeps READY and input flowing.
        // Defer the work and let the fixed-tick scheduler coalesce movement
        // while the previous QUIC auxiliary stream is still draining.
        player->fullSnapshotPending = true;
        ServerSnapshotSchedulerRequest(
            &player->snapshotScheduler);
        // Registration is cheap and must visit every peer. The global
        // snapshot work budget below limits expensive preparation/sending;
        // returning here would let a slow low-index peer starve all others.
    }
}

static uint32_t RunServer(void)
{
    uint32_t exitCode = 0U;
    World* world = NULL;
    DedicatedServer* server = NULL;

    ServerConfiguration* configuration =
        PlatformAllocate(sizeof(*configuration), false);
    if (configuration == NULL)
    {
        WriteServerStartupFailure(L"out of memory reading server.cfg", 1U);
        return 1U;
    }
    ServerConfigurationLoad(configuration);

    world = WorldCreate(configuration->worldSeed);
    if (world == NULL)
    {
        exitCode = 1U;
        WriteServerStartupFailure(L"out of memory creating the world", 1U);
        goto teardown;
    }
    PlatformCreateDirectory(L"saves");
    PlatformCreateDirectory(L"saves\\default");
    server = PlatformAllocate(sizeof(*server), true);
    if (server == NULL)
    {
        exitCode = 2U;
        WriteServerStartupFailure(L"out of memory creating the server", 2U);
        goto teardown;
    }
    server->world = world;
    server->constructs = PhysicalConstructSystemCreate(world, NULL);
    if (server->constructs == NULL)
    {
        exitCode = 2U;
        WriteServerStartupFailure(
            L"out of memory creating physical constructs", 2U);
        goto teardown;
    }
    // World edits and physical constructs are one authoritative persistence
    // domain. The two-slot commit protocol never exposes half of an
    // activation after a crash and retains the previous complete generation.
    if (!PhysicalConstructPersistenceLoad(world, server->constructs,
            g_serverWorldPath, g_serverConstructPath,
            g_serverCommitPath))
    {
        exitCode = 2U;
        WriteServerStartupFailure(
            L"saves/default world state is invalid", 2U);
        goto teardown;
    }
    server->worldSeed = configuration->worldSeed;
    server->timeOfDayHours = 12.0f;
    ServerSnapshotWorkBudgetInitialize(
        &server->snapshotWorkBudget, server->tick);
    server->collision.context = server;
    server->collision.queryBlockPhysics = QueryWorldBlockPhysics;
    server->collision.queryDynamicColliders = QueryConstructColliders;

    ModsInit(&server->mods, L"server_enabled.txt");
    ModsRefresh(&server->mods);
    ModHostBindings modBindings = {
        .world = world,
        .timeOfDayHours = &server->timeOfDayHours,
        .runtimeSide = MOD_SIDE_SERVER,
        .blockMutationPolicy =
            MOD_HOST_BLOCK_MUTATION_CALLBACK,
        .blockMutationContext = server,
        .mutateBlock = MutateServerModBlock,
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
        exitCode = 3U;
        WriteServerStartupFailure(
            L"server_enabled.txt yields no valid mod compatibility set", 3U);
        goto teardown;
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
        exitCode = 4U;
        WriteServerStartupFailure(
            L"cannot build the downloadable content bundle", 4U);
        goto teardown;
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
    // Оба отказа fail-closed и делят код 5, но причины у оператора разные:
    // «поправь server.cfg» против «QUIC-стек не поднялся».
    bool credentialsUsable = ServerCredentialsAreUsable(configuration);
    NetworkServer *network = credentialsUsable
        ? NetworkServerCreate(&networkConfiguration) : NULL;
    if (network == NULL)
    {
        exitCode = 5U;
        WriteServerStartupFailure(credentialsUsable
            ? L"secure transport unavailable, see docs/secure_server.md"
            : L"configuration has no usable certificate or private key,"
              L" see docs/secure_server.md",
            5U);
        goto teardown;
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
    ServerTickClock tickClock;
    ServerTickClockInitialize(&tickClock);
    while (!PlatformTerminationRequested())
    {
        double currentTime = ServerTimeSeconds();
        double elapsed = currentTime - previousTime;
        previousTime = currentTime;
        if (elapsed < 0.0)
        {
            elapsed = 0.0;
        }
        ServerTickClockAccumulate(
            &tickClock, elapsed, SERVER_MAX_BACKLOG_SECONDS);

        NetworkServerUpdate(network);
        HandleNetworkEvents(server);

        uint32_t steps = 0;
        while (ServerTickClockHasTick(
                &tickClock, SERVER_TICK_SECONDS)
            && steps < SERVER_MAX_CATCH_UP_TICKS)
        {
            SimulateTick(server);
            ServerTickClockConsumeTick(
                &tickClock, SERVER_TICK_SECONDS);
            steps++;
        }
        if (steps != 0) RefreshPlayerInterest(server);
        bool caughtUp = !ServerTickClockHasTick(
            &tickClock, SERVER_TICK_SECONDS);
        if (caughtUp)
        {
            ProcessPendingResyncs(server);
            PlatformSleepMilliseconds(1U);
        }
    }

    PlatformRemoveTerminationHandler();
    NetworkServerDestroy(network);
    LaiueContentBundleRelease(&server->downloadableContent);
    ModHostShutdown(&server->modHost);
    bool stateSaved = PhysicalConstructPersistenceSave(
        world, server->constructs, g_serverWorldPath,
        g_serverConstructPath, g_serverCommitPath);
    if (!stateSaved)
    {
        WriteServerMessage(
            L"laiue dedicated server: save failed\r\n", 38U);
    }
    PhysicalConstructSystemDestroy(server->constructs);
    WorldDestroy(world);
    PlatformFree(server);
    PlatformFree(configuration);
    return 0U;

    // Общая лестница отказов старта. Порядок обязателен: моды выгружаются
    // до уничтожения мира, потому что их shutdown ещё держит его bindings.
teardown:
    if (server != NULL)
    {
        LaiueContentBundleRelease(&server->downloadableContent);
        ModHostShutdown(&server->modHost);
        PhysicalConstructSystemDestroy(server->constructs);
    }
    PlatformFree(server);
    WorldDestroy(world);
    PlatformFree(configuration);
    return exitCode;
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
