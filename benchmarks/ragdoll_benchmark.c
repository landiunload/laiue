// Ручной benchmark шага ragdoll: та же 13-тельная модель и тот же
// контроллер, что в примере walk, на полу из вокселей при 128 Гц. Ходьба
// по квадрату с поворотами нагружает решатель суставов, интеграцию и
// контакты одновременно. Не входит в CTest и не собирается по умолчанию.
//
// Печатает медиану мкс на тик по SAMPLE_COUNT выборкам и checksum конечного
// состояния тел. Baseline и candidate обязаны напечатать одинаковый checksum:
// это подтверждает, что изменена только стоимость, а не результат.

#include "physics/ragdoll.h"
#include "physics/numeric_provider.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "../examples/walk/humanoid_ragdoll.h"
#include "../examples/walk/walk_humanoid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Переопределяются при сборке, например для профилирования под callgrind.
#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 7u
#endif
#ifndef TICKS_PER_SAMPLE
#define TICKS_PER_SAMPLE 1024u
#endif

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

static void WriteFixed2(double value)
{
    uint64_t hundredths = (uint64_t)(value * 100.0 + 0.5);
    WriteUnsigned(hundredths / 100u);
    WriteText(".");
    if (hundredths % 100u < 10u)
        WriteText("0");
    WriteUnsigned(hundredths % 100u);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
        text[index] = digits[(value >> ((15u - index) * 4u)) & 15u];
    text[16] = '\0';
    WriteText(text);
}

static void Fail(const char *message)
{
    WriteText("Ragdoll benchmark failed: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && samples[insertion - 1u] > value)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

static uint64_t HashWord(uint64_t hash, uint64_t word)
{
    hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t HashCoordinate(uint64_t hash, const InfiniteCoord *coordinate)
{
    hash = HashWord(hash, (uint64_t)(int64_t)coordinate->sign);
    hash = HashWord(hash, coordinate->limbCount);
    for (uint32_t limb = 0u; limb < coordinate->limbCount; ++limb)
        hash = HashWord(hash, coordinate->limbs[limb]);
    return hash;
}

static uint64_t HashRagdoll(const VoxelRagdoll *ragdoll)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        const VoxelRigidBody *rigid = &ragdoll->bodies[body];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            hash = HashCoordinate(hash, &rigid->position[axis]);
            hash = HashCoordinate(hash, &rigid->linearVelocity[axis]);
            hash = HashCoordinate(hash, &rigid->angularVelocity[axis]);
        }
        uint64_t bits[4];
        memcpy(bits, rigid->orientation, sizeof(bits));
        for (uint32_t word = 0u; word < 4u; ++word)
            hash = HashWord(hash, bits[word]);
    }
    return hash;
}

static void QueryFloor(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    outBlock->flags = z < 1 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.75f;
}

typedef struct Scenario
{
    VoxelCollisionSource collision;
    VoxelRigidStepSettings rigidSettings;
    VoxelRagdollSettings ragdollSettings;
    void *scratch;
    uint32_t scratchBytes;
    VoxelRagdoll ragdoll;
    WalkHumanoidControllerState controller;
    bool grounded;
    double facing;
    double phase;
} Scenario;

static void ScenarioStart(Scenario *scenario, const LaiuePhysicsServiceV1 *physics)
{
    memset(scenario, 0, sizeof(*scenario));
    scenario->collision.queryBlockPhysics = QueryFloor;
    const double origin[3] = {0.0, 0.0, 1.0};
    if (!WalkHumanoidInitialize(physics, &scenario->ragdoll, &scenario->collision, origin, 1000u,
                                &scenario->rigidSettings, &scenario->ragdollSettings,
                                &scenario->scratch, &scenario->scratchBytes))
        Fail("humanoid initializes");
}

static void ScenarioStop(Scenario *scenario)
{
    VoxelRagdollRelease(&scenario->ragdoll);
    PlatformFree(scenario->scratch);
    scenario->scratch = NULL;
}

static void ScenarioRun(Scenario *scenario, const LaiuePhysicsServiceV1 *physics,
                        uint32_t firstTick, uint32_t tickCount)
{
    for (uint32_t tick = firstTick; tick < firstTick + tickCount; ++tick)
    {
        // Квадрат: вперёд, вправо, назад, остановка — повороты и старты.
        const uint32_t leg = (tick / 256u) & 3u;
        const double moveX = leg == 1u ? 1.0 : 0.0;
        const double moveY = leg == 0u ? 1.0 : (leg == 2u ? -1.0 : 0.0);
        WalkHumanoidStepFailure failure = WALK_HUMANOID_STEP_OK;
        if (!WalkHumanoidStep(physics, &scenario->ragdoll, &scenario->controller,
                              &scenario->collision, &scenario->rigidSettings,
                              &scenario->ragdollSettings, scenario->scratch, scenario->scratchBytes,
                              moveX, moveY, false, false, 1.0 / 128.0, &scenario->grounded,
                              &scenario->facing, &scenario->phase, &failure))
            Fail("walking step advances");
    }
}

LAIUE_TEST_ENTRY(RagdollBenchmarkEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    const LaiuePhysicsServiceV1 physics = {
        .configureThread = VoxelPhysicsConfigureThread,
        .stepScratchBytes = VoxelRigidBodyStepScratchBytes,
        .ragdollSettingsDefault = VoxelRagdollSettingsDefault,
        .ragdollInitialize = VoxelRagdollInitialize,
        .ragdollRelease = VoxelRagdollRelease,
        .ragdollStep = VoxelRagdollStep,
    };

    double samples[SAMPLE_COUNT];
    uint64_t checksum = 0u;
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
    {
        static Scenario scenario;
        ScenarioStart(&scenario, &physics);
        // Прогрев: тело встаёт на пол и набирает походку.
        ScenarioRun(&scenario, &physics, 0u, 128u);
        double start = PlatformMonotonicSeconds();
        ScenarioRun(&scenario, &physics, 128u, TICKS_PER_SAMPLE);
        double elapsed = PlatformMonotonicSeconds() - start;
        samples[sample] = elapsed * 1.0e6 / (double)TICKS_PER_SAMPLE;
        uint64_t hash = HashRagdoll(&scenario.ragdoll);
        if (sample == 0u)
            checksum = hash;
        else if (hash != checksum)
            Fail("every sample replays the same state");
        ScenarioStop(&scenario);
    }

    WriteText("ragdoll walk 13 bodies: ");
    WriteFixed2(Median(samples, SAMPLE_COUNT));
    WriteText(" us/tick median of ");
    WriteUnsigned(SAMPLE_COUNT);
    WriteText(" x ");
    WriteUnsigned(TICKS_PER_SAMPLE);
    WriteText(" ticks; checksum ");
    WriteHex(checksum);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
