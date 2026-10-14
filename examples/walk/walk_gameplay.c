#include "walk_gameplay.h"
#include "walk_math.h"
#include "walk_world.h"
#include "math/scalar.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum
{
    WALK_UNIT = 1000,
    WALK_CELL_BLOCKS = 1024,
    WALK_CHUNK_BLOCKS = 64
};

static bool AddBlock(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *out = a + b;
    return true;
}

static bool SubtractBlock(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))
        return false;
    *out = a - b;
    return true;
}

static int64_t FloorDivide(int64_t value, int64_t divisor)
{
    const int64_t quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}

static bool FloorBlock(double value, int64_t *out)
{
    if (!WalkMathFinite(value) || !(value >= -0x1p63 && value < 0x1p63))
        return false;
    *out = (int64_t)WalkMathFloor(value);
    return true;
}

static bool ServicePrefix(const void *table, uint32_t actualSize, uint32_t abi)
{
    /* All private consumers check the actual prefix before reading its words. */
    if (table == NULL || actualSize < 2u * sizeof(uint32_t))
        return false;
    uint32_t words[2];
    memcpy(words, table, sizeof(words));
    return words[0] >= sizeof(words) && words[1] == abi;
}

#define HAS_FIELD(type, table, actual, field)                                                      \
    (WalkServiceFieldPresent((actual), (table)->structSize, offsetof(type, field),                 \
                             sizeof((table)->field)) &&                                            \
     (table)->field != NULL)

static WalkGameplayStatus State(const WalkGameplay *game)
{
    if (game == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (!game->initialized)
        return WALK_GAMEPLAY_INVALID_STATE;
    if (game->failed)
        return game->failure;
    return game->walkContext.providerFailed ? WALK_GAMEPLAY_PROVIDER_FAILURE : WALK_GAMEPLAY_OK;
}

static WalkGameplayStatus Fail(WalkGameplay *game, WalkGameplayStatus failure)
{
    game->failed = true;
    game->failure = failure;
    return failure;
}

WalkGameplayStatus WalkGameplayHealth(const WalkGameplay *game)
{
    return State(game);
}

static bool PositionParts(const LaiueCharacterPositionV1 *position, int64_t blocks[3],
                          double fractions[3])
{
    if (!WalkPositionAxisToBlock(position->cellX, position->localX, &blocks[0]) ||
        !WalkPositionAxisToBlock(position->cellY, position->localY, &blocks[1]))
        return false;
    blocks[2] = FloorDivide(position->localZ, WALK_UNIT);
    const int64_t local[3] = {position->localX, position->localY, position->localZ};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int64_t remainder = local[axis] % WALK_UNIT;
        if (remainder < 0)
            remainder += WALK_UNIT;
        fractions[axis] = (double)remainder / WALK_UNIT;
    }
    return true;
}

static bool ConfigValid(const WalkGameplayConfig *config)
{
    int64_t blocks[3];
    double fractions[3];
    return config != NULL && PositionParts(&config->initialPosition, blocks, fractions) &&
           WalkMathFinite(config->ragdollSpawnHeight) &&
           WalkMathAbs(config->ragdollSpawnHeight) < 0x1p40 && config->characterHalfExtent >= 0 &&
           config->characterHalfExtent < 512000 && config->stableIdBase != 0u &&
           config->stableIdBase <= UINT64_MAX - 13u &&
           (config->requiredCapabilities & ~UINT32_C(63)) == 0u &&
           WalkMathFinite(config->initialYaw) && WalkMathFinite(config->initialPitch) &&
           WalkMathFinite(config->fovRadians) && config->fovRadians > 0.0f &&
           config->fovRadians < 3.1415927f && WalkMathFinite(config->nearPlane) &&
           WalkMathFinite(config->farPlane) && config->nearPlane > 0.0f &&
           config->farPlane > config->nearPlane && config->selectedMaterial >= 1u &&
           config->selectedMaterial <= 3u;
}

void WalkGameplayConfigDefault(WalkGameplayConfig *out)
{
    if (out == NULL)
        return;
    *out = (WalkGameplayConfig){
        .initialPosition = {.localZ = 1400},
        .ragdollSpawnHeight = 1.02,
        .characterHalfExtent = 400,
        .stableIdBase = UINT64_C(0x57414C4B52414744),
        .initialYaw = 0.0f,
        .initialPitch = -0.14f,
        .fovRadians = 1.04719755f,
        .nearPlane = 0.05f,
        .farPlane = 4096.0f,
        .selectedMaterial = 1u,
    };
}

static void SetOrientation(WalkGameplay *game, float yaw, float pitch)
{
    /* Match Camera's existing wrap and clamp even when scene is absent. */
    if (WalkMathAbs(yaw) > 100000000.0)
        yaw = 0.0f;
    const float period = 6.2831853f;
    yaw -= (float)(int32_t)(yaw / period) * period;
    if (yaw > 3.1415927f)
        yaw -= period;
    else if (yaw < -3.1415927f)
        yaw += period;
    if (pitch > 1.570796f)
        pitch = 1.570796f;
    else if (pitch < -1.570796f)
        pitch = -1.570796f;
    game->camera.yaw = yaw;
    game->camera.pitch = pitch;
    const LaiueSceneServiceV1 *scene = game->services.scene;
    if (ServicePrefix(scene, game->services.sceneSize, LAIUE_SCENE_SERVICE_ABI_VERSION_1) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, game->services.sceneSize, cameraInit))
        scene->cameraInit(&game->camera, game->camera.position[0], game->camera.position[1],
                          game->camera.position[2], yaw, pitch);
}

