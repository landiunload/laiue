#include "api.h"
#include "core/application_config.h"
#include "core/block_effects.h"
#include "content/content_bundle.h"
#include "core/camera.h"
#include "core/chunk_streaming.h"
#include "core/game_hud.h"
#include "core/game_time.h"
#include "core/math.h"
#include "core/numeric.h"
#include "core/inventory_ui.h"
#include "core/panorama.h"
#include "mod/mod_host.h"
#include "core/pause_menu.h"
#include "core/save_game.h"
#include "render/shader_pack.h"
#include "render/texture_pack.h"
#include "core/player_command_mapper.h"
#include "core/ui.h"
#include "gameplay/game_mode.h"
#include "gameplay/inventory.h"
#include "gameplay/player_controller.h"
#include "interaction/voxel_interaction.h"
#include "input/input.h"
#include "network/network.h"
#include "platform/time.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "world/block_properties.h"
#include "world/world.h"

#include <stddef.h>
#include <string.h>
#include <windows.h>

typedef struct ApplicationState
{
    Window* window;
    Input* input;
    World* world;
    Renderer* renderer;
    ChunkStreaming* chunkStreaming;
    Camera camera;
    PlayerController player;
    GameMode gameMode;
    int32_t windowWidth;
    int32_t windowHeight;
    double previousTimeSeconds;
    double fpsSampleStartSeconds;
    uint32_t fpsFrameCount;
    uint32_t framesPerSecond;
    bool showDiagnostics;
    NetworkClient* networkClient;
    uint32_t networkPeerId;
    float networkInputAccumulator;
    float networkBreakSendAccumulator;
    NetworkInputCommand networkPendingInput;
    bool networkReady;
    bool networkEverReady;
    bool networkHasSnapshot;
    NetworkModDescriptor serverMods[LAIUE_NETWORK_MAX_MODS];
    ModCompatibilityEntry serverCompatibilityMods[MODS_MAX_ENTRIES];
    ModCompatibilityEntry localCompatibilityMods[MODS_MAX_ENTRIES];
    NetworkModDescriptor localNetworkMods[LAIUE_NETWORK_MAX_MODS];
    uint32_t serverModCount;
    bool sessionActive;
    bool inventoryOpen;
    Inventory inventory;
    BlockEffects blockEffects;
    bool blockEffectsReady;
    struct
    {
        int64_t block[3];
        float progress;
        bool active;
    } breaking;
    uint32_t backgroundWidth;
    uint32_t backgroundHeight;

    GameSettings settings;
    PanoramaCache panoramaCache;
    PauseMenu menu;
    UiContext ui;
    GameHud hud;
    ModsState mods;
    ModHost modHost;
    uint32_t appliedModsRevision;
    bool mouseLookBeforeMenu;
    int64_t worldSeed;
    wchar_t modDataDirectory[SAVE_GAME_PATH_CAPACITY];
} ApplicationState;

static void ResumeGame(ApplicationState* application);

static void QueryWorldBlockPhysics(
    void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    BlockType type = WorldGetBlock((World*)context, x, y, z);
    BlockProperties properties = BlockGetProperties(type);
    outBlock->flags = properties.solid
        ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = properties.friction;
}

static PlayerCollisionSource CreatePlayerCollisionSource(World* world)
{
    PlayerCollisionSource source = {
        .context = world,
        .queryBlockPhysics = QueryWorldBlockPhysics,
    };
    return source;
}

static void InvalidateModBlock(void* context,
    int64_t x, int64_t y, int64_t z)
{
    ChunkStreamingInvalidateBlock((ChunkStreaming*)context, x, y, z);
}

static void GetModViewDirection(void* context, float outDirection[3])
{
    CameraGetForwardVector((const Camera*)context, outDirection);
}

static void InitializeApplicationModHost(ApplicationState* application)
{
    SaveGameEnsureDirectories();
    SaveGameModDataDirectory(application->modDataDirectory,
        SAVE_GAME_PATH_CAPACITY);
    ModHostBindings bindings = {
        .world = application->world,
        .player = &application->player,
        .camera = &application->camera,
        .gameMode = &application->gameMode,
        .timeOfDayHours = &application->settings.timeOfDayHours,
        .runtimeSide = MOD_SIDE_CLIENT,
        .invalidateContext = application->chunkStreaming,
        .invalidateBlock = InvalidateModBlock,
        .viewContext = &application->camera,
        .getViewDirection = GetModViewDirection,
        .modDataDirectory = application->modDataDirectory,
    };
    if (!ModHostInit(&application->modHost, &bindings))
    {
        application->modHost.slots = NULL;
    }
    else
    {
        ModHostSync(&application->modHost, &application->mods);
    }
    application->appliedModsRevision = application->mods.revision;
}

static bool PrepareRendererForSession(ApplicationState* application)
{
    if (!RendererPrepareWorld(application->renderer)) return false;

    application->menu.texturePackStatus =
        RendererGetTexturePackLoadStatus(application->renderer);
    if (application->menu.texturePackStatus == RENDERER_CONTENT_INVALID
        || application->menu.texturePackStatus == RENDERER_CONTENT_IO_ERROR)
    {
        TexturePackActivate(NULL);
        if (!RendererReloadTexturePack(application->renderer)) return false;
        application->menu.texturePackStatus =
            RendererGetTexturePackLoadStatus(application->renderer);
    }

    void* shaders[6] = { 0 };
    uint32_t lengths[6] = { 0 };
    ShaderPackLoadStatus shaderStatus;
    if (ShaderPackLoadActiveBytecode(
            &shaders[0], &lengths[0], &shaders[1], &lengths[1],
            &shaders[2], &lengths[2], &shaders[3], &lengths[3],
            &shaders[4], &lengths[4], &shaders[5], &lengths[5],
            &shaderStatus))
    {
        application->menu.shaderPackStatus = RendererReloadShaders(
            application->renderer,
            shaders[0], lengths[0], shaders[1], lengths[1],
            shaders[2], lengths[2], shaders[3], lengths[3],
            shaders[4], lengths[4], shaders[5], lengths[5])
            ? SHADER_PACK_LOAD_OK : SHADER_PACK_LOAD_PIPELINE_ERROR;
        for (uint32_t i = 0; i < 6; ++i)
        {
            if (shaders[i] != NULL)
                HeapFree(GetProcessHeap(), 0, shaders[i]);
        }
        if (application->menu.shaderPackStatus
            != SHADER_PACK_LOAD_OK) return false;
    }
    else if (shaderStatus != SHADER_PACK_LOAD_NO_ACTIVE_PACK)
    {
        application->menu.shaderPackStatus = shaderStatus;
        ShaderPackActivate(NULL);
    }
    return true;
}

static void InitializeSessionInventory(ApplicationState* application)
{
    InventoryClear(&application->inventory);
    application->inventory.slots[0].item = BLOCK_EARTH;
    application->inventory.slots[0].count = INVENTORY_STACK_LIMIT;
    application->inventory.slots[1].item = BLOCK_GRASS;
    application->inventory.slots[1].count = INVENTORY_STACK_LIMIT;
}

