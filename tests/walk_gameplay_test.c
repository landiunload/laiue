#include "test_runtime.h"
#include "walk_gameplay.h"
#include "walk_physics_binding.h"
#include "numeric/numeric_service.h"
#include "physics/numeric_provider.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

/* Bounded fake providers exercise ownership and failure paths without a GPU. */
typedef struct TestCharacter
{
    bool live;
    LaiueCharacterPositionV1 position;
    LaiueCharacterInputV1 input;
    uint32_t jumps;
} TestCharacter;

typedef struct TestWorld
{
    bool live, explicitBlock;
    LaiueVoxelCoordV1 coordinate;
    LaiueVoxelBlockV1 block;
} TestWorld;

static TestCharacter characters[2];
static TestWorld worlds[2];
static uint32_t characterCreates, characterDestroys, worldCreates, worldDestroys;
static bool failCharacterCreate, failSetPosition, failWorldCreate, failGetProvider;
static bool failBlockRead, legacyProvider, failStep;
static bool failCamera;
static float lastAspect;
static uint32_t rigidStepsA, rigidStepsB, translateCalls;
static bool rejectRollback;
static bool rejectWorldRebase, rejectCharacterRollback;
static uint32_t characterRebaseCalls, invalidPhysicsCalls;

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static uint32_t CreateCharacter(const LaiueCharacterCollisionV1 *collision, int64_t halfExtent,
                                LaiueCharacterControllerV1 **out)
{
    Expect(collision != NULL && collision->context != NULL && collision->sweepAabb != NULL &&
               halfExtent == 400,
           "character receives the shared collision adapter");
    ++characterCreates;
    for (uint32_t i = 0; i < 2; ++i)
        if (!characters[i].live)
        {
            characters[i] = (TestCharacter){.live = true};
            *out = (LaiueCharacterControllerV1 *)&characters[i];
            return failCharacterCreate ? 0u : 1u;
        }
    *out = NULL;
    return 0u;
}

static void DestroyCharacter(LaiueCharacterControllerV1 *controller)
{
    TestCharacter *character = (TestCharacter *)controller;
    Expect(character != NULL && character->live, "character is released exactly once");
    character->live = false;
    ++characterDestroys;
}

static uint32_t SetPosition(LaiueCharacterControllerV1 *controller,
                            const LaiueCharacterPositionV1 *position, uint32_t grounded)
{
    (void)grounded;
    ((TestCharacter *)controller)->position = *position;
    return failSetPosition ? 0u : 1u;
}

static uint32_t GetPosition(const LaiueCharacterControllerV1 *controller,
                            LaiueCharacterPositionV1 *out)
{
    *out = ((const TestCharacter *)controller)->position;
    return 1u;
}

static uint32_t IsGrounded(const LaiueCharacterControllerV1 *controller)
{
    (void)controller;
    return 1u;
}

static uint32_t StepCharacter(LaiueCharacterControllerV1 *controller,
                              const LaiueCharacterInputV1 *input)
{
    TestCharacter *character = (TestCharacter *)controller;
    character->input = *input;
    character->jumps += (input->flags & LAIUE_CHARACTER_INPUT_JUMP) != 0u;
    return failStep ? 0u : 1u;
}

