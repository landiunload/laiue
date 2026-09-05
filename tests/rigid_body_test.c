#include "physics/rigid_body.h"
#include "test_runtime.h"

#include <float.h>
#include <string.h>

// Твёрдое тело с вращением и скоростью произвольной точности. Проверяется
// не «функция вернула true», а поведение: куб ложится на пол на нужной
// высоте, опрокидывается через край, крутится сам по себе, и разгон без
// потолка остаётся разгоном без потолка.

static uint32_t rigidChecks;

static void RigidExpect(bool condition, const char *name)
{
    ++rigidChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Rigid body check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static bool Near(double value, double expected, double tolerance)
{
    return Absolute(value - expected) <= tolerance;
}

// Пол: сплошной слой на z < 0. Ступенька по X > 4 поднимает его на блок —
// на её краю тело обязано опрокидываться.
typedef struct RigidWorld
{
    bool step;
} RigidWorld;

// Порядок параметров задан ABI движка.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryBlocks(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *outBlock)
{
    const RigidWorld *world = (const RigidWorld *)context;
    (void)y;
    bool solid = z < 0;
    if (world->step && x >= 4 && z == 0)
    {
        solid = true;
    }
    outBlock->flags = solid ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.6f;
}

typedef struct RigidHarness
{
    RigidWorld world;
    VoxelCollisionSource collision;
    VoxelRigidStepSettings settings;
    uint8_t scratch[16384];
} RigidHarness;

static void HarnessInit(RigidHarness *harness, bool step)
{
    memset(harness, 0, sizeof(*harness));
    harness->world.step = step;
    harness->collision.context = &harness->world;
    harness->collision.queryBlockPhysics = QueryBlocks;
    harness->collision.queryDynamicColliders = NULL;
    VoxelRigidStepSettingsDefault(&harness->settings);
}

static void DescribeCube(VoxelRigidBodyDescription *description, double x, double y, double z)
{
    memset(description, 0, sizeof(*description));
    description->halfExtent[0] = 0.5;
    description->halfExtent[1] = 0.5;
    description->halfExtent[2] = 0.5;
    description->position[0] = x;
    description->position[1] = y;
    description->position[2] = z;
    description->mass = 1.0;
    description->restitution = 0.0;
    description->friction = 0.6;
}

static bool Advance(RigidHarness *harness, VoxelRigidBody *bodies, uint32_t count, uint32_t steps)
{
    for (uint32_t index = 0; index < steps; ++index)
    {
        if (!VoxelRigidBodyStep(bodies, count, &harness->collision, &harness->settings,
                                harness->scratch, (uint32_t)sizeof(harness->scratch)))
        {
            return false;
        }
    }
    return true;
}

static void TestDescriptionRefusals(void)
{
    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 4.0);

    RigidExpect(!VoxelRigidBodyInitialize(NULL, 1u, &description), "тело обязано быть задано");
    RigidExpect(!VoxelRigidBodyInitialize(&body, 0u, &description), "нулевой идентификатор");
    VoxelRigidBodyRelease(&body);

    description.mass = 0.0;
    RigidExpect(!VoxelRigidBodyInitialize(&body, 1u, &description), "нулевая масса");
    VoxelRigidBodyRelease(&body);

    DescribeCube(&description, 0.0, 0.0, 4.0);
    description.halfExtent[1] = -1.0;
    RigidExpect(!VoxelRigidBodyInitialize(&body, 1u, &description), "отрицательное полуребро");
    VoxelRigidBodyRelease(&body);

    DescribeCube(&description, 0.0, 0.0, 4.0);
    description.restitution = 1.5;
    RigidExpect(!VoxelRigidBodyInitialize(&body, 1u, &description), "упругость вне диапазона");
    VoxelRigidBodyRelease(&body);
}

static void TestRestsOnFloor(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.25, 0.25, 6.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(Advance(&harness, &body, 1u, 512u), "шаги выполнены");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    // Пол кончается на z = 0, центр куба с полуребром 0.5 обязан лечь на 0.5.
    RigidExpect(Near(position[2], 0.5, 0.05), "куб лежит на полу");
    RigidExpect(position[2] > 0.4, "куб не провалился сквозь пол");
    RigidExpect(VoxelRigidBodyLinearSpeed(&body) < 0.2, "куб остановился");

    // Куб упал плашмя: ориентация обязана остаться единичной, иначе решатель
    // выдумывает вращение из симметричного контакта.
    RigidExpect(Near(body.orientation[3], 1.0, 0.02) || Near(body.orientation[3], -1.0, 0.02),
                "падение плашмя не крутит куб");
    RigidExpect(VoxelRigidBodyAngularSpeed(&body) < 0.5, "куб не раскрутился на ровном месте");

    VoxelRigidBodyRelease(&body);
}