static void QueryBodyBlock(void *opaque, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *out)
{
    if (out == NULL)
        return;
    out->flags = VOXEL_BLOCK_PHYSICS_SOLID;
    out->friction = 0.75f;
    WalkGameplay *game = (WalkGameplay *)opaque;
    int64_t provider[3];
    const int64_t local[3] = {x, y, z};
    if (game == NULL)
        return;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!AddBlock(game->ragdollBlockOrigin[axis], local[axis], &provider[axis]))
        {
            game->walkContext.providerFailed = true;
            return;
        }
    if (provider[2] < INT32_MIN || provider[2] > INT32_MAX)
    {
        game->walkContext.providerFailed = true;
        return;
    }
    const LaiueVoxelCoordV1 coordinate = {provider[0], provider[1], (int32_t)provider[2]};
    LaiueVoxelBlockV1 block = {0};
    if (WalkGetBlock(&game->walkProvider, &coordinate, &block) != 0u && block.material == 0u)
        out->flags = 0u;
}

static void CreateSparseWorld(WalkGameplay *game)
{
    const LaiueVoxelServiceV1 *voxel = game->services.voxel;
    const uint32_t bytes = game->services.voxelSize;
    if (!ServicePrefix(voxel, bytes, LAIUE_VOXEL_SERVICE_ABI_VERSION_1) ||
        !HAS_FIELD(LaiueVoxelServiceV1, voxel, bytes, destroy) ||
        !HAS_FIELD(LaiueVoxelServiceV1, voxel, bytes, getProvider))
        return;
    const bool contextCreate =
        HAS_FIELD(LaiueVoxelServiceV1, voxel, bytes, createWithContext) &&
        WalkServiceFieldPresent(bytes, voxel->structSize, offsetof(LaiueVoxelServiceV1, context),
                                sizeof(voxel->context)) &&
        voxel->context != NULL;
    if (!contextCreate && !HAS_FIELD(LaiueVoxelServiceV1, voxel, bytes, create))
        return;
    const LaiueVoxelWorldConfigV1 config = {
        .structSize = sizeof(config),
        .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        .defaultBlock = {0u, 0u},
    };
    const uint32_t created = contextCreate
                                 ? voxel->createWithContext(voxel->context, &config, &game->world)
                                 : voxel->create(&config, &game->world);
    LaiueVoxelProviderV1 *provider = &game->walkContext.sparse;
    if (created != 0u && game->world != NULL && voxel->getProvider(game->world, provider) != 0u &&
        ServicePrefix(provider, sizeof(*provider), LAIUE_VOXEL_ABI_VERSION_1))
    {
        if (!WalkServiceFieldPresent(sizeof(*provider), provider->structSize,
                                     offsetof(LaiueVoxelProviderV1, getBlockState),
                                     sizeof(provider->getBlockState)))
            provider->getBlockState = NULL;
        if (!WalkServiceFieldPresent(sizeof(*provider), provider->structSize,
                                     offsetof(LaiueVoxelProviderV1, getBlock),
                                     sizeof(provider->getBlock)))
            provider->getBlock = NULL;
        if (provider->getBlock != NULL || provider->getBlockState != NULL)
        {
            game->capabilities |= WALK_GAMEPLAY_CAP_SPARSE_WORLD;
            if (provider->getBlockState != NULL &&
                HAS_FIELD(LaiueVoxelServiceV1, voxel, bytes, setBlock))
                game->capabilities |= WALK_GAMEPLAY_CAP_EDIT;
            return;
        }
    }
    if (game->world != NULL)
        voxel->destroy(game->world);
    game->world = NULL;
    *provider = (LaiueVoxelProviderV1){0};
}

static void CreateActor(WalkGameplay *game)
{
    if (WalkPhysicsBind(&game->physicsContext, game->services.physics, game->services.physicsSize))
    {
        int64_t blocks[3];
        double fractions[3];
        if (PositionParts(&game->config.initialPosition, blocks, fractions))
        {
            game->ragdollBlockOrigin[0] = blocks[0];
            game->ragdollBlockOrigin[1] = blocks[1];
            const double origin[3] = {fractions[0], fractions[1], game->config.ragdollSpawnHeight};
            game->ragdollCollision = (VoxelCollisionSource){
                .context = game,
                .queryBlockPhysics = QueryBodyBlock,
            };
            if (WalkHumanoidInitialize(&game->physicsContext, &game->ragdoll,
                                       &game->ragdollCollision, origin, game->config.stableIdBase,
                                       &game->ragdollRigidSettings, &game->ragdollSettings,
                                       &game->ragdollScratch, &game->ragdollScratchBytes))
            {
                game->ragdollReady = true;
                game->actor = WALK_GAMEPLAY_ACTOR_RAGDOLL;
                game->capabilities |= WALK_GAMEPLAY_CAP_RAGDOLL | WALK_GAMEPLAY_CAP_REBASE;
                game->grounded = WalkHumanoidIsGrounded(&game->physicsContext, &game->ragdoll,
                                                        &game->ragdollCollision);
                return;
            }
        }
        WalkPhysicsUnbind(&game->physicsContext);
        memset(game->ragdollBlockOrigin, 0, sizeof(game->ragdollBlockOrigin));
    }
    const LaiueCharacterServiceV1 *character = game->services.character;
    const uint32_t bytes = game->services.characterSize;
    if (!ServicePrefix(character, bytes, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, create) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, destroy) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, step) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, getPosition) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, setPosition) ||
        !HAS_FIELD(LaiueCharacterServiceV1, character, bytes, isGrounded))
        return;
    if (character->create(&game->characterCollision, game->config.characterHalfExtent,
                          &game->controller) != 0u &&
        game->controller != NULL &&
        character->setPosition(game->controller, &game->config.initialPosition, 1u) != 0u &&
        character->getPosition(game->controller, &game->lastPosition) != 0u)
    {
        game->actor = WALK_GAMEPLAY_ACTOR_CHARACTER;
        game->grounded = character->isGrounded(game->controller) != 0u;
        game->capabilities |= WALK_GAMEPLAY_CAP_CHARACTER;
        const LaiueVoxelServiceV1 *voxel = game->services.voxel;
        if (game->world != NULL &&
            HAS_FIELD(LaiueCharacterServiceV1, character, bytes, rebaseOrigin) &&
            HAS_FIELD(LaiueVoxelServiceV1, voxel, game->services.voxelSize, rebase))
            game->capabilities |= WALK_GAMEPLAY_CAP_REBASE;
        return;
    }
    if (game->controller != NULL)
        character->destroy(game->controller);
    game->controller = NULL;
    game->lastPosition = game->config.initialPosition;
}

