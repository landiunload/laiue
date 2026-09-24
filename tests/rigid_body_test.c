#include "physics/rigid_body.h"
#include "physics/numeric_provider.h"
#include "test_runtime.h"

#include <float.h>
#include <string.h>

// Твёрдое тело с вращением и скоростью произвольной точности. Проверяется
// не «функция вернула true», а поведение: куб ложится на пол на нужной
// высоте, опрокидывается через край, крутится сам по себе, и разгон без
// потолка остаётся разгоном без потолка.

static void RigidExpect(bool condition, const char *name)
{
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
    bool removeFloor;
    // Столб шириной в один блок. Повёрнутый куб стоит на нём рёбрами:
    // ни один его угол внутрь столба не попадает.
    bool pillar;
    // Пол ровно в один слой: у него открыта и нижняя грань.
    bool thinFloor;
} RigidWorld;

// Порядок параметров задан ABI движка.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryBlocks(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *outBlock)
{
    const RigidWorld *world = (const RigidWorld *)context;
    bool solid = !world->removeFloor && z < 0;
    if (world->pillar)
    {
        solid = x == 0 && y == 0 && z < 0;
    }
    else if (world->thinFloor)
    {
        solid = z == -1;
    }
    else if (world->step && x >= 4 && z == 0)
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
    uint8_t scratch[131072];
} RigidHarness;

static void HarnessInit(RigidHarness *harness, bool step)
{
    // The destination size is the exact object size, not external input.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
    memset(harness, 0, sizeof(*harness));
    harness->world.step = step;
    harness->collision.context = &harness->world;
    harness->collision.queryBlockPhysics = QueryBlocks;
    harness->collision.queryDynamicColliders = NULL;
    VoxelRigidStepSettingsDefault(&harness->settings);
}

static void DescribeCube(VoxelRigidBodyDescription *description, double x, double y, double z)
{
    // Exact object bounds; this test also runs with the no-CRT runtime.
    // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
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

static void TestRotatedBoxRestsOnPillar(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.world.pillar = true;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.5, 0.5, 2.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");
    // Поворот на 45 градусов вокруг Z уводит все четыре нижних угла за
    // пределы столба: их проекции лежат на 0.207 блока дальше его граней.
    // Держат куб только рёбра, а проверка углов их не видит вовсе.
    body.orientation[0] = 0.0;
    body.orientation[1] = 0.0;
    body.orientation[2] = 0.3826834323650898;
    body.orientation[3] = 0.9238795325112867;

    RigidExpect(Advance(&harness, &body, 1u, 512u), "шаги выполнены");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[2] > 0.4, "повёрнутый куб не проваливается сквозь столб");
    RigidExpect(Near(position[2], 0.5, 0.05), "повёрнутый куб лежит на столбе");
    RigidExpect(Near(position[0], 0.5, 0.2) && Near(position[1], 0.5, 0.2),
                "опора симметрична и куб не съезжает");

    VoxelRigidBodyRelease(&body);
}

static void TestThinFloorPushesUp(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.world.thinFloor = true;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    // Куб уже сидит в полу глубже половины блока: так его продавливает
    // куча сверху. Выталкивать его по ближайшей грани блока нельзя —
    // ближайшей оказывается нижняя, и тело уезжает сквозь пол вниз.
    DescribeCube(&description, 0.25, 0.25, -0.3);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(Advance(&harness, &body, 1u, 2048u), "шаги выполнены");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[2] > 0.0, "куб выдавлен наверх, а не сквозь пол");
    RigidExpect(Near(position[2], 0.5, 0.05), "куб лёг на однослойный пол");

    VoxelRigidBodyRelease(&body);
}

static void TestOverhangingBoxKeepsContact(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.world.pillar = true;

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.5, 0.5, 2.0);
    // Плита шире столба: её углы висят далеко в воздухе, и опирается она
    // на столб серединой грани. Ни один угол снова не внутри блока.
    description.halfExtent[0] = 2.0;
    description.halfExtent[1] = 2.0;
    description.halfExtent[2] = 0.25;
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(Advance(&harness, &body, 1u, 256u), "шаги выполнены");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[2] > 0.15, "плита не проваливается сквозь столб гранью");

    VoxelRigidBodyRelease(&body);
}