static uint32_t RebaseCharacter(LaiueCharacterControllerV1 *controller, int64_t cellX,
                                int64_t cellY, int64_t localX, int64_t localY)
{
    ++characterRebaseCalls;
    if (rejectCharacterRollback && characterRebaseCalls > 1u)
        return 0u;
    TestCharacter *character = (TestCharacter *)controller;
    character->position.cellX += cellX;
    character->position.cellY += cellY;
    character->position.localX += localX;
    character->position.localY += localY;
    while (character->position.localX < 0)
    {
        --character->position.cellX;
        character->position.localX += LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    while (character->position.localY < 0)
    {
        --character->position.cellY;
        character->position.localY += LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    while (character->position.localX >= LAIUE_CHARACTER_LOCAL_CELL_SIZE)
    {
        ++character->position.cellX;
        character->position.localX -= LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    while (character->position.localY >= LAIUE_CHARACTER_LOCAL_CELL_SIZE)
    {
        ++character->position.cellY;
        character->position.localY -= LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    return 1u;
}

static uint32_t CreateWorld(const LaiueVoxelWorldConfigV1 *config, LaiueVoxelWorldV1 **out)
{
    Expect(config != NULL && config->defaultBlock.material == 0u,
           "sparse world leaves generated terrain to the shared adapter");
    ++worldCreates;
    for (uint32_t i = 0; i < 2; ++i)
        if (!worlds[i].live)
        {
            worlds[i] = (TestWorld){.live = true};
            *out = (LaiueVoxelWorldV1 *)&worlds[i];
            return failWorldCreate ? 0u : 1u;
        }
    *out = NULL;
    return 0u;
}

static void DestroyWorld(LaiueVoxelWorldV1 *world)
{
    TestWorld *test = (TestWorld *)world;
    Expect(test != NULL && test->live, "world is released exactly once");
    test->live = false;
    ++worldDestroys;
}

static uint32_t GetBlockState(const LaiueVoxelProviderV1 *provider,
                              const LaiueVoxelCoordV1 *coordinate, LaiueVoxelBlockV1 *out,
                              uint32_t *explicitBlock)
{
    const TestWorld *world = (const TestWorld *)provider->context;
    *explicitBlock = world->explicitBlock && coordinate->x == world->coordinate.x &&
                     coordinate->y == world->coordinate.y && coordinate->z == world->coordinate.z;
    *out = *explicitBlock != 0u ? world->block : (LaiueVoxelBlockV1){0};
    return failBlockRead ? 0u : 1u;
}

static uint32_t GetBlock(const LaiueVoxelProviderV1 *provider, const LaiueVoxelCoordV1 *coordinate,
                         LaiueVoxelBlockV1 *out)
{
    uint32_t explicitBlock;
    return GetBlockState(provider, coordinate, out, &explicitBlock);
}

static uint32_t GetProvider(LaiueVoxelWorldV1 *world, LaiueVoxelProviderV1 *out)
{
    *out = (LaiueVoxelProviderV1){.structSize = sizeof(*out),
                                  .abiVersion = LAIUE_VOXEL_ABI_VERSION_1,
                                  .context = world,
                                  .getBlock = GetBlock,
                                  .getBlockState = legacyProvider ? NULL : GetBlockState};
    return failGetProvider ? 0u : 1u;
}

static uint32_t SetBlock(LaiueVoxelWorldV1 *world, const LaiueVoxelCoordV1 *coordinate,
                         const LaiueVoxelBlockV1 *block)
{
    TestWorld *test = (TestWorld *)world;
    test->explicitBlock = true;
    test->coordinate = *coordinate;
    test->block = *block;
    return 1u;
}

static uint32_t RebaseWorld(LaiueVoxelWorldV1 *world, int64_t x, int64_t y, int64_t z)
{
    if (rejectWorldRebase)
        return 0u;
    TestWorld *test = (TestWorld *)world;
    if (test->explicitBlock)
    {
        test->coordinate.x -= x;
        test->coordinate.y -= y;
        test->coordinate.z -= (int32_t)z;
    }
    return 1u;
}

static void TestCameraInit(Camera *camera, double x, double y, double z, float yaw, float pitch)
{
    *camera = (Camera){.position = {x, y, z}, .yaw = yaw, .pitch = pitch};
}

static void Forward(const Camera *camera, float out[3])
{
    /* Three exact directions are enough to expose ordering and edit mistakes. */
    out[0] = camera->yaw > 1.0f ? 1.0f : 0.0f;
    out[1] = camera->yaw > 1.0f ? 0.0f : 1.0f;
    out[2] = 0.0f;
    if (camera->pitch < -1.0f)
    {
        out[0] = out[1] = 0.0f;
        out[2] = -1.0f;
    }
}

static void Identity(float out[16])
{
    for (uint32_t i = 0; i < 16; ++i)
        out[i] = i % 5u == 0u ? 1.0f : 0.0f;
}

static void View(const Camera *camera, const float eye[3], float out[16])
{
    (void)camera;
    (void)eye;
    Identity(out);
    if (failCamera)
    {
        union
        {
            uint32_t bits;
            float value;
        } nan = {.bits = UINT32_C(0x7fc00000)};
        out[0] = nan.value;
    }
}

static void Projection(float aspect, float fov, float nearPlane, float farPlane, float out[16])
{
    Expect(fov > 0.0f && nearPlane > 0.0f && farPlane > nearPlane, "valid projection settings");
    lastAspect = aspect;
    Identity(out);
}

static void Multiply(const float left[16], const float right[16], float out[16])
{
    (void)right;
    memcpy(out, left, sizeof(float) * 16u);
}

static const LaiueCharacterServiceV1 characterService = {.structSize = sizeof(characterService),
                                                         .abiVersion =
                                                             LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
                                                         .create = CreateCharacter,
                                                         .destroy = DestroyCharacter,
                                                         .step = StepCharacter,
                                                         .getPosition = GetPosition,
                                                         .setPosition = SetPosition,
                                                         .isGrounded = IsGrounded,
                                                         .rebaseOrigin = RebaseCharacter};
static const LaiueVoxelServiceV1 voxelService = {.structSize = sizeof(voxelService),
                                                 .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
                                                 .create = CreateWorld,
                                                 .destroy = DestroyWorld,
                                                 .getProvider = GetProvider,
                                                 .setBlock = SetBlock,
                                                 .rebase = RebaseWorld};
static const LaiueSceneServiceV1 sceneService = {.structSize = sizeof(sceneService),
                                                 .abiVersion = LAIUE_SCENE_SERVICE_ABI_VERSION_1,
                                                 .cameraInit = TestCameraInit,
                                                 .cameraGetForwardVector = Forward,
                                                 .cameraGetViewMatrix = View,
                                                 .cameraGetProjectionMatrix = Projection};
static const LaiueSceneMathServiceV1 mathService = {.structSize = sizeof(mathService),
                                                    .abiVersion =
                                                        LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1,
                                                    .matrix4Multiply = Multiply};

static WalkGameplayServices Services(void)
{
    return (WalkGameplayServices){.character = &characterService,
                                  .characterSize = sizeof(characterService),
                                  .voxel = &voxelService,
                                  .voxelSize = sizeof(voxelService),
                                  .scene = &sceneService,
                                  .sceneSize = sizeof(sceneService),
                                  .sceneMath = &mathService,
                                  .sceneMathSize = sizeof(mathService)};
}

static bool PositionA(const VoxelRigidBody *body, double out[3])
{
    (void)body;
    out[0] = 1.0;
    out[1] = out[2] = 0.0;
    return true;
}

static bool PositionB(const VoxelRigidBody *body, double out[3])
{
    (void)body;
    out[0] = 2.0;
    out[1] = out[2] = 0.0;
    return true;
}

static bool RigidStepA(VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
                       const VoxelRigidStepSettings *rigid, const VoxelRagdollSettings *settings,
                       void *scratch, uint32_t bytes, const VoxelRigidStepOptions *options)
{
    ++rigidStepsA;
    return VoxelRagdollStep(ragdoll, collision, rigid, settings, scratch, bytes, options);
}

static bool RigidStepB(VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
                       const VoxelRigidStepSettings *rigid, const VoxelRagdollSettings *settings,
                       void *scratch, uint32_t bytes, const VoxelRigidStepOptions *options)
{
    ++rigidStepsB;
    return VoxelRagdollStep(ragdoll, collision, rigid, settings, scratch, bytes, options);
}

static bool TranslateWithFailure(VoxelRigidBody *body, const int64_t shift[3])
{
    ++translateCalls;
    if (translateCalls == 3u || (rejectRollback && translateCalls > 3u))
        return false; /* Failed body remains unchanged, as required by the provider. */
    return VoxelRigidBodyTranslateBlocks(body, shift);
}

static void InvalidPhysicsConfigure(void)
{
    ++invalidPhysicsCalls;
}

typedef struct TestBodySnapshot
{
    double position[3], linear[3], angular[3];
    float orientation[9];
} TestBodySnapshot;

static void CaptureBodies(const WalkGameplay *game, TestBodySnapshot *out)
{
    Expect(game->ragdoll.bodyCount <= VOXEL_RAGDOLL_MAX_BODIES, "bounded body snapshots");
    for (uint32_t body = 0u; body < game->ragdoll.bodyCount; ++body)
    {
        const VoxelRigidBody *rigid = &game->ragdoll.bodies[body];
        Expect(WalkBodyLocalPosition(&game->physicsContext, rigid, out[body].position) &&
                   WalkBodyLinearVelocity(&game->physicsContext, rigid, out[body].linear) &&
                   WalkBodyAngularVelocity(&game->physicsContext, rigid, out[body].angular),
               "every limb has readable pose and velocity");
        WalkBodyOrientationMatrix(&game->physicsContext, rigid, out[body].orientation);
    }
}

static void ExpectBodiesUnchanged(const WalkGameplay *game, const TestBodySnapshot *before)
{
    static TestBodySnapshot after[VOXEL_RAGDOLL_MAX_BODIES];
    CaptureBodies(game, after);
    for (uint32_t body = 0u; body < game->ragdoll.bodyCount; ++body)
    {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double difference = before[body].position[axis] - after[body].position[axis];
            Expect(difference > -1.0e-9 && difference < 1.0e-9,
                   "rollback restores every limb, including non-root bodies");
            Expect(before[body].linear[axis] == after[body].linear[axis] &&
                       before[body].angular[axis] == after[body].angular[axis],
                   "rollback does not change any limb velocity");
        }
        for (uint32_t component = 0u; component < 9u; ++component)
            Expect(before[body].orientation[component] == after[body].orientation[component],
                   "rollback does not change any limb orientation");
    }
}

static void TestPhysicsContexts(void)
{
    Expect(BindLinkedWalkPhysics(), "linked physics binding");
    LaiuePhysicsServiceV1 a = linkedWalkPhysicsService, b = linkedWalkPhysicsService;
    a.bodyLocalPosition = PositionA;
    b.bodyLocalPosition = PositionB;
    WalkPhysicsContext first = {0}, second = {0};
    Expect(WalkPhysicsBind(&first, &a, sizeof(a)) && WalkPhysicsBind(&second, &b, sizeof(b)),
           "independent contexts bind");
    double position[3];
    Expect(WalkBodyLocalPosition(&first, NULL, position) && position[0] == 1.0,
           "second binding does not redirect the first instance");
    b.bodyTranslateBlocks = NULL;
    Expect(!WalkPhysicsBind(&first, &b, sizeof(b)) && first.service == &a,
           "failed binding preserves a live context");
    const uint32_t prefix[2] = {sizeof(a), LAIUE_PHYSICS_SERVICE_ABI_VERSION_1};
    Expect(!WalkPhysicsBind(&first, (const LaiuePhysicsServiceV1 *)prefix, sizeof(prefix)) &&
               first.service == &a,
           "actual short physics storage is not read past its bounds");
    WalkPhysicsUnbind(&second);
    Expect(WalkBodyLocalPosition(&first, NULL, position) && position[0] == 1.0,
           "unbinding one context preserves the other");
}

static void TestInitFailures(void)
{
    static WalkGameplay game;
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    WalkGameplayServices services = Services();
    config.selectedMaterial = 0;
    const uint32_t creates = characterCreates + worldCreates;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_INVALID_ARGUMENT &&
               characterCreates + worldCreates == creates,
           "invalid config allocates nothing");
    config.selectedMaterial = 1;
    config.requiredCapabilities = WALK_GAMEPLAY_CAP_RAGDOLL;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_UNSUPPORTED &&
               !game.initialized && characterCreates == characterDestroys &&
               worldCreates == worldDestroys,
           "missing required capabilities release all partial resources");
    config.requiredCapabilities = 0;
    failCharacterCreate = failWorldCreate = true;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK &&
               game.actor == WALK_GAMEPLAY_ACTOR_STATIC && game.world == NULL &&
               characterCreates == characterDestroys && worldCreates == worldDestroys,
           "failed creation with nonnull outputs releases both providers");
    WalkGameplayRelease(&game);
    failCharacterCreate = failWorldCreate = false;
    failSetPosition = failGetProvider = true;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK &&
               game.actor == WALK_GAMEPLAY_ACTOR_STATIC && game.world == NULL &&
               characterCreates == characterDestroys && worldCreates == worldDestroys,
           "post-create setup failures release resources");
    WalkGameplayRelease(&game);
    failSetPosition = failGetProvider = false;
    const uint32_t prefix[2] = {sizeof(characterService), LAIUE_CHARACTER_SERVICE_ABI_VERSION_1};
    services.character = (const LaiueCharacterServiceV1 *)prefix;
    services.characterSize = sizeof(prefix);
    services.voxelSize = services.sceneSize = services.sceneMathSize = 4u;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK &&
               game.capabilities == 0u && game.actor == WALK_GAMEPLAY_ACTOR_STATIC,
           "actual sizes take precedence over advertised sizes");
    WalkGameplaySpatialSnapshot snapshot;
    Expect(WalkGameplayCamera(&game, 640, 480, &snapshot) == WALK_GAMEPLAY_UNSUPPORTED &&
               (snapshot.valid & WALK_GAMEPLAY_SPATIAL_CAMERA_VALID) == 0u,
           "missing camera is explicitly unsupported");
    WalkGameplayRelease(&game);
}