static void ApplicationCloseSession(ApplicationState* application,
    bool saveWorld, bool disconnectNetwork)
{
    if (application->sessionActive && saveWorld
        && !application->networkEverReady)
    {
        SaveGameWriteAll(application->world, &application->camera,
            application->gameMode, application->settings.timeOfDayHours,
            application->worldSeed, &application->mods,
            &application->inventory);
    }
    ModHostShutdown(&application->modHost);
    if (application->blockEffectsReady)
    {
        BlockEffectsShutdown(&application->blockEffects,
            application->renderer);
        application->blockEffectsReady = false;
    }
    ChunkStreamingDestroy(application->chunkStreaming);
    WorldDestroy(application->world);
    application->chunkStreaming = NULL;
    application->world = NULL;
    application->sessionActive = false;
    application->inventoryOpen = false;
    application->breaking.active = false;
    application->breaking.progress = 0.0f;
    RendererReleaseWorld(application->renderer);
    if (disconnectNetwork)
    {
        NetworkClientDestroy(application->networkClient);
        application->networkClient = NULL;
        application->networkReady = false;
        application->networkEverReady = false;
    }
}

static bool ApplicationSwitchWorld(ApplicationState* application,
    const wchar_t* slotName)
{
    wchar_t previousSlot[SAVE_GAME_SLOT_NAME_CAPACITY];
    uint32_t previousLength = 0;
    const wchar_t* currentSlot = SaveGameGetSlot();
    while (currentSlot[previousLength] != L'\0'
        && previousLength + 1U < SAVE_GAME_SLOT_NAME_CAPACITY)
    {
        previousSlot[previousLength] = currentSlot[previousLength];
        ++previousLength;
    }
    previousSlot[previousLength] = L'\0';
    if (application->sessionActive)
        ApplicationCloseSession(application, true, true);
    if (!SaveGameSetSlot(slotName)) return false;
    if (!PrepareRendererForSession(application))
    {
        SaveGameSetSlot(previousSlot);
        return false;
    }
    int64_t seed = g_applicationConfiguration.worldSeed;
    int32_t savedMinutes = -1;
    SaveGameReadMeta(&seed, &savedMinutes);
    World* replacementWorld = WorldCreate(seed);
    if (replacementWorld == NULL)
    {
        RendererReleaseWorld(application->renderer);
        SaveGameSetSlot(previousSlot);
        return false;
    }
    if (savedMinutes >= 0) SaveGameLoadWorld(replacementWorld);
    ChunkStreaming* replacementStreaming = ChunkStreamingCreate(
        replacementWorld, application->renderer,
        g_applicationConfiguration.viewRadiusChunks);
    if (replacementStreaming == NULL)
    {
        WorldDestroy(replacementWorld);
        RendererReleaseWorld(application->renderer);
        SaveGameSetSlot(previousSlot);
        return false;
    }

    application->world = replacementWorld;
    application->chunkStreaming = replacementStreaming;
    application->worldSeed = seed;
    application->networkEverReady = false;
    application->networkReady = false;
    application->gameMode = GAME_MODE_FLY;
    application->settings.timeOfDayHours = savedMinutes >= 0
        ? (float)savedMinutes / 60.0f
        : g_applicationConfiguration.startTimeOfDayHours;
    CameraInit(&application->camera, 0.0, 0.0,
        g_applicationConfiguration.spawnHeight, 0.0f, -0.4f);
    if (savedMinutes >= 0)
    {
        SaveGameLoadPlayer(&application->camera, &application->gameMode);
        SaveGameCheckModsLock(&application->mods);
    }
    PlayerControllerReset(&application->player, &application->camera);
    InitializeSessionInventory(application);
    if (savedMinutes >= 0)
        SaveGameLoadInventory(&application->inventory);
    if (!BlockEffectsInit(&application->blockEffects,
            application->renderer))
    {
        ChunkStreamingDestroy(application->chunkStreaming);
        WorldDestroy(application->world);
        application->chunkStreaming = NULL;
        application->world = NULL;
        RendererReleaseWorld(application->renderer);
        SaveGameSetSlot(previousSlot);
        return false;
    }
    application->blockEffectsReady = true;
    InitializeApplicationModHost(application);
    application->sessionActive = true;
    return true;
}

static bool ApplicationSwitchToNetworkWorld(ApplicationState* application,
    int64_t seed)
{
    if (application->sessionActive)
        ApplicationCloseSession(application, true, false);
    if (!PrepareRendererForSession(application)) return false;
    World* replacementWorld = WorldCreate(seed);
    if (replacementWorld == NULL)
    {
        RendererReleaseWorld(application->renderer);
        return false;
    }
    ChunkStreaming* replacementStreaming = ChunkStreamingCreate(
        replacementWorld, application->renderer,
        g_applicationConfiguration.viewRadiusChunks);
    if (replacementStreaming == NULL)
    {
        WorldDestroy(replacementWorld);
        RendererReleaseWorld(application->renderer);
        return false;
    }
    application->world = replacementWorld;
    application->chunkStreaming = replacementStreaming;
    application->worldSeed = seed;
    CameraInit(&application->camera, 0.0, 0.0,
        g_applicationConfiguration.spawnHeight, 0.0f, -0.4f);
    PlayerControllerReset(&application->player, &application->camera);
    application->gameMode = GAME_MODE_SURVIVAL;
    InitializeSessionInventory(application);
    InventoryClear(&application->inventory);
    if (!BlockEffectsInit(&application->blockEffects,
            application->renderer))
    {
        ChunkStreamingDestroy(application->chunkStreaming);
        WorldDestroy(application->world);
        application->chunkStreaming = NULL;
        application->world = NULL;
        RendererReleaseWorld(application->renderer);
        return false;
    }
    application->blockEffectsReady = true;
    InitializeApplicationModHost(application);
    application->sessionActive = true;
    return true;
}

static bool BuildLocalNetworkMods(ApplicationState* application,
    uint32_t* outCount)
{
    uint32_t count = 0;
    if (!ModsBuildCompatibilitySet(&application->mods,
            application->localCompatibilityMods, MODS_MAX_ENTRIES,
            &count)) return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        memcpy(application->localNetworkMods[i].id,
            application->localCompatibilityMods[i].id,
            sizeof(application->localNetworkMods[i].id));
        memcpy(application->localNetworkMods[i].version,
            application->localCompatibilityMods[i].version,
            sizeof(application->localNetworkMods[i].version));
        memcpy(application->localNetworkMods[i].contentHash,
            application->localCompatibilityMods[i].contentHash,
            LAIUE_NETWORK_MOD_HASH_SIZE);
    }
    *outCount = count;
    return true;
}