static void TestWorldContactsUseSharedBudget(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;

    // Широкая плита пересекает шесть выборок сплошного пола. Каждая даёт
    // четыре опорные точки: общий бюджет двух слотов вмещает все 24, даже
    // когда второй слот неактивен. Личный лимит 16 тихо терял часть опоры.
    VoxelRigidBody bodies[2] = {0};
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.25, 0.5, 0.49);
    description.halfExtent[0] = 16.0;
    RigidExpect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "плита создана");
    RigidExpect(Advance(&harness, bodies, 2u, 1u), "общего бюджета хватает для широкой опоры");
    VoxelRigidStepStats stats;
    RigidExpect(
        VoxelRigidBodyReadStepStats(harness.scratch, 2u, (uint32_t)sizeof(harness.scratch), &stats),
        "статистика широкой опоры читается");
    RigidExpect(stats.activeBodyCount == 1u && stats.contactCount == 24u,
                "одно тело использует общий бюджет без потери опорных контактов");
    VoxelRigidBodyRelease(&bodies[0]);

    // С одним слотом общий бюджет всего 16: такой шаг должен отказать до
    // решения и интеграции, а не сообщать об успехе усечённой симуляции.
    RigidExpect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description), "плита создана заново");
    double before[3];
    double after[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&bodies[0], before), "исходная позиция читается");
    RigidExpect(!Advance(&harness, bodies, 1u, 1u), "неполная опора не считается успешным шагом");
    RigidExpect(VoxelRigidBodyLocalPosition(&bodies[0], after), "позиция после отказа читается");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        RigidExpect(before[axis] == after[axis], "переполнение не интегрирует положение");
    }
    RigidExpect(VoxelRigidBodyLinearSpeed(&bodies[0]) == 0.0 &&
                    VoxelRigidBodyAngularSpeed(&bodies[0]) == 0.0,
                "переполнение не применяет неполный набор импульсов");
    VoxelRigidBodyRelease(&bodies[0]);
}

static void TestTipsOverEdge(void)
{
    // Буфер шага не помещается в кадр стека: сборка без CRT ограничена
    // 4 КиБ, за ними компилятор зовёт отсутствующий __chkstk.
    static RigidHarness harness;
    HarnessInit(&harness, true);

    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    // Центр масс вынесен за край ступеньки: опора кончается на x = 4, а
    // центр стоит на 3.9. Держаться там не за что, и куб обязан
    // опрокинуться — это и есть проверка, что вращение вообще работает.
    DescribeCube(&description, 3.9, 0.5, 2.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    RigidExpect(Advance(&harness, &body, 1u, 256u), "шаги выполнены");

    double columns = Absolute(body.orientation[0]) + Absolute(body.orientation[1]) +
                     Absolute(body.orientation[2]);
    RigidExpect(columns > 0.02, "опора из-под центра масс обязана повернуть куб");

    VoxelRigidBodyRelease(&body);

    // Свес меньше половины ребра опрокидывать нечему: центр масс остаётся
    // над опорой, и грань куба лежит на ступеньке целиком. Проверке углов
    // эта грань была не видна, и она роняла куб на ровном месте.
    VoxelRigidBody resting;
    DescribeCube(&description, 4.3, 0.5, 2.0);
    RigidExpect(VoxelRigidBodyInitialize(&resting, 2u, &description), "тело создано");

    RigidExpect(Advance(&harness, &resting, 1u, 256u), "шаги выполнены");

    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&resting, position), "позиция читается");
    RigidExpect(Near(position[2], 1.5, 0.05), "куб со свесом остался на ступеньке");
    RigidExpect(position[0] > 4.0, "куб со свесом с неё не съехал");

    VoxelRigidBodyRelease(&resting);
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
    double length =
        body.orientation[0] * body.orientation[0] + body.orientation[1] * body.orientation[1] +
        body.orientation[2] * body.orientation[2] + body.orientation[3] * body.orientation[3];
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
    double length =
        body.orientation[0] * body.orientation[0] + body.orientation[1] * body.orientation[1] +
        body.orientation[2] * body.orientation[2] + body.orientation[3] * body.orientation[3];
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