WalkGameplayStatus WalkGameplayInit(WalkGameplay *game, const WalkGameplayServices *services,
                                    const WalkGameplayConfig *config)
{
    if (game == NULL || services == NULL || !ConfigValid(config))
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (game->initialized || game->revision == UINT64_MAX)
        return WALK_GAMEPLAY_INVALID_STATE;
    const WalkGameplayServices selectedServices = *services;
    const WalkGameplayConfig selectedConfig = *config;
    services = &selectedServices;
    config = &selectedConfig;
    const uint64_t revision = game->revision + 1u;
    memset(game, 0, sizeof(*game));
    game->revision = revision;
    game->services = *services;
    game->config = *config;
    game->lastPosition = config->initialPosition;
    game->firstPerson = config->firstPerson;
    game->selectedMaterial = config->selectedMaterial;
    game->jumpArmed = true;
    game->walkProvider = (LaiueVoxelProviderV1){
        .structSize = sizeof(game->walkProvider),
        .abiVersion = LAIUE_VOXEL_ABI_VERSION_1,
        .context = &game->walkContext,
        .getBlock = WalkGetBlock,
    };
    game->characterCollision = (LaiueCharacterCollisionV1){
        .structSize = sizeof(game->characterCollision),
        .abiVersion = LAIUE_CHARACTER_ABI_VERSION_1,
        .context = &game->walkProvider,
        .sweepAabb = WalkSweepAabb,
    };
    CreateSparseWorld(game);
    CreateActor(game);
    const LaiueSceneServiceV1 *scene = services->scene;
    const LaiueSceneMathServiceV1 *math = services->sceneMath;
    if (ServicePrefix(scene, services->sceneSize, LAIUE_SCENE_SERVICE_ABI_VERSION_1) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, services->sceneSize, cameraInit) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, services->sceneSize, cameraGetForwardVector) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, services->sceneSize, cameraGetViewMatrix) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, services->sceneSize, cameraGetProjectionMatrix) &&
        ServicePrefix(math, services->sceneMathSize, LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1) &&
        HAS_FIELD(LaiueSceneMathServiceV1, math, services->sceneMathSize, matrix4Multiply))
        game->capabilities |= WALK_GAMEPLAY_CAP_CAMERA;
    SetOrientation(game, config->initialYaw, config->initialPitch);
    game->initialized = true;
    const bool invalidCamera =
        !WalkMathFinite(game->camera.yaw) || !WalkMathFinite(game->camera.pitch) ||
        !WalkMathFinite(game->camera.position[0]) || !WalkMathFinite(game->camera.position[1]) ||
        !WalkMathFinite(game->camera.position[2]);
    if (game->walkContext.providerFailed || invalidCamera ||
        (game->capabilities & config->requiredCapabilities) != config->requiredCapabilities)
    {
        const WalkGameplayStatus status = game->walkContext.providerFailed || invalidCamera
                                              ? WALK_GAMEPLAY_PROVIDER_FAILURE
                                              : WALK_GAMEPLAY_UNSUPPORTED;
        WalkGameplayRelease(game);
        return status;
    }
    return WALK_GAMEPLAY_OK;
}

void WalkGameplayRelease(WalkGameplay *game)
{
    if (game == NULL || !game->initialized)
        return;
    const uint64_t revision = game->revision;
    if (game->ragdollReady || game->ragdollScratch != NULL)
        WalkHumanoidRelease(&game->physicsContext, &game->ragdoll, &game->ragdollScratch);
    if (game->controller != NULL)
        game->services.character->destroy(game->controller);
    if (game->world != NULL)
        game->services.voxel->destroy(game->world);
    memset(game, 0, sizeof(*game));
    game->revision = revision == UINT64_MAX ? revision : revision + 1u;
}

WalkGameplayStatus WalkGameplaySubmitInput(WalkGameplay *game, WalkGameplayInput *input)
{
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if (input == NULL || !WalkMathFinite(input->strafe) || !WalkMathFinite(input->forward) ||
        (input->pulses & ~UINT32_C(7)) != 0u)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    WalkGameplayInput next = *input;
    double scale = WalkMathAbs(next.strafe);
    if (WalkMathAbs(next.forward) > scale)
        scale = WalkMathAbs(next.forward);
    /* Scale before squaring so any finite analogue vector is accepted safely. */
    if (scale > 1.0)
    {
        next.strafe /= scale;
        next.forward /= scale;
    }
    const double length = ScalarSqrtDouble(next.strafe * next.strafe + next.forward * next.forward);
    if (length > 1.0 || scale > 1.0)
    {
        next.strafe /= length;
        next.forward /= length;
    }
    game->pendingPulses |= next.pulses;
    next.pulses = 0u;
    game->input = next;
    if (!next.jumpHeld)
        game->jumpArmed = true;
    input->pulses = 0u;
    return WALK_GAMEPLAY_OK;
}