static void TestTipsOverEdge(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, true);

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    // Куб свисает со ступеньки: половина ширины лежит на ней, половина
    // висит над обрывом в блок. Опора несимметрична, и он обязан
    // повернуться — это и есть проверка, что вращение вообще работает.
    DescribeCube(&description, 4.3, 0.5, 2.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(Advance(&harness, &body, 1u, 256u), "шаги выполнены");

    double columns = Absolute(body.orientation[0]) + Absolute(body.orientation[1]) +
                     Absolute(body.orientation[2]);
    RigidExpect(columns > 0.02, "несимметричная опора обязана повернуть куб");

    VoxelRigidBodyRelease(&body);
}

static void TestFreeSpinPersists(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 20.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    const double spin[3] = {0.0, 0.0, 3.0};
    RigidExpect(VoxelRigidBodyAddAngularVelocity(&body, spin), "закрутка задана");
    RigidExpect(Near(VoxelRigidBodyAngularSpeed(&body), 3.0, 1e-6), "скорость вращения записана");

    RigidExpect(Advance(&harness, &body, 1u, 128u), "шаги выполнены");
    // Без контактов момент импульса сохраняется точно.
    RigidExpect(Near(VoxelRigidBodyAngularSpeed(&body), 3.0, 1e-6), "свободное вращение не гаснет");

    // За секунду при 3 рад/с куб повернётся на 3 радиана — кватернион обязан
    // это показать и остаться единичным.
    double length = body.orientation[0] * body.orientation[0] +
                    body.orientation[1] * body.orientation[1] +
                    body.orientation[2] * body.orientation[2] +
                    body.orientation[3] * body.orientation[3];
    RigidExpect(Near(length, 1.0, 1e-9), "кватернион остался единичным");
    RigidExpect(Near(body.orientation[3], 0.0707372, 0.01), "повернулся ровно на 3 радиана");

    VoxelRigidBodyRelease(&body);
}

static void TestPointVelocityCarries(void)
{
    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 0.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    const double linear[3] = {2.0, 0.0, 0.0};
    const double spin[3] = {0.0, 0.0, 1.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&body, linear), "скорость задана");
    RigidExpect(VoxelRigidBodyAddAngularVelocity(&body, spin), "вращение задано");

    // Точка на краю по +X: вращение вокруг Z добавляет ей скорость по +Y.
    const double point[3] = {0.5, 0.0, 0.0};
    double velocity[3];
    RigidExpect(VoxelRigidBodyPointVelocity(&body, point, velocity), "скорость точки читается");
    RigidExpect(Near(velocity[0], 2.0, 1e-9), "перенос сохраняется");
    RigidExpect(Near(velocity[1], 0.5, 1e-9), "вращение добавляет касательную");

    VoxelRigidBodyRelease(&body);
}

static void TestUnboundedSpeed(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 8.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    // Разгон до величины, которой нет места ни в int64, ни в блоках мира.
    const double insane[3] = {1e40, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&body, insane), "невероятный разгон принят");
    double speed = VoxelRigidBodyLinearSpeed(&body);
    RigidExpect(speed > 9e39 && speed < 1.1e40, "скорость сохранена как есть");
    RigidExpect(speed <= DBL_MAX, "скорость не стала бесконечностью");

    // Такой разгон повторяется сколько угодно раз: потолка нет.
    for (uint32_t index = 0; index < 64u; ++index)
    {
        RigidExpect(VoxelRigidBodyAddLinearVelocity(&body, insane), "разгон продолжается");
    }
    RigidExpect(VoxelRigidBodyLinearSpeed(&body) > 6e41, "скорость продолжила расти");

    // Шаг обязан пройти без падения, а тело — улететь. Позиция за пределами
    // double, поэтому локальные координаты честно отказывают.
    RigidExpect(Advance(&harness, &body, 1u, 4u), "шаг на невероятной скорости");
    double position[3];
    bool readable = VoxelRigidBodyLocalPosition(&body, position);
    RigidExpect(!readable || position[0] > 1e38, "тело действительно улетело");

    VoxelRigidBodyRelease(&body);
}