static bool NetworkModEqual(const NetworkModDescriptor* left,
    const NetworkModDescriptor* right)
{
    uint8_t hashDifference = 0;
    for (uint32_t hashIndex = 0;
        hashIndex < LAIUE_NETWORK_MOD_HASH_SIZE; ++hashIndex)
    {
        hashDifference |= left->contentHash[hashIndex]
            ^ right->contentHash[hashIndex];
    }
    if (hashDifference != 0) return false;
    uint32_t i = 0;
    while (i < LAIUE_NETWORK_MOD_ID_CAPACITY
        && left->id[i] != '\0' && left->id[i] == right->id[i]) ++i;
    if (i >= LAIUE_NETWORK_MOD_ID_CAPACITY
        || left->id[i] != right->id[i]) return false;
    i = 0;
    while (i < LAIUE_NETWORK_MOD_VERSION_CAPACITY
        && left->version[i] != '\0'
        && left->version[i] == right->version[i]) ++i;
    return i < LAIUE_NETWORK_MOD_VERSION_CAPACITY
        && left->version[i] == right->version[i];
}

static bool SubmitCurrentNetworkMods(ApplicationState* application)
{
    uint32_t count;
    return BuildLocalNetworkMods(application, &count)
        && NetworkClientSubmitMods(application->networkClient,
            application->localNetworkMods, count);
}

static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static int64_t CalculateChunkAlignedShift(double position)
{
    int64_t block = FloorDoubleToInt64(position);
    int64_t chunk = block / CHUNK_SIZE;
    if (block < 0 && block % CHUNK_SIZE != 0)
    {
        --chunk;
    }
    return chunk * CHUNK_SIZE;
}

static int64_t CalculateRebaseShift(double position)
{
    double threshold =
        (double)g_applicationConfiguration.rebaseThresholdBlocks;
    if (position >= -threshold && position <= threshold)
    {
        return 0;
    }
    return CalculateChunkAlignedShift(position);
}

static bool RebaseWorldIfNeeded(ApplicationState* application)
{
    int64_t shift[3] = {
        CalculateRebaseShift(application->camera.position[0]),
        CalculateRebaseShift(application->camera.position[1]),
        CalculateRebaseShift(application->camera.position[2]),
    };
    if (shift[0] == 0 && shift[1] == 0 && shift[2] == 0)
    {
        return true;
    }

    if (!ChunkStreamingPause(application->chunkStreaming))
    {
        return false;
    }
    if (!WorldRebase(application->world, shift[0], shift[1], shift[2]))
    {
        return false;
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        application->camera.position[axis] -= (double)shift[axis];
    }
    BlockEffectsRebase(&application->blockEffects,
        shift[0], shift[1], shift[2]);

    int64_t centerX =
        FloorDoubleToInt64(application->camera.position[0]) >> CHUNK_SIZE_LOG2;
    int64_t centerY =
        FloorDoubleToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ =
        FloorDoubleToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, true,
        shift[0] / CHUNK_SIZE, shift[1] / CHUNK_SIZE,
        shift[2] / CHUNK_SIZE, centerX, centerY, centerZ);
}

static bool SquareAbsoluteX(ApplicationState* application)
{
    int64_t currentBlockX =
        FloorDoubleToInt64(application->camera.position[0]);
    double fractionalX =
        application->camera.position[0] - (double)currentBlockX;

    if (!ChunkStreamingPause(application->chunkStreaming))
    {
        return false;
    }

    int64_t squaredLocalBlockX;
    bool chunkOriginDeltaFits;
    int64_t chunkOriginDeltaX;
    if (!WorldSquareAbsoluteX(application->world, currentBlockX,
            &squaredLocalBlockX, &chunkOriginDeltaFits,
            &chunkOriginDeltaX))
    {
        return false;
    }

    application->camera.position[0] =
        (double)squaredLocalBlockX + fractionalX;
    BlockEffectsClear(&application->blockEffects);
    if (application->gameMode == GAME_MODE_WALK)
    {
        PlayerCollisionSource collision =
            CreatePlayerCollisionSource(application->world);
        PlayerControllerReset(&application->player, &application->camera);
        if (!PlayerControllerResolvePenetration(&application->player,
                &collision, &application->camera))
        {
            application->gameMode = GAME_MODE_FLY;
        }
    }

    int64_t centerX = squaredLocalBlockX >> CHUNK_SIZE_LOG2;
    int64_t centerY =
        FloorDoubleToInt64(application->camera.position[1]) >> CHUNK_SIZE_LOG2;
    int64_t centerZ =
        FloorDoubleToInt64(application->camera.position[2]) >> CHUNK_SIZE_LOG2;
    return ChunkStreamingResumeAfterOriginChange(
        application->chunkStreaming, chunkOriginDeltaFits,
        chunkOriginDeltaX, 0, 0, centerX, centerY, centerZ);
}

static void RecordPresentedFrame(ApplicationState* application)
{
    double currentTimeSeconds = PlatformTimeSeconds();
    application->fpsFrameCount++;

    double elapsedSeconds =
        currentTimeSeconds - application->fpsSampleStartSeconds;
    if (elapsedSeconds < 0.5)
    {
        return;
    }

    application->framesPerSecond = (uint32_t)(
        (double)application->fpsFrameCount / elapsedSeconds + 0.5);
    application->fpsFrameCount = 0;
    application->fpsSampleStartSeconds = currentTimeSeconds;
}

static BlockType SelectedPlacementBlock(const ApplicationState* application)
{
    const InventorySlot* selected =
        InventorySelectedSlot(&application->inventory);
    if (selected == NULL || selected->item < BLOCK_EARTH
        || selected->item > BLOCK_GRASS || selected->count == 0)
    {
        return BLOCK_AIR;
    }
    return (BlockType)selected->item;
}

static bool SameBlockPosition(const int64_t left[3],
    const int64_t right[3])
{
    return left[0] == right[0] && left[1] == right[1]
        && left[2] == right[2];
}

static void ApplyLocalEdit(ApplicationState* application,
    const VoxelEdit* edit, bool survivalBreak)
{
    BlockType previousBlock = WorldGetBlock(application->world,
        edit->block[0], edit->block[1], edit->block[2]);
    WorldSetBlock(application->world,
        edit->block[0], edit->block[1], edit->block[2], edit->replacement);
    ChunkStreamingInvalidateBlock(application->chunkStreaming,
        edit->block[0], edit->block[1], edit->block[2]);
    if (survivalBreak && previousBlock != BLOCK_AIR)
    {
        BlockEffectsSpawnDestroyed(&application->blockEffects,
            previousBlock, edit->block);
    }
    ModHostDispatchBlockEdit(&application->modHost,
        edit->block[0], edit->block[1], edit->block[2],
        (uint8_t)previousBlock, (uint8_t)edit->replacement);
}