void WalkGameplayResetInput(WalkGameplay *game)
{
    if (game == NULL)
        return;
    game->input = (WalkGameplayInput){0};
    game->pendingPulses = 0u;
    game->previousJumpHeld = false;
    game->humanoidController.previousJumpInput = false;
    game->jumpArmed = false;
}

WalkGameplayStatus WalkGameplayOrient(WalkGameplay *game, float yaw, float pitch,
                                      WalkGameplayOrientationMode mode)
{
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if (!WalkMathFinite(yaw) || !WalkMathFinite(pitch) ||
        (mode != WALK_GAMEPLAY_ORIENT_ABSOLUTE && mode != WALK_GAMEPLAY_ORIENT_RELATIVE))
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (game->revision == UINT64_MAX)
        return WALK_GAMEPLAY_INVALID_STATE;
    if (mode == WALK_GAMEPLAY_ORIENT_RELATIVE)
    {
        const double nextYaw = (double)game->camera.yaw + yaw;
        const double nextPitch = (double)game->camera.pitch + pitch;
        if (!WalkMathFinite(nextYaw) || !WalkMathFinite(nextPitch) ||
            WalkMathAbs(nextYaw) > 3.402823466e38 || WalkMathAbs(nextPitch) > 3.402823466e38)
            return WALK_GAMEPLAY_INVALID_ARGUMENT;
        yaw = (float)nextYaw;
        pitch = (float)nextPitch;
    }
    SetOrientation(game, yaw, pitch);
    ++game->revision;
    if (!WalkMathFinite(game->camera.yaw) || !WalkMathFinite(game->camera.pitch) ||
        !WalkMathFinite(game->camera.position[0]) || !WalkMathFinite(game->camera.position[1]) ||
        !WalkMathFinite(game->camera.position[2]))
        return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
    return WALK_GAMEPLAY_OK;
}

WalkGameplayStatus WalkGameplaySetView(WalkGameplay *game, bool firstPerson, uint8_t material)
{
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if (material < 1u || material > 3u)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (game->firstPerson == firstPerson && game->selectedMaterial == material)
        return WALK_GAMEPLAY_OK;
    if (game->revision == UINT64_MAX)
        return WALK_GAMEPLAY_INVALID_STATE;
    game->firstPerson = firstPerson;
    game->selectedMaterial = material;
    ++game->revision;
    return WALK_GAMEPLAY_OK;
}

WalkGameplayStatus WalkGameplaySnapshot(const WalkGameplay *game, WalkGameplaySpatialSnapshot *out)
{
    if (out == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    *out = (WalkGameplaySpatialSnapshot){0};
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    WalkGameplaySpatialSnapshot next = {
        .owner = game,
        .tick = game->tick,
        .revision = game->revision,
        .actor = game->actor,
        .grounded = game->grounded,
        .firstPerson = game->firstPerson,
        .selectedMaterial = game->selectedMaterial,
    };
    memcpy(next.providerFrameOrigin, game->providerFrameOrigin, sizeof(next.providerFrameOrigin));
    if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
    {
        if (!WalkBodyLocalPosition(&game->physicsContext,
                                   &game->ragdoll.bodies[game->ragdoll.rootBody], next.rootLocal))
            return WALK_GAMEPLAY_PROVIDER_FAILURE;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            int64_t local;
            if (!FloorBlock(next.rootLocal[axis], &local) ||
                !AddBlock(game->ragdollBlockOrigin[axis], local, &next.centerBlock[axis]))
                return WALK_GAMEPLAY_INVALID_STATE;
            next.rootFraction[axis] = next.rootLocal[axis] - (double)local;
        }
    }
    else if (!PositionParts(&game->lastPosition, next.centerBlock, next.rootFraction))
        return WALK_GAMEPLAY_INVALID_STATE;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int64_t absolute;
        if (!AddBlock(next.centerBlock[axis], next.providerFrameOrigin[axis], &absolute))
            return WALK_GAMEPLAY_INVALID_STATE;
        next.renderOriginBlock[axis] =
            FloorDivide(next.centerBlock[axis], WALK_CHUNK_BLOCKS) * WALK_CHUNK_BLOCKS;
        if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
        {
            int64_t relative;
            if (!SubtractBlock(next.renderOriginBlock[axis], game->ragdollBlockOrigin[axis],
                               &relative))
                return WALK_GAMEPLAY_INVALID_STATE;
            next.ragdollRenderOrigin[axis] = (double)relative;
        }
    }
    if (game->actor != WALK_GAMEPLAY_ACTOR_STATIC)
        next.valid |= WALK_GAMEPLAY_SPATIAL_ACTOR_VALID;
    const LaiueSceneServiceV1 *scene = game->services.scene;
    if (ServicePrefix(scene, game->services.sceneSize, LAIUE_SCENE_SERVICE_ABI_VERSION_1) &&
        HAS_FIELD(LaiueSceneServiceV1, scene, game->services.sceneSize, cameraGetForwardVector))
    {
        scene->cameraGetForwardVector(&game->camera, next.forward);
        double lengthSquared = 0.0;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (!WalkMathFinite(next.forward[axis]) || WalkMathAbs(next.forward[axis]) > 1.00001)
                return WALK_GAMEPLAY_PROVIDER_FAILURE;
            lengthSquared += (double)next.forward[axis] * next.forward[axis];
        }
        if (!(lengthSquared > 1.0e-12))
            return WALK_GAMEPLAY_PROVIDER_FAILURE;
        next.valid |= WALK_GAMEPLAY_SPATIAL_FORWARD_VALID;
    }
    *out = next;
    return WALK_GAMEPLAY_OK;
}

