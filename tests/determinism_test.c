#include "gameplay/player_controller.h"
#include "test_runtime.h"

// Харнесс-доказательство детерминизма физики. Прогоняет фиксированную ленту
// вводов через настоящий PlayerControllerUpdate и сворачивает состояние
// каждого тика в 64-битный хеш по точным битам double. Если один и тот же
// исходник, собранный разными компиляторами (MSVC и clang-cl) и в разных
// конфигурациях, даёт один и тот же хеш — симуляция побитово детерминирована,
// а значит по сети достаточно гонять вводы, а не состояние.
//
// Сим-модули (player_controller/jump/locomotion/stance, voxel_body)
// компилируются прямо в тест и закреплены за /fp:precise без FMA-контракции:
// глобальный /fp:fast переассоциирует выражения и сливает a*b+c в FMA, из-за
// чего результат зависит от CPU и компилятора.

static void TestWrite(const char* text)
{
    LaiueTestRuntimeWrite(text);
}

static void TestWriteHex64(uint64_t value)
{
    char text[17];
    for (uint32_t i = 0; i < 16u; ++i)
    {
        uint32_t nibble = (uint32_t)((value >> ((15u - i) * 4u)) & 0xfu);
        text[i] = (char)(nibble < 10u ? '0' + nibble : 'a' + (nibble - 10u));
    }
    text[16] = '\0';
    TestWrite(text);
}