// Перенос начала координат проверяется для разных величин и раскладок
// фиксированной точки. Перенос туда и обратно обязан вернуть позицию
// ровно на прежнее место, а не приблизительно.
static void TestRebasingPaths(void)
{
    // Позиция, у которой ни одна ось не равна нулю.
    VoxelRigidBody body;
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 12.25, -7.75, 3.5);
    RigidExpect(VoxelRigidBodyInitialize(&body, 1u, &description), "тело создано");

    const int64_t forward[3] = {1000000, -2000000, 3000000};
    const int64_t backward[3] = {-1000000, 2000000, -3000000};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, forward), "быстрый перенос");
    double position[3];
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(Near(position[0], 12.25 - 1000000.0, 1e-6), "X сдвинут");
    RigidExpect(Near(position[1], -7.75 + 2000000.0, 1e-6), "Y сдвинут");
    RigidExpect(Near(position[2], 3.5 - 3000000.0, 1e-6), "Z сдвинут");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, backward), "быстрый перенос обратно");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 12.25 && position[1] == -7.75 && position[2] == 3.5,
                "возврат обязан быть точным, а не приблизительным");
    VoxelRigidBodyRelease(&body);

    // Ось ровно в нуле: лимба под неё нет; перенос должен корректно создать
    // значение, а обратный перенос — восстановить канонический ноль.
    DescribeCube(&description, 0.0, 5.0, -5.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 2u, &description), "тело в нуле создано");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, forward), "перенос из нуля");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(Near(position[0], -1000000.0, 1e-6), "нулевая ось сдвинута");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, backward), "перенос обратно в ноль");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 0.0 && position[1] == 5.0 && position[2] == -5.0,
                "возврат в ноль обязан быть точным");
    VoxelRigidBodyRelease(&body);

    // Сдвиг за 2^31 блока: произведение на 2^32 уже не помещается в int64.
    DescribeCube(&description, 1.5, 2.5, 3.5);
    RigidExpect(VoxelRigidBodyInitialize(&body, 3u, &description), "тело создано");
    const int64_t huge[3] = {INT64_C(4294967296), 0, 0};
    const int64_t hugeBack[3] = {INT64_C(-4294967296), 0, 0};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, huge), "перенос на 2^32 блока");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] < -4000000000.0, "огромный сдвиг применён");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, hugeBack), "перенос обратно");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 1.5 && position[1] == 2.5 && position[2] == 3.5,
                "возврат после огромного сдвига обязан быть точным");
    VoxelRigidBodyRelease(&body);

    // Позиция за 2^31 блока: в фиксированной точке старший бит лимба занят;
    // результат не должен зависеть от представимости в знаковом int64.
    DescribeCube(&description, 3000000000.0, 1.0, 1.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 4u, &description), "далёкое тело создано");
    const int64_t away[3] = {-1000000000, 0, 0};
    const int64_t awayBack[3] = {1000000000, 0, 0};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, away), "перенос далёкого тела");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(Near(position[0], 4000000000.0, 1.0), "далёкое тело сдвинуто");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, awayBack), "перенос обратно");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 3000000000.0, "возврат далёкого тела обязан быть точным");
    VoxelRigidBodyRelease(&body);

    // Перенос ровно на собственную координату обращает её в точный ноль.
    // Внутри это единственная ветка, которая освобождает лимбы прямо во
    // время переноса, и обратный перенос обязан их вернуть.
    DescribeCube(&description, 4096.0, -8192.0, 2048.0);
    RigidExpect(VoxelRigidBodyInitialize(&body, 6u, &description), "тело на узле создано");
    const int64_t onto[3] = {4096, -8192, 2048};
    const int64_t ontoBack[3] = {-4096, 8192, -2048};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, onto), "перенос ровно на координату");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 0.0 && position[1] == 0.0 && position[2] == 0.0,
                "перенос на собственную координату обязан дать точный ноль");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, ontoBack), "перенос обратно с нуля");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 4096.0 && position[1] == -8192.0 && position[2] == 2048.0,
                "возврат из точного нуля обязан быть точным");
    VoxelRigidBodyRelease(&body);

    // Ровно 2^32 блока: в фиксированной точке это два лимба, младший нулевой.
    // Сдвиг на один блок обязан занять через границу лимба и укоротить число.
    DescribeCube(&description, 4294967296.0, 0.5, 0.5);
    RigidExpect(VoxelRigidBodyInitialize(&body, 7u, &description), "тело на границе лимба");
    const int64_t oneBlock[3] = {1, 0, 0};
    const int64_t oneBlockBack[3] = {-1, 0, 0};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, oneBlock), "заём через границу лимба");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 4294967295.0, "заём через границу лимба обязан быть точным");
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, oneBlockBack), "перенос обратно");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 4294967296.0 && position[1] == 0.5 && position[2] == 0.5,
                "возврат через границу лимба обязан быть точным");
    VoxelRigidBodyRelease(&body);

    // Нулевой сдвиг не обязан ничего менять и не обязан ничего выделять.
    DescribeCube(&description, 9.5, -9.5, 0.5);
    RigidExpect(VoxelRigidBodyInitialize(&body, 5u, &description), "тело создано");
    const int64_t none[3] = {0, 0, 0};
    RigidExpect(VoxelRigidBodyTranslateBlocks(&body, none), "нулевой перенос");
    RigidExpect(VoxelRigidBodyLocalPosition(&body, position), "позиция читается");
    RigidExpect(position[0] == 9.5 && position[1] == -9.5 && position[2] == 0.5,
                "нулевой перенос изменил позицию");
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