bool WalkGameplaySpatialDelta(const WalkGameplaySpatialSnapshot *from,
                              const WalkGameplaySpatialSnapshot *to, double outDelta[3])
{
    if (from == NULL || to == NULL || outDelta == NULL || from->owner == NULL ||
        from->owner != to->owner || (from->valid & WALK_GAMEPLAY_SPATIAL_ACTOR_VALID) == 0u ||
        (to->valid & WALK_GAMEPLAY_SPATIAL_ACTOR_VALID) == 0u)
        return false;
    double delta[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int64_t first, last, difference;
        if (!WalkMathFinite(from->rootFraction[axis]) || !WalkMathFinite(to->rootFraction[axis]) ||
            from->rootFraction[axis] < 0.0 || from->rootFraction[axis] >= 1.0 ||
            to->rootFraction[axis] < 0.0 || to->rootFraction[axis] >= 1.0 ||
            !AddBlock(from->providerFrameOrigin[axis], from->centerBlock[axis], &first) ||
            !AddBlock(to->providerFrameOrigin[axis], to->centerBlock[axis], &last) ||
            !SubtractBlock(last, first, &difference))
            return false;
        delta[axis] = (double)difference + to->rootFraction[axis] - from->rootFraction[axis];
    }
    memcpy(outDelta, delta, sizeof(delta));
    return true;
}

WalkGameplayStatus WalkGameplayCamera(WalkGameplay *game, uint32_t width, uint32_t height,
                                      WalkGameplaySpatialSnapshot *out)
{
    if (out == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    WalkGameplayStatus status = WalkGameplaySnapshot(game, out);
    if (status != WALK_GAMEPLAY_OK)
    {
        if (status == WALK_GAMEPLAY_PROVIDER_FAILURE && game != NULL && game->initialized)
            return Fail(game, status);
        return status;
    }
    if (width == 0u || height == 0u)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if ((game->capabilities & WALK_GAMEPLAY_CAP_CAMERA) == 0u ||
        (out->valid & WALK_GAMEPLAY_SPATIAL_FORWARD_VALID) == 0u)
        return WALK_GAMEPLAY_UNSUPPORTED;
    double eye[3];
    if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
    {
        const double fallback[3] = {out->rootLocal[0], out->rootLocal[1], out->rootLocal[2] + 1.8};
        if (!WalkHumanoidResolveCameraEye(&game->physicsContext, &game->ragdoll,
                                          &game->ragdollCollision, out->forward, game->firstPerson,
                                          fallback, game->lastSafeCameraEye,
                                          &game->lastSafeCameraEyeValid, eye))
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
        if (game->walkContext.providerFailed)
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            out->cameraRelativeEye[axis] = (float)(eye[axis] - out->ragdollRenderOrigin[axis]);
    }
    else
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            int64_t within;
            if (!SubtractBlock(out->centerBlock[axis], out->renderOriginBlock[axis], &within))
                return WALK_GAMEPLAY_INVALID_STATE;
            eye[axis] = (double)within + out->rootFraction[axis] + (axis == 2u ? 1.6 : 0.0);
            out->cameraRelativeEye[axis] = (float)eye[axis];
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!WalkMathFinite(eye[axis]) || !WalkMathFinite(out->cameraRelativeEye[axis]))
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
    memcpy(out->cameraEyeLocal, eye, sizeof(eye));
    memcpy(game->camera.position, eye, sizeof(eye));
    const float aspect = (float)width / (float)height;
    if (!WalkMathFinite(aspect) || !(aspect > 0.0f))
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    float view[16], projection[16];
    game->services.scene->cameraGetViewMatrix(&game->camera, out->cameraRelativeEye, view);
    game->services.scene->cameraGetProjectionMatrix(
        aspect, game->config.fovRadians, game->config.nearPlane, game->config.farPlane, projection);
    game->services.sceneMath->matrix4Multiply(view, projection, out->viewProjection);
    for (uint32_t index = 0u; index < 16u; ++index)
        if (!WalkMathFinite(view[index]) || !WalkMathFinite(projection[index]) ||
            !WalkMathFinite(out->viewProjection[index]))
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
    out->valid |= WALK_GAMEPLAY_SPATIAL_CAMERA_VALID;
    return WALK_GAMEPLAY_OK;
}

uint8_t WalkGameplayReadBlock(const WalkGameplay *game, int64_t x, int64_t y, int64_t z)
{
    if (game == NULL || !game->initialized || z < INT32_MIN || z > INT32_MAX)
        return 0u;
    const LaiueVoxelCoordV1 coordinate = {x, y, (int32_t)z};
    LaiueVoxelBlockV1 block = {0};
    if (WalkGetBlock(&game->walkProvider, &coordinate, &block) == 0u)
        return UINT8_MAX; /* Conservative solid; the following operation reports failure. */
    return block.material <= UINT8_MAX ? (uint8_t)block.material : UINT8_MAX;
}

static uint8_t ReadRayBlock(void *opaque, int64_t x, int64_t y, int64_t z)
{
    return WalkGameplayReadBlock((const WalkGameplay *)opaque, x, y, z);
}