static void HandleBlockEditing(ApplicationState* application,
    float deltaSeconds)
{
    bool survival = application->gameMode == GAME_MODE_SURVIVAL;
    bool breakRequested = survival
        ? InputIsMouseButtonDown(application->input,
            INPUT_MOUSE_BUTTON_LEFT)
        : InputWasMouseButtonPressed(application->input,
            INPUT_MOUSE_BUTTON_LEFT);
    bool placePressed = InputWasMouseButtonPressed(
        application->input, INPUT_MOUSE_BUTTON_RIGHT);
    if (!breakRequested && !placePressed)
    {
        application->breaking.active = false;
        application->breaking.progress = 0.0f;
        application->networkBreakSendAccumulator = 0.0f;
        return;
    }

    float direction[3];
    CameraGetForwardVector(&application->camera, direction);

    VoxelBodyShape bodyShape;
    const VoxelBodyShape* blockingShape = NULL;
    const double* blockingPosition = NULL;
    if (application->gameMode == GAME_MODE_WALK)
    {
        PlayerControllerGetBodyShape(&application->player, &bodyShape);
        blockingShape = &bodyShape;
        blockingPosition = application->camera.position;
    }

    if (breakRequested)
    {
        VoxelEdit edit;
        if (!VoxelInteractionTryCreateEdit(application->world,
                application->camera.position, direction,
                blockingPosition, blockingShape, true, false, BLOCK_AIR,
                g_applicationConfiguration.editReachDistance, &edit))
        {
            application->breaking.active = false;
            application->breaking.progress = 0.0f;
            application->networkBreakSendAccumulator = 0.0f;
        }
        else if (!survival)
        {
            if (application->networkReady)
                NetworkClientSendEditIntent(application->networkClient,
                    true, false, BLOCK_AIR, direction);
            else
                ApplyLocalEdit(application, &edit, false);
        }
        else
        {
            if (!application->breaking.active
                || !SameBlockPosition(application->breaking.block,
                    edit.block))
            {
                application->breaking.active = true;
                application->breaking.block[0] = edit.block[0];
                application->breaking.block[1] = edit.block[1];
                application->breaking.block[2] = edit.block[2];
                application->breaking.progress = 0.0f;
                application->networkBreakSendAccumulator = 0.1f;
            }
            BlockProperties properties = BlockGetProperties(
                WorldGetBlock(application->world,
                    edit.block[0], edit.block[1], edit.block[2]));
            float breakSeconds = properties.breakSeconds > 0.05f
                ? properties.breakSeconds : 0.55f;
            application->breaking.progress += deltaSeconds / breakSeconds;
            if (application->networkReady)
            {
                if (application->breaking.progress > 1.0f)
                    application->breaking.progress = 1.0f;
                application->networkBreakSendAccumulator += deltaSeconds;
                if (application->networkBreakSendAccumulator >= 0.1f)
                {
                    NetworkClientSendEditIntent(application->networkClient,
                        true, false, BLOCK_AIR, direction);
                    application->networkBreakSendAccumulator = 0.0f;
                }
            }
            else if (application->breaking.progress >= 1.0f)
            {
                ApplyLocalEdit(application, &edit, true);
                application->breaking.active = false;
                application->breaking.progress = 0.0f;
            }
        }
    }
    else
    {
        application->breaking.active = false;
        application->breaking.progress = 0.0f;
        application->networkBreakSendAccumulator = 0.0f;
    }

    if (!placePressed) return;
    BlockType placementBlock = SelectedPlacementBlock(application);
    if (placementBlock == BLOCK_AIR) return;
    VoxelEdit placement;
    if (!VoxelInteractionTryCreateEdit(application->world,
            application->camera.position, direction,
            blockingPosition, blockingShape, false, true, placementBlock,
            g_applicationConfiguration.editReachDistance, &placement))
        return;
    if (application->networkReady)
    {
        NetworkClientSendEditIntent(application->networkClient,
            false, true, placementBlock, direction);
        return;
    }
    if (survival && !InventoryConsumeSelected(
            &application->inventory, 1U, NULL))
        return;
    ApplyLocalEdit(application, &placement, false);
}

static void ToggleGameMode(ApplicationState* application)
{
    if (application->gameMode == GAME_MODE_FLY)
    {
        application->gameMode = GAME_MODE_WALK;
        PlayerCollisionSource collision =
            CreatePlayerCollisionSource(application->world);
        PlayerControllerReset(&application->player, &application->camera);
        if (!PlayerControllerResolvePenetration(&application->player,
                &collision, &application->camera))
        {
            application->gameMode = GAME_MODE_FLY;
        }
    }
    else
    {
        application->gameMode = GAME_MODE_FLY;
        PlayerControllerReset(&application->player, &application->camera);
    }
}

static void UpdatePlayer(ApplicationState* application,
    float deltaSeconds, int32_t mouseDeltaX, int32_t mouseDeltaY)
{
    // Настройки раздела «Управление» применяются вживую.
    float sensitivity = g_applicationConfiguration.mouseSensitivity
        * (float)application->settings.mouseSensitivityPercent * 0.01f;
    float flySpeed = (float)application->settings.flySpeedBlocks;

    if (application->networkReady)
    {
        CameraUpdate(&application->camera, deltaSeconds,
            false, false, false, false, false,
            mouseDeltaX, mouseDeltaY, 0.0f, sensitivity);

        PlayerControllerCommand command;
        PlayerCommandMapperBuild(application->input,
            &application->camera, &command);

        // Предсказание существует только для плавности клиента. Сервер
        // повторяет физику сам и регулярно исправляет расхождение.
        PlayerCollisionSource collision =
            CreatePlayerCollisionSource(application->world);
        PlayerControllerUpdate(&application->player,
            &collision, &application->camera, &command, deltaSeconds);

        application->networkPendingInput.movementX =
            (float)command.movementX;
        application->networkPendingInput.movementY =
            (float)command.movementY;
        application->networkPendingInput.yaw = application->camera.yaw;
        application->networkPendingInput.pitch = application->camera.pitch;
        application->networkPendingInput.jumpPressed =
            application->networkPendingInput.jumpPressed
            || command.jumpPressed;
        application->networkPendingInput.jumpHeld = command.jumpHeld;
        application->networkPendingInput.sprintHeld = command.sprintHeld;
        application->networkPendingInput.crouchHeld = command.crouchHeld;
        application->networkInputAccumulator += deltaSeconds;
        if (application->networkInputAccumulator >= (1.0f / 60.0f))
        {
            NetworkClientSendInput(application->networkClient,
                &application->networkPendingInput);
            application->networkPendingInput.jumpPressed = false;
            application->networkInputAccumulator = 0.0f;
        }
        return;
    }

    if (application->gameMode == GAME_MODE_FLY)
    {
        while (InputConsumeKeyPress(
                application->input, INPUT_KEY_SPACE))
        {
        }
        CameraUpdate(&application->camera, deltaSeconds,
            InputIsKeyDown(application->input, INPUT_KEY_W),
            InputIsKeyDown(application->input, INPUT_KEY_A),
            InputIsKeyDown(application->input, INPUT_KEY_S),
            InputIsKeyDown(application->input, INPUT_KEY_D),
            InputIsKeyDown(application->input, INPUT_KEY_SPACE),
            mouseDeltaX, mouseDeltaY,
            flySpeed, sensitivity);
        return;
    }

    CameraUpdate(&application->camera, deltaSeconds,
        false, false, false, false, false,
        mouseDeltaX, mouseDeltaY, 0.0f,
        sensitivity);

    PlayerControllerCommand command;
    PlayerCommandMapperBuild(application->input,
        &application->camera, &command);

    PlayerCollisionSource collision =
        CreatePlayerCollisionSource(application->world);
    PlayerControllerUpdate(&application->player,
        &collision, &application->camera, &command,
        deltaSeconds);
}