static void TestImpactWakesFiniteMassBody(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;
    harness.settings.penetrationCorrection = 0.0;

    VoxelRigidBody bodies[2];
    VoxelRigidBodyDescription description;
    DescribeCube(&description, 0.0, 0.0, 3.0);
    description.friction = 0.0;
    RigidExpect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description),
                "sleeping finite-mass body created");
    bodies[0].sleeping = true;
    description.position[0] = -0.99;
    RigidExpect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description), "impact body created");
    const double impact[3] = {4.0, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&bodies[1], impact), "impact velocity set");
    RigidExpect(Advance(&harness, bodies, 2u, 1u), "impact step executed");
    double firstVelocity[3];
    double secondVelocity[3];
    RigidExpect(VoxelRigidBodyLinearVelocity(&bodies[0], firstVelocity) &&
                    VoxelRigidBodyLinearVelocity(&bodies[1], secondVelocity),
                "impact velocities readable");
    RigidExpect(!bodies[0].sleeping && firstVelocity[0] > 0.5,
                "collision wakes sleeper and transfers momentum");
    RigidExpect(Near(firstVelocity[0] + secondVelocity[0], 4.0, 1e-7),
                "sleeping finite-mass collision conserves linear momentum");

    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestWakePropagatesAgainstStableOrder(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;
    harness.settings.penetrationCorrection = 0.0;
    VoxelRigidBody bodies[3];
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, 0.99 * (double)(2u - index), 0.0, 3.0);
        description.friction = 0.0;
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "wake-chain body created");
        bodies[index].sleeping = index != 2u;
    }
    const double impact[3] = {4.0, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&bodies[2], impact), "wake-chain impact set");
    RigidExpect(Advance(&harness, bodies, 3u, 1u), "wake-chain step executed");
    double momentum = 0.0;
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        double velocity[3];
        RigidExpect(!bodies[index].sleeping, "whole contact chain wakes in one tick");
        RigidExpect(VoxelRigidBodyLinearVelocity(&bodies[index], velocity),
                    "wake-chain velocity readable");
        momentum += velocity[0];
        VoxelRigidBodyRelease(&bodies[index]);
    }
    RigidExpect(Near(momentum, 4.0, 1e-7), "wake-chain preserves total momentum");
}

static void TestRemovedSupportWakesWholeIsland(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.penetrationCorrection = 0.0;
    VoxelRigidBody bodies[2];
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, 0.0, 0.0, 0.49 + 0.99 * (double)index);
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "support-removal body created");
        bodies[index].sleeping = true;
    }
    // World mutations invalidate sleeping contacts through the explicit API.
    harness.world.removeFloor = true;
    VoxelRigidBodyWake(&bodies[1]);
    RigidExpect(Advance(&harness, bodies, 2u, 1u), "removed-support step executed");
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        double velocity[3];
        RigidExpect(!bodies[index].sleeping, "removed-support island wakes together");
        RigidExpect(VoxelRigidBodyLinearVelocity(&bodies[index], velocity),
                    "removed-support velocity readable");
        RigidExpect(Near(velocity[2], -24.0 / 128.0, 1e-7),
                    "newly awakened member receives gravity in the same tick");
        VoxelRigidBodyRelease(&bodies[index]);
    }
}