WalkGameplayStatus WalkGameplayEdit(WalkGameplay *game,
                                    const WalkGameplaySpatialSnapshot *cameraSnapshot,
                                    WalkGameplayEditResult *out)
{
    if (out == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    *out = (WalkGameplayEditResult){0};
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if (cameraSnapshot != NULL && cameraSnapshot->owner != game)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (cameraSnapshot != NULL && cameraSnapshot->revision != game->revision)
        return WALK_GAMEPLAY_STALE_SNAPSHOT;
    if ((game->capabilities & WALK_GAMEPLAY_CAP_CAMERA) == 0u)
    {
        const uint32_t pending =
            game->pendingPulses & (WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE);
        game->pendingPulses &= ~(WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE);
        for (uint32_t index = 0u; index < 2u; ++index)
            if ((pending & (index == 0u ? WALK_GAMEPLAY_PULSE_BREAK : WALK_GAMEPLAY_PULSE_PLACE)) !=
                0u)
                out->items[out->count++] = (WalkGameplayEditItem){
                    .place = index != 0u,
                    .status = WALK_GAMEPLAY_UNSUPPORTED,
                };
        out->revision = game->revision;
        return pending != 0u ? WALK_GAMEPLAY_UNSUPPORTED : WALK_GAMEPLAY_OK;
    }
    if (cameraSnapshot == NULL || cameraSnapshot->owner != game ||
        (cameraSnapshot->valid &
         (WALK_GAMEPLAY_SPATIAL_CAMERA_VALID | WALK_GAMEPLAY_SPATIAL_FORWARD_VALID)) !=
            (WALK_GAMEPLAY_SPATIAL_CAMERA_VALID | WALK_GAMEPLAY_SPATIAL_FORWARD_VALID))
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    if (cameraSnapshot->revision != game->revision)
        return WALK_GAMEPLAY_STALE_SNAPSHOT;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (!WalkMathFinite(cameraSnapshot->cameraRelativeEye[axis]) ||
            !WalkMathFinite(cameraSnapshot->forward[axis]))
            return WALK_GAMEPLAY_INVALID_ARGUMENT;
    const uint32_t edits =
        game->pendingPulses & (WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE);
    if (edits != 0u && game->revision > UINT64_MAX - 2u)
        return WALK_GAMEPLAY_INVALID_STATE;
    game->pendingPulses &= ~(WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE);
    WalkGameplayStatus result = WALK_GAMEPLAY_OK;
    const double origin[3] = {cameraSnapshot->cameraRelativeEye[0],
                              cameraSnapshot->cameraRelativeEye[1],
                              cameraSnapshot->cameraRelativeEye[2]};
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        if ((edits & (index == 0u ? WALK_GAMEPLAY_PULSE_BREAK : WALK_GAMEPLAY_PULSE_PLACE)) == 0u)
            continue;
        WalkGameplayEditItem *item = &out->items[out->count++];
        item->place = index != 0u;
        item->status = WALK_GAMEPLAY_OK;
        if ((game->capabilities & WALK_GAMEPLAY_CAP_EDIT) == 0u)
            item->status = WALK_GAMEPLAY_UNSUPPORTED;
        else
        {
            WalkWorldBlockHit hit;
            if (WalkWorldRaycast(ReadRayBlock, game, origin, cameraSnapshot->renderOriginBlock,
                                 cameraSnapshot->forward, WALK_WORLD_INTERACTION_DISTANCE, &hit))
            {
                const int64_t *target = item->place ? hit.previousBlock : hit.block;
                if (target[2] < INT32_MIN || target[2] > INT32_MAX)
                    item->status = WALK_GAMEPLAY_INVALID_ARGUMENT;
                else if (!item->place || target[0] != hit.block[0] || target[1] != hit.block[1] ||
                         target[2] != hit.block[2])
                {
                    const LaiueVoxelCoordV1 coordinate = {target[0], target[1], (int32_t)target[2]};
                    const uint8_t material = item->place ? game->selectedMaterial : 0u;
                    const uint8_t before =
                        WalkGameplayReadBlock(game, target[0], target[1], target[2]);
                    const LaiueVoxelBlockV1 block = {material, 0u};
                    if (game->walkContext.providerFailed ||
                        game->services.voxel->setBlock(game->world, &coordinate, &block) == 0u ||
                        WalkGameplayReadBlock(game, target[0], target[1], target[2]) != material ||
                        game->walkContext.providerFailed)
                        item->status = Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
                    else if (before != material)
                    {
                        item->changed = true;
                        item->coordinate = coordinate;
                        ++game->revision;
                    }
                }
            }
            if (game->walkContext.providerFailed)
                item->status = Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
        }
        if (item->status != WALK_GAMEPLAY_OK)
        {
            result = item->status;
            if (game->failed)
                break;
        }
    }
    out->revision = game->revision;
    return result;
}

static bool ShiftCharacterAxis(int64_t shift, int64_t *cellDelta, int64_t *localDelta)
{
    int64_t cells = shift / WALK_CELL_BLOCKS;
    int64_t remainder = shift % WALK_CELL_BLOCKS;
    if (remainder < 0)
    {
        --cells;
        remainder += WALK_CELL_BLOCKS;
    }
    *cellDelta = -cells;
    *localDelta = -remainder * WALK_UNIT;
    return true;
}

static bool CharacterShiftedPosition(const LaiueCharacterPositionV1 *old, const int64_t cells[2],
                                     const int64_t local[2], LaiueCharacterPositionV1 *next)
{
    *next = *old;
    int64_t *nextCells[2] = {&next->cellX, &next->cellY};
    int64_t *nextLocal[2] = {&next->localX, &next->localY};
    for (uint32_t axis = 0u; axis < 2u; ++axis)
    {
        if (!AddBlock(*nextCells[axis], cells[axis], nextCells[axis]) ||
            !AddBlock(*nextLocal[axis], local[axis], nextLocal[axis]))
            return false;
        const int64_t carry = FloorDivide(*nextLocal[axis], LAIUE_CHARACTER_LOCAL_CELL_SIZE);
        const int64_t remainder = *nextLocal[axis] % LAIUE_CHARACTER_LOCAL_CELL_SIZE;
        if (!AddBlock(*nextCells[axis], carry, nextCells[axis]))
            return false;
        *nextLocal[axis] = remainder < 0 ? remainder + LAIUE_CHARACTER_LOCAL_CELL_SIZE : remainder;
    }
    return true;
}