static void TestUnavailableServiceContracts(void)
{
    static WalkGameplay game;
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    for (uint32_t mode = 0u; mode < 3u; ++mode)
    {
        LaiueCharacterServiceV1 character = characterService;
        LaiueVoxelServiceV1 voxel = voxelService;
        LaiuePhysicsServiceV1 physics = linkedWalkPhysicsService;
        LaiueSceneServiceV1 scene = sceneService;
        LaiueSceneMathServiceV1 math = mathService;
        physics.configureThread = InvalidPhysicsConfigure;
        if (mode == 0u)
            character.abiVersion = voxel.abiVersion = physics.abiVersion = scene.abiVersion =
                math.abiVersion = 99u;
        else if (mode == 1u)
            character.structSize = voxel.structSize = physics.structSize = scene.structSize =
                math.structSize = 2u * sizeof(uint32_t);
        else
        {
            character.destroy = NULL;
            voxel.destroy = NULL;
            physics.ragdollRelease = NULL;
        }
        const WalkGameplayServices services = {
            .character = &character,
            .characterSize = sizeof(character),
            .voxel = &voxel,
            .voxelSize = sizeof(voxel),
            .physics = &physics,
            .physicsSize = sizeof(physics),
            .scene = mode == 2u ? NULL : &scene,
            .sceneSize = sizeof(scene),
            .sceneMath = mode == 2u ? NULL : &math,
            .sceneMathSize = sizeof(math),
        };
        const uint32_t creates = characterCreates + worldCreates;
        const uint32_t physicsCalls = invalidPhysicsCalls;
        Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK &&
                   game.actor == WALK_GAMEPLAY_ACTOR_STATIC && game.capabilities == 0u &&
                   characterCreates + worldCreates == creates &&
                   invalidPhysicsCalls == physicsCalls && game.ragdollScratch == NULL,
               "wrong ABI, short declared size and missing release callbacks allocate nothing");
        WalkGameplayInput input = {.pulses = WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE};
        WalkGameplayEditResult edit;
        Expect(WalkGameplaySubmitInput(&game, &input) == WALK_GAMEPLAY_OK &&
                   WalkGameplayEdit(&game, NULL, &edit) == WALK_GAMEPLAY_UNSUPPORTED &&
                   edit.count == 2u && edit.items[0].status == WALK_GAMEPLAY_UNSUPPORTED &&
                   edit.items[1].status == WALK_GAMEPLAY_UNSUPPORTED && game.pendingPulses == 0u &&
                   WalkGameplayHealth(&game) == WALK_GAMEPLAY_OK,
               "unsupported editing consumes each queued action without poisoning static gameplay");
        WalkGameplayRelease(&game);
    }
}