static void TestContactIslandSleepsTogether(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;
    harness.settings.penetrationCorrection = 0.0;
    harness.settings.sleepLinearSpeed = 0.1;
    harness.settings.sleepAngularSpeed = 0.1;
    harness.settings.sleepFrames = 2u;
    VoxelRigidBody bodies[2];
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, 0.99 * (double)index, 0.0, 3.0);
        description.friction = 0.0;
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "island-sleep body created");
    }
    const double separating[3] = {0.11, 0.0, 0.0};
    RigidExpect(VoxelRigidBodyAddLinearVelocity(&bodies[1], separating),
                "island member remains above sleep speed");
    RigidExpect(Advance(&harness, bodies, 2u, 4u), "island sleep steps executed");
    RigidExpect(!bodies[0].sleeping && !bodies[1].sleeping,
                "quiet body does not freeze inside a moving contact island");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestRotatedBroadphaseExtent(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;
    VoxelRigidBody bodies[2];
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, index == 0u ? 0.99 : 2.1, 0.0, 3.0);
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "rotated broadphase body created");
        bodies[index].orientation[2] = 0.38268343236508977173;
        bodies[index].orientation[3] = 0.92387953251128675613;
    }
    RigidExpect(Advance(&harness, bodies, 2u, 1u), "rotated broadphase step executed");
    VoxelRigidStepStats stats;
    RigidExpect(
        VoxelRigidBodyReadStepStats(harness.scratch, 2u, (uint32_t)sizeof(harness.scratch), &stats),
        "rotated broadphase statistics readable");
    RigidExpect(stats.contactCount != 0u,
                "rotated boxes touching across two old centre cells still collide");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestGridCellBoundaryCandidate(void)
{
    static RigidHarness harness;
    HarnessInit(&harness, false);
    harness.settings.gravity[2] = 0.0;
    VoxelRigidBody bodies[2];
    VoxelRigidBodyDescription description;
    // Два куба, чьи центры почти ровно на клетку врозь: левый чуть ниже
    // нуля, правый чуть ниже следующей границы. При верном размере клетки
    // (два наибольших радиуса AABB) они попадают в соседние клетки и обязаны
    // найтись широким отбором. Если размер клетки посчитан не из радиуса AABB,
    // граница уезжает, пара теряется, и контакт пропадает.
    DescribeCube(&description, -0.002, 0.0, 3.0);
    RigidExpect(VoxelRigidBodyInitialize(&bodies[0], 1u, &description),
                "grid boundary body created");
    DescribeCube(&description, 0.996, 0.0, 3.0);
    RigidExpect(VoxelRigidBodyInitialize(&bodies[1], 2u, &description),
                "grid boundary body created");
    RigidExpect(Advance(&harness, bodies, 2u, 1u), "grid boundary step executed");
    VoxelRigidStepStats stats;
    RigidExpect(
        VoxelRigidBodyReadStepStats(harness.scratch, 2u, (uint32_t)sizeof(harness.scratch), &stats),
        "grid boundary statistics readable");
    RigidExpect(stats.candidatePairCount == 1u && stats.contactCount != 0u,
                "tight AABB pair across a cell boundary is not lost");
    VoxelRigidBodyRelease(&bodies[0]);
    VoxelRigidBodyRelease(&bodies[1]);
}