static void PumpNetwork(ApplicationState* application)
{
    if (application->networkClient == NULL)
    {
        return;
    }
    NetworkClientUpdate(application->networkClient);

    bool destroyClient = false;
    NetworkClientEvent event;
    while (NetworkClientPollEvent(application->networkClient, &event))
    {
        if (event.type == NETWORK_CLIENT_EVENT_SERVER_MODS)
        {
            application->serverModCount = 0;
            if (!NetworkClientCopyServerMods(application->networkClient,
                    application->serverMods, LAIUE_NETWORK_MAX_MODS,
                    &application->serverModCount))
            {
                destroyClient = true;
                continue;
            }
            for (uint32_t i = 0; i < application->serverModCount; ++i)
            {
                memcpy(application->serverCompatibilityMods[i].id,
                    application->serverMods[i].id,
                    sizeof(application->serverCompatibilityMods[i].id));
                memcpy(application->serverCompatibilityMods[i].version,
                    application->serverMods[i].version,
                    sizeof(application->serverCompatibilityMods[i].version));
                memcpy(application->serverCompatibilityMods[i].contentHash,
                    application->serverMods[i].contentHash,
                    LAIUE_NETWORK_MOD_HASH_SIZE);
            }

            uint32_t localCount = 0;
            bool exact = BuildLocalNetworkMods(application, &localCount)
                && localCount == application->serverModCount;
            for (uint32_t i = 0; i < localCount && exact; ++i)
            {
                exact = NetworkModEqual(&application->localNetworkMods[i],
                    &application->serverMods[i]);
            }
            if (exact)
            {
                if (!SubmitCurrentNetworkMods(application)) destroyClient = true;
            }
            else
            {
                bool installed = ModsCanApplyServerCompatibilitySet(
                    &application->mods,
                    application->serverCompatibilityMods,
                    application->serverModCount);
                PauseMenuShowModCompatibility(&application->menu,
                    application->serverModCount, installed,
                    event.data.serverMods.downloadsAllowed);
                WindowSetMouseLook(application->window, false);
            }
        }
        else if (event.type == NETWORK_CLIENT_EVENT_READY)
        {
            if (!ApplicationSwitchToNetworkWorld(application,
                    event.data.ready.worldSeed))
            {
                destroyClient = true;
                continue;
            }
            application->networkPeerId = event.data.ready.peerId;
            application->networkReady = true;
            application->networkEverReady = true;
            application->networkHasSnapshot = false;
            application->gameMode = GAME_MODE_WALK;
            memset(&application->networkPendingInput, 0,
                sizeof(application->networkPendingInput));
            PlayerControllerReset(
                &application->player, &application->camera);
            application->menu.networkConnecting = false;
            application->sessionActive = true;
            application->mouseLookBeforeMenu = true;
            ResumeGame(application);
        }
        else if (event.type == NETWORK_CLIENT_EVENT_CONTENT_READY)
        {
            uint8_t* bytes = NULL;
            uint64_t size = 0;
            bool installed = NetworkClientTakeContent(
                    application->networkClient, &bytes, &size);
            if (application->sessionActive)
                ModHostShutdown(&application->modHost);
            if (installed)
            {
                installed = LaiueContentBundleInstall(bytes, size);
            }
            if (bytes != NULL) HeapFree(GetProcessHeap(), 0, bytes);
            ModsRefresh(&application->mods);
            bool applied = installed
                && ModsApplyServerCompatibilitySet(&application->mods,
                    application->serverCompatibilityMods,
                    application->serverModCount);
            if (application->sessionActive)
                InitializeApplicationModHost(application);
            if (applied && SubmitCurrentNetworkMods(application))
            {
                application->menu.contentDownloading = false;
                application->menu.screen = PAUSE_MENU_MULTIPLAYER;
            }
            else
            {
                application->menu.contentDownloading = false;
                application->menu.contentDownloadFailed = true;
                application->menu.networkConnecting = false;
                application->menu.screen = PAUSE_MENU_MULTIPLAYER;
                destroyClient = true;
            }
        }
        else if (event.type == NETWORK_CLIENT_EVENT_PLAYER_STATE
            && application->networkReady
            && event.data.playerState.peerId == application->networkPeerId)
        {
            double errorSquared = 0.0;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                double error = event.data.playerState.position[axis]
                    - application->camera.position[axis];
                errorSquared += error * error;
            }
            bool hardCorrection = !application->networkHasSnapshot
                || errorSquared > 16.0;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                double authoritative =
                    event.data.playerState.position[axis];
                application->camera.position[axis] = hardCorrection
                    ? authoritative
                    : application->camera.position[axis]
                        + (authoritative
                            - application->camera.position[axis]) * 0.25;
            }
            if (hardCorrection)
            {
                PlayerControllerReset(
                    &application->player, &application->camera);
            }
            application->networkHasSnapshot = true;
        }
        else if (event.type == NETWORK_CLIENT_EVENT_BLOCK_DELTA
            && application->networkReady)
        {
            const int64_t* block = event.data.blockDelta.block;
            WorldSetBlock(application->world,
                block[0], block[1], block[2],
                event.data.blockDelta.replacement);
            ChunkStreamingInvalidateBlock(application->chunkStreaming,
                block[0], block[1], block[2]);
        }
        else if (event.type == NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN
            && application->networkReady)
        {
            BlockEffectsSpawnNetworkDrop(&application->blockEffects,
                event.data.blockDrop.id, event.data.blockDrop.block,
                event.data.blockDrop.position);
        }
        else if (event.type == NETWORK_CLIENT_EVENT_BLOCK_DROP_REMOVE
            && application->networkReady)
        {
            BlockEffectsRemoveNetworkDrop(&application->blockEffects,
                event.data.removedDropId);
        }
        else if (event.type == NETWORK_CLIENT_EVENT_INVENTORY_STATE
            && application->networkReady)
        {
            application->inventory.selectedHotbarSlot =
                event.data.inventory.selectedHotbarSlot;
            for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT; ++i)
            {
                application->inventory.slots[i].item =
                    event.data.inventory.slots[i].item;
                application->inventory.slots[i].count =
                    event.data.inventory.slots[i].count;
            }
        }
        else if (event.type == NETWORK_CLIENT_EVENT_REJECTED)
        {
            application->networkReady = false;
            application->menu.networkConnecting = false;
            application->menu.networkRejected = true;
            application->menu.screen = PAUSE_MENU_MULTIPLAYER;
        }
        else if (event.type == NETWORK_CLIENT_EVENT_DISCONNECTED)
        {
            application->networkReady = false;
            application->networkPeerId = 0;
            application->networkHasSnapshot = false;
            if (!application->networkEverReady)
            {
                application->menu.networkConnecting = false;
                if (application->menu.contentDownloading)
                {
                    application->menu.contentDownloading = false;
                    application->menu.contentDownloadFailed = true;
                }
                application->menu.screen = PAUSE_MENU_MULTIPLAYER;
            }
        }
    }
    if (destroyClient)
    {
        NetworkClientDestroy(application->networkClient);
        application->networkClient = NULL;
        application->networkReady = false;
    }
}

