#include "game/camera.h"
#include "gameplay/player_controller.h"
#include "interaction/voxel_interaction.h"
#include "network/network.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERVER_WORLD_SEED 42LL
#define SERVER_SPAWN_HEIGHT 100.0
#define SERVER_TICK_SECONDS (1.0 / 60.0)
#define SERVER_SNAPSHOT_TICKS 3U
#define SERVER_MAX_CATCH_UP_TICKS 8U
#define SERVER_INPUT_TIMEOUT_MS 250ULL
#define SERVER_EDIT_COOLDOWN_MS 125ULL
#define SERVER_EDIT_REACH 8.0f

static const wchar_t g_serverWorldPath[] = L"saves\\default\\chunks.dat";

typedef struct ServerPlayer
{
    uint32_t peerId;
    Camera camera;
    PlayerController controller;
    PlayerControllerCommand command;
    uint64_t lastInputAtMs;
    uint64_t nextEditAtMs;
    bool active;
} ServerPlayer;

typedef struct DedicatedServer
{
    NetworkServer *network;
    World *world;
    PlayerCollisionSource collision;
    ServerPlayer players[LAIUE_NETWORK_MAX_PEERS];
    uint32_t tick;
} DedicatedServer;

static volatile LONG g_stopRequested;

void __cdecl __security_init_cookie(void);

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

static void HandleEditIntent(DedicatedServer *server, ServerPlayer *player,
                             const NetworkServerEvent *event)
{
    uint64_t now = GetTickCount64();
    if (now < player->nextEditAtMs)
    {
        return;
    }
    player->nextEditAtMs = now + SERVER_EDIT_COOLDOWN_MS;

    VoxelBodyShape bodyShape;
    PlayerControllerGetBodyShape(&player->controller, &bodyShape);
    VoxelEdit edit;
    if (!VoxelInteractionTryCreateEdit(server->world, player->camera.position,
                                       event->data.editIntent.direction, player->camera.position,
                                       &bodyShape, event->data.editIntent.breakBlock,
                                       event->data.editIntent.placeBlock, SERVER_EDIT_REACH, &edit))
    {
        return;
    }

    WorldSetBlock(server->world, edit.block[0], edit.block[1], edit.block[2], edit.replacement);
    NetworkBlockDelta delta = {
        .serverTick = server->tick,
        .block = {edit.block[0], edit.block[1], edit.block[2]},
        .replacement = edit.replacement,
    };
    NetworkServerBroadcastBlockDelta(server->network, &delta);
}

static void HandleNetworkEvents(DedicatedServer *server)
{
    NetworkServerEvent event;
    while (NetworkServerPollEvent(server->network, &event))
    {
        if (event.type == NETWORK_SERVER_EVENT_CONNECTED)
        {
            AddPlayer(server, event.peerId);
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
    }
}

static void SimulateTick(DedicatedServer *server)
{
    uint64_t now = GetTickCount64();
    server->tick++;
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
    World *world = WorldCreate(SERVER_WORLD_SEED);
    if (world == NULL)
    {
        return 1U;
    }
    CreateDirectoryW(L"saves", NULL);
    CreateDirectoryW(L"saves\\default", NULL);
    // Для локального split-runtime это тот же мир, который клиент уже
    // загрузил до handshake. После подключения клиент больше его не пишет.
    WorldLoadDeltas(world, g_serverWorldPath);
    NetworkServerConfiguration networkConfiguration = {
        .port = LAIUE_NETWORK_DEFAULT_PORT,
        .maximumPeers = LAIUE_NETWORK_MAX_PEERS,
        .worldSeed = SERVER_WORLD_SEED,
    };
    NetworkServer *network = NetworkServerCreateLoopback(&networkConfiguration);
    if (network == NULL)
    {
        WorldDestroy(world);
        return 2U;
    }

    DedicatedServer *server = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*server));
    if (server == NULL)
    {
        NetworkServerDestroy(network);
        WorldDestroy(world);
        return 3U;
    }
    server->network = network;
    server->world = world;
    server->collision.context = world;
    server->collision.queryBlockPhysics = QueryWorldBlockPhysics;

    static const wchar_t readyMessage[] =
        L"laiue dedicated server: 127.0.0.1:27180 (loopback only)\r\n"
        L"Ctrl+C to stop.\r\n";
    WriteServerMessage(readyMessage,
                       (uint32_t)(sizeof(readyMessage) / sizeof(readyMessage[0]) - 1U));

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
    WorldSaveDeltas(world, g_serverWorldPath);
    WorldDestroy(world);
    HeapFree(GetProcessHeap(), 0, server);
    return 0U;
}

void __stdcall ServerEntryPoint(void)
{
    __security_init_cookie();
    ExitProcess(RunServer());
}