static void TestExactScratchLayout(void)
{
    static RigidHarness harness;
    static VoxelRigidBody bodies[7];
    HarnessInit(&harness, false);
    // Точная раскладка scratch прибита. На одно тело: кэш тела 256 Б, сетка
    // 32, кэш геометрии узкой фазы 8*64, холодные контакты 16*80, горячее
    // зеркало решателя 16*344, пять связных массивов 5*4, расписание цветов
    // 8 + 16*(1+4), корзины 64*4, статистика 16 и запас на выравнивание 64.
    // Смена раскладки обязана сломать эту проверку, а не пройти молча.
    RigidExpect(VoxelRigidBodyStepScratchBytes(1u) == 8028u, "exact scratch bytes for one body");
    RigidExpect(VoxelRigidBodyStepScratchBytes(7u) == 54180u, "exact scratch bytes for seven bodies");
    const uint32_t counts[] = {1u, 2u, 3u, 7u};
    const uint32_t offsets[] = {1u, 2u, 32u, 64u};
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, (double)index * 4.0, 0.0, 4.0);
        RigidExpect(VoxelRigidBodyInitialize(&bodies[index], index + 1u, &description),
                    "scratch layout body initialized");
    }
    for (uint32_t sample = 0u; sample < sizeof(counts) / sizeof(counts[0]); ++sample)
    {
        uint32_t count = counts[sample];
        uint32_t required = VoxelRigidBodyStepScratchBytes(count);
        for (uint32_t alignment = 0u; alignment < sizeof(offsets) / sizeof(offsets[0]); ++alignment)
        {
            uint32_t offset = offsets[alignment];
            RigidExpect(required != 0u && required + offset < sizeof(harness.scratch),
                        "exact scratch and guards fit test storage");
            for (uint32_t byte = 0u; byte < sizeof(harness.scratch); ++byte)
                harness.scratch[byte] = 0xa5u;
            RigidExpect(VoxelRigidBodyStep(bodies, count, &harness.collision, &harness.settings,
                                           harness.scratch + offset, required),
                        "step accepts exactly the reported scratch bytes");
            VoxelRigidStepStats stats;
            RigidExpect(
                VoxelRigidBodyReadStepStats(harness.scratch + offset, count, required, &stats),
                "stats share the exact scratch layout");
            RigidExpect(stats.activeBodyCount == count && stats.awakeBodyCount == count,
                        "exact scratch stats preserve active and awake counts");
            for (uint32_t byte = 0u; byte < offset; ++byte)
                RigidExpect(harness.scratch[byte] == 0xa5u, "scratch prefix guard unchanged");
            for (uint32_t byte = offset + required; byte < sizeof(harness.scratch); ++byte)
                RigidExpect(harness.scratch[byte] == 0xa5u, "scratch suffix guard unchanged");
        }
    }
    for (uint32_t index = 0u; index < 7u; ++index)
        VoxelRigidBodyRelease(&bodies[index]);
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
    VoxelRigidStepStats stats;
    RigidExpect(VoxelRigidBodyReadStepStats(harness.scratch + 1u, 1u,
                                            (uint32_t)sizeof(harness.scratch) - 1u, &stats),
                "статистика шага читается из scratch");
    RigidExpect(stats.activeBodyCount == 1u && stats.awakeBodyCount == 1u,
                "статистика содержит активное и бодрствующее тело");
    RigidExpect(!VoxelRigidBodyReadStepStats(harness.scratch + 1u, 1u, required - 1u, &stats),
                "статистика отвергает малый scratch");
    RigidExpect(!VoxelRigidBodyStep(&body, 1u, &harness.collision, &harness.settings,
                                    harness.scratch, required - 1u),
                "малый буфер отвергается");
    RigidExpect(!VoxelRigidBodyStep(&body, 1u, NULL, &harness.settings, harness.scratch, required),
                "источник столкновений обязателен");

    VoxelRigidStepSettings broken = harness.settings;
    broken.solverIterations = 0u;
    RigidExpect(
        !VoxelRigidBodyStep(&body, 1u, &harness.collision, &broken, harness.scratch, required),
        "ноль итераций отвергается");

    VoxelRigidBodyRelease(&body);
}

// Полоса переполнения цветного решателя: манифольдов больше, чем цветов,
// поэтому несколько манифольдов делят одно тело. Парный путь для этой полосы
// запрещён — он нарушил бы последовательный порядок импульсов. Хеш прибит к
// эталону без парного пути; если полосу переполнения снова начать решать
// парами, получается 0x6eb08a9579dc70ba вместо эталона.
static uint64_t OverflowHashWord(uint64_t hash, uint64_t word)
{
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        hash = (hash ^ (word & 255u)) * UINT64_C(1099511628211);
        word >>= 8;
    }
    return hash;
}

static uint64_t OverflowDoubleBits(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return representation.bits;
}