static void TestInputsAndLifetime(void)
{
    static WalkGameplay first, second;
    WalkGameplayServices services = Services();
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    Expect(WalkGameplayInit(&first, &services, &config) == WALK_GAMEPLAY_OK &&
               WalkGameplayInit(&second, &services, &config) == WALK_GAMEPLAY_OK,
           "two independent gameplay sessions initialize");
    Expect(first.actor == WALK_GAMEPLAY_ACTOR_CHARACTER &&
               first.characterCollision.context == &first.walkProvider &&
               first.walkProvider.context == &first.walkContext,
           "callbacks point into stable caller-owned state");
    WalkGameplayInput input = {
        .forward = 0.125, .jumpHeld = true, .pulses = WALK_GAMEPLAY_PULSE_JUMP};
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK && input.pulses == 0u &&
               (first.pendingPulses & WALK_GAMEPLAY_PULSE_JUMP) != 0u,
           "accepted pulse survives a frame without a fixed tick");
    WalkGameplayStepResult step;
    for (uint32_t i = 0; i < 8; ++i)
        Expect(WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK, "fixed tick advances");
    TestCharacter *character = (TestCharacter *)first.controller;
    Expect(character->jumps == 1u && character->input.moveY == 4096 &&
               (character->input.flags & LAIUE_CHARACTER_INPUT_ANALOG) != 0u && second.tick == 0u,
           "one pulse does not replay, analogue magnitude and independent ticks survive");
    input = (WalkGameplayInput){.forward = 0.25};
    Expect(WalkGameplayOrient(&first, 1.5707963f, 0.0f, WALK_GAMEPLAY_ORIENT_ABSOLUTE) ==
                   WALK_GAMEPLAY_OK &&
               WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK &&
               character->input.moveX == 8192 && character->input.moveY == 0,
           "new orientation affects camera-relative movement in the same tick");
    Expect(WalkGameplayOrient(&first, 1.5707963f, -1.570796f, WALK_GAMEPLAY_ORIENT_ABSOLUTE) ==
                   WALK_GAMEPLAY_OK &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK &&
               character->input.moveX == 8192 && character->input.moveY == 0,
           "looking vertically preserves the camera's horizontal heading");
    input = (WalkGameplayInput){.strafe = 100.0, .forward = 100.0};
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK &&
               first.input.strafe > 0.707 && first.input.strafe < 0.708 &&
               first.input.forward == first.input.strafe,
           "movement is limited to a circle");
    union
    {
        uint64_t bits;
        double value;
    } nan = {.bits = UINT64_C(0x7ff8000000000000)};
    const WalkGameplayInput before = first.input;
    input = (WalkGameplayInput){.forward = nan.value, .pulses = WALK_GAMEPLAY_PULSE_JUMP};
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_INVALID_ARGUMENT &&
               input.pulses == WALK_GAMEPLAY_PULSE_JUMP &&
               memcmp(&first.input, &before, sizeof(before)) == 0,
           "invalid input preserves caller pulses and accepted state");
    Expect(WalkGameplaySetView(&first, true, 3) == WALK_GAMEPLAY_OK, "view changes");
    WalkGameplaySpatialSnapshot landscape, portrait;
    Expect(WalkGameplayCamera(&first, 1920, 1080, &landscape) == WALK_GAMEPLAY_OK &&
               lastAspect > 1.77f &&
               WalkGameplayCamera(&first, 1080, 1920, &portrait) == WALK_GAMEPLAY_OK &&
               lastAspect < 0.57f,
           "rotation changes projection without reinitializing gameplay");
    const uint64_t tick = first.tick, revision = first.revision;
    WalkGameplayResetInput(&first);
    Expect(first.tick == tick && first.revision == revision && first.firstPerson &&
               first.selectedMaterial == 3u && first.pendingPulses == 0u,
           "pause preserves world, tick and view while clearing controls");
    input = (WalkGameplayInput){.jumpHeld = true};
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK && character->jumps == 1u,
           "stale held jump cannot restart after resume");
    input.jumpHeld = false;
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK,
           "release arms next jump");
    input.jumpHeld = true;
    Expect(WalkGameplaySubmitInput(&first, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK && character->jumps == 2u,
           "next press jumps once");
    WalkGameplayRelease(&first);
    WalkGameplayRelease(&first);
    Expect(WalkGameplayStep(&second, &step) == WALK_GAMEPLAY_OK && second.tick == 1u,
           "releasing one session leaves another operational");
    failStep = true;
    Expect(WalkGameplayStep(&second, &step) == WALK_GAMEPLAY_PROVIDER_FAILURE && second.failed,
           "failed actor tick latches a visible provider error");
    failStep = false;
    Expect(WalkGameplayStep(&second, &step) == WALK_GAMEPLAY_PROVIDER_FAILURE,
           "provider recovery cannot silently resume partially mutated gameplay");
    WalkGameplayRelease(&second);
    const uint64_t oldRevision = first.revision;
    first.services = services;
    first.config = config;
    Expect(WalkGameplayInit(&first, &first.services, &first.config) == WALK_GAMEPLAY_OK &&
               first.revision > oldRevision,
           "reinitialization accepts aliased stored configuration without losing revision");
    failCamera = true;
    Expect(WalkGameplayCamera(&first, 640, 480, &portrait) == WALK_GAMEPLAY_PROVIDER_FAILURE &&
               WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_PROVIDER_FAILURE,
           "invalid camera provider output immediately stops simulation");
    failCamera = false;
    WalkGameplayRelease(&first);
}