static void TestUnboundedSpinStaysSane(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 20.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    const double spin[3] = {0.0, 0.0, 1e30};
    RigidExpect(VoxelRigidBodyAddAngularVelocity(&body, spin), "невероятная закрутка принята");
    RigidExpect(VoxelRigidBodyAngularSpeed(&body) > 9e29, "угловая скорость сохранена");

    RigidExpect(Advance(&harness, &body, 1u, 16u), "шаги на невероятной закрутке");
    double length = body.orientation[0] * body.orientation[0] +
                    body.orientation[1] * body.orientation[1] +
                    body.orientation[2] * body.orientation[2] +
                    body.orientation[3] * body.orientation[3];
    RigidExpect(Near(length, 1.0, 1e-9), "кватернион уцелел");
    RigidExpect(VoxelRigidBodyAngularSpeed(&body) > 9e29, "закрутка не потерялась");
    VoxelRigidBodyRelease(&body);

    // Проверка по существу: закрутка и та же закрутка плюс целое число
    // оборотов за шаг обязаны дать одну и ту же ориентацию. Ровно кратный
    // оборот для этого не годится — он даёт верный ответ и без приведения,
    // потому что синус половины угла там и так обращается в ноль.
    const double twoPi = 6.283185307179586;
    VoxelRigidBody plain;
    VoxelRigidBody wrapped;
    DescribeCube(&description, 0.0, 0.0, 20.0);
    RigidExpect(VoxelRigidBodyInitialize(&plain, 2u, &description), "образец создан");
    RigidExpect(VoxelRigidBodyInitialize(&wrapped, 3u, &description), "сравниваемое создано");

    const double slow[3] = {0.0, 0.0, 1.0};
    const double slowPlusTurns[3] = {0.0, 0.0, 1.0 + twoPi * 128.0 * 1000.0};
    RigidExpect(VoxelRigidBodyAddAngularVelocity(&plain, slow), "малая закрутка");
    RigidExpect(VoxelRigidBodyAddAngularVelocity(&wrapped, slowPlusTurns),
                "та же закрутка плюс тысяча оборотов за шаг");

    RigidExpect(Advance(&harness, &plain, 1u, 16u), "шаги образца");
    RigidExpect(Advance(&harness, &wrapped, 1u, 16u), "шаги сравниваемого");
    for (int32_t index = 0; index < 4; ++index)
    {
        RigidExpect(Near(plain.orientation[index], wrapped.orientation[index], 1e-3),
                    "целые обороты не меняют ориентацию");
    }
    VoxelRigidBodyRelease(&plain);
    VoxelRigidBodyRelease(&wrapped);
}

static void TestRebasing(void)
{
    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 3.5, -2.5, 7.5);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    const int64_t shift[3] = {512, -512, 0};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, shift), "перенос выполнен");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(Near(position[0], 3.5 - 512.0, 1e-9), "X уехал вместе с сеткой");
    RigidExpect(Near(position[1], -2.5 + 512.0, 1e-9), "Y уехал вместе с сеткой");
    RigidExpect(Near(position[2], 7.5, 1e-9), "Z не тронут");

    VoxelRigidBodyRelease(&body);
}

static void TestStableIdOrder(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;

    VoxelRigidBody ordered[2];
    VoxelRigidBody shuffled[2];
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 0.5);
    RigidExpect(VoxelRigidBodyInitialize(&ordered[0], 11u, &description),
                "stable-order first body created");
    description.position[0] = 0.8;
    RigidExpect(VoxelRigidBodyInitialize(&ordered[1], 22u, &description),
                "stable-order second body created");
    const double firstVelocity[3] = {0.25, 0.0, 0.0};
    const double secondVelocity[3] = {-0.1, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&ordered[0], firstVelocity),
                "stable-order first velocity set");
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&ordered[1], secondVelocity),
                "stable-order second velocity set");

    // Same physical state, deliberately reversed in memory. stableId must
    // make contact traversal and the resulting bits independent of storage.
    description.position[0] = 0.8;
    RigidExpect(VoxelRigidBodyInitialize(&shuffled[0], 22u, &description),
                "shuffled second body created");
    description.position[0] = 0.0;
    RigidExpect(VoxelRigidBodyInitialize(&shuffled[1], 11u, &description),
                "shuffled first body created");
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&shuffled[0], secondVelocity),
                "shuffled second velocity set");
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&shuffled[1], firstVelocity),
                "shuffled first velocity set");

    RigidExpect(VoxelRigidBodyStep(ordered, 2u, &harness.collision, &harness.settings,
                                   harness.scratch, (uint32_t)sizeof(harness.scratch)),
                "stable-order canonical step");
    RigidExpect(VoxelRigidBodyStep(shuffled, 2u, &harness.collision, &harness.settings,
                                   harness.scratch, (uint32_t)sizeof(harness.scratch)),
                "stable-order shuffled step");

    for (uint32_t index = 0; index < 2u; ++index)
    {
        const VoxelRigidBody *left = ordered + index;
        const VoxelRigidBody *right = index == 0u ? shuffled + 1u : shuffled;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            RigidExpect(InfiniteCoordCompare(&left->position[axis], &right->position[axis]) == 0,
                        "stable-order position is storage-independent");
            RigidExpect(InfiniteCoordCompare(&left->linearVelocity[axis],
                                             &right->linearVelocity[axis]) == 0,
                        "stable-order velocity is storage-independent");
        }
        for (int32_t component = 0; component < 4; ++component)
        {
            RigidExpect(left->orientation[component] == right->orientation[component],
                        "stable-order orientation is storage-independent");
        }
    }

    VoxelRigidBodyRelease(&ordered[0]);
    VoxelRigidBodyRelease(&ordered[1]);
    VoxelRigidBodyRelease(&shuffled[0]);
    VoxelRigidBodyRelease(&shuffled[1]);
}