static void TestExpect(bool condition, const char* message)
{
    if (condition)
    {
        return;
    }
    TestWrite("physics regression failed: ");
    TestWrite(message);
    TestWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static double AbsoluteDouble(double value)
{
    return value < 0.0 ? -value : value;
}

// --- Хеш по точным битам --------------------------------------------------

static uint64_t g_hash = 14695981039346656037ULL; // FNV-1a offset

static void HashBytes(const void* data, uint32_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    for (uint32_t i = 0; i < size; ++i)
    {
        g_hash ^= bytes[i];
        g_hash *= 1099511628211ULL; // FNV-1a prime
    }
}

static void HashDouble(double value)
{
    HashBytes(&value, sizeof(value));
}

// --- Детерминированный мир ------------------------------------------------

// Пол при z <= 0 и стена при x >= 6 (до z = 4). Игрок раз за разом врезается
// в стену на бегу и в прыжке — столкновения усиливают любое расхождение в
// младшем бите, поэтому длинный прогон ловит недетерминизм надёжнее гладкого.
static void QueryWorld(void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    (void)context;
    (void)y;
    bool solid = (z <= 0) || (x >= 6 && z <= 4);
    outBlock->flags = solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.6f;
}

static void QueryAir(void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = 0u;
    outBlock->friction = 0.0f;
}

static void QueryStaticFractionTestWorld(void* context,
    int64_t x, int64_t y, int64_t z, VoxelBlockPhysics* outBlock)
{
    (void)context;
    bool solid = x == 1 && y == 0 && z >= 0 && z <= 1;
    outBlock->flags = solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.6f;
}

typedef struct DynamicTestWorld
{
    VoxelDynamicCollider colliders[2];
    uint32_t count;
    uint32_t queryCount;
    uint32_t observedCapacity;
    bool filterToQueryBounds;
    bool truncated;
} DynamicTestWorld;

static double g_observedDynamicQueryMaximumX;

static bool TestBoundsOverlapStrict(const VoxelBodyBounds* left,
    const VoxelBodyBounds* right)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (left->maximum[axis] <= right->minimum[axis]
            || left->minimum[axis] >= right->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

static bool QueryTestDynamicColliders(void* context,
    const VoxelBodyBounds* queryBounds,
    VoxelDynamicCollider* outColliders, uint32_t colliderCapacity,
    uint32_t* outColliderCount)
{
    DynamicTestWorld* world = (DynamicTestWorld*)context;
    ++world->queryCount;
    world->observedCapacity = colliderCapacity;
    g_observedDynamicQueryMaximumX = queryBounds->maximum[0];
    uint32_t matchedCount = 0u;
    for (uint32_t index = 0u; index < world->count; ++index)
    {
        if (world->filterToQueryBounds
            && !TestBoundsOverlapStrict(
                queryBounds, &world->colliders[index].bounds))
        {
            continue;
        }
        if (matchedCount < colliderCapacity)
        {
            outColliders[matchedCount] = world->colliders[index];
        }
        ++matchedCount;
    }
    uint32_t copyCount = matchedCount < colliderCapacity
        ? matchedCount : colliderCapacity;
    *outColliderCount = copyCount;
    return !world->truncated && matchedCount <= colliderCapacity;
}

static void QueryPartialStaticSupport(void* context,
    int64_t x, int64_t y, int64_t z, VoxelBlockPhysics* outBlock)
{
    (void)context;
    bool solid = x == 0 && y == 0 && z == 0;
    outBlock->flags = solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.25f;
}

// --- Лента вводов ---------------------------------------------------------

// Полностью детерминированный узор: целочисленные периоды, никаких
// вещественных часов и никакого рандома. Направления — либо оси, либо
// заранее нормализованная диагональ, чтобы не завязываться на sqrt-нормировку.
static void ScriptCommand(uint32_t tick, PlayerControllerCommand* command)
{
    static const double diagonal = 0.70710678118654752440;

    uint32_t phase = tick % 512u;
    if (phase < 128u)
    {
        command->movementX = 1.0;
        command->movementY = 0.0;
    }
    else if (phase < 256u)
    {
        command->movementX = diagonal;
        command->movementY = diagonal;
    }
    else if (phase < 384u)
    {
        command->movementX = 0.0;
        command->movementY = -1.0;
    }
    else
    {
        command->movementX = -diagonal;
        command->movementY = diagonal;
    }

    command->jumpHeld = (tick % 137u) < 24u;
    command->jumpPressed = (tick % 137u) == 0u;
    command->sprintHeld = (tick % 200u) < 100u;
    command->crouchHeld = (tick % 90u) < 30u;
}

// --- Прогон ---------------------------------------------------------------

#define DETERMINISM_TICKS 60000u

// Замороженный сценарий: точное побитовое ожидание для этой ленты вводов и
// этого конфига. Значение снято прогоном под MSVC и clang-cl (Debug/Release,
// /fp:fast и /fp:precise, с FMA/AVX2 и без) — все дали ровно этот хеш.
// Если он изменился — physics перестала воспроизводиться 1-в-1: ищи новую
// трансценденту в шаге симуляции, переставленное выражение или возврат
// /fp:fast с реально включённым FMA. Осознанная правка формул/сценария —
// пересними значение здесь.
#define DETERMINISM_EXPECTED_HASH 0x6426d01ca5957726ULL

// Конфиг заморожен в тесте, а не берётся из PlayerControllerGetDefaultConfig:
// тюнинг геймплейных дефолтов не должен ронять проверку детерминизма. Это
// фиксированный эталонный вход, как и лента вводов ниже.
static void FrozenConfig(PlayerControllerConfig* config)
{
    const PlayerControllerConfig frozen = {
        .walkingSpeed = 4.0f,
        .sprintingSpeed = 6.0f,
        .crouchingSpeed = 3.5f,
        .groundAcceleration = 20.0f,
        .groundDeceleration = 30.0f,
        .airAcceleration = 4.0f,
        .sprintJumpSpeed = 8.0f,
        .gravity = 26.0f,
        .maximumFallSpeed = 55.0f,
        .jumpBufferSeconds = 0.14f,
        .coyoteTimeSeconds = 0.10f,
        .externalVelocityDamping = 8.0f,
        .fixedStepSeconds = 1.0 / 240.0,
        .maximumSubsteps = 32u,
        .jumpHeight = 1.275,
        .radius = 0.30,
        .standingHeight = 1.80,
        .standingEyeHeight = 1.75,
        .crouchingHeight = 1.30,
        .crouchingEyeHeight = 1.25,
        .collisionEpsilon = 0.001,
        .groundProbeDepth = 0.03,
        .sneakProbeDepth = 0.60,
        .crouchEyeDuration = 0.175,
        .crouchColliderDuration = 0.200,
        .standColliderDuration = 0.200,
        .standEyeDuration = 0.250,
    };
    *config = frozen;
}

static void InitializeDynamicCollider(VoxelDynamicCollider* collider,
    double minimumX, double maximumX,
    double minimumY, double maximumY,
    double minimumZ, double maximumZ,
    double velocityX, double velocityY, double velocityZ,
    uint64_t stableId)
{
    collider->bounds.minimum[0] = minimumX;
    collider->bounds.minimum[1] = minimumY;
    collider->bounds.minimum[2] = minimumZ;
    collider->bounds.maximum[0] = maximumX;
    collider->bounds.maximum[1] = maximumY;
    collider->bounds.maximum[2] = maximumZ;
    collider->velocity[0] = velocityX;
    collider->velocity[1] = velocityY;
    collider->velocity[2] = velocityZ;
    collider->friction = 0.6f;
    collider->stableId = stableId;
}

static void TestMovingPlatformRegression(const VoxelBodyShape* shape)
{
    DynamicTestWorld platformWorld = {0};
    platformWorld.count = 1u;
    platformWorld.filterToQueryBounds = true;
    InitializeDynamicCollider(&platformWorld.colliders[0],
        -2.0, 2.0, -2.0, 2.0, 0.0, 1.0,
        1.2, 0.0, 0.0, 41u);
    PlayerCollisionSource platformSource = {
        .context = &platformWorld,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    PlayerControllerConfig config;
    FrozenConfig(&config);
    double contactEyeZ = 1.0 + config.collisionEpsilon
        + config.standingEyeHeight;

    VoxelGroundContact contact;
    double contactPosition[3] = { 0.0, 0.0, contactEyeZ };
    VoxelBodyQueryGroundContact(&platformSource, contactPosition,
        shape, config.groundProbeDepth, &contact);
    TestExpect(contact.supported && contact.surfaceStableId == 41u
            && contact.surfaceVelocity[0] == 1.2,
        "dynamic ground contact exposes surface velocity and stable id");

    PlayerController carried;
    PlayerControllerInit(&carried, &config);
    Camera carriedCamera = {
        { 0.0, 0.0, contactEyeZ }, 0.0f, 0.0f
    };
    PlayerControllerCommand idle = {0};
    PlayerControllerSimulateFixedSteps(&carried,
        &platformSource, &carriedCamera, &idle, 1u);
    double expectedCarry = 1.2 * config.fixedStepSeconds;
    TestExpect(PlayerControllerIsGrounded(&carried),
        "player grounds on fractional moving platform");
    TestExpect(AbsoluteDouble(
            carriedCamera.position[0] - expectedCarry) < 1e-12,
        "grounded player receives one bounded horizontal carry step");

    platformWorld.colliders[0].velocity[0] = 1048576.0;
    PlayerControllerInit(&carried, &config);
    carriedCamera.position[0] = 0.0;
    carriedCamera.position[1] = 0.0;
    carriedCamera.position[2] = contactEyeZ;
    PlayerControllerSimulateFixedSteps(&carried,
        &platformSource, &carriedCamera, &idle, 1u);
    TestExpect(carriedCamera.position[0] == 0.5,
        "platform carry is bounded per substep");
    platformWorld.colliders[0].velocity[0] = 1.2;

    PlayerController jumping;
    PlayerControllerInit(&jumping, &config);
    Camera jumpingCamera = {
        { 0.0, 0.0, contactEyeZ }, 0.0f, 0.0f
    };
    PlayerControllerCommand jump = {
        .jumpPressed = true,
    };
    PlayerControllerSimulateFixedSteps(&jumping,
        &platformSource, &jumpingCamera, &jump, 1u);
    TestExpect(!PlayerControllerIsGrounded(&jumping)
            && jumping.externalVelocityX > 0.0,
        "jump inherits horizontal platform velocity");
    TestExpect(AbsoluteDouble(
            jumpingCamera.position[0] - expectedCarry) < 1e-12,
        "jump inheritance advances once without duplicate carry");

    PlayerController rising;
    PlayerControllerInit(&rising, &config);
    Camera risingCamera = {
        { 0.0, 0.0, contactEyeZ }, 0.0f, 0.0f
    };
    PlayerControllerApplyImpulse(&rising, 0.0f, 0.0f, 1.0f);
    PlayerControllerSimulateFixedSteps(&rising,
        &platformSource, &risingCamera, &idle, 1u);
    TestExpect(risingCamera.position[0] == 0.0,
        "airborne body inside probe depth receives no platform carry");

    expectedCarry = -1.2 * config.fixedStepSeconds;
    platformWorld.colliders[0].bounds.minimum[2] += expectedCarry;
    platformWorld.colliders[0].bounds.maximum[2] += expectedCarry;
    platformWorld.colliders[0].velocity[0] = 0.0;
    platformWorld.colliders[0].velocity[2] = -1.2;
    PlayerControllerInit(&rising, &config);
    risingCamera.position[0] = 0.0;
    risingCamera.position[1] = 0.0;
    risingCamera.position[2] = contactEyeZ;
    PlayerControllerSimulateFixedSteps(&rising,
        &platformSource, &risingCamera, &idle, 1u);
    TestExpect(PlayerControllerIsGrounded(&rising),
        "player remains grounded on a descending platform");
    TestExpect(AbsoluteDouble(risingCamera.position[2]
            - (contactEyeZ + expectedCarry)) < 1e-12,
        "descending platform displacement is applied exactly once");
}

static void TestDynamicAabbRegression(void)
{
    const VoxelBodyShape shape = {
        .radius = 0.30,
        .height = 1.80,
        .eyeHeight = 1.75,
        .collisionEpsilon = 0.001,
    };

    // A fractional face at x=1.25 must clip at that exact face. Treating the
    // collider as its containing voxel would stop at x=0.699 instead.
    DynamicTestWorld fractionalWorld = {0};
    fractionalWorld.count = 1u;
    fractionalWorld.filterToQueryBounds = true;
    InitializeDynamicCollider(&fractionalWorld.colliders[0],
        1.25, 1.75, -1.0, 1.0, -1.0, 2.0,
        0.0, 0.0, 0.0, 17u);
    VoxelCollisionSource fractionalSource = {
        .context = &fractionalWorld,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    double fractionalPosition[3] = { 0.0, 0.0, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            fractionalPosition, &shape, 0, 2.0),
        "fractional dynamic face reports collision");
    TestExpect(AbsoluteDouble(fractionalPosition[0] - 0.949) < 1e-12,
        "fractional dynamic face clips analytically");
    TestExpect(fractionalPosition[0] > 0.9,
        "fractional collider does not occupy a phantom full voxel");
    TestExpect(!VoxelBodyCollides(
            &fractionalSource, fractionalPosition, &shape),
        "clipped body remains outside fractional collider");
    TestExpect(fractionalWorld.observedCapacity
            == VOXEL_DYNAMIC_COLLIDER_CAPACITY,
        "dynamic broadphase capacity is fixed at 32");
    double negativePosition[3] = { 2.5, 0.0, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            negativePosition, &shape, 0, -2.0),
        "negative fractional sweep reports collision");
    TestExpect(AbsoluteDouble(negativePosition[0] - 2.051) < 1e-12,
        "negative fractional sweep clips at exact opposite face");

    double exactNegativePosition[3] = { 2.5, 0.0, 1.75 };
    double exactNegativeDistance =
        1.75 + shape.collisionEpsilon + shape.radius - 2.5;
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            exactNegativePosition, &shape, 0, exactNegativeDistance),
        "exact negative dynamic end contact reports collision");
    TestExpect(AbsoluteDouble(exactNegativePosition[0] - 2.051) < 1e-12,
        "exact negative dynamic end contact keeps analytic position");

    double exactContactPosition[3] = { 0.0, 0.0, 1.75 };
    double exactContactDistance =
        1.25 - shape.collisionEpsilon - shape.radius;
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            exactContactPosition, &shape, 0, exactContactDistance),
        "exact dynamic end contact reports collision");
    TestExpect(AbsoluteDouble(exactContactPosition[0] - 0.949) < 1e-12,
        "exact dynamic end contact keeps analytic position");

    // A body restored inside the collision margin is neither safely outside
    // nor allowed to cross the collider through the opposite face.
    double marginPosition[3] = { 0.95, 0.0, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            marginPosition, &shape, 0, 2.0),
        "dynamic epsilon-band start fails closed");
    TestExpect(marginPosition[0] == 0.95,
        "dynamic epsilon-band start cannot tunnel");
    double collisionMarginPosition[3] = { 0.9495, 0.0, 1.75 };
    TestExpect(VoxelBodyCollides(
            &fractionalSource, collisionMarginPosition, &shape),
        "dynamic overlap includes the collision margin");
    TestExpect(g_observedDynamicQueryMaximumX >= 1.25,
        "dynamic overlap broadphase includes the collision margin");
    double oppositeMarginPosition[3] = { 2.05, 0.0, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            oppositeMarginPosition, &shape, 0, -2.0),
        "opposite dynamic epsilon-band start fails closed");
    TestExpect(oppositeMarginPosition[0] == 2.05,
        "opposite dynamic epsilon-band start cannot tunnel");
    double transverseMarginPosition[3] = { 0.0, 1.3005, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            transverseMarginPosition, &shape, 0, 2.0),
        "transverse epsilon margin participates in axis clipping");
    TestExpect(AbsoluteDouble(
            transverseMarginPosition[0] - 0.949) < 1e-12,
        "transverse epsilon margin cannot bypass a dynamic face");

    union
    {
        uint64_t bits;
        double value;
    } quietNan = { 0x7ff8000000000000ULL };
    uint32_t queriesBeforeInvalidMove = fractionalWorld.queryCount;
    double invalidMovePosition[3] = { 0.0, 0.0, 1.75 };
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            invalidMovePosition, &shape, 0, quietNan.value),
        "NaN movement fails closed");
    TestExpect(invalidMovePosition[0] == 0.0
            && fractionalWorld.queryCount == queriesBeforeInvalidMove,
        "NaN movement neither mutates nor queries callbacks");
    TestExpect(VoxelBodyMoveAxis(&fractionalSource,
            invalidMovePosition, &shape, 0, 1.0e300),
        "out-of-range movement fails closed");
    TestExpect(invalidMovePosition[0] == 0.0
            && fractionalWorld.queryCount == queriesBeforeInvalidMove,
        "out-of-range movement avoids casts and callbacks");
    double invalidBodyPosition[3] = {
        quietNan.value, 0.0, 1.75
    };
    TestExpect(VoxelBodyCollides(
            &fractionalSource, invalidBodyPosition, &shape),
        "NaN body position fails closed");
    TestExpect(fractionalWorld.queryCount == queriesBeforeInvalidMove,
        "NaN body position avoids casts and callbacks");
    double invalidSneakX = quietNan.value;
    double invalidSneakY = 0.25;
    VoxelBodyClipSneakingMovement(&fractionalSource,
        invalidMovePosition, &shape, 0.6,
        &invalidSneakX, &invalidSneakY);
    TestExpect(invalidSneakX == 0.0 && invalidSneakY == 0.0
            && fractionalWorld.queryCount == queriesBeforeInvalidMove,
        "NaN sneak movement fails closed before callbacks");

    DynamicTestWorld duplicateWorld = {0};
    duplicateWorld.count = 2u;
    InitializeDynamicCollider(&duplicateWorld.colliders[0],
        10.0, 11.0, 10.0, 11.0, 10.0, 11.0,
        0.0, 0.0, 0.0, 29u);
    InitializeDynamicCollider(&duplicateWorld.colliders[1],
        12.0, 13.0, 12.0, 13.0, 12.0, 13.0,
        0.0, 0.0, 0.0, 29u);
    VoxelCollisionSource duplicateSource = {
        .context = &duplicateWorld,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    double duplicatePosition[3] = { 0.0, 0.0, 1.75 };
    TestExpect(VoxelBodyCollides(
            &duplicateSource, duplicatePosition, &shape),
        "duplicate dynamic stable ids fail closed");
    TestExpect(duplicateWorld.queryCount == 1u,
        "one collision operation invokes one dynamic callback");
    duplicateWorld.colliders[1].stableId = 30u;
    duplicateWorld.colliders[1].velocity[0] = quietNan.value;
    TestExpect(VoxelBodyCollides(
            &duplicateSource, duplicatePosition, &shape),
        "non-finite dynamic collider fails closed");

    // The legacy NULL callback path must retain the exact voxel clipping
    // convention and remains covered by the long deterministic hash below.
    VoxelCollisionSource staticSource = {
        .context = NULL,
        .queryBlockPhysics = QueryStaticFractionTestWorld,
        .queryDynamicColliders = NULL,
    };
    double staticPosition[3] = { 0.0, 0.5, 1.75 };
    TestExpect(VoxelBodyMoveAxis(
            &staticSource, staticPosition, &shape, 0, 2.0),
        "legacy voxel wall reports collision");
    TestExpect(AbsoluteDouble(staticPosition[0] - 0.699) < 1e-12,
        "legacy voxel clipping is unchanged");

    // Incomplete broadphase information blocks collision-sensitive movement,
    // grounding and sneak instead of silently tunnelling through omitted AABBs.
    DynamicTestWorld truncatedWorld = {0};
    truncatedWorld.count = 1u;
    truncatedWorld.truncated = true;
    InitializeDynamicCollider(&truncatedWorld.colliders[0],
        100.0, 101.0, 100.0, 101.0, 100.0, 101.0,
        0.0, 0.0, 0.0, 23u);
    VoxelCollisionSource truncatedSource = {
        .context = &truncatedWorld,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    double blockedPosition[3] = { 0.0, 0.0, 1.75 };
    TestExpect(VoxelBodyCollides(
            &truncatedSource, blockedPosition, &shape),
        "truncated collision query fails closed");
    double originalX = blockedPosition[0];
    TestExpect(VoxelBodyMoveAxis(
            &truncatedSource, blockedPosition, &shape, 0, 0.25)
            && blockedPosition[0] == originalX,
        "truncated sweep cannot move unchecked body");
    double sneakX = 0.25;
    double sneakY = 0.0;
    VoxelBodyClipSneakingMovement(&truncatedSource,
        blockedPosition, &shape, 0.6, &sneakX, &sneakY);
    TestExpect(sneakX == 0.0 && sneakY == 0.0,
        "truncated sneak support query blocks voluntary movement");

    DynamicTestWorld narrowSupportWorld = {0};
    narrowSupportWorld.count = 1u;
    narrowSupportWorld.filterToQueryBounds = true;
    InitializeDynamicCollider(&narrowSupportWorld.colliders[0],
        -0.35, 0.35, -0.35, 0.35, 0.0, 1.0,
        0.0, 0.0, 0.0, 31u);
    VoxelCollisionSource narrowSupportSource = {
        .context = &narrowSupportWorld,
        .queryBlockPhysics = QueryAir,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    double narrowSupportPosition[3] = { 0.0, 0.0, 2.751 };
    TestExpect(VoxelBodyHasGroundContact(&narrowSupportSource,
            narrowSupportPosition, &shape, 0.03)
            && VoxelBodyHasStableGround(&narrowSupportSource,
                narrowSupportPosition, &shape, 0.03, 0.1),
        "fractional dynamic surface supports ground probes");
    double exactProbePosition[3] = {
        0.0,
        0.0,
        1.0 + 0.03 + shape.collisionEpsilon + shape.eyeHeight,
    };
    TestExpect(VoxelBodyHasGroundContact(&narrowSupportSource,
            exactProbePosition, &shape, 0.03)
            && VoxelBodyHasStableGround(&narrowSupportSource,
                exactProbePosition, &shape, 0.03, 0.1),
        "exact dynamic probe-depth boundary survives broadphase");
    double clippedSneakX = 1.0;
    double clippedSneakY = 0.0;
    VoxelBodyClipSneakingMovement(&narrowSupportSource,
        narrowSupportPosition, &shape, 0.6,
        &clippedSneakX, &clippedSneakY);
    TestExpect(clippedSneakX > 0.0 && clippedSneakX < 1.0,
        "sneak clips against exact fractional support edge");

    // A coincident moving AABB must not steal ownership from voxel ground.
    // Otherwise a larger overlapping platform footprint injects velocity
    // even though the selected top is ordinary static terrain.
    DynamicTestWorld coincidentSupportWorld = {0};
    coincidentSupportWorld.count = 1u;
    InitializeDynamicCollider(&coincidentSupportWorld.colliders[0],
        -2.0, 2.0, -2.0, 2.0, 0.0, 1.0,
        9.0, 0.0, 0.0, 37u);
    VoxelCollisionSource coincidentSupportSource = {
        .context = &coincidentSupportWorld,
        .queryBlockPhysics = QueryPartialStaticSupport,
        .queryDynamicColliders = QueryTestDynamicColliders,
    };
    double coincidentSupportPosition[3] = { 0.9, 0.5, 2.751 };
    VoxelGroundContact coincidentContact;
    VoxelBodyQueryGroundContact(&coincidentSupportSource,
        coincidentSupportPosition, &shape, 0.03, &coincidentContact);
    TestExpect(coincidentContact.supported
            && coincidentContact.surfaceStableId == 0u
            && coincidentContact.surfaceVelocity[0] == 0.0,
        "static support wins a same-height dynamic tie");
    coincidentSupportWorld.colliders[0].bounds.minimum[2] += 0.01;
    coincidentSupportWorld.colliders[0].bounds.maximum[2] += 0.01;
    coincidentSupportPosition[2] += 0.01;
    VoxelBodyQueryGroundContact(&coincidentSupportSource,
        coincidentSupportPosition, &shape, 0.03, &coincidentContact);
    TestExpect(coincidentContact.surfaceStableId == 37u
            && coincidentContact.surfaceVelocity[0] == 9.0,
        "higher dynamic support wins over lower static ground");

    coincidentSupportWorld.count = 2u;
    InitializeDynamicCollider(&coincidentSupportWorld.colliders[0],
        -2.0, 2.0, -2.0, 2.0, 0.0, 1.0,
        7.0, 0.0, 0.0, 83u);
    InitializeDynamicCollider(&coincidentSupportWorld.colliders[1],
        -2.0, 2.0, -2.0, 2.0, 0.0, 1.0,
        3.0, 0.0, 0.0, 79u);
    coincidentSupportSource.queryBlockPhysics = QueryAir;
    double orderedSupportPosition[3] = { 0.0, 0.0, 2.751 };
    VoxelBodyQueryGroundContact(&coincidentSupportSource,
        orderedSupportPosition, &shape, 0.03, &coincidentContact);
    TestExpect(coincidentContact.surfaceStableId == 79u
            && coincidentContact.surfaceVelocity[0] == 3.0,
        "dynamic support selection is stable-id deterministic");
    VoxelDynamicCollider swap = coincidentSupportWorld.colliders[0];
    coincidentSupportWorld.colliders[0] =
        coincidentSupportWorld.colliders[1];
    coincidentSupportWorld.colliders[1] = swap;
    VoxelBodyQueryGroundContact(&coincidentSupportSource,
        orderedSupportPosition, &shape, 0.03, &coincidentContact);
    TestExpect(coincidentContact.surfaceStableId == 79u
            && coincidentContact.surfaceVelocity[0] == 3.0,
        "dynamic callback order does not change selected support");

    TestMovingPlatformRegression(&shape);
}

static void FoldState(const PlayerController* controller, const Camera* camera)
{
    HashDouble(camera->position[0]);
    HashDouble(camera->position[1]);
    HashDouble(camera->position[2]);
    HashDouble(controller->locomotion.velocityX);
    HashDouble(controller->locomotion.velocityY);
    HashDouble(controller->jump.verticalVelocity);
    HashDouble(controller->jump.jumpBufferRemaining);
    HashDouble(controller->jump.coyoteTimeRemaining);
    HashDouble(controller->externalVelocityX);
    HashDouble(controller->externalVelocityY);
    HashDouble(controller->stance.colliderCrouchProgress);
    HashDouble(controller->stance.eyeCrouchProgress);
    int32_t airJumpsRemaining =
        controller->jump.airJumpsRemaining;
    HashBytes(&airJumpsRemaining, sizeof(airJumpsRemaining));
    uint8_t crouchingRequested =
        controller->stance.crouchingRequested ? 1u : 0u;
    HashBytes(&crouchingRequested, sizeof(crouchingRequested));
    uint8_t grounded = controller->grounded ? 1u : 0u;
    HashBytes(&grounded, sizeof(grounded));
}

LAIUE_TEST_ENTRY(DeterminismTestEntryPoint)
{
    TestDynamicAabbRegression();

    PlayerControllerConfig config;
    FrozenConfig(&config);

    PlayerController controller;
    PlayerControllerInit(&controller, &config);

    Camera camera = { { 0.5, 0.5, 3.0 }, 0.0f, 0.0f };
    PlayerCollisionSource collision = {
        .context = NULL,
        .queryBlockPhysics = QueryWorld,
        .queryDynamicColliders = NULL,
    };

    // Ровно один фиксированный шаг на тик: подаём deltaSeconds == fixedStep,
    // и аккумулятор исполняет один SimulateStep. Число шагов не зависит от
    // настенных часов — это условие детерминизма поверх сети.
    double step = config.fixedStepSeconds;

    for (uint32_t tick = 0; tick < DETERMINISM_TICKS; ++tick)
    {
        PlayerControllerCommand command;
        ScriptCommand(tick, &command);
        PlayerControllerUpdate(&controller, &collision, &camera, &command, step);
        FoldState(&controller, &camera);
    }

    TestWrite("determinism-hash: ");
    TestWriteHex64(g_hash);
    TestWrite("\r\n");

#ifdef DETERMINISM_DIAGNOSTIC
    // Диагностика: подтверждает, что симуляция реально двигалась (иначе хеш
    // совпал бы тривиально). Печатаем биты финального состояния.
    union { double value; uint64_t bits; } view;
    for (uint32_t i = 0; i < 3u; ++i)
    {
        view.value = camera.position[i];
        TestWrite("pos "); TestWriteHex64(view.bits); TestWrite("\r\n");
    }
    view.value = controller.jump.verticalVelocity;
    TestWrite("vvel "); TestWriteHex64(view.bits); TestWrite("\r\n");
#endif

    if (g_hash != DETERMINISM_EXPECTED_HASH)
    {
        TestWrite("ДЕТЕРМИНИЗМ НАРУШЕН: ожидался ");
        TestWriteHex64(DETERMINISM_EXPECTED_HASH);
        TestWrite("\r\n");
        LaiueTestRuntimeExit(1);
    }
    LAIUE_TEST_SUCCESS();
}