static void TestRagdollContextsAndRebase(void)
{
    static WalkGameplay first, second;
    LaiuePhysicsServiceV1 a = linkedWalkPhysicsService, b = linkedWalkPhysicsService;
    a.ragdollStep = RigidStepA;
    b.ragdollStep = RigidStepB;
    a.bodyTranslateBlocks = TranslateWithFailure;
    WalkGameplayServices services = Services();
    services.voxel = NULL;
    services.voxelSize = 0u;
    services.physics = &a;
    services.physicsSize = sizeof(a);
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    config.initialPosition.cellX = INT64_C(1) << 40;
    const uint32_t creates = characterCreates;
    Expect(WalkGameplayInit(&first, &services, &config) == WALK_GAMEPLAY_OK,
           "first rigid session initializes");
    services.physics = &b;
    Expect(WalkGameplayInit(&second, &services, &config) == WALK_GAMEPLAY_OK &&
               first.actor == WALK_GAMEPLAY_ACTOR_RAGDOLL && second.actor == first.actor &&
               characterCreates == creates,
           "ragdoll precedence avoids allocating an unused fallback controller");
    WalkGameplayStepResult step;
    Expect(WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_OK && rigidStepsA > 0u &&
               rigidStepsB == 0u,
           "one rigid session uses only its own service table");
    WalkGameplaySpatialSnapshot before, after;
    Expect(WalkGameplaySnapshot(&first, &before) == WALK_GAMEPLAY_OK, "rigid snapshot");
    static TestBodySnapshot bodiesBefore[VOXEL_RAGDOLL_MAX_BODIES];
    CaptureBodies(&first, bodiesBefore);
    const WalkHumanoidControllerState controllerBefore = first.humanoidController;
    const int64_t shift[3] = {64, -128, 64};
    WalkGameplayRebaseResult rebase;
    Expect(WalkGameplayRebase(&first, shift, &rebase) == WALK_GAMEPLAY_PROVIDER_FAILURE &&
               !rebase.changed && !first.failed &&
               WalkGameplaySnapshot(&first, &after) == WALK_GAMEPLAY_OK,
           "failed rigid rebase rolls back the successfully translated prefix");
    double delta[3];
    Expect(WalkGameplaySpatialDelta(&before, &after, delta) && delta[0] > -1e-9 &&
               delta[0] < 1e-9 && delta[1] > -1e-9 && delta[1] < 1e-9 && delta[2] > -1e-9 &&
               delta[2] < 1e-9 && before.revision == after.revision,
           "rollback preserves huge absolute coordinates and fractional local pose");
    ExpectBodiesUnchanged(&first, bodiesBefore);
    Expect(memcmp(&controllerBefore, &first.humanoidController, sizeof(controllerBefore)) == 0,
           "failed rigid rebase leaves gait and support caches unchanged");
    Expect(WalkGameplayRebase(&first, shift, &rebase) == WALK_GAMEPLAY_OK && rebase.changed &&
               WalkGameplaySnapshot(&first, &after) == WALK_GAMEPLAY_OK &&
               WalkGameplaySpatialDelta(&before, &after, delta) && delta[0] > -1e-9 &&
               delta[0] < 1e-9 && delta[1] > -1e-9 && delta[1] < 1e-9 && delta[2] > -1e-9 &&
               delta[2] < 1e-9,
           "successful 3D rigid rebase preserves position without shallow owned copies");
    translateCalls = 0u;
    rejectRollback = true;
    Expect(WalkGameplayRebase(&first, shift, &rebase) == WALK_GAMEPLAY_INCONSISTENT_STATE &&
               first.failed && WalkGameplayStep(&first, &step) == WALK_GAMEPLAY_INCONSISTENT_STATE,
           "failed rollback forbids further simulation instead of pretending consistency");
    rejectRollback = false;
    WalkGameplayRelease(&first);
    Expect(WalkGameplayStep(&second, &step) == WALK_GAMEPLAY_OK && rigidStepsB > 0u,
           "a failed released session does not poison another rigid session");
    WalkGameplayRelease(&second);
}