static void TestStackSettles(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);

    VoxelRigidBody bodies[3];
    for (uint32_t index = 0; index < 3u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, 0.0, 0.0, 1.0 + 1.2 * (double)index);
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "тело стопки создано");
    }

    RigidExpect(Advance(&harness, bodies, 3u, 768u), "шаги выполнены");

    double heights[3];
    for (uint32_t index = 0; index < 3u; ++index)
    {
        double position[3];
        RigidExpect(VoxelRigidBodyLocalPosition(&bodies[index], position), "позиция читается");
        heights[index] = position[2];
        RigidExpect(position[2] > 0.4, "ни один куб не провалился сквозь пол");
        RigidExpect(VoxelRigidBodyLinearSpeed(&bodies[index]) < 0.5, "стопка успокоилась");
    }
    // Три куба высотой 1 обязаны выстроиться примерно по 0.5, 1.5 и 2.5.
    RigidExpect(heights[0] < heights[1] && heights[1] < heights[2], "порядок стопки сохранился");
    RigidExpect(heights[2] < 3.2, "стопка не раздулась");

    for (uint32_t index = 0; index < 3u; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
}

static void TestSleepAndWake(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.sleepLinearSpeed = 0.1;
    harness.settings.sleepAngularSpeed = 0.1;
    harness.settings.sleepFrames = 4u;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.25, 0.25, 0.49);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "sleep body created");
    RigidExpect(Advance(&harness, &body, 1u, 16u), "sleep steps executed");
    RigidExpect(body.sleeping, "resting body enters sleep");

    double before[3];
    double after[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, before), "sleep position readable");
    RigidExpect(Advance(&harness, &body, 1u, 16u), "sleep fast path executed");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, after), "sleep position remains readable");
    RigidExpect(Near(before[0], after[0], 1e-12) && Near(before[1], after[1], 1e-12) &&
                    Near(before[2], after[2], 1e-12),
                "sleep fast path keeps position stable");

    const double impulse[3] = {1.0, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&body, impulse), "external impulse wakes body");
    RigidExpect(!body.sleeping, "external impulse clears sleep");
    VoxelRigidBodyWake(&body);
    RigidExpect(!body.sleeping && body.sleepCounter == 0u, "explicit wake is idempotent");
    VoxelRigidBodyRelease(&body);
}

static void TestScratchRefusals(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 4.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(VoxelRigidBodyStepScratchBytes(0u) == 0u, "нулевое число тел не имеет размера");
    uint32_t required = VoxelRigidBodyStepScratchBytes(1u);
    RigidExpect(required > 0u, "размер буфера известен");
    RigidExpect(VoxelRigidBodyStep(&body, 1u, &harness.collision, &harness.settings,
                                   harness.scratch + 1u, (uint32_t)sizeof(harness.scratch) - 1u),
                "буфер шага допускает произвольное выравнивание");
    RigidExpect(!VoxelRigidBodyStep(&body, 1u, &harness.collision, &harness.settings,
                                    harness.scratch, required - 1u),
                "малый буфер отвергается");
    RigidExpect(!VoxelRigidBodyStep(&body, 1u, NULL, &harness.settings, harness.scratch,
                                    required),
                "источник столкновений обязателен");

    VoxelRigidStepSettings broken = harness.settings;
    broken.solverIterations = 0u;
    RigidExpect(!VoxelRigidBodyStep(&body, 1u, &harness.collision, &broken, harness.scratch,
                                    required),
                "ноль итераций отвергается");

    VoxelRigidBodyRelease(&body);
}

LAIUE_TEST_ENTRY(RigidBodyTestEntryPoint)
{
    TestDescriptionRefusals();
    TestRestsOnFloor();
    TestTipsOverEdge();
    TestFreeSpinPersists();
    TestPointVelocityCarries();
    TestUnboundedSpeed();
    TestUnboundedSpinStaysSane();
    TestRebasing();
    TestStableIdOrder();
    TestStackSettles();
    TestSleepAndWake();
    TestScratchRefusals();

    LaiueTestRuntimeWrite("Rigid body tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