// Закрывает меню и возвращает игру: восстанавливает режим взгляда,
// сбрасывает накопленный за паузу ввод.
static void ResumeGame(ApplicationState* application)
{
    if (!application->sessionActive) return;
    application->menu.screen = PAUSE_MENU_CLOSED;
    WindowSetMouseLook(application->window, application->mouseLookBeforeMenu);
    InputResetState(application->input);
}

static void DrawTitleBackground(ApplicationState* application)
{
    if (application->backgroundWidth == 0
        || application->backgroundHeight == 0) return;
    float viewportWidth = (float)application->windowWidth;
    float viewportHeight = (float)application->windowHeight;
    float imageAspect = (float)application->backgroundWidth
        / (float)application->backgroundHeight;
    float viewportAspect = viewportWidth / viewportHeight;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    if (imageAspect > viewportAspect)
    {
        float visible = viewportAspect / imageAspect;
        u0 = (1.0f - visible) * 0.5f;
        u1 = 1.0f - u0;
    }
    else
    {
        float visible = imageAspect / viewportAspect;
        v0 = (1.0f - visible) * 0.5f;
        v1 = 1.0f - v0;
    }
    UiImage(&application->ui, 0.0f, 0.0f,
        viewportWidth, viewportHeight, u0, v0, u1, v1,
        UiColor(255, 255, 255, 255));
}

static void UpdateHotbarSelection(ApplicationState* application,
    float wheelSteps)
{
    uint8_t previous = application->inventory.selectedHotbarSlot;
    for (uint32_t i = 0; i < INVENTORY_HOTBAR_SLOT_COUNT; ++i)
    {
        InputKey key = (InputKey)(INPUT_KEY_1 + i);
        if (InputConsumeKeyPress(application->input, key))
            InventorySelectHotbar(&application->inventory, i);
    }
    if (wheelSteps != 0.0f)
    {
        int32_t slot = application->inventory.selectedHotbarSlot;
        slot += wheelSteps > 0.0f ? 1 : -1;
        if (slot < 0) slot = INVENTORY_HOTBAR_SLOT_COUNT - 1;
        if (slot >= (int32_t)INVENTORY_HOTBAR_SLOT_COUNT) slot = 0;
        InventorySelectHotbar(&application->inventory, (uint32_t)slot);
    }
    if (application->networkReady
        && previous != application->inventory.selectedHotbarSlot)
    {
        NetworkClientSendSelectedHotbarSlot(application->networkClient,
            application->inventory.selectedHotbarSlot);
    }
}