static void TestEditsAndSpatialFrames(void)
{
    static WalkGameplay game;
    WalkGameplayServices services = Services();
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    config.initialPosition.localX = config.initialPosition.localY = -250;
    config.initialPosition.localZ = 1400;
    config.initialPitch = -1.5f;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK, "edit session init");
    WalkGameplaySpatialSnapshot first, second;
    Expect(WalkGameplayCamera(&game, 640, 480, &first) == WALK_GAMEPLAY_OK &&
               first.centerBlock[0] == -1 && first.rootFraction[0] == 0.75 &&
               first.renderOriginBlock[0] == -64,
           "negative coordinates use floor division and bounded relative camera");
    WalkGameplayInput input = {.pulses = WALK_GAMEPLAY_PULSE_BREAK};
    Expect(WalkGameplaySubmitInput(&game, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplaySetView(&game, true, 2) == WALK_GAMEPLAY_OK,
           "pending edit accepted");
    WalkGameplayEditResult edit;
    Expect(WalkGameplayEdit(&game, &first, &edit) == WALK_GAMEPLAY_STALE_SNAPSHOT &&
               (game.pendingPulses & WALK_GAMEPLAY_PULSE_BREAK) != 0u,
           "stale camera cannot consume an edit");
    Expect(WalkGameplayCamera(&game, 640, 480, &first) == WALK_GAMEPLAY_OK &&
               WalkGameplayEdit(&game, &first, &edit) == WALK_GAMEPLAY_OK && edit.count == 1u &&
               edit.items[0].changed &&
               WalkGameplayReadBlock(&game, edit.items[0].coordinate.x, edit.items[0].coordinate.y,
                                     edit.items[0].coordinate.z) == 0u,
           "breaking generated terrain creates an explicit air override");
    Expect(WalkGameplayCamera(&game, 640, 480, &second) == WALK_GAMEPLAY_OK &&
               WalkGameplayEdit(&game, &second, &edit) == WALK_GAMEPLAY_OK && edit.count == 0u,
           "edit pulse executes only once");
    const int64_t shift[3] = {64, -128, 0};
    WalkGameplayRebaseResult rebase;
    first = second;
    Expect(WalkGameplayRebase(&game, shift, &rebase) == WALK_GAMEPLAY_OK && rebase.changed &&
               WalkGameplaySnapshot(&game, &second) == WALK_GAMEPLAY_OK,
           "character and sparse world rebase together");
    double delta[3] = {99, 98, 97};
    Expect(WalkGameplaySpatialDelta(&first, &second, delta) && delta[0] == 0.0 && delta[1] == 0.0 &&
               delta[2] == 0.0,
           "rebase preserves the authoritative absolute position");
    first.centerBlock[0] = INT64_MIN;
    first.providerFrameOrigin[0] = -1;
    delta[0] = 99.0;
    Expect(!WalkGameplaySpatialDelta(&first, &second, delta) && delta[0] == 99.0,
           "spatial overflow leaves outputs unchanged");
    failBlockRead = true;
    Expect(WalkGameplayReadBlock(&game, 0, 0, 0) != 0u &&
               WalkGameplayHealth(&game) == WALK_GAMEPLAY_PROVIDER_FAILURE &&
               WalkGameplayCamera(&game, 640, 480, &second) == WALK_GAMEPLAY_PROVIDER_FAILURE,
           "provider failure is immediately visible before the next camera or fixed tick");
    failBlockRead = false;
    WalkGameplayRelease(&game);
    legacyProvider = true;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK &&
               (game.capabilities & WALK_GAMEPLAY_CAP_EDIT) == 0u,
           "legacy sparse provider cannot promise generated terrain editing");
    WalkGameplayRelease(&game);
    legacyProvider = false;
}