WalkGameplayStatus WalkGameplayRebase(WalkGameplay *game, const int64_t *explicitShift,
                                      WalkGameplayRebaseResult *out)
{
    if (out == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    *out = (WalkGameplayRebaseResult){0};
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if ((game->capabilities & WALK_GAMEPLAY_CAP_REBASE) == 0u)
        return WALK_GAMEPLAY_UNSUPPORTED;
    int64_t shift[3] = {0};
    if (explicitShift != NULL)
        memcpy(shift, explicitShift, sizeof(shift));
    else
    {
        WalkGameplaySpatialSnapshot position;
        const WalkGameplayStatus status = WalkGameplaySnapshot(game, &position);
        if (status != WALK_GAMEPLAY_OK)
            return status;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
            {
                int64_t local;
                if (!FloorBlock(position.rootLocal[axis], &local))
                    return WALK_GAMEPLAY_INVALID_STATE;
                shift[axis] = FloorDivide(local, WALK_CHUNK_BLOCKS) * WALK_CHUNK_BLOCKS;
            }
            else if (axis < 2u &&
                     (position.centerBlock[axis] > 8192 || position.centerBlock[axis] < -8192))
                shift[axis] = position.renderOriginBlock[axis];
        }
    }
    if (shift[0] == 0 && shift[1] == 0 && shift[2] == 0)
        return WALK_GAMEPLAY_OK;
    if (game->revision == UINT64_MAX)
        return WALK_GAMEPLAY_INVALID_STATE;
    if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
    {
        int64_t nextOrigin[3], inverse[3];
        double nextRootCache[3], nextSafeEye[3], nextCamera[3];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (shift[axis] <= -INT64_C(4503599627370496) ||
                shift[axis] >= INT64_C(4503599627370496) ||
                !AddBlock(game->ragdollBlockOrigin[axis], shift[axis], &nextOrigin[axis]))
                return WALK_GAMEPLAY_INVALID_ARGUMENT;
            inverse[axis] = -shift[axis];
            const double amount = (double)shift[axis];
            const double cached[3] = {game->humanoidController.previousRootPosition[axis],
                                      game->lastSafeCameraEye[axis], game->camera.position[axis]};
            double *next[3] = {&nextRootCache[axis], &nextSafeEye[axis], &nextCamera[axis]};
            for (uint32_t index = 0u; index < 3u; ++index)
            {
                if (!WalkMathFinite(cached[index]) || !WalkMathFinite(cached[index] - amount) ||
                    WalkMathAbs((cached[index] - amount) + amount - cached[index]) > 1.0e-9)
                    return WALK_GAMEPLAY_INVALID_ARGUMENT;
                *next[index] = cached[index] - amount;
            }
        }
        for (uint32_t body = 0u; body < game->ragdoll.bodyCount; ++body)
        {
            double position[3];
            if (!WalkBodyLocalPosition(&game->physicsContext, &game->ragdoll.bodies[body],
                                       position))
                return WALK_GAMEPLAY_PROVIDER_FAILURE;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                if (!WalkMathFinite(position[axis]) ||
                    !WalkMathFinite(position[axis] - (double)shift[axis]) ||
                    WalkMathAbs((position[axis] - (double)shift[axis]) + (double)shift[axis] -
                                position[axis]) > 1.0e-9)
                    return WALK_GAMEPLAY_INVALID_ARGUMENT;
        }
        for (uint32_t body = 0u; body < game->ragdoll.bodyCount; ++body)
            if (!WalkBodyTranslateBlocks(&game->physicsContext, &game->ragdoll.bodies[body], shift))
            {
                bool rolledBack = true;
                while (body != 0u)
                {
                    --body;
                    if (!WalkBodyTranslateBlocks(&game->physicsContext, &game->ragdoll.bodies[body],
                                                 inverse))
                        rolledBack = false;
                }
                return rolledBack ? WALK_GAMEPLAY_PROVIDER_FAILURE
                                  : Fail(game, WALK_GAMEPLAY_INCONSISTENT_STATE);
            }
        memcpy(game->ragdollBlockOrigin, nextOrigin, sizeof(nextOrigin));
        if (game->humanoidController.initialized)
            memcpy(game->humanoidController.previousRootPosition, nextRootCache,
                   sizeof(nextRootCache));
        if (game->lastSafeCameraEyeValid)
            memcpy(game->lastSafeCameraEye, nextSafeEye, sizeof(nextSafeEye));
        memcpy(game->camera.position, nextCamera, sizeof(nextCamera));
    }
    else
    {
        if (shift[2] != 0)
            return WALK_GAMEPLAY_INVALID_ARGUMENT;
        int64_t nextOrigin[3], cells[2], local[2];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!AddBlock(game->providerFrameOrigin[axis], shift[axis], &nextOrigin[axis]))
                return WALK_GAMEPLAY_INVALID_ARGUMENT;
        for (uint32_t axis = 0u; axis < 2u; ++axis)
            if (!ShiftCharacterAxis(shift[axis], &cells[axis], &local[axis]))
                return WALK_GAMEPLAY_INVALID_ARGUMENT;
        LaiueCharacterPositionV1 old, next;
        const LaiueCharacterServiceV1 *character = game->services.character;
        if (character->getPosition(game->controller, &old) == 0u)
            return WALK_GAMEPLAY_PROVIDER_FAILURE;
        if (!CharacterShiftedPosition(&old, cells, local, &next))
            return WALK_GAMEPLAY_INVALID_ARGUMENT;
        int64_t nextBlocks[3];
        double nextFractions[3];
        if (!PositionParts(&next, nextBlocks, nextFractions))
            return WALK_GAMEPLAY_INVALID_ARGUMENT;
        if (character->rebaseOrigin(game->controller, cells[0], cells[1], local[0], local[1]) == 0u)
            return WALK_GAMEPLAY_PROVIDER_FAILURE;
        if (game->services.voxel->rebase(game->world, shift[0], shift[1], 0) == 0u)
        {
            if (character->rebaseOrigin(game->controller, -cells[0], -cells[1], -local[0],
                                        -local[1]) == 0u)
                return Fail(game, WALK_GAMEPLAY_INCONSISTENT_STATE);
            return WALK_GAMEPLAY_PROVIDER_FAILURE;
        }
        game->lastPosition = next;
        memcpy(game->providerFrameOrigin, nextOrigin, sizeof(nextOrigin));
        memcpy(out->providerFrameShift, shift, sizeof(shift));
    }
    ++game->revision;
    out->changed = true;
    memcpy(out->localBlockShift, shift, sizeof(shift));
    return WALK_GAMEPLAY_OK;
}