static void OnFrame(void* userData)
{
    ApplicationState* application = userData;

    PumpNetwork(application);

    // Смена состава модов (тумблер на вкладке, правка enabled.txt):
    // хост перезагружает цепочку DLL в порядке включения.
    if (application->sessionActive
        && application->mods.revision != application->appliedModsRevision)
    {
        ModHostSync(&application->modHost, &application->mods);
        application->appliedModsRevision = application->mods.revision;
    }

    bool escapePressed = InputConsumeKeyPress(
        application->input, INPUT_KEY_ESCAPE);
    bool menuOpen = application->menu.screen != PAUSE_MENU_CLOSED;

    if (application->inventoryOpen && escapePressed)
    {
        application->inventoryOpen = false;
        WindowSetMouseLook(application->window, true);
        InputResetState(application->input);
        escapePressed = false;
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_F3))
    {
        application->showDiagnostics = !application->showDiagnostics;
    }

    if (escapePressed && !menuOpen && application->sessionActive)
    {
        // Открытие меню: курсор освобождается, режим взгляда запоминается.
        PauseMenuOpen(&application->menu);
        application->mouseLookBeforeMenu =
            WindowIsMouseLookEnabled(application->window);
        WindowSetMouseLook(application->window, false);
        menuOpen = true;
        escapePressed = false;
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(application->window,
            !InputIsKeyDown(application->input, INPUT_KEY_SHIFT));
    }

    if (!menuOpen && InputConsumeKeyPress(application->input, INPUT_KEY_V))
    {
        RendererSetVerticalSync(application->renderer,
            !RendererIsVerticalSyncEnabled(application->renderer));
    }

    if (WindowConsumeFocusLoss(application->window))
    {
        InputResetState(application->input);
    }

    if (application->sessionActive && !menuOpen
        && InputConsumeKeyPress(application->input, INPUT_KEY_E))
    {
        application->inventoryOpen = !application->inventoryOpen;
        WindowSetMouseLook(application->window,
            !application->inventoryOpen);
        InputResetState(application->input);
    }

    if (application->sessionActive && !menuOpen
        && !application->inventoryOpen && !application->networkReady
        && InputConsumeKeyPress(application->input, INPUT_KEY_G))
    {
        ToggleGameMode(application);
    }

    if (application->sessionActive && !menuOpen
        && !application->inventoryOpen && !application->networkReady
        && InputConsumeKeyPress(application->input, INPUT_KEY_T)
        && !SquareAbsoluteX(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (WindowConsumeResize(application->window))
    {
        int32_t clientWidth;
        int32_t clientHeight;
        WindowGetClientSize(application->window,
            &clientWidth, &clientHeight);
        if (clientWidth > 0 && clientHeight > 0)
        {
            application->windowWidth = clientWidth;
            application->windowHeight = clientHeight;
            RendererResize(application->renderer,
                clientWidth, clientHeight);
        }
    }

    double currentTimeSeconds = PlatformTimeSeconds();
    float deltaSeconds = (float)(
        currentTimeSeconds - application->previousTimeSeconds);
    application->previousTimeSeconds = currentTimeSeconds;
    if (deltaSeconds > 0.1f)
    {
        deltaSeconds = 0.1f;
    }

    bool mouseLookEnabled =
        WindowIsMouseLookEnabled(application->window);
    int32_t mouseDeltaX = 0;
    int32_t mouseDeltaY = 0;
    if (mouseLookEnabled)
    {
        InputGetMouseDelta(application->input,
            &mouseDeltaX, &mouseDeltaY);
    }

    bool gameplayActive = application->sessionActive && !menuOpen
        && !application->inventoryOpen;
    if (gameplayActive)
    {
        UpdatePlayer(application,
            deltaSeconds, mouseDeltaX, mouseDeltaY);
        // Игровое время идёт, пока не открыто меню (пауза).
        application->settings.timeOfDayHours = GameTimeAdvance(
            application->settings.timeOfDayHours,
            application->settings.timeSpeed,
            g_applicationConfiguration.dayLengthMinutes,
            deltaSeconds);

        // Кадровые хуки DLL-модов — после игрока, до отрисовки.
        ModHostDispatchFrame(&application->modHost, deltaSeconds);
    }

    if (application->sessionActive && !application->networkReady
        && !RebaseWorldIfNeeded(application))
    {
        WindowRequestClose(application->window);
        return;
    }

    if (mouseLookEnabled && gameplayActive)
    {
        HandleBlockEditing(application, deltaSeconds);
    }

    int64_t cameraBlockPosition[3] = { 0, 0, 0 };
    float relativeEyePosition[3] = { 0.0f, 0.0f, 0.0f };
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cameraBlockPosition[axis] =
            FloorDoubleToInt64(application->camera.position[axis]);
        relativeEyePosition[axis] = (float)(
            application->camera.position[axis]
            - (double)cameraBlockPosition[axis]);
    }

    if (application->sessionActive)
    {
        ChunkStreamingSetCenter(application->chunkStreaming,
            cameraBlockPosition[0] >> CHUNK_SIZE_LOG2,
            cameraBlockPosition[1] >> CHUNK_SIZE_LOG2,
            cameraBlockPosition[2] >> CHUNK_SIZE_LOG2);
        ChunkStreamingPump(application->chunkStreaming);
        if (application->gameMode == GAME_MODE_SURVIVAL)
        {
            BlockEffectsUpdate(&application->blockEffects,
                application->world, &application->inventory,
                application->camera.position, deltaSeconds,
                !application->networkReady);
        }
        else
        {
            BlockEffectsClear(&application->blockEffects);
            application->breaking.active = false;
            application->breaking.progress = 0.0f;
        }
    }

    // HUD и меню собираются одним UI-контекстом. При закрытом меню ввод
    // виджетам не передаётся, но шрифт и HUD всё равно доступны с первого кадра.
    int32_t cursorX = -1;
    int32_t cursorY = -1;
    bool mouseDown = false;
    bool mousePressed = false;
    bool uiInteractive = menuOpen || application->inventoryOpen;
    if (uiInteractive)
    {
        WindowGetCursorClientPosition(application->window,
            &cursorX, &cursorY);
        mouseDown = InputIsMouseButtonDown(
            application->input, INPUT_MOUSE_BUTTON_LEFT);
        mousePressed = InputWasMouseButtonPressed(
            application->input, INPUT_MOUSE_BUTTON_LEFT);
    }

    float wheelSteps = WindowConsumeMouseWheelSteps(application->window);
    if (application->sessionActive && !uiInteractive)
        UpdateHotbarSelection(application, wheelSteps);
    bool uiReady = UiBegin(&application->ui,
        application->windowWidth, application->windowHeight,
        (float)cursorX, (float)cursorY,
        mouseDown, mousePressed,
        uiInteractive ? wheelSteps : 0.0f, deltaSeconds);
    if (uiReady)
    {
        if (application->ui.fontDirty
            && RendererUiSetFontAtlas(application->renderer,
                application->ui.font.atlas,
                application->ui.font.atlasWidth,
                application->ui.font.atlasHeight))
        {
            application->ui.fontDirty = false;
        }

        if (!application->sessionActive) DrawTitleBackground(application);

        if (menuOpen)
        {
            PauseMenuAction action = PauseMenuUpdate(&application->menu,
                &application->ui, &application->settings,
                application->renderer, application->window,
                &application->mods,
                g_applicationConfiguration.dayLengthMinutes,
                application->windowWidth, application->windowHeight,
                escapePressed);
            if (action == PAUSE_MENU_ACTION_QUIT)
            {
                WindowRequestClose(application->window);
                return;
            }
            if (action == PAUSE_MENU_ACTION_RESUME)
            {
                ResumeGame(application);
                application->ui.quadCount = 0;
            }
            if (action == PAUSE_MENU_ACTION_RETURN_TITLE)
            {
                ApplicationCloseSession(application, true, true);
                PauseMenuOpenTitle(&application->menu);
                WindowSetMouseLook(application->window, false);
                application->ui.quadCount = 0;
                DrawTitleBackground(application);
                PauseMenuUpdate(&application->menu, &application->ui,
                    &application->settings, application->renderer,
                    application->window, &application->mods,
                    g_applicationConfiguration.dayLengthMinutes,
                    application->windowWidth, application->windowHeight,
                    false);
            }
            if (action == PAUSE_MENU_ACTION_PLAY_WORLD)
            {
                NetworkClientDestroy(application->networkClient);
                application->networkClient = NULL;
                if (ApplicationSwitchWorld(application,
                        application->menu.selectedWorld))
                {
                    application->mouseLookBeforeMenu = true;
                    ResumeGame(application);
                    application->ui.quadCount = 0;
                }
            }
            if (action == PAUSE_MENU_ACTION_CONNECT_LOCAL)
            {
                NetworkClientDestroy(application->networkClient);
                application->networkClient = NetworkClientCreateLoopback(
                    application->menu.selectedServerPort != 0
                        ? application->menu.selectedServerPort
                        : LAIUE_NETWORK_DEFAULT_PORT);
                if (application->networkClient == NULL)
                {
                    application->menu.networkConnecting = false;
                    application->menu.networkRejected = true;
                }
            }
            if (action == PAUSE_MENU_ACTION_APPLY_SERVER_MODS)
            {
                if (ModsApplyServerCompatibilitySet(&application->mods,
                        application->serverCompatibilityMods,
                        application->serverModCount)
                    && SubmitCurrentNetworkMods(application))
                {
                    application->menu.screen = PAUSE_MENU_MULTIPLAYER;
                }
                else
                {
                    application->menu.networkConnecting = false;
                    application->menu.networkRejected = true;
                    application->menu.screen = PAUSE_MENU_MULTIPLAYER;
                }
            }
            if (action == PAUSE_MENU_ACTION_DOWNLOAD_SERVER_CONTENT)
            {
                if (!NetworkClientRequestContent(application->networkClient))
                {
                    application->menu.contentDownloading = false;
                    application->menu.contentDownloadFailed = true;
                }
            }
            if (action == PAUSE_MENU_ACTION_CANCEL_CONNECT)
            {
                NetworkClientDestroy(application->networkClient);
                application->networkClient = NULL;
                application->networkReady = false;
            }

            if (application->menu.saveRequested
                && application->sessionActive
                && !application->networkEverReady)
            {
                application->menu.saveRequested = false;
                SaveGameWriteAll(application->world,
                    &application->camera, application->gameMode,
                    application->settings.timeOfDayHours,
                    application->worldSeed, &application->mods,
                    &application->inventory);
            }
        }
        else if (application->sessionActive)
        {
            uint32_t timeMinutes = (uint32_t)(
                application->settings.timeOfDayHours * 60.0f) % 1440u;
            ChunkStreamingStats streamingStats;
            RendererStats rendererStats;
            ChunkStreamingGetStats(application->chunkStreaming,
                &streamingStats);
            RendererGetStats(application->renderer, &rendererStats);
            GameHudDraw(&application->hud, &application->ui,
                application->world, &application->player,
                application->gameMode, application->framesPerSecond,
                timeMinutes, &streamingStats, &rendererStats,
                application->showDiagnostics,
                application->networkReady, application->networkPeerId,
                cameraBlockPosition,
                application->windowWidth, application->windowHeight);
            uint8_t selectedBeforeUi =
                application->inventory.selectedHotbarSlot;
            InventoryUiDraw(&application->ui, &application->inventory,
                application->gameMode, application->inventoryOpen,
                !application->networkReady,
                application->breaking.progress,
                application->windowWidth, application->windowHeight);
            if (application->networkReady
                && selectedBeforeUi
                    != application->inventory.selectedHotbarSlot)
            {
                NetworkClientSendSelectedHotbarSlot(
                    application->networkClient,
                    application->inventory.selectedHotbarSlot);
            }
        }
    }
    else if (menuOpen && application->sessionActive)
    {
        // Шрифт недоступен — меню нарисовать нечем, не запираем игрока.
        ResumeGame(application);
    }

    RendererFrameSetup frameSetup;
    memset(&frameSetup, 0, sizeof(frameSetup));
    frameSetup.gamma = 1.0f;
    if (application->sessionActive)
    {
        // Описание кадра: обычная перспектива или панорама.
        float viewMatrix[16];
        CameraGetViewMatrix(&application->camera,
            relativeEyePosition, viewMatrix);
        PanoramaBuildFrameSetup(&application->panoramaCache,
            application->settings.projection,
            (float)application->settings.fovDegrees,
            application->windowWidth, application->windowHeight,
            g_applicationConfiguration.nearPlane,
            g_applicationConfiguration.farPlane,
            viewMatrix, &frameSetup);

        DayLighting lighting;
        GameTimeGetLighting(application->settings.timeOfDayHours, &lighting);
        for (int32_t channel = 0; channel < 3; ++channel)
        {
            frameSetup.sunDirection[channel] = lighting.sunDirection[channel];
            frameSetup.sunColor[channel] = lighting.sunColor[channel];
            frameSetup.ambientColor[channel] = lighting.ambientColor[channel];
            frameSetup.skyColor[channel] = lighting.skyColor[channel];
        }
        frameSetup.gamma = (float)application->settings.gamma * 0.01f;
    }

    if (RendererBeginFrame(application->renderer, &frameSetup))
    {
        for (uint32_t pass = 0; pass < frameSetup.passCount; ++pass)
        {
            RendererBeginScenePass(application->renderer, pass);
            ChunkStreamingDraw(application->chunkStreaming,
                frameSetup.passes[pass].viewProjection,
                cameraBlockPosition);
            if (application->gameMode == GAME_MODE_SURVIVAL)
            {
                BlockEffectsDraw(&application->blockEffects,
                    application->renderer, cameraBlockPosition);
            }
        }

        if (uiReady && application->ui.quadCount > 0)
        {
            RendererUiQueue(application->renderer,
                application->ui.quads, application->ui.quadCount);
        }

        if (!RendererEndFrame(application->renderer))
        {
            WindowRequestClose(application->window);
            return;
        }
        RecordPresentedFrame(application);
    }

    InputEndFrame(application->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration windowConfiguration = {
        .title = g_applicationConfiguration.windowTitle,
        .width = g_applicationConfiguration.windowWidth,
        .height = g_applicationConfiguration.windowHeight,
    };

    Window* window = WindowCreate(&windowConfiguration);
    if (window == NULL)
    {
        return;
    }

    Input* input = InputCreate(WindowGetNativeHandle(window));
    if (input == NULL)
    {
        WindowDestroy(window);
        return;
    }

    int32_t clientWidth;
    int32_t clientHeight;
    WindowGetClientSize(window, &clientWidth, &clientHeight);

    Renderer* renderer = RendererCreate(
        WindowGetNativeHandle(window), clientWidth, clientHeight);
    if (renderer == NULL)
    {
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    // Состояние приложения крупное (квады интерфейса, кеш панорамы) —
    // живёт на куче: стек без CRT не имеет проб роста (__chkstk),
    // поэтому кадры функций обязаны оставаться меньше страницы.
    ApplicationState* application = HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*application));
    if (application == NULL)
    {
        RendererDestroy(renderer);
        InputDestroy(input);
        WindowDestroy(window);
        return;
    }

    double startTimeSeconds = PlatformTimeSeconds();
    application->window = window;
    application->input = input;
    application->renderer = renderer;
    application->gameMode = GAME_MODE_CREATIVE;
    application->windowWidth = clientWidth;
    application->windowHeight = clientHeight;
    application->previousTimeSeconds = startTimeSeconds;
    application->fpsSampleStartSeconds = startTimeSeconds;
    application->settings.fovDegrees =
        g_applicationConfiguration.defaultFieldOfViewDegrees;
    application->settings.projection = RENDER_PROJECTION_AUTO;
    application->settings.timeOfDayHours =
        g_applicationConfiguration.startTimeOfDayHours;
    application->settings.timeSpeed = TIME_SPEED_NORMAL;
    application->settings.mouseSensitivityPercent = 100;
    application->settings.flySpeedBlocks =
        (int32_t)g_applicationConfiguration.cameraSpeed;
    application->settings.wireframe = false;
    application->settings.gamma = 100;

    application->networkClient = NULL;

    RendererUiLoadBackground(renderer, L"ui\\main_menu_background.png",
        &application->backgroundWidth, &application->backgroundHeight);
    application->settings.selectedTexturePack = -1;
    application->settings.selectedShaderPack = -1;
    application->settings.applyTexturePack = false;
    application->settings.applyShaderPack = false;

    GameHudInit(&application->hud);
    PlayerControllerConfig playerConfiguration;
    PlayerControllerGetDefaultConfig(&playerConfiguration);
    PlayerControllerInit(&application->player, &playerConfiguration);

    // Состояние из сохранения: seed запомнен для записи, время суток
    // продолжается с сохранённой минуты.
    application->worldSeed = g_applicationConfiguration.worldSeed;

    // Моды: каталог перечитывается на старте, включённые применяются
    // первым кадром (сравнение ревизий в OnFrame). Хост DLL-модов
    // получает адреса подсистем — они живут в application на куче.
    ModsInit(&application->mods, L"enabled.txt");
    ModsRefresh(&application->mods);

    CameraInit(&application->camera,
        0.0, 0.0, g_applicationConfiguration.spawnHeight,
        0.0f, -0.4f);
    InitializeSessionInventory(application);
    PauseMenuOpenTitle(&application->menu);
    WindowSetMouseLook(window, false);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, application);

    ApplicationCloseSession(application, true, true);
    UiRelease(&application->ui);
    RendererDestroy(renderer);
    InputDestroy(input);
    WindowDestroy(window);
    HeapFree(GetProcessHeap(), 0, application);
}