static void TestEditBatches(void)
{
    static WalkGameplay game, foreignOwner;
    const WalkGameplayServices services = Services();
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    config.initialPitch = -1.5f;
    config.selectedMaterial = 2u;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK, "edit batch session");
    WalkGameplaySpatialSnapshot camera;
    Expect(WalkGameplayCamera(&game, 640, 480, &camera) == WALK_GAMEPLAY_OK, "edit batch camera");
    WalkGameplayInput input = {.pulses = WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE};
    WalkGameplayEditResult edit;
    const uint64_t beforeRevision = camera.revision;
    Expect(WalkGameplaySubmitInput(&game, &input) == WALK_GAMEPLAY_OK &&
               WalkGameplayEdit(&game, &camera, &edit) == WALK_GAMEPLAY_OK && edit.count == 2u &&
               !edit.items[0].place && edit.items[0].changed && edit.items[1].place &&
               edit.items[1].changed && edit.items[0].coordinate.z == 0 &&
               edit.items[0].coordinate.x == edit.items[1].coordinate.x &&
               edit.items[0].coordinate.y == edit.items[1].coordinate.y &&
               edit.items[0].coordinate.z == edit.items[1].coordinate.z &&
               WalkGameplayReadBlock(&game, edit.items[1].coordinate.x, edit.items[1].coordinate.y,
                                     edit.items[1].coordinate.z) == 2u &&
               edit.revision == beforeRevision + 2u && game.pendingPulses == 0u,
           "same batch breaks first, placement ray sees the hole and fills it once");
    Expect(WalkGameplayOrient(&game, 0.0f, 0.0f, WALK_GAMEPLAY_ORIENT_ABSOLUTE) ==
                   WALK_GAMEPLAY_OK &&
               WalkGameplayCamera(&game, 640, 480, &camera) == WALK_GAMEPLAY_OK,
           "horizontal camera looks only through air");
    input.pulses = WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE;
    Expect(WalkGameplaySubmitInput(&game, &input) == WALK_GAMEPLAY_OK, "no-hit actions queued");
    WalkGameplaySpatialSnapshot foreign = camera;
    foreign.owner = &foreignOwner;
    Expect(WalkGameplayEdit(&game, &foreign, &edit) == WALK_GAMEPLAY_INVALID_ARGUMENT &&
               game.pendingPulses == (WALK_GAMEPLAY_PULSE_BREAK | WALK_GAMEPLAY_PULSE_PLACE),
           "foreign snapshots do not consume either pending action");
    Expect(WalkGameplayEdit(&game, &camera, &edit) == WALK_GAMEPLAY_OK && edit.count == 2u &&
               !edit.items[0].changed && !edit.items[1].changed &&
               edit.items[0].status == WALK_GAMEPLAY_OK &&
               edit.items[1].status == WALK_GAMEPLAY_OK && game.pendingPulses == 0u &&
               game.revision == camera.revision,
           "no-hit batch consumes both pulses without inventing successful edits or revisions");
    Expect(WalkGameplayEdit(&game, &camera, &edit) == WALK_GAMEPLAY_OK && edit.count == 0u,
           "consumed no-hit batch is not replayed on the next frame");
    WalkGameplayRelease(&game);
}

