#include "game/camera.h"
#include "content/content_bundle.h"
#include "content/content_catalog.h"
#include "gameplay/player_controller.h"
#include "gameplay/inventory.h"
#include "interaction/voxel_interaction.h"
#include "mod/mod_host.h"
#include "mod/mods.h"
#include "network/network.h"
#include "server/server_config.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <windows.h>

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
    float timeOfDayHours;
    uint32_t tick;
    ServerBlockDrop drops[SERVER_DROP_CAPACITY];
    uint32_t nextDropSlot;
    uint32_t nextDropId;
} DedicatedServer;

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
    LaiueContentBundleSource* sources = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, LAIUE_CONTENT_BUNDLE_MAX_SOURCES * sizeof(*sources));
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
    HeapFree(GetProcessHeap(), 0, sources);
    return succeeded;
}

static volatile LONG g_stopRequested;

static BOOL WINAPI HandleConsoleControl(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT || controlType == CTRL_SHUTDOWN_EVENT)
    {
        InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    }
    return FALSE;
}

static double ServerTimeSeconds(void)
{
    static LARGE_INTEGER frequency;
    static volatile LONG initialized;
    if (InterlockedCompareExchange(&initialized, 1, 0) == 0)
    {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

static void WriteServerMessage(const wchar_t *message, uint32_t length)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written;
    WriteConsoleW(output, message, length, &written, NULL);
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
        player->lastInputAtMs = GetTickCount64();

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

static void SendPlayerInventory(DedicatedServer* server,
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
    NetworkServerSendInventory(server->network, player->peerId, &state);
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
    uint64_t now = GetTickCount64();
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
    NetworkBlockDelta delta = {
        .serverTick = server->tick,
        .block = {edit.block[0], edit.block[1], edit.block[2]},
        .replacement = edit.replacement,
    };
    NetworkServerBroadcastBlockDelta(server->network, &delta);
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
            if (connected != NULL) SendPlayerInventory(server, connected);
            continue;
        }
        if (event.type == NETWORK_SERVER_EVENT_DISCONNECTED)
        {
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
            player->lastInputAtMs = GetTickCount64();
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
    }
}

static void SimulateTick(DedicatedServer *server)
{
    uint64_t now = GetTickCount64();
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

static uint32_t RunServer(void)
{
    ServerConfiguration configuration;
    ServerConfigurationLoad(&configuration);

    World *world = WorldCreate(configuration.worldSeed);
    if (world == NULL)
    {
        return 1U;
    }
    CreateDirectoryW(L"saves", NULL);
    CreateDirectoryW(L"saves\\default", NULL);
    // Для локального split-runtime это тот же мир, который клиент уже
    // загрузил до handshake. После подключения клиент больше его не пишет.
    WorldLoadDeltas(world, g_serverWorldPath);

    DedicatedServer *server = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*server));
    if (server == NULL)
    {
        WorldDestroy(world);
        return 2U;
    }
    server->world = world;
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
    CreateDirectoryW(L"saves\\default\\moddata", NULL);
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
        HeapFree(GetProcessHeap(), 0, server);
        WorldDestroy(world);
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
    bool downloadsReady = !configuration.allowContentDownloads
        || BuildDownloadableContent(server);
    if (!downloadsReady)
    {
        ModHostShutdown(&server->modHost);
        HeapFree(GetProcessHeap(), 0, server);
        WorldDestroy(world);
        return 4U;
    }
    NetworkServerConfiguration networkConfiguration = {
        .port = configuration.port,
        .maximumPeers = configuration.maximumPeers,
        .worldSeed = configuration.worldSeed,
        .mods = server->networkMods,
        .modCount = server->networkModCount,
        .allowContentDownloads = configuration.allowContentDownloads,
        .contentBundle = server->downloadableContent.bytes,
        .contentBundleSize = server->downloadableContent.size,
    };
    memcpy(networkConfiguration.contentBundleSha256,
        server->downloadableContent.sha256,
        LAIUE_NETWORK_CONTENT_HASH_SIZE);
    NetworkServer *network = NetworkServerCreateLoopback(&networkConfiguration);
    if (network == NULL)
    {
        LaiueContentBundleRelease(&server->downloadableContent);
        ModHostShutdown(&server->modHost);
        HeapFree(GetProcessHeap(), 0, server);
        WorldDestroy(world);
        return 5U;
    }
    server->network = network;

    wchar_t readyMessage[192];
    uint32_t readyLength = AppendServerText(readyMessage, 192U, 0,
        L"laiue dedicated server: 127.0.0.1:");
    readyLength = AppendServerUnsigned(readyMessage, 192U,
        readyLength, configuration.port);
    readyLength = AppendServerText(readyMessage, 192U, readyLength,
        L" (loopback only)\r\ncontent downloads: ");
    readyLength = AppendServerText(readyMessage, 192U, readyLength,
        configuration.allowContentDownloads ? L"enabled" : L"disabled");
    readyLength = AppendServerText(readyMessage, 192U, readyLength,
        L"\r\nCtrl+C to stop.\r\n");
    WriteServerMessage(readyMessage, readyLength);

    SetConsoleCtrlHandler(HandleConsoleControl, TRUE);
    double previousTime = ServerTimeSeconds();
    double accumulator = 0.0;
    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
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
        Sleep(1);
    }

    SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
    NetworkServerDestroy(network);
    LaiueContentBundleRelease(&server->downloadableContent);
    ModHostShutdown(&server->modHost);
    WorldSaveDeltas(world, g_serverWorldPath);
    WorldDestroy(world);
    HeapFree(GetProcessHeap(), 0, server);
    return 0U;
}

void __stdcall ServerEntryPoint(void)
{
    ExitProcess(RunServer());
}