static void TestColoredOverflowReplay(void)
{
    static RigidHarness harness;
    static VoxelRigidBody bodies[82];
    // Точная раскладка scratch для 82 тел не влезает в общий harness.
    static uint8_t scratch[800000];
    HarnessInit(&harness, false);
    harness.world.removeFloor = true;
    for (uint32_t slot = 0u; slot < 82u; ++slot)
    {
        VoxelRigidBodyDescription description;
        DescribeCube(&description, 0.0, 0.0, 2.0);
        description.friction = 0.5;
        if (slot == 0u)
        {
            description.halfExtent[0] = 10.0;
            description.halfExtent[1] = 10.0;
            description.mass = 100.0;
            description.position[2] = 0.0;
        }
        else
        {
            uint32_t row = (slot - 1u) / 9u;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                description.halfExtent[axis] = 0.4;
            }
            description.position[0] = (double)((slot - 1u) % 9u) * 2.0 - 8.0;
            description.position[1] = (double)row * 2.0 - 8.0;
            description.position[2] = 0.8;
        }
        RigidExpect(VoxelRigidBodyInitialize(&bodies[slot], (uint64_t)slot + 1u, &description),
                    "colored overflow body created");
    }
    uint32_t required = VoxelRigidBodyStepScratchBytes(82u);
    RigidExpect(required != 0u && required <= (uint32_t)sizeof(scratch),
                "colored overflow scratch fits");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        harness.settings.gravity[axis] = 0.0;
    }
    harness.settings.sleepFrames = 0u;

    VoxelRigidStepOptions options = {0};
    options.structSize = (uint32_t)sizeof(options);
    options.solverOrder = VOXEL_RIGID_SOLVER_COLORED;
    VoxelRigidStepProfile profile = {0};
    profile.structSize = (uint32_t)sizeof(profile);
    options.profile = &profile;

    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t overflowSeen = 0u;
    for (uint32_t step = 0u; step < 32u; ++step)
    {
        RigidExpect(VoxelRigidBodyStepEx(bodies, 82u, &harness.collision, &harness.settings,
                                         scratch, (uint32_t)sizeof(scratch), &options),
                    "colored overflow step executed");
        if (profile.solverOverflowContacts > overflowSeen)
        {
            overflowSeen = profile.solverOverflowContacts;
        }
        for (uint32_t slot = 0u; slot < 82u; ++slot)
        {
            double position[3];
            double linear[3];
            double angular[3];
            RigidExpect(VoxelRigidBodyLocalPosition(&bodies[slot], position) &&
                            VoxelRigidBodyLinearVelocity(&bodies[slot], linear) &&
                            VoxelRigidBodyAngularVelocity(&bodies[slot], angular),
                        "colored overflow state readable");
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                hash = OverflowHashWord(hash, OverflowDoubleBits(position[axis]));
                hash = OverflowHashWord(hash, OverflowDoubleBits(linear[axis]));
                hash = OverflowHashWord(hash, OverflowDoubleBits(angular[axis]));
            }
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                hash =
                    OverflowHashWord(hash, OverflowDoubleBits(bodies[slot].orientation[component]));
            }
        }
    }
    RigidExpect(overflowSeen > 0u, "colored overflow scene actually overflows colors");
    RigidExpect(hash == UINT64_C(0x493245ececa466b1),
                "colored overflow stays scalar and replay-stable");
    for (uint32_t slot = 0u; slot < 82u; ++slot)
    {
        VoxelRigidBodyRelease(&bodies[slot]);
    }
}

LAIUE_TEST_ENTRY(RigidBodyTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());
    TestDescriptionRefusals();
    TestWorldContactsUseSharedBudget();
    TestRestsOnFloor();
    TestRotatedBoxRestsOnPillar();
    TestThinFloorPushesUp();
    TestOverhangingBoxKeepsContact();
    TestTipsOverEdge();
    TestFreeSpinPersists();
    TestPointVelocityCarries();
    TestUnboundedSpeed();
    TestUnboundedSpinStaysSane();
    TestRebasing();
    TestRebasingPaths();
    TestStableIdOrder();
    TestStackSettles();
    TestSleepAndWake();
    TestImpactWakesFiniteMassBody();
    TestWakePropagatesAgainstStableOrder();
    TestRemovedSupportWakesWholeIsland();
    TestContactIslandSleepsTogether();
    TestRotatedBroadphaseExtent();
    TestGridCellBoundaryCandidate();
    TestColoredOverflowReplay();
    TestExactScratchLayout();
    TestScratchRefusals();

    LaiueTestRuntimeWrite("Rigid body tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