static void TestCharacterRebaseFailures(void)
{
    static WalkGameplay game;
    const WalkGameplayServices services = Services();
    WalkGameplayConfig config;
    WalkGameplayConfigDefault(&config);
    config.initialPosition.localX = 250;
    config.initialPosition.localY = 500;
    Expect(WalkGameplayInit(&game, &services, &config) == WALK_GAMEPLAY_OK,
           "character rebase failure session");
    const LaiueCharacterPositionV1 beforePosition = ((TestCharacter *)game.controller)->position;
    TestWorld *world = (TestWorld *)game.world;
    world->explicitBlock = true;
    world->coordinate = (LaiueVoxelCoordV1){4, 5, 0};
    world->block = (LaiueVoxelBlockV1){2u, 0u};
    const TestWorld beforeWorld = *world;
    WalkGameplaySpatialSnapshot before, after;
    Expect(WalkGameplaySnapshot(&game, &before) == WALK_GAMEPLAY_OK, "character before rebase");
    const int64_t shift[3] = {64, -128, 0};
    WalkGameplayRebaseResult rebase;
    const int64_t invalidShift[3] = {INT64_MIN, 0, 0};
    const uint32_t callsBeforeInvalid = characterRebaseCalls;
    Expect(WalkGameplayRebase(&game, invalidShift, &rebase) == WALK_GAMEPLAY_INVALID_ARGUMENT &&
               characterRebaseCalls == callsBeforeInvalid &&
               WalkGameplaySnapshot(&game, &after) == WALK_GAMEPLAY_OK &&
               after.revision == before.revision,
           "unrepresentable provider-frame position is rejected before rebasing either owner");
    characterRebaseCalls = 0u;
    rejectWorldRebase = true;
    Expect(WalkGameplayRebase(&game, shift, &rebase) == WALK_GAMEPLAY_PROVIDER_FAILURE &&
               characterRebaseCalls == 2u && !rebase.changed && !game.failed &&
               memcmp(&((TestCharacter *)game.controller)->position, &beforePosition,
                      sizeof(beforePosition)) == 0 &&
               memcmp(world, &beforeWorld, sizeof(beforeWorld)) == 0 &&
               WalkGameplaySnapshot(&game, &after) == WALK_GAMEPLAY_OK,
           "rejected world rebase restores the complete character before publishing its origin");
    double delta[3];
    Expect(WalkGameplaySpatialDelta(&before, &after, delta) && delta[0] == 0.0 && delta[1] == 0.0 &&
               delta[2] == 0.0 && before.revision == after.revision,
           "failed character rebase preserves absolute position and snapshot revision");
    characterRebaseCalls = 0u;
    rejectCharacterRollback = true;
    Expect(WalkGameplayRebase(&game, shift, &rebase) == WALK_GAMEPLAY_INCONSISTENT_STATE &&
               characterRebaseCalls == 2u && game.failed &&
               WalkGameplayHealth(&game) == WALK_GAMEPLAY_INCONSISTENT_STATE,
           "character rollback failure latches inconsistency");
    WalkGameplayStepResult step;
    WalkGameplayEditResult edit;
    Expect(WalkGameplayStep(&game, &step) == WALK_GAMEPLAY_INCONSISTENT_STATE &&
               WalkGameplayEdit(&game, &before, &edit) == WALK_GAMEPLAY_INCONSISTENT_STATE &&
               WalkGameplayRebase(&game, shift, &rebase) == WALK_GAMEPLAY_INCONSISTENT_STATE &&
               characterRebaseCalls == 2u,
           "inconsistent character cannot step, edit or invoke another rebase callback");
    rejectCharacterRollback = rejectWorldRebase = false;
    WalkGameplayRelease(&game);
}

LAIUE_TEST_ENTRY(WalkGameplayTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    TestPhysicsContexts();
    TestInitFailures();
    TestUnavailableServiceContracts();
    TestInputsAndLifetime();
    TestRagdollContextsAndRebase();
    TestEditsAndSpatialFrames();
    TestEditBatches();
    TestCharacterRebaseFailures();
    Expect(characterCreates == characterDestroys && worldCreates == worldDestroys,
           "all test resources released");
    LaiueTestRuntimeWrite("walk gameplay tests passed\n");
    LAIUE_TEST_SUCCESS();
}