WalkGameplayStatus WalkGameplayStep(WalkGameplay *game, WalkGameplayStepResult *out)
{
    if (out == NULL)
        return WALK_GAMEPLAY_INVALID_ARGUMENT;
    *out = (WalkGameplayStepResult){0};
    const WalkGameplayStatus state = State(game);
    if (state != WALK_GAMEPLAY_OK)
        return state;
    if (game->tick == UINT64_MAX || game->revision > UINT64_MAX - 2u)
        return WALK_GAMEPLAY_INVALID_STATE;
    double moveX = game->input.strafe, moveY = game->input.forward;
    WalkGameplaySpatialSnapshot position;
    const WalkGameplayStatus snapshot = WalkGameplaySnapshot(game, &position);
    if (snapshot != WALK_GAMEPLAY_OK)
        return Fail(game, snapshot);
    if ((position.valid & WALK_GAMEPLAY_SPATIAL_FORWARD_VALID) != 0u)
    {
        double x = position.forward[0], y = position.forward[1];
        double length = ScalarSqrtDouble(x * x + y * y);
        if (length <= 1.0e-6)
        {
            /* A vertical view still has a horizontal heading. Preserve yaw
             * rather than switching the controls to world axes at the poles. */
            Camera horizontal = game->camera;
            horizontal.pitch = 0.0f;
            float forward[3];
            game->services.scene->cameraGetForwardVector(&horizontal, forward);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
                if (!WalkMathFinite(forward[axis]) || WalkMathAbs(forward[axis]) > 1.00001)
                    return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
            x = forward[0];
            y = forward[1];
            length = ScalarSqrtDouble(x * x + y * y);
            if (!(length > 1.0e-6))
                return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
        }
        moveX = (x * game->input.forward + y * game->input.strafe) / length;
        moveY = (y * game->input.forward - x * game->input.strafe) / length;
    }
    const bool jump = (game->pendingPulses & WALK_GAMEPLAY_PULSE_JUMP) != 0u ||
                      (game->jumpArmed && game->input.jumpHeld && !game->previousJumpHeld);
    game->pendingPulses &= ~WALK_GAMEPLAY_PULSE_JUMP;
    game->previousJumpHeld = game->input.jumpHeld;
    ++game->revision; /* Any partial provider mutation also invalidates old snapshots. */
    if (game->actor == WALK_GAMEPLAY_ACTOR_RAGDOLL)
    {
        /* Shared input already performs edge detection, including explicit pulses. */
        game->humanoidController.previousJumpInput = false;
        if (!WalkHumanoidStep(&game->physicsContext, &game->ragdoll, &game->humanoidController,
                              &game->ragdollCollision, &game->ragdollRigidSettings,
                              &game->ragdollSettings, game->ragdollScratch,
                              game->ragdollScratchBytes, moveX, moveY, game->input.sprint, jump,
                              1.0 / LAIUE_CHARACTER_TICK_HZ, &game->grounded, &game->facingYaw,
                              &game->gaitPhase, &out->humanoidFailure))
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
    }
    else if (game->actor == WALK_GAMEPLAY_ACTOR_CHARACTER)
    {
        const LaiueCharacterInputV1 input = {
            .moveX = (int32_t)(moveX * LAIUE_CHARACTER_INPUT_AXIS_SCALE),
            .moveY = (int32_t)(moveY * LAIUE_CHARACTER_INPUT_AXIS_SCALE),
            .flags = LAIUE_CHARACTER_INPUT_ANALOG |
                     (game->input.sprint ? LAIUE_CHARACTER_INPUT_SPRINT : 0u) |
                     (jump ? LAIUE_CHARACTER_INPUT_JUMP : 0u),
        };
        if (game->services.character->step(game->controller, &input) == 0u ||
            game->services.character->getPosition(game->controller, &game->lastPosition) == 0u)
            return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
        game->grounded = game->services.character->isGrounded(game->controller) != 0u;
    }
    if (game->walkContext.providerFailed)
        return Fail(game, WALK_GAMEPLAY_PROVIDER_FAILURE);
    ++game->tick;
    if ((game->capabilities & WALK_GAMEPLAY_CAP_REBASE) != 0u)
    {
        const WalkGameplayStatus status = WalkGameplayRebase(game, NULL, &out->rebase);
        if (status != WALK_GAMEPLAY_OK)
            return Fail(game, status);
    }
    out->tick = game->tick;
    out->revision = game->revision;
    return WALK_GAMEPLAY_OK;
}

#undef HAS_FIELD
